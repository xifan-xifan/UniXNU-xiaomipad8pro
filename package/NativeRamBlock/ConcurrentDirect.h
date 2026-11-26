// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include "DirectDisk.h"

// Adapted from RamBlock 525abe5's caller-thread servicing, stage pool, range exclusion and separate commit.
// Env supplies the disk gate and its sleep/wakeup, plus a separate bitmap publication mutex.
struct DirectAdmission {
  static constexpr unsigned kDepth = 8;
  enum : int { kOffline = -1, kSerial = -2, kNoStage = -3 };

  struct Request {
    uint64_t first, last;
    bool write;
    Request *next = nullptr;
    Request(uint64_t offset, uint64_t length, bool out)
      : first(offset / persist::kBlock), last((offset + length - 1) / persist::kBlock), write(out) {}
    bool conflicts(const Request &other) const {
      return (write || other.write) && first <= other.last && other.first <= last;
    }
  };

  unsigned depth = 0, inflight = 0, peak = 0, waiting = 0, quarantined = 0;
  uint64_t waits = 0;
  bool online = false, parallel = false, held = false, fenced = false;

  void configure(unsigned count, bool startParallel) {
    depth = count;
    online = true;
    parallel = startParallel;
  }

  // Called with the gate. Stage waiters never hold a page range or an inflight slot.
  template<class Env> int take(Env &env, Request &request) {
    if (!online) return kOffline;
    if (!fenced && !held && !parallel) return kSerial;
    if (tail) tail->next = &request; else head = &request;
    tail = &request;
    ++waiting;
    int slot;
    bool waited = false;
    for (;;) {
      if (!online) { slot = kOffline; break; }
      if (!fenced && !held) {
        if (!parallel) { slot = kSerial; break; }
        if (quarantined == (1U << depth) - 1) { slot = kNoStage; break; }
        slot = freeSlot();
        if (slot >= 0 && eligible() == &request) {
          active[slot] = &request;
          if (++inflight > peak) peak = inflight;
          break;
        }
      }
      if (!waited) { ++waits; waited = true; }
      env.wait();
    }
    Request *previous = nullptr;
    for (auto *it = head; it != &request; it = it->next) previous = it;
    if (previous) previous->next = request.next; else head = request.next;
    if (tail == &request) tail = previous;
    --waiting;
    if (waiting || !online) env.wake();
    return slot;
  }

  template<class Env> void give(Env &env, unsigned slot, bool liveDMA) {
    if (liveDMA) quarantined |= 1U << slot;
    active[slot] = nullptr;
    --inflight;
    if (waiting || fenced || !online) env.wake();
  }

  // Admission stays closed until the owner reopens it. Wait releases the gate so requests can finish.
  template<class Env> bool beginFence(Env &env) {
    while (online && fenced) env.wait();
    if (!online) return false;
    fenced = true;
    while (inflight) env.wait();
    if (online) return true;
    endFence(env);
    return false;
  }
  template<class Env> void endFence(Env &env) { fenced = false; env.wake(); }

  // A read failure can degrade the store without latching a write failure or losing a DMA stage.
  bool modeChangeAllowed(bool enable, bool degraded) const {
    return !quarantined && (!enable || parallel || !degraded);
  }

  template<class Env> void stop(Env &env) {
    online = false;
    env.wake();
    while (inflight || waiting || fenced) env.wait();
  }

private:
  Request *head = nullptr, *tail = nullptr;
  Request *active[kDepth] = {};

  int freeSlot() const {
    for (unsigned k = 0; k < depth; ++k)
      if (!active[k] && !(quarantined & (1U << k))) return static_cast<int>(k);
    return kNoStage;
  }
  Request *eligible() const {
    for (auto *it = head; it; it = it->next) {
      bool conflict = false;
      for (auto *request : active)
        if (request && it->conflicts(*request)) { conflict = true; break; }
      if (conflict) continue;
      // A later matching read must not overtake an earlier waiting writer.
      for (auto *older = head; older != it; older = older->next)
        if (it->conflicts(*older)) { conflict = true; break; }
      if (!conflict) return it;
    }
    return nullptr;
  }
};

struct DirectWriteState {
  bool failed() const { return __atomic_load_n(&broken, __ATOMIC_ACQUIRE); }
  void fail() { __atomic_store_n(&broken, true, __ATOMIC_RELEASE); }
private:
  bool broken = false;
};

struct DirectCommitQueue {
  unsigned pending = 0;   // guarded by the admission gate

  template<class Env, class Store>
  bool commit(Env &env, persist::Overlay &overlay, Store &store, uint8_t *scratch, uint64_t first, uint32_t count) {
    Run run{first, first + count - 1};
    env.lock();
    if (tail) tail->next = &run; else head = &run;
    tail = &run;
    ++pending;
    env.unlock();

    env.lockCommit();
    if (!run.done) {
      env.lock();
      auto *ready = head;
      head = tail = nullptr;
      pending = 0;
      env.unlock();
      // Only combine data runs already durable and waiting for the mutex. The leader's data
      // stage is now free to hold the snapshot, and stays private until the bitmap FUA returns.
      publish(overlay, store, scratch, ready);
    }
    env.unlockCommit();
    return run.ok;
  }

private:
  static constexpr uint64_t kPagesPerBitmap = 8ULL * persist::kBlock;
  struct Run {
    uint64_t first, last;
    Run *next = nullptr;
    bool done = false, ok = true;
  };
  Run *head = nullptr, *tail = nullptr;

  template<class Store>
  static void publish(persist::Overlay &overlay, Store &store, uint8_t *scratch, Run *runs) {
    while (true) {
      uint64_t block = UINT64_MAX;
      for (auto *run = runs; run; run = run->next)
        if (!run->done && run->first / kPagesPerBitmap < block) block = run->first / kPagesPerBitmap;
      if (block == UINT64_MAX) return;
      const uint64_t base = block * kPagesPerBitmap, limit = base + kPagesPerBitmap;
      auto *bits = overlay.bitmap + block * persist::kBlock;
      // All other accesses during the copy are reads. The commit mutex excludes every bitmap writer.
      memcpy(scratch, bits, persist::kBlock);
      uint64_t gained = 0;
      for (auto *run = runs; run; run = run->next) {
        if (run->done || run->first >= limit) continue;
        for (uint64_t page = run->first - base; page < kPagesPerBitmap && page <= run->last - base; ++page) {
          const uint8_t mask = 1U << (page & 7);
          auto &byte = scratch[page >> 3];
          if (!(byte & mask)) { byte |= mask; ++gained; }
        }
      }
      const bool ok = !gained || store.write(1 + block, 1, scratch);
      if (ok && gained) {
        for (unsigned i = 0; i < persist::kBlock; ++i)
          if (scratch[i] != bits[i]) __atomic_store_n(&bits[i], scratch[i], __ATOMIC_RELEASE);
        overlay.present += gained;
        store.published(overlay.present);
      }
      for (auto *run = runs; run; run = run->next) {
        if (run->done) continue;
        if (!ok) { run->ok = false; run->done = true; }
        else if (run->first < limit) {
          if (run->last < limit) run->done = true;
          else run->first = limit;
        }
      }
    }
  }
};

template<class Env, class Store> struct ConcurrentDirect {
  persist::Overlay &overlay;
  DirectCommitQueue &commits;
  Env &env;
  Store &store;
  uint8_t *scratch;

  bool has(uint64_t page) const {
    return __atomic_load_n(&overlay.bitmap[page >> 3], __ATOMIC_ACQUIRE) >> (page & 7) & 1;
  }
  bool write(uint64_t first, uint32_t count, const void *data) {
    if (!store.write(persist::pageBlock(overlay.pages, first), count, data)) return false;
    for (uint64_t page = first; page < first + count; ++page)
      if (!has(page)) return commits.commit(env, overlay, store, scratch, first, count);
    return true;
  }
};
