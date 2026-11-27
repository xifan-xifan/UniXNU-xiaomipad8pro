// SPDX-License-Identifier: MIT
// Build: cc -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -c ../../kits/AdrenoPM4/AdrenoPM4.c -o "$(dirname /tmp/test_gpu_fault_tier_recovery)/test_gpu_fault_pm4.o" && src="$PWD/../.." && pm4="$(dirname /tmp/test_gpu_fault_tier_recovery)/test_gpu_fault_pm4.o" && obj=$(mktemp -d) && trap 'rm -rf "$obj"; rm -f "$pm4"' EXIT && mkdir "$obj/IOKit" "$obj/kern" && for h in IOBufferMemoryDescriptor IOLib IOReturn; do ln -s "$src/adreno830/tests/XnuMemoryFixture.h" "$obj/IOKit/$h.h" || exit; done && ln -s "$src/adreno830/tests/XnuMemoryFixture.h" "$obj/kern/task.h" && ln -s "$src/adreno830/tests/Adreno830HostClock.h" "$obj/kern/clock.h" && (cd "$obj" && cc -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -c "$src"/kits/ArmSmmu4K/ArmSmmu4K*.c "$src"/adreno830/platform/sm8750/Sm8750Tsens.c) && c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread -DADRENO830_HOST_TEST -DADRENO830_MACOS -I"$obj" -Ihost-include -I../../adreno830 -I../../adreno830/tests -I../../adreno830/platform/sm8750 -I../../kits/ArmSmmu4K -I../../kits/AdrenoPM4 -I../../../include test_gpu_fault_tier_recovery.cpp ../../adreno830/{A830Dcvs,A830Fence,A830Irq,A830PowerState,A830Recovery,A830Smmu,NativeAdrenoMemory,NativeAdrenoRing,NativeAdrenoSmmuContext,NativeAdrenoSmmuMemory,NativeAdrenoSubmit,NativeAdrenoTimer,NativeAdrenoUserClient,NativeAdrenoVm,tests/A830HostSched}.cpp "$pm4" "$obj"/*.o -o /tmp/test_gpu_fault_tier_recovery && /tmp/test_gpu_fault_tier_recovery

#include "A830PowerFakes.h"
#include "A830Fence.h"
#include "A830HostSched.h"
#include "A830SmmuModel.h"
#include "A830GpuTier.h"
#include "NativeAdrenoPublish.h"
#include "NativeAdrenoUserClient.h"
#include <chrono>
#include <cstring>
#include <iterator>
#include <memory>
#include <thread>

IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *state)
{
    CHECK(state);
    *state = {};
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoMaintainOwnedPages(void *mapping, size_t bytes, NativeAdrenoCacheOperation op)
{
    CHECK(mapping && bytes && op <= NativeAdrenoInvalidate);
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans, size_t count, bool)
{
    return spans && count ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans, size_t count, uint32_t *reset)
{
    return spans && count && reset ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical, size_t bytes, NativeAdrenoCacheOperation op)
{
    CHECK(physical && bytes && !((physical | bytes) & 4095) && op <= NativeAdrenoInvalidate);
    return kIOReturnSuccess;
}

extern "C" int proc_selfpid() { return 100; }
void adreno830_status_irq_off(uint32_t bits)
{
    __atomic_fetch_or(&adreno830_status.irq_off, bits, __ATOMIC_RELAXED);
}

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr uint64_t Page = 4096, MiB = 1ull << 20, Aperture = 64ull << 30;
constexpr uint64_t TierBytes = 126 * MiB;
constexpr unsigned RoundsPerMode = 32, RenderSlot = 0, FaultSlot = 37;
constexpr uint64_t FenceDeadlineNs = 2'000'000'000;
constexpr size_t TierSizesMiB[] = {2, 4, 8, 16, 32, 64};

struct Client {
    NativeAdrenoStatus status;
    NativeAdrenoUserClient core;
};

NativeAdrenoAbiSubmit submission(uint64_t handle, uint64_t iova)
{
    NativeAdrenoAbiSubmit request{};
    request.buffer_count = 1;
    request.buffers[0] = {handle, iova, 4};
    return request;
}

uint64_t alloc(Client &client, uint64_t bytes, uint32_t flags)
{
    const NativeAdrenoAbiBoAlloc request{bytes, 12, flags};
    NativeAdrenoAbiBoAllocReply reply{};
    CHECK(client.core.allocBo(&request, &reply) == kIOReturnSuccess && reply.handle);
    return reply.handle;
}

void map(Client &client, uint64_t handle, uint64_t iova)
{
    const NativeAdrenoAbiBoMap request{handle, iova, 3, 0, 2, 0};
    NativeAdrenoAbiBoMapReply reply{};
    CHECK(client.core.mapBo(&request, &reply) == kIOReturnSuccess && reply.iova == iova);
}

// The power and register peers stand in for hardware. These callbacks mirror the publish shell's
// gate, fault counters and replay handoff. The VM, SMMU, ABI client, submit, ring and fence are real.
struct Rig {
    A830SmmuModel mmio;
    A830SmmuHostPlatform smmuHost;
    A830Smmu smmu;
    NativeAdrenoSmmuContext context;
    NativeAdrenoVm vm;
    NativeAdrenoRing ring;
    NativeAdrenoSubmit submit;
    A830Fence fence;
    A830PowerState power;
    A830Recovery recovery;
    Client render;
    Client *peer = nullptr;
    const NativeAdrenoClientWindow *windows = NativeAdrenoClientWindowsFor(Aperture);
    const uint64_t renderVa = windows[RenderSlot].start;
    const uint64_t peerVa = windows[FaultSlot].start;
    NativeAdrenoAbiSubmit renderJob{};
    NativeAdrenoSubmitPlan plan{};
    volatile uint32_t attribution[NativeAdrenoFaultVerdicts]{};
    uint32_t ringWords[8192]{}, memptrs[1024]{}, replayWords[NativeAdrenoSubmitReplayStoreWords]{};
    uint32_t publishedWptr = 0;
    A830Gate gate = A830GateOpen;
    unsigned signals = 0, waiterWakeups = 0;

    const NativeAdrenoRingClock clock{nullptr, [](void *) { return a830Now() / 1000; }, nullptr};

    void start()
    {
        CHECK(windows && windows[FaultSlot].bytes > TierBytes);
        mmio.boot(91);
        smmuHost.fenced = true;
        CHECK(smmu.init(A830SmmuBoardConfig(mmio, smmuHost, &adreno830_status)) == kIOReturnSuccess);
        CHECK(smmu.survey(nullptr) == kIOReturnSuccess);
        CHECK(smmu.resetBank(0) == kIOReturnSuccess && smmu.resetBank(2) == kIOReturnSuccess);
        CHECK(smmu.attachIrqs() == 2);

        const NativeAdrenoVmPagedConfig config{.input_bits = 36, .output_bits = 36, .aperture = Aperture};
        uint64_t root = 0;
        CHECK(vm.preparePaged(config, &root) == kIOReturnSuccess && root);
        CHECK(context.preparePaged(root, Aperture, Aperture / Page, 0, 1, 1000, 36, 36) == kIOReturnSuccess);
        const auto stream = smmu.gpuStream();
        const ArmSmmu4KStream routes[] = {{stream.slot, stream.sid}};
        CHECK(context.program(smmu.programmer(), routes, 1) == kIOReturnSuccess);
        CHECK(context.attach() == kIOReturnSuccess && vm.attachPaged(context.binding(), 1000) == kIOReturnSuccess);

        const NativeAdrenoRingIo io{
            this,
            [](void *cookie, uint32_t reg, uint32_t value) {
                auto &rig = *static_cast<Rig *>(cookie);
                CHECK(reg == rig.ring.regs().wptr && value < std::size(rig.ringWords));
                rig.publishedWptr = value;
                return 0;
            },
            [](void *, uint32_t, uint32_t *value) { *value = 0; return 0; },
            [](void *) { return 0; },
            nullptr, nullptr, nullptr,
        };
        const NativeAdrenoRingCache cache{};
        CHECK(ring.attach(&io, &cache, ringWords, sizeof ringWords, 0x100000,
                          memptrs, sizeof memptrs, 0x108000) == kIOReturnSuccess);
        vm.setFenceSource(&memptrs[1]);
        CHECK(submit.attach(&vm, &ring) == kIOReturnSuccess);
        CHECK(submit.attachReplayStore(replayWords, std::size(replayWords)) == kIOReturnSuccess);
        CHECK(fence.start(A830FenceConfig{.memptrs = memptrs}) == kIOReturnSuccess);
        fence.stopWatchdog();
        const NativeAdrenoRingWaiter waiter{&fence, A830Fence::ringWaiter, A830Fence::ringStamp};
        ring.setWaiter(&waiter);

        A830PowerOps powerOps = gPowerOps;
        powerOps.context = this;
        powerOps.lastSubmitted = [](void *cookie) { return static_cast<Rig *>(cookie)->ring.lastSubmitted(); };
        powerOps.retired = [](void *cookie) { return static_cast<Rig *>(cookie)->fence.completed(); };
        powerOps.cpStart = [](void *cookie, uint32_t restart, uint16_t *) {
            auto &rig = *static_cast<Rig *>(cookie);
            CHECK(rig.gate == A830GateRecovering && gOps.hit("cpStart") == kIOReturnSuccess);
            uint32_t last = 0;
            rig.fence.queueSkip(restart);
            CHECK(rig.ring.recover(&last, &restart) == kIOReturnSuccess);
            CHECK(last == rig.plan.issued && rig.fence.completed() == restart);
            return kIOReturnSuccess;
        };
        A830RecoveryOps recoveryOps = gRecoveryOps;
        recoveryOps.context = this;
        recoveryOps.gate = [](void *cookie, uint8_t state, uint8_t) {
            auto &rig = *static_cast<Rig *>(cookie);
            switch (state) {
            case A830GateRecovering: {
                CHECK(rig.gate == A830GateOpen && rig.recovery.replaying());
                CHECK(rig.submit.halt(nullptr, nullptr) == kIOReturnSuccess);
                NativeAdrenoAbiSubmitReply reply{};
                CHECK(rig.render.core.submit(&rig.renderJob, &reply, &rig.clock) == kIOReturnBusy && !reply.fence);
                break;
            }
            case A830GateOpen:
                CHECK(rig.gate == A830GateRecovering && rig.submit.halted());
                rig.render.status.noteSuspend();
                rig.peer->status.noteSuspend();
                rig.submit.resume();
                break;
            default:
                CHECK(false);
            }
            rig.gate = static_cast<A830Gate>(state);
        };
        recoveryOps.plan = [](void *cookie, uint8_t cause, uint32_t *restart) {
            auto &rig = *static_cast<Rig *>(cookie);
            CHECK(rig.submit.plan(A830FaultBlamesClient(cause), &rig.plan) == kIOReturnSuccess);
            if (rig.plan.fault_ranges & NativeAdrenoRangeBit(RenderSlot)) rig.render.status.noteFault();
            if (rig.plan.fault_ranges & NativeAdrenoRangeBit(FaultSlot)) rig.peer->status.noteFault();
            *restart = rig.plan.restart;
            return rig.plan.failed;
        };
        recoveryOps.fenceSignal = [](void *cookie) {
            auto &rig = *static_cast<Rig *>(cookie);
            ++rig.signals;
            rig.fence.wakeAll();
        };
        recoveryOps.replay = [](void *cookie, uint32_t *replayed) {
            return static_cast<Rig *>(cookie)->submit.replay(replayed);
        };
        CHECK(power.init(A830PowerConfig{}, gIo, gPowerEnv, powerOps) == kIOReturnSuccess);
        CHECK(recovery.init(A830RecoveryConfig{}, gIo, gPowerEnv, powerOps, recoveryOps, &power) == kIOReturnSuccess);
        power.start(4, 3);
        open(render, RenderSlot);
        render.core.setRenderServer(true);
        const uint64_t bo = alloc(render, 2 * Page, 0);
        map(render, bo, renderVa);
        renderJob = submission(bo, renderVa);
    }

    void open(Client &client, unsigned slot)
    {
        const auto &window = windows[slot];
        CHECK(client.core.open(NativeAdrenoAbiVersion, &vm, &submit, &ring, 36, 36) == kIOReturnSuccess);
        CHECK(client.core.setScope(slot * NativeAdrenoAbiMaxQueues, window.start, window.bytes) == kIOReturnSuccess);
        CHECK(client.core.attachStatus(&client.status) == kIOReturnSuccess);
    }

    uint32_t enqueue(Client &client, const NativeAdrenoAbiSubmit &request)
    {
        const uint32_t issued = ring.lastSubmitted();
        NativeAdrenoAbiSubmitReply reply{};
        CHECK(client.core.submit(&request, &reply, &clock) == kIOReturnSuccess);
        CHECK(reply.fence == issued + 1 && publishedWptr == ring.writeIndex());
        fence.submitted(uint32_t(reply.fence));
        return uint32_t(reply.fence);
    }

    void complete(uint32_t seq)
    {
        __atomic_store_n(&memptrs[0], publishedWptr, __ATOMIC_RELAXED);
        __atomic_store_n(&memptrs[3], seq, __ATOMIC_RELAXED);
        __atomic_store_n(&memptrs[1], seq, __ATOMIC_RELEASE);
        fence.signal();
        CHECK(ring.wait(seq, &clock, 0, nullptr) == NativeAdrenoRingFenceComplete);
        CHECK(submit.retireCompleted() == kIOReturnSuccess);
    }

    void raisePageFault(uint64_t far)
    {
        smmuHost.completed = fence.completed();
        smmuHost.issued = ring.lastSubmitted();
        mmio.fault(0, 0x8, far, smmu.gpuStream().sid);
        CHECK(smmuHost.fire(710) == 1);
    }

    void drainPageFault(uint64_t far, uint32_t sampledFence, NativeAdrenoFaultVerdict verdict)
    {
        A830SmmuFault fault{};
        CHECK(smmu.drainFaults([](void *out, const A830SmmuFault &event) {
            *static_cast<A830SmmuFault *>(out) = event;
        }, &fault) == 1);
        CHECK(fault.cb == 0 && fault.far == far && fault.sampled && fault.issued == sampledFence);
        CHECK(fault.completed + 1 == fault.issued);
        const auto before = attribution[verdict];
        const unsigned at = NativeAdrenoFaultNote(windows, &submit, fault.far, fault.completed,
                                                   fault.issued, fault.sampled != 0, attribution);
        CHECK(attribution[verdict] == before + 1);
        if (at == RenderSlot) render.status.noteFault();
        if (at == FaultSlot) peer->status.noteFault();
        CHECK(at == FaultSlot && !recovery.pending() && !vm.poisoned());
    }

    void finish()
    {
        CHECK(render.core.releaseAll(&clock, 0) == kIOReturnSuccess);
        CHECK(submit.detach() == kIOReturnSuccess && ring.detach() == kIOReturnSuccess);
        CHECK(vm.auditTables() == kIOReturnSuccess && !vm.objectCount());
        CHECK(vm.detach() == kIOReturnSuccess && context.disable() == kIOReturnSuccess);
        CHECK(vm.releasePaged() == kIOReturnSuccess);
        CHECK(!mmio.violations && !mmio.aborts);
    }
};

void configureTier(unsigned mode)
{
    uint8_t property[pianoxram::kMaxRuns * pianoxram::kRecordBytes]{};
    for (unsigned b = 0; b < 8; ++b) {
        property[b] = uint8_t(a830tier::kRegionLo >> (8 * b));
        property[8 + b] = uint8_t(TierBytes >> (8 * b));
    }
    property[16] = pianoxram::Region1;
    property[20] = pianoxram::UseGpu;
    CHECK(NativeAdrenoMemory::configureTier(mode, property, sizeof property, 0x8c0004000ull, 0x33fe00000ull)
          == (mode ? a830tier::Parse::ok : a830tier::Parse::absent));
    unsigned probes = 0;
    NativeAdrenoMemory::qualifyTier(36, true, false, [](uint64_t, unsigned, void *cookie) {
        ++*static_cast<unsigned *>(cookie);
        return true;
    }, &probes, nullptr);
    CHECK(probes == (mode ? 2u : 0u));
}

void expectTier(unsigned mode, uint64_t used, uint64_t allocs, uint64_t refusals, uint64_t denials)
{
    const auto tier = NativeAdrenoMemory::tierStats();
    CHECK(tier.mode == mode && !std::strcmp(tier.state, mode ? "ready" : "off"));
    CHECK(tier.used_bytes == used && tier.free_bytes == (mode ? TierBytes - used : 0));
    CHECK(tier.allocations == allocs && tier.refusals == refusals);
    CHECK(tier.requests[NativeAdrenoTierCpuMap] == 0);
    CHECK(tier.requests[NativeAdrenoTierImportDenied] == denials);
    CHECK(tier.requests[NativeAdrenoTierCpuMapDenied] == denials);
}

void denyCpuMapAndImport(Client &client, uint64_t handle)
{
    auto *memory = client.core.memoryOf(handle);
    CHECK(memory && memory->tier() && !memory->mapping() && !memory->retainVmCpuDescriptor());
    const auto token = client.core.tokenOf(handle);
    const NativeAdrenoAbiBoCpuMap cpuMap{handle};
    NativeAdrenoAbiBoCpuMapReply view{};
    CHECK(client.core.cpuMap(&cpuMap, &view) == kIOReturnUnsupported && !view.token);
    CHECK(!client.core.memoryForToken(token) && !client.core.mappingLeases(token));

    const size_t live = client.core.liveObjects(), imports = fixture.imports_live;
    const uint64_t savedPhysical = fixture.next_physical;
    fixture.next_physical = memory->spans()[0].physical;
    const NativeAdrenoAbiBoImport request{0x10000000, Page};
    NativeAdrenoAbiBoAllocReply reply{};
    CHECK(client.core.importBo(&request, &client, &reply) == kIOReturnUnsupported && !reply.handle);
    fixture.next_physical = savedPhysical;
    CHECK(client.core.liveObjects() == live && fixture.imports_live == imports);
}

void expectReclaimed(Rig &rig, uint64_t renderLeaf, uint64_t tablePages, size_t descriptors, size_t metadata)
{
    CHECK(rig.vm.trimTables() == kIOReturnSuccess && rig.vm.auditTables() == kIOReturnSuccess);
    CHECK(rig.vm.objectCount() == 1 && rig.vm.mappedBytes() == 2 * Page && !rig.vm.poisoned());
    NativeAdrenoVmPagedStats stats{};
    rig.vm.pagedStats(&stats);
    CHECK(stats.table_pages == tablePages && stats.leaves == 2 && !stats.trim_pending);
    CHECK(!rig.vm.pagedLeaf(rig.peerVa) && !rig.vm.pagedLeaf(rig.peerVa + TierBytes));
    CHECK(rig.vm.pagedLeaf(rig.renderVa) == renderLeaf && rig.context.observe() == kIOReturnSuccess);
    CHECK(fixture.live.size() == descriptors && fixture.metadata.size() == metadata && !fixture.imports_live);
}

void runMode(Rig &rig, unsigned mode)
{
    configureTier(mode);
    expectTier(mode, 0, 0, 0, 0);
    const auto renderLeaf = rig.vm.pagedLeaf(rig.renderVa);
    CHECK(renderLeaf);
    NativeAdrenoVmPagedStats baseline{};
    rig.vm.pagedStats(&baseline);
    const auto descriptors = fixture.live.size(), metadata = fixture.metadata.size();
    for (unsigned round = 0; round < RoundsPerMode; ++round) {
        Client peer;
        rig.peer = &peer;
        rig.open(peer, FaultSlot);
        NativeAdrenoAbiParams params{};
        CHECK(peer.core.query(&params) == kIOReturnSuccess);
        CHECK(bool(params.flags & NativeAdrenoParamsGpuPrivate) == bool(mode));

        uint64_t first = 0, used = 0;
        if (mode) {
            for (uint64_t mib : TierSizesMiB) {
                const auto handle = alloc(peer, mib * MiB, NativeAdrenoBoAllocGpuPrivate);
                const auto *memory = peer.core.memoryOf(handle);
                CHECK(memory && memory->tier() && memory->spanCount() == 1);
                CHECK(memory->spans()[0].physical == a830tier::kRegionLo + used);
                map(peer, handle, rig.peerVa + used);
                if (!first) first = handle;
                used += mib * MiB;
                CHECK(NativeAdrenoMemory::tierStats().used_bytes == used);
            }
            CHECK(used == TierBytes);
            denyCpuMapAndImport(peer, first);
        }

        // Exhaustion falls back to wired pageable memory. Fail that allocation once, then retry.
        const auto beforeOom = fixture.live.size(), beforeMeta = fixture.metadata.size(), beforeBo = peer.core.liveObjects();
        const NativeAdrenoAbiBoAlloc exhausted{a830tier::kSlab, 12, NativeAdrenoBoAllocGpuPrivate};
        fixture.allocation_fail = true;
        NativeAdrenoAbiBoAllocReply failed{};
        CHECK(peer.core.allocBo(&exhausted, &failed) != kIOReturnSuccess && !failed.handle);
        fixture.allocation_fail = false;
        CHECK(fixture.live.size() == beforeOom && fixture.metadata.size() == beforeMeta && peer.core.liveObjects() == beforeBo);
        const uint64_t fallback = alloc(peer, exhausted.bytes, exhausted.contiguous);
        const auto *memory = peer.core.memoryOf(fallback);
        CHECK(memory && memory->unmapped() && !memory->tier() && !memory->pooled() && memory->spanCount() == exhausted.bytes / Page);
        const auto mappings = rig.vm.objectCount();
        NativeAdrenoAbiBoMapReply badMap{};
        NativeAdrenoAbiBoMap across{fallback, rig.renderVa, 3, 0, 2, 0};
        CHECK(peer.core.mapBo(&across, &badMap) == kIOReturnBadArgument && !badMap.iova);
        across.iova = rig.peerVa + rig.windows[FaultSlot].bytes - Page;
        CHECK(peer.core.mapBo(&across, &badMap) == kIOReturnBadArgument && !badMap.iova);
        CHECK(!rig.vm.pagedLeaf(across.iova) && rig.vm.objectCount() == mappings);
        map(peer, fallback, rig.peerVa + used);
        if (!first) first = fallback;
        CHECK(rig.vm.pagedLeaf(rig.peerVa) && rig.vm.auditTables() == kIOReturnSuccess);
        const auto crossSubmit = submission(first, rig.renderVa);
        NativeAdrenoAbiSubmitReply rejected{};
        CHECK(peer.core.submit(&crossSubmit, &rejected, &rig.clock) == kIOReturnBadArgument && !rejected.fence);
        const auto renderCrossSubmit = submission(rig.renderJob.buffers[0].handle, rig.peerVa);
        CHECK(rig.render.core.submit(&renderCrossSubmit, &rejected, &rig.clock) == kIOReturnBadArgument && !rejected.fence);
        CHECK(rig.vm.pagedLeaf(rig.renderVa) == renderLeaf && !rig.submit.outstanding());

        // On alternate rounds, let the top half sample WindowServer alone while FAR names the
        // held slot. Retire WindowServer before draining the record so current fences differ.
        const bool apart = !(round % 2);
        const uint64_t far = rig.peerVa + Page;
        uint32_t sampled = 0;
        if (apart) {
            sampled = rig.enqueue(rig.render, rig.renderJob);
            rig.raisePageFault(far);
            rig.complete(sampled);
        }
        const uint32_t culprit = rig.enqueue(peer, submission(first, rig.peerVa));
        if (!apart) {
            sampled = culprit;
            rig.raisePageFault(far);
        }
        rig.drainPageFault(far, sampled, apart ? NativeAdrenoFaultApart : NativeAdrenoFaultAlike);
        NativeAdrenoAbiStatus status{};
        CHECK(peer.core.status(&status) == kIOReturnSuccess && status.faults == 1);
        CHECK(rig.render.core.status(&status) == kIOReturnSuccess && !status.faults);
        CHECK(peer.core.releaseAll(&rig.clock, 0) == kIOReturnTimeout && peer.core.liveObjects() == beforeBo + 1);
        const uint64_t tierRounds = mode ? uint64_t(round) + 1 : 0;
        expectTier(mode, used, tierRounds * std::size(TierSizesMiB), tierRounds * 2, tierRounds);

        const uint32_t innocent = rig.enqueue(rig.render, rig.renderJob);
        IOReturn result = kIOReturnNotReady;
        A830WaitStats waited{};
        std::thread waiter([&] { result = rig.fence.waitNs(culprit, FenceDeadlineNs, &waited); });
        IOReturn retry = kIOReturnNotReady;
        A830WaitStats interrupted{};
        std::thread replayWaiter([&] { retry = rig.fence.waitNs(innocent, FenceDeadlineNs, &interrupted); });
        const auto deadline = Clock::now() + 1s;
        unsigned waiting;
        do {
            waiting = a830HostWaiting(rig.fence.event());
            if (waiting == 2) break;
            std::this_thread::yield();
        } while (Clock::now() < deadline);
        CHECK(waiting == 2);

        // Keep the production two-per-minute policy. Only the virtual clock advances.
        gEnv.now += uint64_t(rig.recovery.config().windowMs) * 1'000'000;
        const uint8_t cause = apart ? A830FaultHang : A830FaultCpHwError;
        const auto before = rig.recovery.stats();
        rig.recovery.report(cause, far);
        rig.recovery.report(A830FaultWatchdog, 0);
        CHECK(rig.recovery.pending() && rig.recovery.runPending() == kIOReturnSuccess);
        waiter.join();
        // An interrupted wait for the still-pending replay returns the retryable timeout.
        a830HostAbort(replayWaiter.get_id());
        replayWaiter.join();
        CHECK(Clock::now() < deadline && result == kIOReturnSuccess);
        CHECK(retry == kIOReturnTimeout && interrupted.interrupted);
        rig.waiterWakeups += waited.woken;
        const auto stats = rig.recovery.stats();
        CHECK(!rig.recovery.pending() && !rig.recovery.busy() && !rig.recovery.lost() && !gEnv.locked);
        CHECK(rig.gate == A830GateOpen && !rig.submit.halted() && rig.power.phase() == A830PowerActive);
        CHECK(stats.recoveries == before.recoveries + 1 && stats.jobsFailed == before.jobsFailed + 1);
        CHECK(stats.jobsReplayed == before.jobsReplayed + 1 && stats.coalesced == before.coalesced + 1);
        CHECK(!stats.failures && rig.recovery.snapshot().cause == cause && rig.recovery.snapshot().detail == far);
        CHECK(rig.plan.culprit_fence == culprit && rig.plan.fault_ranges == NativeAdrenoRangeBit(FaultSlot));
        CHECK(rig.plan.first_replay == innocent && rig.plan.replay_ranges == NativeAdrenoRangeBit(RenderSlot));
        CHECK(rig.fence.completed() == culprit && rig.submit.outstanding() == 1);
        CHECK(peer.core.status(&status) == kIOReturnSuccess && status.faults == 2 && status.suspends == 1);

        // The skipped fence advances during reset; the replayed fence still has a bounded timeout.
        uint32_t observed = 0;
        const auto waitStart = Clock::now();
        CHECK(rig.ring.observeFence(innocent, &rig.clock, 2000, &observed) == NativeAdrenoRingFenceTimeout);
        CHECK(observed == culprit && Clock::now() - waitStart < 1s);
        CHECK(peer.core.releaseAll(&rig.clock, 0) == kIOReturnSuccess && !peer.core.liveObjects());
        expectTier(mode, 0, tierRounds * std::size(TierSizesMiB), tierRounds * 2, tierRounds);
        expectReclaimed(rig, renderLeaf, baseline.table_pages, descriptors, metadata);
        rig.peer = nullptr;

        rig.complete(innocent);
        const NativeAdrenoAbiWaitFence wait{innocent, 0};
        CHECK(rig.render.core.waitFence(&wait, &rig.clock) == kIOReturnSuccess);
        rig.complete(rig.enqueue(rig.render, rig.renderJob));
        CHECK(!rig.submit.outstanding() && rig.render.core.status(&status) == kIOReturnSuccess && !status.faults);
    }

    // Close a healthy connection in the same slot without a reset to retire its private BO.
    Client clean;
    rig.open(clean, FaultSlot);
    map(clean, alloc(clean, a830tier::kSlab, NativeAdrenoBoAllocGpuPrivate), rig.peerVa);
    const uint64_t allocs = mode ? RoundsPerMode * std::size(TierSizesMiB) + 1 : 0;
    expectTier(mode, mode ? a830tier::kSlab : 0, allocs, mode * RoundsPerMode * 2, mode * RoundsPerMode);
    CHECK(clean.core.releaseAll(&rig.clock, 0) == kIOReturnSuccess);
    expectTier(mode, 0, allocs, mode * RoundsPerMode * 2, mode * RoundsPerMode);
    expectReclaimed(rig, renderLeaf, baseline.table_pages, descriptors, metadata);
}
} // namespace

int main()
{
    resetAll();
    adreno830_profile_value = A830ProfileDesign; // Sample fences in the SMMU interrupt top half.
    fixture.mode = ScatterUnlessContiguous;
    fixture.zero_physical = [](uint64_t, size_t) {}; // Tier backing is modelled by its physical spans.
    NativeAdrenoMemory::setUnmappedUser(true);
    a830HostManualCalls(true);
    auto rig = std::make_unique<Rig>();
    rig->start();

    CHECK(a830tier::bootArgMode("") == 0 && NativeAdrenoMemory::tierStats().mode == 0);
    runMode(*rig, 0);
    runMode(*rig, 1);
    CHECK(rig->signals == 2 * RoundsPerMode && rig->recovery.stats().recoveries == rig->signals && rig->waiterWakeups);
    CHECK(rig->attribution[NativeAdrenoFaultAlike] == RoundsPerMode);
    CHECK(rig->attribution[NativeAdrenoFaultApart] == RoundsPerMode);
    rig->finish();
    CHECK(fixture.live.empty() && fixture.metadata.empty());
    a830HostShutdown();
    std::printf("PASS: %u GPU fault/recovery rounds, isolated VM slots, bounded fence waits, 126 MiB tier reclaim and pageable fallback\n",
                2 * RoundsPerMode);
}
