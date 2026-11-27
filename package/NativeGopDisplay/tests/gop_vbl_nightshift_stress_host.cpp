// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I.. -I../../PianoDpu gop_vbl_nightshift_stress_host.cpp -o /tmp/gop_vbl_nightshift_stress_host && /tmp/gop_vbl_nightshift_stress_host
//
// Drive the work loop with a virtual clock. Threads rendezvous at the panel
// call and scanout copy boundaries so contention is repeatable under ASan.
#include "GopColorLog.h"
#include "GopCompose.h"
#include "GopPanelPower.h"
#include "GopRefresh.h"
#include "GopShotDiagnostics.h"
#include "../../PianoBacklight/NativeBrightness.h"
#include "../../PianoDpu/DpuFlip.h"
#include "../../PianoDpu/DpuNightShift.h"
#include "../../PianoDpu/DpuPanel.h"
#include "../../PianoDpu/DpuSliceArgs.h"
#include "../../PianoDpu/tests/dpu_model.h"
#include "../../../tooling/native/gop-shot/Pixels.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <semaphore>
#include <thread>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expr); std::abort(); \
} } while (false)

namespace {
namespace bl = pianobacklight;
namespace flip = pianodpu::flip;
namespace panel = pianodpu::panel;
namespace pcc = pianodpu::pcc;
namespace slices = pianodpu::slices;
using gopshot::BusyReason;

void wait(std::binary_semaphore &event)
{
    CHECK(event.try_acquire_for(std::chrono::seconds(10)));
}

// The relative timer has microsecond granularity. Absolute mode keeps the
// current phase when swaps arrive early and skips missed slots when late.
struct Vbl {
    unsigned hz;
    bool absolute;
    uint64_t due = 0, scheduled = 0;
    unsigned ticks = 0;

    void stop() { due = scheduled = 0; }
    void arm(uint64_t now)
    {
        const auto period = goprefresh::periodNs(hz);
        due = absolute ? goprefresh::nextDeadline(due, now, period) : now + period;
        scheduled = absolute ? due : now + (1000000 / hz) * 1000ull;
    }
    void fire(uint64_t now)
    {
        CHECK(scheduled && now >= scheduled);
        ++ticks;
    }
};

void testCadence()
{
    const auto defaults = slices::readStartupArgs([](auto...) { return false; }, [](auto...) { return false; });
    CHECK(!defaults.nightShift && !defaults.panel && !gopcolor::nightShiftEnabled(0, false));
    CHECK(goprefresh::periodNs(60) == 16666666 && goprefresh::periodFixed(60) == 0x04444444);
    Vbl daily{60, false};
    daily.arm(1000000);
    CHECK(daily.scheduled == 17666000);
    daily.arm(2000000);
    CHECK(daily.due == 18666666 && daily.scheduled == 18666000);
    std::mt19937 jitter(0x56424c);
    uint64_t now = 2000000;
    for (unsigned tick = 1; tick <= 512; ++tick) {
        now += jitter() % 8000000;
        daily.arm(now);
        CHECK(daily.due == now + 16666666 && daily.scheduled == now + 16666000);
        now = daily.scheduled + jitter() % 25000000;
        daily.fire(now);
        daily.arm(now);
        CHECK(daily.ticks == tick && daily.scheduled == now + 16666000);
    }

    Vbl fixed{120, true};
    constexpr uint64_t period = 8333333;
    now = 1000000000;
    uint64_t grid = now;
    unsigned skipped = 0, restarts = 0;
    fixed.arm(now);
    for (unsigned tick = 1; tick <= 2048; ++tick) {
        const auto deadline = fixed.scheduled;
        CHECK(deadline > now && (deadline - grid) % period == 0);
        for (unsigned swap = 0; swap < 4; ++swap) {
            now += jitter() % (deadline - now);
            fixed.arm(now);
            CHECK(fixed.scheduled == deadline);
        }
        const uint64_t late = tick % 64 == 0 ? 8 * period + 1 : tick % 31 == 0 ? 8 * period : jitter() % (3 * period);
        now = deadline + late;
        fixed.fire(now);
        fixed.arm(now);
        CHECK(fixed.ticks == tick && fixed.scheduled > now && fixed.scheduled <= now + period);
        if (late > 8 * period) {
            grid = now;
            ++restarts;
            CHECK(fixed.scheduled == now + period);
        } else {
            auto next = deadline + period;
            while (next <= now) {
                next += period;
                ++skipped;
            }
            CHECK(fixed.scheduled == next);
        }
    }
    CHECK(skipped > 1000 && restarts == 32);
    std::printf("VBL cadence: %u relative and %u absolute ticks, %u skipped slots, %u long-stall restarts\n",
                daily.ticks, fixed.ticks, skipped, restarts);
}

struct BacklightRegisters final : bl::Ics {
    uint8_t regs[bl::kIcs][bl::kRegCount]{};
    uint16_t expectedOn = 800;
    unsigned litWrites = 0;

    BacklightRegisters()
    {
        for (auto &ic : regs) {
            ic[bl::kRegId] = 0x21;
            ic[bl::kRegEn] = bl::kEnBacklight | bl::kEnSinks;
            ic[bl::kRegBrtMsb] = 100;
        }
    }
    uint16_t code(unsigned ic) const { return bl::codeOf(regs[ic][bl::kRegBrtLsb], regs[ic][bl::kRegBrtMsb]); }
    bool read(unsigned ic, uint8_t reg, uint8_t *value) override
    {
        *value = regs[ic][reg];
        return true;
    }
    bool write(unsigned ic, uint8_t reg, uint8_t value) override
    {
        CHECK(bl::writable(reg));
        if (reg == bl::kRegEn && (value & bl::kEnBacklight)) {
            CHECK(code(ic) == expectedOn);
            ++litWrites;
        }
        regs[ic][reg] = value;
        return true;
    }
    void done() override {}
    void log(const char *) override {}
    uint64_t nowMs() override { return 0; }
};

constexpr uint64_t kS3132One = 1ull << 32;
constexpr int32_t kQ15One = 1 << 15;
constexpr int32_t kBaseWords[2][3][3] = {
    {{kQ15One, -4096, 0}, {0, 28672, 0}, {2048, 0, 24576}},
    {{101, 102, 103}, {104, 105, 106}, {107, 108, 109}}, // saved words of the bypassed DSPP
};
constexpr uint32_t pccWord(int32_t q15) { return uint32_t(q15) & 0x3ffffu; }

struct Color {
    // Exact dyadic coefficients. The cold reference is diag(2, 1, 1/2).
    unsigned green = 8, blue = 8, gain = 8;
    bool off() const { return green == 8 && blue == 8 && gain == 8; }
    std::array<uint64_t, 9> matrix() const
    {
        return {2 * kS3132One, 0, 0, 0, green * kS3132One / 8, 0, 0, 0, blue * kS3132One / 16};
    }
    uint32_t expectedWord(unsigned d, unsigned out, unsigned in) const
    {
        if (off())
            return pccWord(kBaseWords[d][out][in]);
        const unsigned scale[] = {8, green, blue};
        const int32_t base = d == 0 ? kBaseWords[d][out][in] : (out == in ? kQ15One : 0);
        return pccWord(base * int32_t(scale[in] * gain) / 64);
    }
};

struct Brightness {
    uint8_t flags;
    uint64_t raw;
    int32_t fixed;
    uint32_t clamped; // zero leaves the previous positive request in the mailbox
};

constexpr Brightness kBrightness[] = {
    {1, 0x7ff8000000000001ull, 0, 0},                                      // NaN
    {1, std::bit_cast<uint64_t>(-35.0), 0, 0},                             // negative
    {3, std::bit_cast<uint64_t>(0x1p-16), 1, 31u << 16},                   // below the floor
    {1, std::bit_cast<uint64_t>(65536.0), INT32_MAX, 500u << 16},           // above both maxima
    {2, std::bit_cast<uint64_t>(19.0), -1, 0},                             // brightness flag clear
    {1, std::bit_cast<uint64_t>(200.25), (200 << 16) + 16384, (200u << 16) + 16384},
    {1, std::bit_cast<uint64_t>(355.0), 355 << 16, 355u << 16},
    {1, std::bit_cast<uint64_t>(501.0), 501 << 16, 500u << 16},
};
constexpr uint16_t expectedCode(uint32_t fixed)
{
    // The non-Android native SDR range for the configured floor (128..2047).
    // Use a 64-bit reference for the independent round-to-nearest check.
    const uint64_t span = 469ull << 16;
    return uint16_t(128 + ((fixed - (31u << 16)) * 1919ull + span / 2) / span);
}

struct Display {
    const bool panelEnabled;
    const bool colorEnabled;
    bool publishedOn = true, fallback = false, unsafe = false;
    std::mutex copyLock, colorLock;
    gopcompose::PanelPower power;
    pcc::NightShift nightShift;
    pcc::PanelPcc base, last;
    model::Dpu io;
    slices::LivePath sleepPath;
    panel::Result sleepResult;
    bool needsWake = false;
    unsigned panelWakes = 0, wakePublications = 0, replays = 0, errors = 0;

    BacklightRegisters regs;
    bl::Controller backlight{regs};
    const bl::NativeBrightness range{128};
    uint32_t pendingNits = 0, appliedNits = 0, expectedNits = 0;
    int32_t lastFixed = -1;
    unsigned swaps = 0, displayed = 0;
    Vbl vbl{60, false};
    uint64_t now = 1000000000;

    Display(unsigned panelArg, unsigned colorArg)
        : panelEnabled(panelArg == 1), colorEnabled(gopcolor::nightShiftEnabled(colorArg, false))
    {
        backlight.configure({.bootPercent = 0});
        CHECK(backlight.survey() && backlight.start());
        backlight.display(true, 1, 1);
        for (unsigned d = 0; d < 2; ++d) {
            io.pccActive[d][0] = io.pccShadow[d][0] = io.regs[slices::pccReg(d, 0)] = d == 0;
            for (unsigned o = 0; o < 3; ++o)
                for (unsigned i = 0; i < 3; ++i) {
                    const auto reg = pianodpu::kPccCoef[o][i];
                    io.pccActive[d][reg / 4] = io.pccShadow[d][reg / 4] = io.regs[slices::pccReg(d, reg)] =
                        pccWord(kBaseWords[d][o][i]);
                }
        }
        CHECK(pcc::capturePanelPcc(io, slices::readPath(io), base));
        last = base;
        const auto cold = Color{}.matrix();
        if (colorEnabled)
            CHECK(nightShift.setMatrix(cold.data()) == pcc::NightShift::Input::accepted);
    }

    void submit(const Brightness &sample)
    {
        // rec+0x3be is byte-packed even if the record starts on an 8-byte boundary.
        constexpr size_t flag = 0x3b4, nits = 0x3be;
        alignas(uint64_t) std::array<uint8_t, nits + sizeof(uint64_t)> rec{};
        rec[flag] = sample.flags;
        std::memcpy(rec.data() + nits, &sample.raw, sizeof sample.raw);
        if (rec[flag] & 1) {
            uint64_t bits = 0;
            std::memcpy(&bits, rec.data() + nits, sizeof bits);
            const int32_t fixed = bl::nitsFixedFromDouble(bits);
            CHECK(fixed == sample.fixed);
            if (fixed != lastFixed && fixed > 0) {
                pendingNits = std::clamp(uint32_t(fixed), range.minFixed(), range.maxFixed());
                CHECK(pendingNits == sample.clamped);
            }
            lastFixed = fixed;
        }
        if (sample.clamped)
            expectedNits = sample.clamped;
        ++swaps;
        arm();
    }

    void drainBrightness()
    {
        if (pendingNits) {
            uint16_t code = 0;
            CHECK(range.decode(pendingNits, code));
            appliedNits = pendingNits;
            pendingNits = 0;
            backlight.manualCode(code, "swap");
        }
        CHECK(appliedNits == expectedNits && backlight.code() == expectedCode(expectedNits));
    }

    void updateColor(Color color)
    {
        if (!colorEnabled)
            return;
        const std::lock_guard lock(colorLock);
        const auto matrix = color.matrix();
        CHECK(nightShift.setMatrix(matrix.data()) == pcc::NightShift::Input::accepted);
        nightShift.setCorrection(color.gain * (pcc::kOne / 8));
        const uint64_t blank[9]{};
        CHECK(nightShift.setMatrix(blank) == pcc::NightShift::Input::blank);
    }

    void replay()
    {
        if (!colorEnabled || unsafe)
            return;
        const auto state = [&] {
            const std::lock_guard lock(colorLock);
            return nightShift;
        }();
        const auto wanted = pcc::nightShiftPrograms(base, state);
        const auto result = pcc::updateNightShift(io, slices::readPath(io), wanted, last, true);
        CHECK(result.attempted && result.result.ok);
        ++replays;
    }

    void checkPcc(Color color) const
    {
        for (unsigned d = 0; d < 2; ++d) {
            CHECK(io.pccActive[d][0] == unsigned(!color.off() || d == 0));
            for (unsigned o = 0; o < 3; ++o)
                for (unsigned i = 0; i < 3; ++i)
                    CHECK(io.pccActive[d][pianodpu::kPccCoef[o][i] / 4] == color.expectedWord(d, o, i));
        }
    }

    void edge(bool on)
    {
        if (!on) {
            publishedOn = false;
            backlight.display(false, 1, 0);
            backlight.sleep(true);
        }
        const std::lock_guard lock(copyLock);
        if (panelEnabled)
            CHECK(power.request(on));
    }

    bool begin()
    {
        const std::lock_guard lock(copyLock);
        bool on = false;
        CHECK(power.begin(on));
        return on;
    }

    void finish(bool on, bool failWake = false)
    {
        if (!on && !fallback) {
            CHECK(backlight.bothDark());
            sleepPath = slices::readPath(io);
            sleepResult = panel::panelOff(io, sleepPath, {});
            CHECK(sleepResult.ok);
            needsWake = true;
        }
        if (on && needsWake) {
            needsWake = false;
            ++panelWakes;
            io.engineDead[0] = io.engineDead[1] = failWake;
            const auto result = panel::panelOn(io, sleepPath, {}, sleepResult.enginesStopped, sleepResult.fetch);
            CHECK(result.ok != failWake && result.scanoutRestored != failWake);
            fallback = !result.ok;
            unsafe = !result.scanoutRestored;
            errors += unsigned(fallback);
            if (result.ok) {
                // Model lost PCC registers while keeping the boot base and last programmed coefficients.
                std::memset(io.pccActive, 0, sizeof io.pccActive);
                std::memset(io.pccShadow, 0, sizeof io.pccShadow);
            }
        }
        if (on)
            replay();
        const std::lock_guard lock(copyLock);
        power.finish(on); // software presentation also resumes on panel error
    }

    void publishWake()
    {
        if (panelEnabled) {
            const std::lock_guard lock(copyLock);
            if (!power.wakeComplete)
                return;
            CHECK(power.canPresent());
            power.wakeComplete = false;
        }
        CHECK(!publishedOn && backlight.bothDark());
        regs.expectedOn = expectedCode(expectedNits);
        backlight.sleep(false);
        CHECK(backlight.bothDark());
        backlight.display(true, 1, 1);
        publishedOn = true;
        ++wakePublications;
        CHECK(backlight.bothLit() && regs.code(0) == regs.expectedOn && regs.code(1) == regs.expectedOn);
        arm();
    }

    void arm()
    {
        const bool queued = swaps != displayed;
        if ((panelEnabled && !publishedOn) ||
            gopcompose::vblIdleStops(true, 0, false, false, queued) ||
            gopcompose::vblStops(true, !publishedOn, 0, false, false, queued))
            vbl.stop();
        else
            vbl.arm(now);
    }

    void advanceTo(uint64_t next)
    {
        CHECK(next >= now);
        now = next;
        if (!vbl.scheduled || now < vbl.scheduled)
            return;
        vbl.fire(now);
        if (publishedOn)
            displayed = swaps;
        arm();
    }

    void tick()
    {
        CHECK(vbl.scheduled);
        advanceTo(vbl.scheduled + 31000);
    }

    void checkDark()
    {
        CHECK(!publishedOn && backlight.bothDark() && !vbl.scheduled);
        const auto ticks = vbl.ticks;
        advanceTo(now + 9 * goprefresh::periodNs(60));
        CHECK(vbl.ticks == ticks && !vbl.due && displayed < swaps);
    }
};

void burst(Display &display, Color finalColor, unsigned brightness)
{
    std::binary_semaphore ready{0}, start{0}, finished{0};
    std::jthread color([&] {
        ready.release();
        wait(start);
        display.updateColor({7, 4, 7});
        display.updateColor({});
        display.updateColor(finalColor);
        finished.release();
    });
    wait(ready);
    start.release();
    for (unsigned i = 0; i < 16; ++i)
        display.submit(kBrightness[i % std::size(kBrightness)]);
    display.submit(kBrightness[brightness]);
    display.drainBrightness();
    wait(finished);
}

void testPowerReplay()
{
    Display daily(0, 0);
    daily.submit(kBrightness[5]);
    daily.drainBrightness();
    daily.tick();
    daily.edge(false);
    CHECK(!daily.vbl.scheduled && daily.backlight.bothDark());
    daily.updateColor({7, 5, 7});
    daily.submit(kBrightness[6]);
    daily.drainBrightness();
    daily.publishWake();
    daily.tick();
    daily.checkPcc({});
    CHECK(daily.panelWakes == 0 && daily.replays == 0 && daily.displayed == daily.swaps);

    Display display(1, 1);
    constexpr unsigned cycles = 48;
    unsigned superseded = 0;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        const Color warm{5 + cycle % 3, 2 + cycle % 4, 7};
        const Color final = cycle % 2 == 0 ? warm : Color{};
        const auto previouslyDisplayed = display.displayed;
        display.submit(kBrightness[5]);
        display.drainBrightness();
        display.edge(false);
        const auto darkCode = display.regs.code(0);
        const auto litBefore = display.regs.litWrites;
        const auto publications = display.wakePublications;
        CHECK(!display.begin());
        display.finish(false);
        display.tick(); // IOTimer callback observes the published off edge
        const auto darkTickCount = display.vbl.ticks;
        burst(display, warm, 6);
        display.checkDark();

        display.edge(true);
        std::binary_semaphore entered{0}, allowReply{0}, replied{0};
        const bool failWake = cycle == cycles - 2;
        const bool cutWake = cycle % 5 == 0;
        const Color inFlight = cutWake ? (final.off() ? warm : Color{}) : final;
        std::jthread panelWorker([&] {
            CHECK(display.begin());
            entered.release();
            wait(allowReply);
            display.finish(true, failWake);
            replied.release();
        });
        wait(entered);
        burst(display, inFlight, std::array{2u, 3u, 5u, 6u}[cycle % 4]);
        display.publishWake(); // in-flight reply must not publish early
        display.checkDark();
        CHECK(display.regs.code(0) == darkCode && display.regs.code(1) == darkCode);
        if (cutWake)
            display.edge(false);
        allowReply.release();
        wait(replied);
        panelWorker.join();

        if (cutWake) {
            ++superseded;
            display.checkPcc(inFlight);
            display.publishWake();
            display.checkDark();
            CHECK(display.regs.litWrites == litBefore && display.wakePublications == publications);
            CHECK(!display.begin());
            display.finish(false);
            burst(display, final, 7);
            display.edge(true);
            CHECK(display.begin());
            display.finish(true);
        }
        CHECK(display.vbl.ticks == darkTickCount && display.displayed == previouslyDisplayed);
        CHECK(display.regs.litWrites == litBefore);
        if (!display.fallback)
            display.checkPcc(final); // both DSPPs are latched before the first enable write
        display.publishWake();
        CHECK(display.wakePublications == publications + 1 && display.regs.litWrites == litBefore + 2);
        CHECK(display.vbl.scheduled == display.now + 16666000);
        display.tick();
        CHECK(display.vbl.ticks == darkTickCount + 1 && display.displayed == display.swaps && !display.vbl.scheduled);
    }
    CHECK(display.swaps >= 1000 && superseded == 10 && display.errors == 1 && display.fallback);
    CHECK(display.panelWakes == cycles + superseded - 1 && display.replays == display.panelWakes - 1);
    std::printf("Power replay: %u swaps, %u wakes, %u superseded wakes, %u failed wake followed by backlight-only wake\n",
                display.swaps, display.wakePublications, superseded, display.errors);
}

constexpr size_t kImageBytes = GOP_SHOT_BYTES - sizeof(GopShotHeader);
constexpr uint32_t kPaddedStride = GOP_SHOT_ROW_BYTES + 64;
constexpr uint8_t kGuard = 0xee;

struct Mapping {
    uint32_t stride;
    size_t offset = 1; // misalign the host mapping, including the first pixel in every row
    std::vector<uint8_t> bytes;

    Mapping(uint32_t pitch, uint8_t seed)
        : stride(pitch), bytes(offset + (GOP_SHOT_HEIGHT - 1ull) * stride + GOP_SHOT_ROW_BYTES, kGuard)
    {
        for (unsigned row = 0; row < GOP_SHOT_HEIGHT; ++row)
            std::memset(bytes.data() + offset + row * stride, rowByte(seed, row), GOP_SHOT_ROW_BYTES);
    }

    static uint8_t rowByte(uint8_t seed, unsigned row) { return uint8_t(seed ^ (row % 127)); }
};

struct Attempt {
    size_t bytes = 0;
    int error = 0;
    BusyReason reason = BusyReason::count;
};

struct Scanout {
    std::atomic<uint64_t> copyEpoch{0};
    std::mutex flipLock;
    flip::Target target{0x10000000, kPaddedStride, true};
    uint64_t generation = 1;
    bool pendingFlip = false, dpuBusy = false;
    std::shared_ptr<Mapping> flipMap = std::make_shared<Mapping>(kPaddedStride, 0x42);
    std::shared_ptr<Mapping> bootMap = std::make_shared<Mapping>(GOP_SHOT_ROW_BYTES, 0xa3);
    gopshot::BusyCounts busy;

    template <class DuringCopy>
    Attempt capture(uint8_t *staging, DuringCopy duringCopy, bool failCopy)
    {
        Attempt out;
        const auto rejected = [&](BusyReason reason) {
            busy.record(reason);
            out.reason = reason;
            out.error = EBUSY;
            return out;
        };
        const auto epoch = copyEpoch.load(std::memory_order_acquire);
        if (epoch & 1)
            return rejected(BusyReason::oddEpoch);

        std::unique_lock lock(flipLock, std::try_to_lock);
        if (!lock)
            return rejected(gopshot::dpuBusyReason(PianoDpuScanoutBusy::initialLock));
        if (dpuBusy)
            return rejected(gopshot::dpuBusyReason(PianoDpuScanoutBusy::none));
        const auto before = target;
        const auto beforeGeneration = generation;
        const bool boot = flip::sameTarget(before, {}) && !pendingFlip;
        // Hold the selected mapping across the unlocked copy, as scanoutRead retains its cookie.
        const auto map = before.surface ? flipMap : boot ? bootMap : nullptr;
        lock.unlock();
        if (!map) {
            out.error = ENXIO;
            return out;
        }

        out.error = gop_shot_bounds(true, map->bytes.size(), map->offset, map->stride);
        if (out.error)
            return out;
        const GopShotHeader header{GOP_SHOT_VERSION, GOP_SHOT_WIDTH, GOP_SHOT_HEIGHT,
                                   boot ? GOP_SHOT_BGRA8 : GOP_SHOT_BGA,
                                   boot ? uint32_t(GOP_SHOT_BOOT) : uint32_t(GOP_SHOT_FLIP)};
        std::memcpy(staging, &header, sizeof header);
        size_t position = 0;
        for (;;) {
            uint64_t at = 0;
            const size_t chunk = gop_shot_chunk(position, map->stride, &at);
            if (!chunk)
                break;
            CHECK(chunk <= GOP_SHOT_CHUNK_BYTES && chunk <= kImageBytes - position);
            CHECK(map->offset + at + chunk <= map->bytes.size());
            std::memcpy(staging + sizeof header + position, map->bytes.data() + map->offset + at, chunk);
            position += chunk;
            if (!out.bytes) {
                duringCopy();
                if (failCopy)
                    out.error = EFAULT;
            }
            out.bytes = sizeof header + position;
            if (out.error)
                break;
        }

        if (!out.error) {
            if (!lock.try_lock())
                return rejected(gopshot::dpuBusyReason(PianoDpuScanoutBusy::finalLock));
            if (!flip::sameTarget(before, target) || beforeGeneration != generation)
                return rejected(gopshot::dpuBusyReason(PianoDpuScanoutBusy::changed));
            lock.unlock();
        }
        std::atomic_thread_fence(std::memory_order_acq_rel);
        return copyEpoch.load(std::memory_order_acquire) == epoch ? out : rejected(BusyReason::finalEpoch);
    }
};

struct ReadResult {
    int error;
    unsigned attempts;
    size_t publishedBytes;
};

constexpr unsigned kMaxAttempts = 8; // read_frame in the gop-shot CLI

template <class DuringCopy, class AfterAttempt>
ReadResult readFrame(Scanout &scanout, uint8_t *staging, DuringCopy duringCopy, AfterAttempt afterAttempt, bool failCopy = false)
{
    for (unsigned attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        const auto out = scanout.capture(staging, [&] { duringCopy(attempt); }, failCopy);
        afterAttempt(attempt, out);
        if (!out.error) {
            CHECK(out.bytes == GOP_SHOT_BYTES);
            return {0, attempt, out.bytes};
        }
        if (out.error != EBUSY)
            return {out.error, attempt, 0};
    }
    return {EBUSY, kMaxAttempts, 0};
}

void checkFrame(const uint8_t *staging, uint8_t seed, bool boot)
{
    GopShotHeader header{};
    std::memcpy(&header, staging, sizeof header);
    CHECK(header.version == GOP_SHOT_VERSION && header.width == GOP_SHOT_WIDTH && header.height == GOP_SHOT_HEIGHT);
    CHECK(header.source == (boot ? GOP_SHOT_BOOT : GOP_SHOT_FLIP));
    CHECK(header.format == (boot ? GOP_SHOT_BGRA8 : GOP_SHOT_BGA));
    std::array<uint8_t, GOP_SHOT_ROW_BYTES> expected;
    for (unsigned row = 0; row < GOP_SHOT_HEIGHT; ++row) {
        expected.fill(Mapping::rowByte(seed, row));
        CHECK(!std::memcmp(staging + sizeof header + row * GOP_SHOT_ROW_BYTES, expected.data(), expected.size()));
    }
}

enum class Race { oddEpoch, copyInFlight, initialLock, finalLock, targetChanged, generationChanged, bootTarget, genericBusy };

struct Scenario {
    Race race;
    BusyReason reason;
    const char *label;
    bool beforeCopy; // collision at the initial sample or after the first chunk
};

constexpr Scenario kRaces[] = {
    {Race::oddEpoch, BusyReason::oddEpoch, "odd-epoch", true},
    {Race::copyInFlight, BusyReason::finalEpoch, "copy-in-flight", false},
    {Race::initialLock, BusyReason::initialLock, "dpu-lock-busy (initial)", true},
    {Race::finalLock, BusyReason::finalLock, "dpu-lock-busy (final)", false},
    {Race::targetChanged, BusyReason::scanoutChanged, "target-changed", false},
    {Race::generationChanged, BusyReason::scanoutChanged, "generation-changed", false},
    {Race::bootTarget, BusyReason::scanoutChanged, "boot-target", false},
    {Race::genericBusy, BusyReason::dpuBusy, "unclassified DPU busy", true},
};

void testShotRace(const Scenario &scenario, uint8_t *staging)
{
    Scanout scanout;
    const bool startsBoot = scenario.race == Race::bootTarget || scenario.race == Race::copyInFlight;
    if (startsBoot)
        scanout.target = {};
    auto nextTarget = flip::Target{0x20000000, kPaddedStride, true};
    auto nextMap = std::make_shared<Mapping>(kPaddedStride, 0x64);
    const std::weak_ptr<Mapping> oldMap = scanout.flipMap;

    std::binary_semaphore ready{0}, copying{0}, changed{0}, checked{0}, finished{0};
    std::jthread mutator([&] {
        std::unique_lock lock(scanout.flipLock, std::defer_lock);
        if (scenario.beforeCopy) {
            switch (scenario.race) {
            case Race::oddEpoch: scanout.copyEpoch.fetch_add(1, std::memory_order_acq_rel); break;
            case Race::initialLock: lock.lock(); break;
            case Race::genericBusy: scanout.dpuBusy = true; break;
            default: CHECK(false);
            }
        }
        ready.release();
        if (!scenario.beforeCopy) {
            wait(copying);
            switch (scenario.race) {
            case Race::copyInFlight:
                // CPU copy starts after capture sampled an even epoch. Overwrite boot rows
                // between chunks, with no unsynchronized access to the host byte storage.
                scanout.copyEpoch.fetch_add(1, std::memory_order_acq_rel);
                for (unsigned row = 0; row < GOP_SHOT_HEIGHT; ++row)
                    std::memset(scanout.bootMap->bytes.data() + scanout.bootMap->offset + row * GOP_SHOT_ROW_BYTES,
                                Mapping::rowByte(0x31, row), GOP_SHOT_ROW_BYTES);
                break;
            case Race::finalLock:
                lock.lock();
                break;
            case Race::targetChanged:
            case Race::bootTarget: {
                const std::lock_guard guard(scanout.flipLock);
                scanout.target = nextTarget;
                scanout.flipMap = std::move(nextMap);
                break;
            }
            case Race::generationChanged: {
                const std::lock_guard guard(scanout.flipLock);
                ++scanout.generation; // re-show the same target, with no identity change
                break;
            }
            default: CHECK(false);
            }
            changed.release();
        }
        wait(checked);
        if (scenario.race == Race::oddEpoch || scenario.race == Race::copyInFlight)
            scanout.copyEpoch.fetch_add(1, std::memory_order_release);
        if (scenario.race == Race::genericBusy)
            scanout.dpuBusy = false;
        if (lock)
            lock.unlock();
        finished.release();
    });

    wait(ready);
    const auto result = readFrame(scanout, staging, [&](unsigned attempt) {
        if (attempt != 1)
            return;
        CHECK(!scenario.beforeCopy);
        copying.release();
        wait(changed);
        if (scenario.race == Race::targetChanged)
            CHECK(!oldMap.expired()); // the mutator dropped the last owner except for the reader
    }, [&](unsigned attempt, Attempt out) {
        if (attempt != 1)
            return;
        if (out.error != EBUSY || out.reason != scenario.reason)
            std::fprintf(stderr, "capture race %s returned error %d and reason %u\n", scenario.label, out.error, unsigned(out.reason));
        CHECK(out.error == EBUSY && out.reason == scenario.reason);
        CHECK(out.bytes == (scenario.beforeCopy ? 0 : GOP_SHOT_BYTES));
        if (scenario.race == Race::copyInFlight) {
            CHECK(scanout.copyEpoch.load(std::memory_order_acquire) == 1);
            CHECK(staging[sizeof(GopShotHeader)] == Mapping::rowByte(0xa3, 0));
            CHECK(staging[GOP_SHOT_BYTES - 1] == Mapping::rowByte(0x31, GOP_SHOT_HEIGHT - 1));
        }
        if (scenario.race == Race::targetChanged)
            CHECK(oldMap.expired());
        checked.release();
        wait(finished);
    });
    CHECK(!result.error && result.attempts == 2 && result.publishedBytes == GOP_SHOT_BYTES);
    for (unsigned i = 0; i < unsigned(BusyReason::count); ++i)
        CHECK(scanout.busy.byReason[i] == unsigned(i == unsigned(scenario.reason)));
    const bool boot = scenario.race == Race::copyInFlight;
    const bool changedTarget = scenario.race == Race::targetChanged || scenario.race == Race::bootTarget;
    checkFrame(staging, boot ? 0x31 : changedTarget ? 0x64 : 0x42, boot);
    std::printf("gop-shot %s: %s, then full retry\n", scenario.label, gopshot::busyReasonName(scenario.reason));
}

void testShots()
{
    // Staging has guards and an unaligned wire header. Use memcpy for the ABI
    // and exercise the pixel reader on an unaligned word under UBSan as well.
    std::vector<uint8_t> buffer(GOP_SHOT_BYTES + 2, kGuard);
    uint8_t *const staging = buffer.data() + 1;
    for (const auto &scenario : kRaces) {
        testShotRace(scenario, staging);
        CHECK(buffer.front() == kGuard && buffer.back() == kGuard);
    }

    Scanout scanout;
    scanout.target = {};
    scanout.pendingFlip = true;
    const auto noCopy = [](unsigned) { CHECK(false); };
    const auto ignore = [](unsigned, Attempt) {};
    const auto pending = readFrame(scanout, staging, noCopy, ignore);
    CHECK(pending.error == ENXIO && pending.attempts == 1 && !pending.publishedBytes);
    scanout.pendingFlip = false;

    const auto fault = readFrame(scanout, staging, [](unsigned) {}, [](unsigned, Attempt attempt) {
        CHECK(attempt.error == EFAULT && attempt.bytes == sizeof(GopShotHeader) + GOP_SHOT_CHUNK_BYTES);
    }, true);
    CHECK(fault.error == EFAULT && fault.attempts == 1 && !fault.publishedBytes);

    scanout.copyEpoch.store(1, std::memory_order_release);
    const auto exhausted = readFrame(scanout, staging, noCopy, ignore);
    CHECK(exhausted.error == EBUSY && exhausted.attempts == kMaxAttempts && !exhausted.publishedBytes);
    CHECK(scanout.busy.byReason[unsigned(BusyReason::oddEpoch)] == kMaxAttempts);
    scanout.copyEpoch.store(2, std::memory_order_release);
    const auto lateBusy = readFrame(scanout, staging, [&](unsigned) {
        scanout.copyEpoch.fetch_add(2, std::memory_order_acq_rel);
    }, [](unsigned, Attempt attempt) {
        CHECK(attempt.error == EBUSY && attempt.reason == BusyReason::finalEpoch && attempt.bytes == GOP_SHOT_BYTES);
    });
    CHECK(lateBusy.error == EBUSY && lateBusy.attempts == kMaxAttempts && !lateBusy.publishedBytes);
    CHECK(scanout.busy.byReason[unsigned(BusyReason::finalEpoch)] == kMaxAttempts);
    const auto stable = readFrame(scanout, staging, [](unsigned) {}, ignore);
    CHECK(!stable.error && stable.attempts == 1 && stable.publishedBytes == GOP_SHOT_BYTES);
    checkFrame(staging, 0xa3, true);
    uint8_t *const pixel = staging + sizeof(GopShotHeader);
    CHECK(reinterpret_cast<uintptr_t>(pixel) % alignof(uint32_t) != 0);
    const uint8_t packedRed[] = {0x00, 0x00, 0xf0, 0x3f};
    std::memcpy(pixel, packedRed, sizeof packedRed);
    gop_shot_rgbx(pixel, true);
    const uint8_t red[] = {255, 0, 0, 255};
    CHECK(!std::memcmp(pixel, red, sizeof red));
    CHECK(buffer.front() == kGuard && buffer.back() == kGuard);
    std::puts("gop-shot: all busy counters checked, eight-attempt bound, short copy discarded, stable boot capture");
}
} // namespace

int main()
{
    testCadence();
    testPowerReplay();
    testShots();
}
