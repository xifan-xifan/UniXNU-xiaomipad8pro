// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I.. -I../../PianoDpu gop_power_shot_model_host.cpp -o /tmp/gop_power_shot_model_host && /tmp/gop_power_shot_model_host
//
// Model the kernel call boundaries as explicit steps. Power requests, copy completion,
// panel replies, work-loop callbacks and capture mutations are scheduled without threads.
#include "GopColorLog.h"
#include "GopCompose.h"
#include "GopFence.h"
#include "GopPanelPower.h"
#include "GopRefresh.h"
#include "GopShot.h"
#include "GopShotDiagnostics.h"
#include "../../PianoBacklight/NativeBrightness.h"
#include "../../PianoDpu/DpuFlip.h"
#include "../../PianoDpu/DpuNightShift.h"
#include "../../PianoDpu/DpuPanel.h"
#include "../../PianoDpu/tests/dpu_model.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expr); std::abort(); \
} } while (false)

namespace {
namespace bl = pianobacklight;
namespace flip = pianodpu::flip;
namespace panel = pianodpu::panel;
namespace pcc = pianodpu::pcc;
using namespace gopcompose;

struct VblTimer {
    uint32_t hz;
    bool fixed;
    uint64_t due = 0, scheduled = 0;
    unsigned ticks = 0, late = 0;
    uint64_t maxLate = 0;

    void stop() { due = scheduled = 0; }

    void arm(uint64_t now)
    {
        const uint64_t period = goprefresh::periodNs(hz);
        due = fixed ? goprefresh::nextDeadline(due, now, period) : now + period;
        scheduled = fixed ? due : now + (1000000 / hz) * 1000ull;
    }

    void fire(uint64_t now)
    {
        CHECK(scheduled && now >= scheduled);
        ++ticks;
        if (now > due && now - due > 20000000) {
            ++late;
            maxLate = std::max(maxLate, now - due);
        }
    }
};

struct BacklightRegisters final : bl::Ics {
    uint8_t registers[bl::kIcs][bl::kRegCount] = {};
    unsigned writes = 0;

    BacklightRegisters()
    {
        for (auto &ic : registers) {
            ic[bl::kRegId] = 0x21;
            ic[bl::kRegEn] = bl::kEnBacklight | bl::kEnSinks;
            ic[bl::kRegBrtMsb] = 100;
        }
    }

    bool read(unsigned ic, uint8_t reg, uint8_t *value) override
    {
        *value = registers[ic][reg];
        return true;
    }
    bool write(unsigned ic, uint8_t reg, uint8_t value) override
    {
        CHECK(bl::writable(reg));
        registers[ic][reg] = value;
        ++writes;
        return true;
    }
    void done() override {}
    void log(const char *) override {}
    uint64_t nowMs() override { return 0; }
};

enum class PowerEvent { blank, parkTouch, sleep, wake, replay, releaseTouch, publishOn, light };
using enum PowerEvent;

struct PowerModel {
    PanelPower power;
    Pipeline pipe;
    Slot slot;
    Busy copy;
    uint64_t doneSerial = 0;
    unsigned completed = 0, wakeNotifications = 0;
    bool familyQueued = false, jobDonePosted = false;
    int frameSurface = 0;
    VblTimer vbl{60, false};

    BacklightRegisters registers;
    bl::Controller backlight{registers};
    model::Dpu io;
    panel::RequestGate dpuGate{.ready = true};
    pianodpu::slices::LivePath panelPath;
    panel::Result sleepResult, wakeResult;
    bool needsWake = false, backlightOnly = false, touchHeld = false, panelAsleep = false, panelUnsafe = false;
    pcc::NightShift nightShift;
    pcc::PanelPcc base, last;
    const bool nightShiftEnabled = gopcolor::nightShiftEnabled(1, false);

    struct Edge { bool on; uint32_t system, api; };
    Edge request{};
    std::optional<Edge> workerEdge;
    std::optional<bool> published;
    std::vector<PowerEvent> events;

    PowerModel()
    {
        backlight.configure({.bootPercent = 0});
        CHECK(backlight.survey() && backlight.start() && backlight.bothLit());
        // Use a programmable-fetch path so the sleep rollback can be fault-injected.
        using namespace pianodpu;
        using namespace pianodpu::slices;
        for (unsigned link = 1; link <= 2; ++link) {
            io.regs[intfReg(link, kIntfConfig)] = kIntfConfigProgFetch;
            io.intf[link].fetch = io.regs[intfReg(link, kIntfProgFetch)] = 2290 * model::kHtotal;
        }
        CHECK(pcc::capturePanelPcc(io, readPath(io), base));
        last = base;
    }

    void displayOn()
    {
        if (published != true)
            events.push_back(publishOn);
        published = true;
        const bool wasLit = backlight.bothLit();
        backlight.display(true, request.system, request.api);
        if (!wasLit && backlight.bothLit())
            events.push_back(light);
    }

    void powerChange(uint32_t system, uint32_t api)
    {
        request = {system != 0 && api != 0, system, api};
        if (!request.on) {
            published = false;
            backlight.display(false, system, api);
        }
        power.request(request.on);
        if (request.on && power.canPresent())
            displayOn();
    }

    bool beginEdge()
    {
        bool on = false;
        if (copy.serial || workerEdge || !power.begin(on))
            return false;
        CHECK(on == request.on);
        workerEdge = request;
        return true;
    }

    void replayColor()
    {
        if (!nightShiftEnabled || panelUnsafe || panelAsleep)
            return;
        const auto wanted = pcc::nightShiftPrograms(base, nightShift);
        const auto update = pcc::updateNightShift(io, pianodpu::slices::readPath(io), wanted, last, true);
        CHECK(update.attempted && update.result.ok);
        events.push_back(replay);
    }

    void panelOff(const Edge &edge)
    {
        if (backlightOnly)
            return;
        backlight.display(false, edge.system, edge.api);
        if (!backlight.bothDark())
            return;
        events.push_back(blank);
        CHECK(!touchHeld);
        touchHeld = true;
        events.push_back(parkTouch);
        if (!dpuGate.admit(false, [](const char *) {}))
            return;
        events.push_back(sleep);
        panelPath = pianodpu::slices::readPath(io);
        sleepResult = panel::panelOff(io, panelPath, panel::Options{});
        panelAsleep = sleepResult.ok;
        panelUnsafe = !sleepResult.ok && !sleepResult.safeAwake && !sleepResult.restored;
        dpuGate.ready = sleepResult.ok;
        dpuGate.wakePending = !sleepResult.ok;
        needsWake = true; // success or a failure from a dispatched DPU sleep
    }

    void panelOn()
    {
        if (needsWake) {
            needsWake = false;
            CHECK(dpuGate.admit(true, [](const char *) {}));
            events.push_back(wake);
            if (panelAsleep) {
                wakeResult = panel::panelOn(io, panelPath, panel::Options{}, sleepResult.enginesStopped, sleepResult.fetch);
                panelAsleep = false;
                panelUnsafe = !wakeResult.scanoutRestored;
                backlightOnly = !wakeResult.ok || panelUnsafe;
            } else {
                backlightOnly = panelUnsafe; // safe sleep rollback: panelWake has nothing to restore
            }
            dpuGate.wakePending = false;
            if (backlightOnly)
                dpuGate.latchFallback();
            else
                replayColor();
        } else {
            replayColor(); // backlight-only wake requests a mode-set replay
        }
        if (touchHeld) {
            touchHeld = false;
            events.push_back(releaseTouch);
        }
    }

    void completeEdge()
    {
        CHECK(workerEdge && !copy.serial);
        const bool on = workerEdge->on;
        if (on)
            panelOn();
        else
            panelOff(*workerEdge);
        workerEdge.reset();
        power.finish(on);  // a failed hardware wake still completes the software wake
        jobDonePosted |= power.wakeComplete;
    }

    void swapBegin()
    {
        if (published == false) {
            vbl.stop();  // M5 blocks even with queued work until jobDone publishes the wake
            return;
        }
        if (familyQueued && !pipe.pending) {
            SwapLayers layers;
            layers.mask = 1;
            layers.surface[0] = &frameSurface;
            LayerChange change;
            const Job job = pipe.swapJob(layers, false, 0, true, &change);
            const auto admission = admitSwap(slot, copy, job, change, kDeferAlways);
            CHECK(admission.waitSerial == job.serial);
            pipe.pending = true;
            pipe.waitSerial = admission.waitSerial;
            familyQueued = false;
        } else {
            Job refresh;
            if (pipe.refreshJob(&refresh)) {
                Job displaced;
                offer(slot, refresh, &displaced);
            }
        }
        if (vblIdleStops(true, pipe.refreshLeft, false, pipe.pending, familyQueued))
            vbl.stop();
        else
            vbl.arm(io.ns);
    }

    void submit()
    {
        CHECK(!familyQueued);
        familyQueued = true;
        swapBegin();
    }

    bool takeCopy()
    {
        Job job;
        return !workerEdge && !copy.serial && power.canPresent() && takeJob(slot, copy, &job);
    }

    void finishCopy()
    {
        CHECK(copy.serial);
        jobDonePosted |= finishJob(copy, &doneSerial, pipe.waitSerial);
    }

    void jobDone()
    {
        CHECK(jobDonePosted);
        jobDonePosted = false;
        const bool woke = power.wakeComplete;
        power.wakeComplete = false;
        const bool done = pipe.pending && doneSerial >= pipe.waitSerial;
        if (woke) {
            ++wakeNotifications;
            displayOn();
        }
        if (done) {
            pipe.swapDone(false);
            ++completed;
        }
        if (woke || done)
            swapBegin();
    }
};

void testPowerInterleavings()
{
    PowerModel m;
    m.powerChange(0, 0);
    m.powerChange(1, 0);
    CHECK(m.beginEdge() && !m.beginEdge());
    m.completeEdge();
    CHECK(m.backlight.bothLit() && m.registers.writes == 0 && m.io.dcs.empty() && m.events.empty());
    CHECK(!m.power.canPresent() && !m.touchHeld && !m.beginEdge());
    m.powerChange(1, 2);
    CHECK(m.beginEdge());
    m.completeEdge();
    m.jobDone();
    CHECK(m.wakeNotifications == 1 && m.published == true);

    m.events.clear();
    m.submit();
    CHECK(m.takeCopy());
    m.powerChange(0, 2); // system off while the worker is copying
    m.powerChange(0, 0); // WindowServer also off
    CHECK(m.backlight.bothDark() && !m.beginEdge() && !m.takeCopy() && m.io.dcs.empty());
    m.powerChange(1, 0); // one source alone keeps it off
    CHECK(!m.power.wantedOn);
    m.powerChange(1, 2); // wake before the current copy drains, coalescing the off
    m.finishCopy();
    m.jobDone();         // the copy completes, but only the wake publishes on
    CHECK(m.completed == 1 && m.published == false && !m.vbl.due);
    CHECK(m.beginEdge() && m.workerEdge->on);
    m.powerChange(1, 1); // repeated on while wake runs
    m.completeEdge();
    m.jobDone();
    CHECK(m.wakeNotifications == 2 && m.io.dcs.empty());
    CHECK(m.events == (std::vector<PowerEvent>{replay, publishOn, light}));

    m.events.clear();
    m.powerChange(1, 0);
    CHECK(m.beginEdge());
    m.powerChange(0, 0); // repeated off while the dispatched edge runs
    m.powerChange(1, 2);
    m.completeEdge();
    CHECK(m.panelAsleep && m.touchHeld && m.power.pending && !m.power.canPresent());
    CHECK(m.beginEdge());
    m.completeEdge();
    CHECK(m.power.wakeComplete && !m.touchHeld && m.backlight.bothDark());
    m.powerChange(1, 0); // invalidate a completed wake whose work-loop event is already posted
    m.jobDone();
    CHECK(m.published == false && m.backlight.bothDark() && m.wakeNotifications == 2);
    CHECK(m.events == (std::vector<PowerEvent>{blank, parkTouch, sleep, wake, replay, releaseTouch}));
    CHECK(m.beginEdge());
    m.completeEdge();
    m.powerChange(1, 2);
    CHECK(m.beginEdge());
    m.completeEdge();
    m.jobDone();
    const unsigned writes = m.registers.writes;
    const size_t commands = m.io.dcs.size();
    m.powerChange(2, 3);
    m.powerChange(1, 1);
    CHECK(!m.beginEdge() && !m.jobDonePosted && m.wakeNotifications == 3);
    CHECK(m.registers.writes == writes && m.io.dcs.size() == commands && m.backlight.bothLit());

    m.powerChange(0, 1);
    CHECK(m.beginEdge());
    m.completeEdge();
    m.powerChange(1, 1);
    CHECK(m.beginEdge() && m.workerEdge->on);
    m.powerChange(1, 0); // supersede the wake before the panel worker returns
    m.completeEdge();
    CHECK(!m.power.wakeComplete && !m.jobDonePosted && !m.power.canPresent());
    CHECK(!m.touchHeld && m.backlight.bothDark() && m.published == false && m.wakeNotifications == 3);
    CHECK(m.beginEdge() && !m.workerEdge->on);
    m.completeEdge();
}

void testPanelFailures()
{
    for (const bool failWake : {false, true}) {
        PowerModel m;
        using namespace pianodpu;
        using namespace pianodpu::slices;
        m.powerChange(1, 1);
        m.events.clear();
        if (!failWake) {
            // Lose the slave fetch-clear store after handoff. The real panel helper rolls back both links.
            m.io.dropStore = intfReg(2, kIntfConfig);
            m.io.dropValue = 0;
            m.io.dropCount = 1;
        }
        m.powerChange(1, 0);
        CHECK(m.beginEdge());
        m.completeEdge();
        CHECK(m.needsWake && m.touchHeld && m.backlight.bothDark() && !m.backlightOnly);
        CHECK(m.sleepResult.ok == failWake && m.panelAsleep == failWake);
        if (failWake) {
            m.io.engineDead[0] = m.io.engineDead[1] = true; // command path wakes, neither timing engine starts
        } else {
            CHECK(m.sleepResult.restored && m.io.half[0].displayOn && m.io.half[1].displayOn);
            CHECK(m.io.intf[1].on && m.io.intf[2].on && m.dpuGate.wakePending);
        }
        m.submit();
        CHECK(!m.vbl.due && m.familyQueued && !m.takeCopy());

        m.powerChange(1, 1);
        const uint64_t wakeStarted = m.io.ns;
        CHECK(m.beginEdge());
        m.completeEdge();
        CHECK(m.io.ns - wakeStarted < panel::kPanelReplyTimeoutMs * 1000000ull);
        CHECK(m.power.canPresent() && m.power.wakeComplete && !m.touchHeld && m.backlight.bothDark());
        CHECK(m.backlightOnly == failWake && m.dpuGate.fallback == failWake);
        if (failWake) {
            CHECK(!m.wakeResult.ok && !m.wakeResult.scanoutRestored && m.panelUnsafe);
            CHECK(m.events == (std::vector<PowerEvent>{blank, parkTouch, sleep, wake, releaseTouch}));
        } else {
            CHECK(m.events == (std::vector<PowerEvent>{blank, parkTouch, sleep, wake, replay, releaseTouch}));
        }
        m.jobDone();
        CHECK(m.events[m.events.size() - 2] == publishOn && m.events.back() == light);
        CHECK(m.vbl.due > m.io.ns && !m.familyQueued && m.takeCopy());
        m.finishCopy();
        m.jobDone();
        CHECK(m.wakeNotifications == 1 && m.completed == 1 && !m.pipe.pending && m.backlight.bothLit());
        m.io.advanceTo(m.vbl.scheduled);
        m.vbl.fire(m.io.ns);
        m.swapBegin();
        CHECK(m.vbl.ticks == 1 && m.vbl.scheduled > m.io.ns);
        if (failWake) {
            m.events.clear();
            m.powerChange(0, 1);
            CHECK(m.beginEdge());
            m.completeEdge();
            m.powerChange(1, 1);
            CHECK(m.beginEdge());
            m.completeEdge();
            m.jobDone();
            CHECK(m.events == (std::vector<PowerEvent>{publishOn, light}) && m.wakeNotifications == 2);
        }
    }
}

void testNightShiftReplay()
{
    PowerModel m;
    m.powerChange(1, 1);
    constexpr uint64_t one = 1ull << 32;
    const uint64_t reference[9] = {one, 0, 0, 0, one, 0, 0, 0, one};
    const uint64_t warm[9] = {one, 0, 0, 0, one * 3 / 4, 0, 0, 0, one / 2};
    const uint64_t blankMatrix[9] = {};
    CHECK(gopcolor::kNightShiftLocation == 9 && gopcolor::shape(warm) == gopcolor::Shape::diagonal);
    CHECK(m.nightShift.setMatrix(reference) == pcc::NightShift::Input::accepted);
    CHECK(m.nightShift.setMatrix(warm) == pcc::NightShift::Input::accepted);
    m.nightShift.setCorrection(0x8000);
    m.replayColor();
    const auto wanted = m.last;
    const auto checkRegisters = [&] {
        for (unsigned d = 0; d < 2; ++d) {
            CHECK(m.io.pccActive[d][0] == 1);
            CHECK(m.io.pccActive[d][4] == 0x4000);  // 1/2 red, 3/8 green, 1/4 blue in Q15
            CHECK(m.io.pccActive[d][8] == 0x3000);
            CHECK(m.io.pccActive[d][12] == 0x2000);
        }
        CHECK(pcc::same(m.last, wanted));
    };
    checkRegisters();
    m.powerChange(1, 0);
    CHECK(m.beginEdge());
    m.completeEdge();
    CHECK(m.nightShift.setMatrix(blankMatrix) == pcc::NightShift::Input::blank);
    const auto losePcc = [&] {
        std::memset(m.io.pccActive, 0, sizeof m.io.pccActive);
        std::memset(m.io.pccShadow, 0, sizeof m.io.pccShadow);
    };
    losePcc();
    m.powerChange(1, 1);
    CHECK(m.beginEdge());
    m.completeEdge();
    checkRegisters();
    CHECK(m.events[m.events.size() - 2] == replay && m.events.back() == releaseTouch && m.backlight.bothDark());
    m.jobDone();

    losePcc(); // a mode-set reset with the same desired state
    const auto unchanged = pcc::updateNightShift(m.io, pianodpu::slices::readPath(m.io), wanted, m.last, false);
    CHECK(!unchanged.attempted && m.io.pccActive[0][0] == 0);
    m.replayColor();
    checkRegisters();
}

// The two record layouts are kernel ABI details. The portable PianoBacklight converter
// uses the same integer conversion as swapNitsFixed in NativeGopDisplay.cpp.
struct SwapBrightness {
    static constexpr size_t flag = 0x3b4, nits = 0x3be;
    std::array<uint8_t, nits + sizeof(uint64_t)> record{};
    const bl::NativeBrightness range{128, PA_SDR_MAX_CODE};
    int32_t last = -1;
    unsigned delivered = 0;
    uint32_t applied = 0;

    void submit(uint8_t flags, uint64_t bits, bool known = true)
    {
        record[flag] = flags;
        std::memcpy(record.data() + nits, &bits, sizeof bits);
        if (!known || !(record[flag] & 1))
            return;
        uint64_t raw = 0;
        std::memcpy(&raw, record.data() + nits, sizeof raw);
        const int32_t fixed = bl::nitsFixedFromDouble(raw);
        if (fixed == last)
            return;
        last = fixed;
        ++delivered;
        if (fixed > 0)
            applied = std::clamp(uint32_t(fixed), range.minFixed(), range.maxFixed());
    }
};

void testSwapBrightness()
{
    const auto bits = [](double nits) { return std::bit_cast<uint64_t>(nits); };
    SwapBrightness stream;
    stream.submit(2, bits(100.25));
    stream.submit(1, bits(100.25), false);
    CHECK(stream.delivered == 0);
    stream.submit(3, bits(100.25));
    CHECK(stream.delivered == 1 && stream.last == 0x644000 && stream.applied == 0x644000);

    for (const uint64_t invalid : {bits(0.0), bits(-0.0), bits(-20.0), bits(std::numeric_limits<double>::denorm_min()),
                                  bits(std::numeric_limits<double>::quiet_NaN()), bits(std::numeric_limits<double>::infinity()),
                                  bits(-std::numeric_limits<double>::infinity()), bits(std::nextafter(0x1p-16, 0.0))}) {
        stream.submit(1, invalid);
        CHECK(stream.last == 0 && stream.applied == 0x644000);
    }
    CHECK(stream.delivered == 2); // one zero is forwarded; the backlight drops it and later duplicates are suppressed

    stream.submit(1, bits(0x1p-16));
    CHECK(stream.last == 1 && stream.applied == stream.range.minFixed());
    stream.submit(1, bits(600.0));
    CHECK(stream.last == (600 << 16) && stream.applied == stream.range.maxFixed());
    stream.submit(1, bits(std::nextafter(32768.0, 0.0)));
    CHECK(stream.last == INT32_MAX && stream.applied == (600u << 16));
    const unsigned delivered = stream.delivered;
    stream.submit(1, bits(32768.0));
    stream.submit(1, bits(std::numeric_limits<double>::max()));
    CHECK(stream.delivered == delivered && stream.last == INT32_MAX);
    uint16_t code = 0;
    CHECK(!stream.range.decode(0, code) && !stream.range.decode(INT32_MAX, code));
    CHECK(!stream.range.decode(stream.range.minFixed() - 1, code));
    CHECK(!stream.range.decode(uint64_t(stream.range.maxFixed()) + 1, code));
    CHECK(stream.range.decode(stream.applied, code) && code == PA_SDR_MAX_CODE);
}

void testDeadlines()
{
    constexpr uint64_t start = 1000000000;
    VblTimer daily{60, false};
    daily.arm(start);
    CHECK(daily.due == start + 16666666 && daily.scheduled == start + 16666000);
    daily.arm(start + 8000000);
    CHECK(daily.due == start + 24666666 && daily.scheduled == start + 24666000);
    daily.fire(daily.scheduled);
    CHECK(daily.ticks == 1 && daily.late == 0);

    constexpr uint64_t period = 8333333;
    VblTimer fixed{120, true}; // opt-in contract, exercised offline while the 120 Hz gate is HOLD
    uint64_t attributes[goprefresh::kWords] = {};
    CHECK(goprefresh::fixedAttributes(120, [](uint64_t ns) { return ns; }, attributes));
    CHECK(attributes[goprefresh::kMinAbs] == period && attributes[goprefresh::kMaxAbs] == period);
    CHECK(attributes[goprefresh::kMinFixed] == (1ull << 32) / 120 && attributes[goprefresh::kStepAbs] == 0);
    CHECK(!gopfence::deadlineControlAvailable(120, 60, true, true));
    CHECK(gopfence::deadlineControlAvailable(120, 120, true, true));
    fixed.arm(start);
    fixed.arm(start + period / 2);
    CHECK(fixed.scheduled == start + period);
    const uint64_t late = fixed.due + 3 * period + 7;
    fixed.fire(late);
    fixed.arm(late);
    CHECK(fixed.due == start + 5 * period && fixed.ticks == 1 && fixed.late == 1 && fixed.maxLate == 3 * period + 7);
    const uint64_t stalled = fixed.due + 8 * period + 1;
    fixed.fire(stalled);
    fixed.arm(stalled);
    CHECK(fixed.due == stalled + period && fixed.ticks == 2 && fixed.late == 2 && fixed.maxLate == 8 * period + 1);
    fixed.stop();
    fixed.arm(stalled + 1);
    CHECK(fixed.due == stalled + 1 + period);
    daily.stop();
    daily.arm(stalled);
    const uint64_t onThreshold = daily.due + 20000000;
    daily.fire(onThreshold);
    CHECK(daily.late == 0);
    daily.arm(onThreshold);
    daily.fire(daily.due + 20000001);
    CHECK(daily.late == 1 && daily.maxLate == 20000001);
}

using gopshot::BusyReason;
constexpr size_t pixelBytes = GOP_SHOT_ROW_BYTES * GOP_SHOT_HEIGHT;
static_assert(GOP_SHOT_BYTES == 27340820);
constexpr uint32_t flipStride = GOP_SHOT_ROW_BYTES + 64;
constexpr uint64_t flipOffset = 4096;
constexpr uint64_t mappedBytes(uint32_t stride) { return (GOP_SHOT_HEIGHT - 1ull) * stride + GOP_SHOT_ROW_BYTES; }

struct ShotReport {
    int error = 0;
    GopShotHeader header{};
    size_t bytes = 0;  // successfully emitted pixel bytes, excluding the header
    uint8_t firstPixelByte = 0, lastPixelByte = 0;
    PianoDpuScanoutDiagnostics dpu;
    BusyReason reason = BusyReason::count;
    uint64_t count = 0, startUs = 0, endUs = 0, startEpoch = 0, endEpoch = 0;
};

struct ShotModel {
    gopshot::BusyCounts counts;
    bool diagnose = true, hasDpu = true, initialLock = true, finalLock = true, genericBusy = false;
    bool pinnedFlip = true, pendingFlip = false;
    uint64_t epoch = 0, ns = 0;
    uint64_t flips = 1, reshows = 0, toBoot = 0;
    flip::Target target{0x10000000, flipStride, true};
    flip::State state = flip::State::ready;
    size_t failChunk = SIZE_MAX;
    std::vector<uint8_t> boot = std::vector<uint8_t>(pixelBytes, 0xb0);
    std::vector<uint8_t> surface = std::vector<uint8_t>(flipOffset + mappedBytes(flipStride), 0xf1);

    PianoDpuScanoutSample sample() const
    {
        return {true, target.surface, target.base, target.stride, uint32_t(state), flips, reshows, toBoot};
    }

    uint64_t generation() const { return flips + reshows + toBoot; }

    template <class DuringCopy>
    ShotReport capture(DuringCopy duringCopy, size_t oldlen = GOP_SHOT_BYTES)
    {
        ShotReport out;
        if (oldlen < GOP_SHOT_BYTES) {
            out.error = ENOMEM;
            return out;
        }
        const uint64_t start = epoch;
        const uint64_t startUs = ns / 1000;
        const auto busy = [&](BusyReason reason) {
            out.error = EBUSY;
            if (diagnose) {
                out.reason = reason;
                out.count = counts.record(reason);
                out.startUs = startUs;
                out.endUs = ns / 1000;
                out.startEpoch = start;
                out.endEpoch = epoch;
            }
            return out;
        };
        if (start & 1)
            return busy(BusyReason::oddEpoch);

        const auto before = target;
        const auto beforeState = state;
        const uint64_t beforeGeneration = generation();
        if (hasDpu) {
            if (genericBusy)
                return busy(gopshot::dpuBusyReason(out.dpu.busy));
            if (!initialLock) {
                if (diagnose) out.dpu.busy = PianoDpuScanoutBusy::initialLock;
                return busy(gopshot::dpuBusyReason(out.dpu.busy));
            }
            if (diagnose) out.dpu.before = sample();
        }
        const bool useFlip = hasDpu && before.surface && beforeState == flip::State::ready && pinnedFlip;
        const bool useBoot = !hasDpu || (flip::sameTarget(before, flip::Target{}) && !pendingFlip);
        if (!useFlip && !useBoot) {
            out.error = ENXIO;
            return out;
        }

        const uint32_t stride = useFlip ? before.stride : GOP_SHOT_ROW_BYTES;
        const uint64_t offset = useFlip ? flipOffset : 0;
        const auto &mapping = useFlip ? surface : boot;
        out.error = gop_shot_bounds(!mapping.empty(), mapping.size(), offset, stride);
        if (!out.error) {
            out.header = {GOP_SHOT_VERSION, GOP_SHOT_WIDTH, GOP_SHOT_HEIGHT, useFlip ? GOP_SHOT_BGA : GOP_SHOT_BGRA8,
                          useFlip ? uint32_t(GOP_SHOT_FLIP) : uint32_t(GOP_SHOT_BOOT)};
            for (size_t chunk = 0; ; ++chunk) {
                uint64_t at = 0;
                const size_t size = gop_shot_chunk(out.bytes, stride, &at);
                if (!size)
                    break;
                CHECK(size <= GOP_SHOT_CHUNK_BYTES && size <= pixelBytes - out.bytes);
                CHECK(at <= mapping.size() - offset && size <= mapping.size() - offset - at);
                if (useFlip) {
                    CHECK(at == chunk * stride && size == GOP_SHOT_ROW_BYTES);
                }
                if (chunk == failChunk) {
                    out.error = EFAULT;
                    break;
                }
                if (chunk == 0)
                    out.firstPixelByte = mapping[offset + at];
                out.lastPixelByte = mapping[offset + at + size - 1];
                out.bytes += size;
                if (chunk == 0)
                    duringCopy();
            }
        }
        // A callback error bypasses the final DPU try-lock, but the GOP epoch check still runs.
        if (hasDpu && !out.error) {
            if (!finalLock) {
                if (diagnose) out.dpu.busy = PianoDpuScanoutBusy::finalLock;
                return busy(gopshot::dpuBusyReason(out.dpu.busy));
            }
            if (diagnose) out.dpu.after = sample();
            if (state != beforeState || generation() != beforeGeneration || !flip::sameTarget(target, before)) {
                if (diagnose) out.dpu.busy = PianoDpuScanoutBusy::changed;
                return busy(gopshot::dpuBusyReason(out.dpu.busy));
            }
        }
        return start == epoch ? out : busy(BusyReason::finalEpoch);
    }
};

void testShotDiagnostics()
{
    ShotModel m;
    const auto stable = [] {};
    const auto checkBusy = [&](const ShotReport &r, BusyReason why, const char *name, uint64_t count,
                               size_t bytes, bool before, bool after) {
        CHECK(r.error == EBUSY && r.reason == why && std::strcmp(gopshot::busyReasonName(r.reason), name) == 0);
        CHECK(r.count == count && r.bytes == bytes && r.dpu.before.available == before && r.dpu.after.available == after);
        CHECK(r.startUs <= r.endUs && r.endUs == m.ns / 1000 && r.endEpoch == m.epoch);
    };

    m.ns = (uint64_t(UINT32_MAX) + 1) * 1000;
    m.epoch = 1;
    m.initialLock = false;
    checkBusy(m.capture(stable), BusyReason::oddEpoch, "odd-epoch", 1, 0, false, false);
    m.epoch = 2;
    auto initial = m.capture(stable);
    checkBusy(initial, BusyReason::initialLock, "initial-flip-lock", 1, 0, false, false);
    CHECK(initial.startUs == 4294967296ull && initial.startEpoch == 2);
    m.initialLock = true;
    m.finalLock = false;
    auto final = m.capture([&] { ++m.epoch; }); // the DPU error takes precedence over the final epoch
    checkBusy(final, BusyReason::finalLock, "final-flip-lock", 1, pixelBytes, true, false);
    CHECK(final.startEpoch == 2 && final.endEpoch == 3 && final.dpu.before.surface);
    m.finalLock = true;
    m.epoch = 4;
    const auto newTarget = m.capture([&] { m.target.base += 4096; });
    checkBusy(newTarget, BusyReason::scanoutChanged, "scanout-changed", 1, pixelBytes, true, true);
    CHECK(newTarget.dpu.before.base != newTarget.dpu.after.base && newTarget.dpu.before.flips == newTarget.dpu.after.flips);

    m.ns = UINT64_MAX - 1500;
    m.reshows = UINT32_MAX;
    const auto reshown = m.capture([&] { ++m.reshows; m.ns += 1500; });
    checkBusy(reshown, BusyReason::scanoutChanged, "scanout-changed", 2, pixelBytes, true, true);
    CHECK(reshown.dpu.before.base == reshown.dpu.after.base && reshown.dpu.after.reshows == 4294967296ull);
    CHECK(reshown.startUs == (UINT64_MAX - 1500) / 1000 && reshown.endUs == UINT64_MAX / 1000);
    const auto stateChanged = m.capture([&] { m.state = flip::State::failed; });
    checkBusy(stateChanged, BusyReason::scanoutChanged, "scanout-changed", 3, pixelBytes, true, true);
    m.state = flip::State::ready;

    m.epoch = UINT64_MAX - 1;
    const auto changedEpoch = m.capture([&] { m.epoch += 2; });
    checkBusy(changedEpoch, BusyReason::finalEpoch, "final-epoch", 1, pixelBytes, true, true);
    CHECK(changedEpoch.startEpoch == UINT64_MAX - 1 && changedEpoch.endEpoch == 0);

    m.genericBusy = true;
    auto &genericCount = m.counts.byReason[unsigned(BusyReason::dpuBusy)];
    genericCount = UINT32_MAX;
    checkBusy(m.capture(stable), BusyReason::dpuBusy, "dpu-busy", 4294967296ull, 0, false, false);
    genericCount = UINT64_MAX - 1;
    checkBusy(m.capture(stable), BusyReason::dpuBusy, "dpu-busy", UINT64_MAX, 0, false, false);
    // The production counter is unsigned and wraps at its type limit. The refusal still has its original classification.
    checkBusy(m.capture(stable), BusyReason::dpuBusy, "dpu-busy", 0, 0, false, false);
    checkBusy(m.capture(stable), BusyReason::dpuBusy, "dpu-busy", 1, 0, false, false);
    constexpr std::array<uint64_t, unsigned(BusyReason::count)> expectedCounts{1, 1, 1, 3, 1, 1};
    CHECK(std::equal(expectedCounts.begin(), expectedCounts.end(), m.counts.byReason));

    m.diagnose = false;
    const auto quiet = m.capture(stable);
    CHECK(quiet.error == EBUSY && quiet.reason == BusyReason::count && quiet.count == 0 && quiet.startUs == 0);
    CHECK(std::equal(expectedCounts.begin(), expectedCounts.end(), m.counts.byReason));
    m.diagnose = true;
    m.genericBusy = false;
    m.finalLock = false;
    m.failChunk = 2;
    const auto fault = m.capture(stable);
    CHECK(fault.error == EFAULT && fault.bytes == 2 * GOP_SHOT_ROW_BYTES && fault.reason == BusyReason::count);
    CHECK(fault.dpu.before.available && !fault.dpu.after.available);
    const auto partial = m.capture([&] { ++m.epoch; });
    checkBusy(partial, BusyReason::finalEpoch, "final-epoch", 2, 2 * GOP_SHOT_ROW_BYTES, true, false);
    CHECK(m.capture(stable, GOP_SHOT_BYTES - 1).error == ENOMEM);
}

void testShotSourceAndBounds()
{
    ShotModel m;
    PowerModel power;
    power.powerChange(1, 1);
    power.powerChange(1, 0);
    CHECK(power.beginEdge());
    power.completeEdge();
    CHECK(power.panelAsleep && power.published == false && power.backlight.bothDark());

    const auto stable = [] {};
    // M5 pauses new flips without changing the selected backing. A stable flip is still readable while off.
    m.surface[flipOffset - 1] = 0;
    m.surface[flipOffset + GOP_SHOT_ROW_BYTES] = 0; // first row padding
    const auto flip = m.capture(stable);
    CHECK(flip.error == 0 && flip.header.source == GOP_SHOT_FLIP && flip.header.format == GOP_SHOT_BGA);
    CHECK(flip.bytes == pixelBytes && flip.firstPixelByte == 0xf1 && flip.lastPixelByte == 0xf1);
    m.pinnedFlip = false;
    CHECK(m.capture(stable).error == ENXIO); // losing a flip mapping does not select the boot buffer
    m.target = {};
    m.pendingFlip = true;
    CHECK(m.capture(stable).error == ENXIO); // boot scanout must be stable and have no pending flip
    m.pendingFlip = false;
    const auto boot = m.capture(stable);
    CHECK(boot.error == 0 && boot.header.source == GOP_SHOT_BOOT && boot.header.format == GOP_SHOT_BGRA8);
    CHECK(boot.bytes == pixelBytes && boot.firstPixelByte == 0xb0 && boot.lastPixelByte == 0xb0);
    CHECK(boot.header.version == 1 && boot.header.width == 3200 && boot.header.height == 2136);
    m.hasDpu = false;
    m.target = {0x10000000, flipStride, true};
    const auto fallback = m.capture(stable);
    CHECK(fallback.error == 0 && fallback.header.source == GOP_SHOT_BOOT && !fallback.dpu.before.available);
    m.boot.pop_back();
    CHECK(m.capture(stable).error == EINVAL);
    m.boot.clear();
    CHECK(m.capture(stable).error == ENXIO);

    const auto required = mappedBytes(UINT16_MAX);
    CHECK(gop_shot_bounds(1, UINT64_MAX, UINT64_MAX - required, UINT16_MAX) == 0);
    CHECK(gop_shot_bounds(1, UINT64_MAX, UINT64_MAX - required + 1, UINT16_MAX) == EINVAL);
    CHECK(gop_shot_bounds(1, UINT64_MAX, UINT64_MAX, UINT64_MAX) == EINVAL);
    uint64_t offset = 0;
    CHECK(gop_shot_chunk(pixelBytes - 1, UINT16_MAX, &offset) == 1 && offset == required - 1);
    CHECK(gop_shot_chunk(SIZE_MAX, UINT16_MAX, &offset) == 0);
}

} // namespace

int main()
{
    testPowerInterleavings();
    testPanelFailures();
    testNightShiftReplay();
    testSwapBrightness();
    testDeadlines();
    testShotDiagnostics();
    testShotSourceAndBounds();
    std::puts("NativeGopDisplay power, pacing, and GopShot model passed");
}
