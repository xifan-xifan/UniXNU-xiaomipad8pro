#pragma once

#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

inline bool SurfaceResidencyEnabled(const char *args) {
    constexpr char enabled[] = "piano-surface-residency=1";
    for (const char *at = args; *at;) {
        at += std::strspn(at, " \t");
        const size_t length = std::strcspn(at, " \t");
        if (length == sizeof enabled - 1 && !std::strncmp(at, enabled, length))
            return true;
        at += length;
    }
    return false;
}

template<class Generation> class SurfaceResidency {
    std::mutex lock_;
    std::condition_variable changed_;
    std::shared_ptr<Generation> resident_;
    bool changing_ = false;
    bool pinned_ = false;

    void endChange(std::unique_lock<std::mutex> &lock) {
        changing_ = false;
        lock.unlock();
        changed_.notify_all();
    }

public:
    explicit SurfaceResidency(std::shared_ptr<Generation> initial) : resident_(std::move(initial)) {}

    std::shared_ptr<Generation> borrowResident() {
        std::lock_guard<std::mutex> lock(lock_);
        return changing_ ? nullptr : resident_;
    }

    template<class Restore> std::shared_ptr<Generation> acquire(bool pin, Restore restore) {
        std::unique_lock<std::mutex> lock(lock_);
        changed_.wait(lock, [&] {return !changing_;});
        if (resident_) {
            pinned_ |= pin;
            return resident_;
        }
        changing_ = true;
        lock.unlock();
        try {
            auto restored = restore();
            lock.lock();
            resident_ = restored;
            pinned_ |= pin && bool(restored);
            endChange(lock);
            return restored;
        } catch (...) {
            lock.lock();
            endChange(lock);
            throw;
        }
    }

    bool evictIdle() {
        std::unique_lock<std::mutex> lock(lock_);
        if (changing_ || pinned_ || !resident_ || resident_.use_count() != 1)
            return false;
        changing_ = true;
        auto idle = std::move(resident_);
        lock.unlock();
        idle.reset();
        lock.lock();
        endChange(lock);
        return true;
    }
};
