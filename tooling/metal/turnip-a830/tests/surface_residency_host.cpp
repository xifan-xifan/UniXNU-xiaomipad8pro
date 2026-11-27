#include "SurfaceResidency.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <future>
#include <set>
#include <stdexcept>

struct FakeGpu {
    std::mutex lock;
    std::set<unsigned> mapped;
    std::set<unsigned> quarantined;
    unsigned next = 0;
    unsigned rejected = 0;
    bool failMap = false;
    bool deferFree = false;

    unsigned map() {
        std::lock_guard<std::mutex> guard(lock);
        if (std::exchange(failMap, false) || mapped.size() == 2) {
            ++rejected;
            return 0;
        }
        const unsigned address = ++next;
        assert(mapped.insert(address).second);
        return address;
    }

    void release(unsigned address) {
        std::lock_guard<std::mutex> guard(lock);
        if (std::exchange(deferFree, false)) {
            assert(mapped.count(address));
            assert(quarantined.insert(address).second);
        } else {
            assert(mapped.erase(address) == 1);
        }
    }

    void retire(unsigned address) {
        std::lock_guard<std::mutex> guard(lock);
        assert(quarantined.erase(address) == 1);
        assert(mapped.erase(address) == 1);
    }

    bool usable(unsigned address) {
        std::lock_guard<std::mutex> guard(lock);
        return mapped.count(address) && !quarantined.count(address);
    }
};

struct Generation {
    FakeGpu &gpu;
    unsigned address;
    std::shared_ptr<unsigned> surface;
    Generation(FakeGpu &backend, unsigned mapping, std::shared_ptr<unsigned> pixels)
        : gpu(backend), address(mapping), surface(std::move(pixels)) {}
    ~Generation() {gpu.release(address);}
};

int main() {
    assert(!SurfaceResidencyEnabled(""));
    assert(!SurfaceResidencyEnabled("piano-surface-residency=0 other=1"));
    assert(!SurfaceResidencyEnabled("other-piano-surface-residency=1 piano-surface-residency=10 "));
    assert(SurfaceResidencyEnabled("other=1\tpiano-surface-residency=1 reset=1"));
    FakeGpu gpu;
    auto pixels = std::make_shared<unsigned>(42);
    auto restore = [&]() -> std::shared_ptr<Generation> {
        const auto address = gpu.map();
        return address ? std::make_shared<Generation>(gpu, address, pixels) : nullptr;
    };
    SurfaceResidency<Generation> decoder(restore()), unusedPool(restore()), capture(nullptr);
    assert(!capture.borrowResident() && !capture.evictIdle());
    try {
        capture.acquire(true, []() -> std::shared_ptr<Generation> {throw std::runtime_error("map failed");});
        assert(false);
    } catch (const std::runtime_error &) {
        assert(!capture.borrowResident() && !capture.evictIdle());
    }
    auto decodeLease = decoder.acquire(false, restore);
    assert(decodeLease && !decoder.evictIdle() && gpu.usable(1));
    assert(!capture.acquire(false, restore));
    assert(unusedPool.evictIdle() && !gpu.usable(2));
    auto captureLease = capture.acquire(false, restore);
    assert(captureLease && gpu.usable(captureLease->address));

    std::promise<void> decodeRetired, captureRetired;
    auto checkDecode = std::async(std::launch::async, [&, lease = std::move(decodeLease)]() mutable {
        decodeRetired.get_future().wait();
        assert(gpu.usable(lease->address) && *lease->surface == 42);
        lease.reset();
    });
    auto checkCapture = std::async(std::launch::async, [&, lease = std::move(captureLease)]() mutable {
        captureRetired.get_future().wait();
        assert(gpu.usable(lease->address) && *lease->surface == 42);
        lease.reset();
    });
    assert(!decoder.evictIdle() && !capture.evictIdle() && !unusedPool.acquire(false, restore));
    decodeRetired.set_value();
    checkDecode.get();
    gpu.deferFree = true;
    assert(decoder.evictIdle() && !decoder.borrowResident());
    assert(!unusedPool.acquire(false, restore));
    gpu.retire(1);
    assert(!capture.evictIdle());

    gpu.failMap = true;
    assert(!unusedPool.acquire(false, restore) && !unusedPool.borrowResident());
    std::atomic<unsigned> restores{0};
    std::promise<void> remapping, allowMap, secondStarted;
    auto delayed = [&] {
        assert(++restores == 1);
        remapping.set_value();
        allowMap.get_future().wait();
        return restore();
    };
    auto first = std::async(std::launch::async, [&] {return unusedPool.acquire(false, delayed);});
    remapping.get_future().wait();
    assert(!unusedPool.evictIdle() && !unusedPool.borrowResident());
    auto second = std::async(std::launch::async, [&] {
        secondStarted.set_value();
        return unusedPool.acquire(true, delayed);
    });
    secondStarted.get_future().wait();
    allowMap.set_value();
    auto remapped = first.get();
    assert(remapped && remapped == second.get() && remapped->address != 2 && *remapped->surface == 42);
    remapped.reset();
    assert(!unusedPool.evictIdle() && restores == 1);
    captureRetired.set_value();
    checkCapture.get();
    assert(capture.evictIdle() && gpu.rejected == 4);
    assert(!capture.evictIdle() && !capture.borrowResident());
    const auto borrow = decoder.acquire(false, restore)->address;
    {
        auto borrowed = decoder.borrowResident();
        assert(borrowed && borrowed->address == borrow && !decoder.evictIdle());
    }
    assert(decoder.evictIdle() && !gpu.usable(borrow));
    std::puts("PASS concurrent decode/capture leases, idle VA release, busy quarantine, failed map, coalesced remap and escaped pin");
}
