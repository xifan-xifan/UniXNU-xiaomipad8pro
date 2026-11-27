// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: clang++ -std=c++20 -O1 -g -UNDEBUG -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-sanitize-recover=all ramblock_concurrent_quarantine_host.cpp -o /tmp/ramblock_concurrent_quarantine_host && /tmp/ramblock_concurrent_quarantine_host
// The provider and UFS controller are host models. Admission, bitmap publication,
// volatile allocation, CDB construction, slot allocation and polling use driver headers.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "../DirectAsync.h"
#include "../VolatileDisk.h"
#include "../VolatilePool.h"
#include "../../NativeUfs/RootDiagnostics.h"
#include "../../NativeUfs/RootTransportLayout.h"
#include "../../NativeUfs/TransferPoll.h"
#pragma clang diagnostic pop

namespace {
using namespace std::chrono_literals;
constexpr unsigned kBlock = persist::kBlock, kSector = 512, kPages = 32;
constexpr unsigned kStages = 4, kStagePages = 2, kStageBytes = kStagePages * kBlock;
constexpr unsigned kReadStages = 0b0011, kWriteStages = 0b1100, kStageMask = kReadStages | kWriteStages;
constexpr unsigned kWindowBlocks = kPages + 2;  // header, one bitmap block, data
constexpr uint64_t kEnd = UINT32_MAX, kFirst = kEnd - kWindowBlocks;
constexpr uint64_t kPhys = 0x10000000;
constexpr unsigned kDmaSize = kDmaBytes + kStages * kStageBytes;
using Block = std::array<uint8_t, kBlock>;
using Image = std::array<Block, kWindowBlocks + 2>;  // includes both neighboring partitions

template<class Predicate>
void await(std::condition_variable &changed, std::unique_lock<std::mutex> &lock, Predicate ready)
{
  assert(changed.wait_for(lock, 10s, ready));
}

// RootCommands.h's port. CPU and device have separate views of the DMA region.
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnBusy = 2,
                   kIOReturnIOError = 3, kIOReturnTimeout = 4, kIOReturnNotReady = 5, kIOReturnError = 6;
constexpr int THREAD_UNINT = 0;
constexpr unsigned kMicrosecondScale = 1;
using IOLock = std::mutex;
IOLock slotLock;
std::condition_variable slotChanged;
struct {
  uint8_t *virt;
  uint64_t phys;
  UfsAtomicField<bool, true> ready, poisoned;
  uintptr_t hc;
} gHost;
UfsRootDiagnostics gRootDiagnostic;
alignas(kBlock) uint8_t cpu[kDmaSize];
alignas(kBlock) uint8_t device[kDmaSize];
thread_local uint64_t ticks;

void IOLockLock(IOLock *lock) { lock->lock(); }
void IOLockUnlock(IOLock *lock) { lock->unlock(); }
bool IOLockTryLock(IOLock *lock) { return lock->try_lock(); }
void IOLockSleep(IOLock *lock, void *, int)
{
  std::unique_lock held(*lock, std::adopt_lock);
  assert(slotChanged.wait_for(held, 10s) != std::cv_status::timeout);
  held.release();
}
void IOLockWakeup(IOLock *, void *, bool) { slotChanged.notify_all(); }
uint64_t mach_absolute_time() { return ++ticks; }
void clock_interval_to_deadline(uint32_t interval, unsigned scale, uint64_t *deadline)
{
  *deadline = mach_absolute_time() + uint64_t(interval) * scale;
}
void IODelay(unsigned us) { ticks += us; }
bool hostInterruptMode() { return false; }
unsigned hostCompletionCount(unsigned) { return 0; }
void hostWaitCompletion(unsigned, unsigned, uint64_t) { IODelay(2); }
uint64_t hostPerfNow() { return 0; }
void hostPerfSpan(unsigned, uint64_t, uint64_t) {}
void hostPerfDepth(int) {}
IOReturn recordTransport(IOReturn result, uint32_t ocs)
{
  gRootDiagnostic.result = result;
  gRootDiagnostic.ocs = ocs;
  return result;
}
uint64_t hostDmaAddress(const void *buffer)
{
  const auto offset = reinterpret_cast<uintptr_t>(buffer) - reinterpret_cast<uintptr_t>(cpu);
  return offset >= kDmaBytes && offset < kDmaSize ? kPhys + offset : 0;
}
void cacheCivac(const void *buffer, size_t bytes)
{
  const auto offset = static_cast<const uint8_t *>(buffer) - cpu;
  assert(offset >= 0 && size_t(offset) <= sizeof(device) && bytes <= sizeof(device) - offset);
  memcpy(device + offset, buffer, bytes);
}
void cacheInvalidate(const void *buffer, size_t bytes)
{
  const auto offset = static_cast<const uint8_t *>(buffer) - cpu;
  assert(offset >= 0 && size_t(offset) <= sizeof(device) && bytes <= sizeof(device) - offset);
  memcpy(cpu + offset, device + offset, bytes);
}
uint32_t read32(uintptr_t, unsigned reg);
void hostWrite(unsigned reg, uint32_t value);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "../../NativeUfs/RootCommands.h"
#pragma clang diagnostic pop

enum class Fault { None, Timeout, Fatal };

struct Controller {
  std::mutex lock;
  std::condition_variable changed;
  Image durable{};
  std::array<std::optional<Block>, kWindowBlocks + 2> cached{};
  uint32_t doorbells = 0, interrupt = 0;
  unsigned submissions = 0, syncs = 0, fuaWrites = 0, cachedWrites = 0;
  bool pauseData = false, pauseSync = false, syncWaiting = false;
  Fault fault = Fault::None;

  // Called only with no live driver threads, after the previous simulated
  // controller's final DMA. Reset loses cache and leaves durable blocks intact.
  void boot() {
    memset(cpu, 0, sizeof(cpu));
    memset(device, 0, sizeof(device));
    cached = {};
    doorbells = interrupt = submissions = syncs = fuaWrites = cachedWrites = 0;
    pauseData = pauseSync = syncWaiting = false;
    fault = Fault::None;
    gHost = {cpu, kPhys, true, false, 0};
    gStoreOpen = gLogOpen = false;
    gOutstanding = 0;
    gSlotsBusy = gSlotsPeak = 0;
    gSlotWaitHead = gSlotWaitTail = nullptr;
    hostSlotGeometryRegionA(kStages + 1);
    hostSlotsInit(kStages + 1, &slotLock);  // four data slots plus the reserved panic slot
  }

  uint8_t *command(unsigned entry) {
    assert(entry % 2 == 0 && entry / 2 < kStages);
    return device + ufsSlotUcdOffset(entry / 2);
  }

  uint8_t *dmaBuffer(const uint8_t *cmd, uint32_t bytes) {
    const auto *table = cmd + kPrdtOffset;
    uint32_t prd[4];
    memcpy(prd, table, sizeof(prd));
    const uint64_t address = uint64_t(prd[1]) << 32 | prd[0];
    assert(bytes && prd[3] == bytes - 1 && address >= kPhys && address - kPhys < sizeof(device));
    assert(bytes <= sizeof(device) - (address - kPhys));
    return device + (address - kPhys);
  }

  void complete(unsigned entry, uint8_t *cmd) {
    assert(cmd[0] == 1 && cmd[2] == 0 && cmd[3] == entry);
    const auto *cdb = cmd + 16;
    if (cdb[0] == 0x35) {
      // Both provider sync options must reach a whole-LUN, non-immediate sync.
      assert(std::all_of(cdb + 1, cdb + 10, [](uint8_t byte) { return byte == 0; }));
      assert(doorbells == (1U << entry));
      ++syncs;
      for (unsigned i = 0; i < cached.size(); ++i) {
        if (cached[i]) durable[i] = *cached[i];
        cached[i].reset();
      }
    } else {
      const uint64_t first = loadBe32(cdb + 2);
      const unsigned count = unsigned(cdb[7]) << 8 | cdb[8];
      // Independent CDB check. The outer blocks model adjacent Android data.
      assert(count > 0 && first >= kFirst && first + count <= kEnd);
      auto *data = dmaBuffer(cmd, count * kBlock);
      const bool fua = cdb[1] & 8;
      if (cdb[0] == 0x2a) {
        if (fua) ++fuaWrites; else ++cachedWrites;
      } else assert(cdb[0] == 0x28);
      for (unsigned i = 0; i < count; ++i) {
        const auto block = first - kFirst + 1 + i;
        if (cdb[0] == 0x28) {
          const auto &source = cached[block] ? *cached[block] : durable[block];
          memcpy(data + i * kBlock, source.data(), kBlock);
        } else if (fua) {
          memcpy(durable[block].data(), data + i * kBlock, kBlock);
          cached[block].reset();
        } else {
          memcpy(cached[block].emplace().data(), data + i * kBlock, kBlock);
        }
      }
    }
    const uint32_t ocs = 0;
    memcpy(device + entry * 32 + 8, &ocs, sizeof(ocs));
    auto *rsp = cmd + kRspOffset;
    memset(rsp, 0, 512);
    rsp[0] = 0x21;
    rsp[2] = cmd[2];
    rsp[3] = cmd[3];
    doorbells &= ~(1U << entry);
  }
} controller;

uint32_t read32(uintptr_t, unsigned reg)
{
  std::lock_guard held(controller.lock);
  assert(reg == 0x58 || reg == 0x20);
  return reg == 0x58 ? controller.doorbells : controller.interrupt;
}
void hostWrite(unsigned reg, uint32_t value)
{
  auto &hc = controller;
  std::unique_lock held(hc.lock);
  if (reg == 0x20) {
    assert(value == 1);
    hc.interrupt &= ~value;
    return;
  }
  assert(reg == 0x58 && std::has_single_bit(value) && !(hc.doorbells & value));
  hc.doorbells |= value;
  ++hc.submissions;
  const unsigned entry = std::countr_zero(value);
  auto *cmd = hc.command(entry);
  const bool sync = cmd[16] == 0x35;
  if (sync) hc.syncWaiting = true;
  hc.changed.notify_all();
  await(hc.changed, held, [&] { return !(sync ? hc.pauseSync : hc.pauseData); });
  if (sync) hc.syncWaiting = false;
  if (hc.fault == Fault::None) hc.complete(entry, cmd);
  // Keep the doorbell set and OCS invalid in both cases. Only the timed-out
  // thread's clock advances, so another slot cannot inherit its deadline.
  else if (hc.fault == Fault::Timeout) IODelay(2'000'000);
  else hc.interrupt |= 0x20000;
}

struct Env {
  std::mutex gate, commit;
  std::condition_variable changed;
  void lock() { gate.lock(); }
  void unlock() { gate.unlock(); }
  void lockCommit() { commit.lock(); }
  void unlockCommit() { commit.unlock(); }
  // DirectAdmission sleeps with the provider gate dropped.
  void wait() {
    std::unique_lock held(gate, std::adopt_lock);
    assert(changed.wait_for(held, 10s) != std::cv_status::timeout);
    held.release();
  }
  void wake() { changed.notify_all(); }
};

struct Operation {
  bool write;
  uint64_t offset;
  std::vector<uint8_t> bytes;
  unsigned started = 0, retired = 0, callbacks = 0;
  uint64_t actual = 0;

  Operation(bool out, uint64_t at, unsigned size, unsigned pattern = 0) : write(out), offset(at), bytes(size) {
    if (write) {
      std::mt19937 random(pattern);
      std::generate(bytes.begin(), bytes.end(), [&] { return uint8_t(random()); });
    }
  }
  uint64_t read(uint64_t at, void *dst, uint64_t n) {
    assert(at <= bytes.size() && n <= bytes.size() - at);
    memcpy(dst, bytes.data() + at, n);
    return n;
  }
  uint64_t copy(uint64_t at, const void *src, uint64_t n) {
    assert(at <= bytes.size() && n <= bytes.size() - at);
    memcpy(bytes.data() + at, src, n);
    return n;
  }
};

struct Store {
  DirectWriteState &state;
  bool writeRequest = false, writeBack = false, live = false;

  bool read(uint64_t first, uint32_t count, void *data) {
    assert(first < kWindowBlocks && count && count <= kWindowBlocks - first);
    const bool ok = hostRead4k(0, kFirst + first, count, data, &live, !writeRequest) == kIOReturnSuccess;
    if (!ok && (writeRequest || writeBack)) state.fail();
    return ok;
  }
  bool put(uint64_t first, uint32_t count, const void *data, bool fua) {
    assert(first < kWindowBlocks && count && count <= kWindowBlocks - first);
    if (state.failed()) return false;
    if (!fua) state.markCached();
    const bool ok = hostWrite4k(0, kFirst + first, count, data, fua, &live) == kIOReturnSuccess;
    if (!ok) state.fail();
    return ok;
  }
  bool write(uint64_t first, uint32_t count, const void *data) { return put(first, count, data, true); }
  bool writeCached(uint64_t first, uint32_t count, const void *data) { return put(first, count, data, false); }
  bool sync() { return hostSyncCache() == kIOReturnSuccess; }
  void published(uint64_t) {}
};

enum class Sync { Full, Barrier };

struct Disk {
  using Queue = DirectAsync<Operation, void (*)(Operation &, uint64_t)>;
  Env env;
  DirectAdmission admission;
  Queue queue;
  DirectCommitQueue commits;
  DirectWriteState state;
  SeedDisk seed{kPages * kBlock};
  persist::Overlay overlay;
  Block bitmap{};
  const bool writeBack;
  unsigned clock = 0, submitted = 0, retired = 0, completed = 0, noRecord = 0;
  unsigned barriers = 0, fenceWaiters = 0;
  std::atomic<unsigned> syncReturns{0};
  bool ending = false;
  std::vector<std::jthread> workers;

  explicit Disk(persist::Open expected, bool cache = false, bool queued = true) : writeBack(cache) {
    assert(hostStoreOpen(kFirst, kWindowBlocks));
    const uint8_t digest[32] = {0x65};
    assert(kWindowBlocks == persist::windowBlocks(kPages));
    Store store{state};
    assert(overlay.open(store, kPages, digest, false, bitmap.data(), false, 0, kFirst) == expected);
    // data-store-concurrent=2 arms parallel I/O. The live switch opens it
    // after the initial single-gate mount.
    admission.configure(kStages, false);
    assert(admission.beginFence(env) && admission.modeChangeAllowed(true, false));
    admission.parallel = true;
    admission.endFence(env);
    if (!queued) return;
    queue.initialize();
    for (unsigned i = 0; i < kStages; ++i)
      workers.emplace_back([this, i] { work((1U << i) & kWriteStages); });
    workers.emplace_back([this] { complete(); });
  }
  ~Disk() {
    {
      std::lock_guard held(env.gate);
      assert(submitted == retired && retired == completed);
      admission.stop(env);
      ending = true;
      env.wake();
    }
    workers.clear();
  }

  bool enqueue(Operation &op) {
    assert(admission.online && !admission.fenced && !op.bytes.empty() &&
           op.offset < seed.bytes && op.bytes.size() <= seed.bytes - op.offset);
    auto *record = queue.reserve(op.write);
    if (!record) { ++noRecord; return false; }
    static_cast<DirectAdmission::Request &>(*record) = {op.offset, op.bytes.size(), op.write};
    record->buffer = &op;
    record->completion = [](Operation &done, uint64_t actual) {
      ++done.callbacks;
      done.actual = actual;
    };
    ++submitted;
    admission.accept(env, *record);
    return true;
  }

  void submit(Operation &op) {
    std::unique_lock held(env.gate);
    if (admission.fenced) {
      ++admission.waiting;
      env.wake();
      await(env.changed, held, [&] { return !admission.fenced; });
      --admission.waiting;
    }
    assert(enqueue(op));
  }

  void drain() {
    std::unique_lock held(env.gate);
    await(env.changed, held, [&] { return completed == submitted; });
  }

  bool synchronize(Sync kind, unsigned mustRetire) {
    std::unique_lock held(env.gate);
    if (admission.fenced) ++fenceWaiters;
    env.wake();
    assert(admission.beginFence(env));
    assert(!admission.queued && !admission.inflight && !commits.pending && retired >= mustRetire);
    if (kind == Sync::Barrier) ++barriers;
    Store store{state};
    const bool ok = !state.failed() && (!writeBack || state.flush(store));
    ++syncReturns;
    admission.endFence(env);
    return ok;
  }

  // Both call sites enter and leave holding the gate. A returned result has
  // released its page range and stage. The callback can run independently.
  uint64_t execute(std::unique_lock<std::mutex> &held, DirectAdmission::Request &request, int slot, Operation &op) {
    op.started = ++clock;
    held.unlock();
    Store store{state, op.write, writeBack};
    uint64_t actual = 0;
    if (slot >= 0 && (!op.write || !state.failed())) {
      auto *stage = cpu + kDmaBytes + slot * kStageBytes;
      bool storeError = false;
      ConcurrentDirect<Env, Store> fua{overlay, commits, env, store, stage};
      CachedOverwrite<Store> cached{overlay, store};
      auto transfer = [&](auto &policy) {
        return directDiskIOWithPolicy(seed, overlay, store, stage, kStagePages, op.write,
                                      op.offset, op.bytes.size(), op, &storeError, policy);
      };
      actual = writeBack && cached.eligible(request) ? transfer(cached) : transfer(fua);
      assert(storeError == (actual != op.bytes.size()));
    }
    if (writeBack && op.write && actual != op.bytes.size()) state.fail();
    held.lock();
    if (slot >= 0) admission.give(env, slot, store.live);
    op.retired = ++clock;
    ++retired;
    return actual;
  }

  void serveCaller(Operation &op) {
    std::unique_lock held(env.gate);
    DirectAdmission::Request request(op.offset, op.bytes.size(), op.write);
    env.wake();
    const int slot = admission.take(env, request);
    assert(slot >= 0);
    ++submitted;
    const auto actual = execute(held, request, slot, op);
    held.unlock();
    op.actual = actual;
    ++op.callbacks;
    held.lock();
    ++completed;
    env.wake();
  }

  void work(bool write) {
    std::unique_lock held(env.gate);
    while (!ending) {
      DirectAdmission::Request *request;
      const int slot = admission.dispatch(env, write, write ? kWriteStages : kReadStages, request);
      if (!request) { env.wait(); continue; }
      auto &record = *static_cast<Queue::Record *>(request);
      record.actual = execute(held, record, slot, *record.buffer);
      queue.complete(record);
      env.wake();
    }
  }

  void complete() {
    std::unique_lock held(env.gate);
    while (!ending) {
      auto *record = queue.nextCompletion();
      if (!record) { env.wait(); continue; }
      held.unlock();
      record->completion(*record->buffer, record->actual);
      held.lock();
      queue.reclaim(*record);
      ++completed;
      env.wake();
    }
  }
};

void setNeighbors()
{
  controller.durable.front().fill(0xa5);
  controller.durable.back().fill(0x5a);
}

void checkNeighbors()
{
  assert(std::ranges::all_of(controller.durable.front(), [](uint8_t x) { return x == 0xa5; }));
  assert(std::ranges::all_of(controller.durable.back(), [](uint8_t x) { return x == 0x5a; }));
}

void checkWindow()
{
  controller.boot();
  setNeighbors();
  assert(!hostStoreOpen(kFirst, 0));
  assert(!hostStoreOpen(kFirst, kWindowBlocks + 1));  // 32-bit end wraps
  assert(!hostStoreOpen(kFirst, UINT64_MAX));          // 64-bit end wraps
  assert(!hostStoreOpen(UINT64_MAX, 2));
  assert(hostStoreOpen(kFirst, kWindowBlocks));
  assert(!hostLogOpen(kFirst - 1, 2));                 // no second grant across the edge
  std::array<Block, 2> payload{};
  payload[0].fill(0x9a);
  payload[1].fill(0x6b);
  for (const auto lba : {kFirst, kEnd - 1}) {
    assert(hostWrite4k(0, lba, 1, payload.data(), true) == kIOReturnSuccess);
    Block readback{};
    assert(hostRead4k(0, lba, 1, readback.data()) == kIOReturnSuccess && readback == payload[0]);
  }
  assert(hostWrite4k(0, kEnd - 2, 2, payload.data(), true) == kIOReturnSuccess);
  assert(controller.durable[kWindowBlocks] == payload[1]);
  const auto baseline = controller.durable;
  const auto commands = controller.submissions;
  const struct { uint64_t lba; uint32_t count; } rejected[] = {
    {kFirst - 1, 1}, {kFirst - 1, 2}, {kEnd, 1}, {kEnd - 1, 2},
    {kEnd - 1, 3}, {kFirst, UINT32_MAX}, {kFirst + (1ULL << 32), 1},
    {UINT64_MAX, 2}, {UINT64_MAX - 1, 4}, {kFirst, 0}, {kEnd, 0},
  };
  for (const auto [lba, count] : rejected) {
    bool live = true;
    assert(hostWrite4k(0, lba, count, payload.data(), true, &live) == kIOReturnBadArgument && !live);
  }
  assert(hostWrite4k(1, kFirst, 1, payload.data(), true) == kIOReturnBadArgument);
  assert(controller.submissions == commands && controller.durable == baseline);
  checkNeighbors();
}

constexpr unsigned kRecords = Disk::Queue::kRecordsPerDirection;
constexpr unsigned kRounds = 48;
static_assert(2 * kRecords * kRounds >= 5000);

std::vector<Operation> burst(unsigned round, unsigned records = kRecords)
{
  std::vector<Operation> ops;
  ops.reserve(2 * records);
  // Four disjoint, already-present ranges fill every stage and command slot.
  ops.emplace_back(false, 0, 2 * kBlock);
  ops.emplace_back(true, 8 * kBlock, 2 * kBlock, round * 4096 + 1);
  ops.emplace_back(false, 4 * kBlock, kBlock);
  ops.emplace_back(true, 12 * kBlock, kBlock, round * 4096 + 3);
  std::mt19937 random(0xa240 + round);
  for (unsigned i = ops.size(); i < 2 * records; ++i) {
    const unsigned sectors = 1 + random() % 16;
    const unsigned span = i % 3 ? 6 * kBlock : kPages * kBlock;
    const unsigned first = random() % (span / kSector - sectors + 1);
    ops.emplace_back(i % 2, first * kSector, sectors * kSector, round * 4096 + i);
  }
  return ops;
}

unsigned fillQueue(Disk &disk, std::vector<Operation> &ops, bool pauseSync)
{
  {
    std::lock_guard lock(controller.lock);
    controller.pauseData = true;
    controller.pauseSync = pauseSync;
  }
  Operation extraRead(false, 0, kBlock), extraWrite(true, 0, kBlock);
  {
    std::lock_guard held(disk.env.gate);
    for (auto &op : ops) assert(disk.enqueue(op));
    assert(disk.admission.queued == 2 * kRecords);
    assert(!disk.enqueue(extraRead) && !disk.enqueue(extraWrite));
  }
  {
    std::unique_lock lock(controller.lock);
    await(controller.changed, lock, [] { return std::popcount(controller.doorbells) == kStages; });
  }
  std::lock_guard held(disk.env.gate);
  assert(disk.admission.inflight == kStages && disk.admission.queued == 2 * kRecords - kStages);
  return disk.submitted;
}

void releaseData(Fault fault = Fault::None)
{
  std::lock_guard lock(controller.lock);
  controller.fault = fault;
  controller.pauseData = false;
  controller.changed.notify_all();
}

void verifyHistory(const std::vector<Operation> &ops, std::vector<uint8_t> &reference)
{
  // Replay an independent byte-array disk in submission order. Disjoint
  // operations commute. Conflicting page ranges must execute in this order,
  // including read-modify-write requests within a single 4 KiB page.
  for (unsigned i = 0; i < ops.size(); ++i) {
    const auto &op = ops[i];
    assert(op.callbacks == 1 && op.actual == op.bytes.size() && op.started < op.retired);
    const auto begin = reference.begin() + op.offset;
    if (op.write) std::copy(op.bytes.begin(), op.bytes.end(), begin);
    else assert(std::equal(op.bytes.begin(), op.bytes.end(), begin));
    const uint64_t last = (op.offset + op.bytes.size() - 1) / kBlock;
    for (unsigned j = i + 1; j < ops.size(); ++j) {
      const auto &later = ops[j];
      const uint64_t laterLast = (later.offset + later.bytes.size() - 1) / kBlock;
      if ((op.write || later.write) && op.offset / kBlock <= laterLast && later.offset / kBlock <= last)
        assert(op.retired < later.started);
    }
  }
}

void checkDurable(const std::vector<uint8_t> &reference)
{
  for (unsigned page = 0; page < kPages; ++page) {
    const auto &block = controller.durable[1 + persist::pageBlock(kPages, page)];
    assert(std::equal(block.begin(), block.end(), reference.begin() + page * kBlock));
  }
  assert(std::ranges::none_of(controller.cached, [](const auto &block) { return block.has_value(); }));
  checkNeighbors();
}

std::vector<uint8_t> stress(bool writeBack)
{
  controller.durable = {};
  controller.boot();
  setNeighbors();
  Disk disk(persist::Open::Fresh, writeBack);
  std::vector<uint8_t> reference(kPages * kBlock);
  Operation prime(true, 0, 16 * kBlock, 0x789);
  disk.submit(prime);
  disk.drain();
  assert(prime.actual == prime.bytes.size() && prime.callbacks == 1);
  std::copy(prime.bytes.begin(), prime.bytes.end(), reference.begin());
  const auto setupSyncs = controller.syncs;

  for (unsigned round = 0; round < kRounds; ++round) {
    auto ops = burst(round);
    const bool inspectSync = writeBack && round == 0;
    const auto accepted = fillQueue(disk, ops, inspectSync);
    const auto before = disk.syncReturns.load();
    const auto kind = round % 2 ? Sync::Barrier : Sync::Full;
    std::jthread fence([&] { assert(disk.synchronize(kind, accepted)); });
    {
      std::unique_lock held(disk.env.gate);
      await(disk.env.changed, held, [&] { return disk.admission.fenced; });
    }
    // Probe after the fence opens. This checks exact read-after-write and
    // verifies that a later reader stays outside the draining batch.
    Operation probe(false, 0, 6 * kBlock);
    std::jthread later([&] { disk.submit(probe); });
    std::jthread second;
    if (inspectSync) second = std::jthread([&] { assert(disk.synchronize(Sync::Barrier, accepted)); });
    {
      std::unique_lock held(disk.env.gate);
      await(disk.env.changed, held, [&] { return disk.admission.waiting && (!inspectSync || disk.fenceWaiters); });
      assert(disk.admission.inflight == kStages && disk.retired < accepted);
      assert(disk.syncReturns == before && disk.submitted == accepted && !probe.started);
    }
    releaseData();
    if (inspectSync) {
      std::unique_lock held(controller.lock);
      await(controller.changed, held, [] { return controller.syncWaiting; });
      assert(disk.syncReturns == before);  // drained, but the device has not acknowledged SYNCHRONIZE CACHE
      assert(std::popcount(controller.doorbells) == 1);
      controller.pauseSync = false;
      controller.changed.notify_all();
    }
    fence.join();
    if (second.joinable()) second.join();
    later.join();
    disk.drain();
    verifyHistory(ops, reference);
    for (const auto &op : ops) assert(op.retired < probe.started);
    assert(probe.callbacks == 1 && probe.actual == probe.bytes.size());
    assert(std::equal(probe.bytes.begin(), probe.bytes.end(), reference.begin()));
    checkDurable(reference);
  }
  const unsigned extraFence = writeBack ? 1 : 0;
  assert(disk.noRecord == 2 * kRounds && disk.barriers == kRounds / 2 + extraFence);
  assert(disk.syncReturns == kRounds + extraFence && !disk.state.dirty());
  assert(disk.admission.peak == kStages && gSlotsPeak == kStages && controller.fuaWrites);
  if (writeBack) {
    assert(controller.cachedWrites && controller.syncs == setupSyncs + kRounds + extraFence);
  } else {
    assert(!controller.cachedWrites && controller.syncs == setupSyncs);
  }
  printf("PASS %u queued I/Os with four live slots, byte-exact reads and full/barrier drains (%s)\n",
         2 * kRecords * kRounds, writeBack ? "opt-in write-back" : "default write-through");
  return reference;
}

void checkCallerThreads(std::vector<uint8_t> reference)
{
  controller.boot();
  Disk disk(persist::Open::Resumed, false, false);
  auto ops = burst(0, kStages / 2);
  {
    std::lock_guard lock(controller.lock);
    controller.pauseData = true;
  }
  std::vector<std::jthread> callers;
  for (auto &op : ops) callers.emplace_back([&disk, &op] { disk.serveCaller(op); });
  {
    std::unique_lock lock(controller.lock);
    await(controller.changed, lock, [] { return std::popcount(controller.doorbells) == kStages; });
  }
  std::jthread fence([&] { assert(disk.synchronize(Sync::Full, kStages)); });
  {
    std::unique_lock held(disk.env.gate);
    await(disk.env.changed, held, [&] { return disk.admission.fenced; });
  }
  Operation probe(false, 8 * kBlock, 2 * kBlock);
  std::jthread waiter([&] { disk.serveCaller(probe); });
  {
    std::unique_lock held(disk.env.gate);
    await(disk.env.changed, held, [&] { return disk.admission.waiting == 1; });
    assert(disk.admission.inflight == kStages && !disk.retired && !disk.syncReturns);
  }
  releaseData();
  callers.clear();
  fence.join();
  waiter.join();
  verifyHistory(ops, reference);
  assert(disk.admission.peak == kStages && disk.admission.waits && gSlotsPeak == kStages && !controller.syncs);
  assert(probe.callbacks == 1 && probe.actual == probe.bytes.size() && probe.bytes == ops[1].bytes);
  for (const auto &op : ops) assert(op.retired < probe.started);
  checkDurable(reference);
}

void checkFault(Fault fault, bool writeBack, const std::vector<uint8_t> &reference)
{
  controller.boot();
  const auto baseline = controller.durable;
  {
    Disk disk(persist::Open::Resumed, writeBack);
    auto ops = burst(0);
    const auto accepted = fillQueue(disk, ops, false);
    const auto commands = controller.submissions;
    releaseData(fault);
    disk.drain();
    assert(gHost.poisoned && !gHost.ready && disk.state.failed() && disk.state.dirty() == writeBack);
    assert(disk.admission.quarantined == kStageMask && !disk.admission.modeChangeAllowed(true, true));
    assert(controller.submissions == commands && controller.durable == baseline);
    assert(std::popcount(gOutstanding) == kStages && gOutstanding == controller.doorbells);
    for (const auto &op : ops) assert(op.callbacks == 1 && !op.actual);
    assert(!disk.synchronize(Sync::Full, accepted) && !disk.synchronize(Sync::Barrier, accepted));

    // Model late access to every still-owned DMA buffer. No later request
    // may reuse or clean/invalidate one of those stages.
    unsigned lateStages = 0, lateReads = 0;
    for (unsigned entry = 0; entry < 32; ++entry) {
      if (!(controller.doorbells & (1U << entry))) continue;
      const auto *cmd = controller.command(entry);
      const auto bytes = loadBe32(cmd + 12);
      auto *data = controller.dmaBuffer(cmd, bytes);
      const auto offset = data - (device + kDmaBytes);
      assert(offset >= 0 && offset % kStageBytes == 0 && offset / kStageBytes < kStages);
      const auto stage = 1U << (offset / kStageBytes);
      assert(!(lateStages & stage));
      lateStages |= stage;
      if (cmd[16] == 0x28) {
        lateReads |= stage;
        memset(data, 0xde, bytes);
      } else {
        assert(cmd[16] == 0x2a);
        const auto &op = ops[loadBe32(cmd + 18) == kFirst + persist::pageBlock(kPages, 8) ? 1 : 3];
        assert(bytes == op.bytes.size() && std::equal(op.bytes.begin(), op.bytes.end(), data));
      }
    }
    assert(lateStages == kStageMask && lateReads == kReadStages);
    std::vector<uint8_t> lateImage(device + kDmaBytes, device + kDmaSize);
    std::vector<uint8_t> cpuImage(cpu + kDmaBytes, cpu + kDmaSize);
    auto more = burst(1);
    {
      std::lock_guard held(disk.env.gate);
      for (auto &op : more) assert(disk.enqueue(op));
    }
    disk.drain();
    for (const auto &op : more) assert(op.callbacks == 1 && !op.actual);
    bool live = true;
    assert(hostWrite4k(0, kFirst, 1, more[1].bytes.data(), true, &live) != kIOReturnSuccess && !live);
    assert(std::equal(lateImage.begin(), lateImage.end(), device + kDmaBytes));
    assert(std::equal(cpuImage.begin(), cpuImage.end(), cpu + kDmaBytes));
    assert(controller.submissions == commands && controller.durable == baseline);
  }

  // Recovery is a new transport boot after late DMA has stopped.
  controller.boot();
  Disk recovered(persist::Open::Resumed);
  Operation all(false, 0, kPages * kBlock);
  recovered.submit(all);
  recovered.drain();
  assert(all.callbacks == 1 && all.actual == all.bytes.size() && all.bytes == reference);
  assert(!recovered.admission.quarantined && !recovered.state.failed());
  constexpr uint64_t lastPage = (kPages - 1) * kBlock;
  Operation write(true, lastPage, kBlock, 0xab), read(false, lastPage, kBlock);
  recovered.submit(write);
  recovered.submit(read);
  recovered.drain();
  assert(write.callbacks == 1 && write.actual == kBlock && read.callbacks == 1 && read.actual == kBlock);
  assert(read.bytes == write.bytes && !std::equal(write.bytes.begin(), write.bytes.end(), reference.end() - kBlock));
  auto changed = baseline;
  const auto lastBlock = 1 + persist::pageBlock(kPages, kPages - 1);
  std::copy(write.bytes.begin(), write.bytes.end(), changed[lastBlock].begin());
  assert(controller.durable == changed);
  // Restore the reference through the recovered path for the next boot.
  Operation restore(true, lastPage, kBlock);
  std::copy(reference.end() - kBlock, reference.end(), restore.bytes.begin());
  recovered.submit(restore);
  recovered.drain();
  assert(restore.callbacks == 1 && restore.actual == kBlock && controller.durable == baseline);
}

void checkVolatileBoot()
{
  const auto baseline = controller.durable;
  controller.boot();  // data-persist=0 leaves the owner window closed
  constexpr unsigned budget = 4;  // scaled down from 3072 MiB, including the directory leaf
  alignas(kBlock) std::array<uint8_t, budget * kBlock> ram{};
  VolatileChunk chunk{};
  VolatilePool pool;
  pool.budgetPages = budget;
  pool.chunks = &chunk;
  pool.chunkCapacity = 1;
  assert(pool.addChunk(ram.data(), budget, 0));
  uint8_t **directory[1]{};
  SeedDisk seed{kPages * kBlock};
  VolatileDisk disk{seed.bytes, 0, 0, directory};
  struct RamIO {
    VolatilePool &pool;
    VolatileDisk &disk;
    Operation &op;
    uint8_t *allocate() { return pool.take(); }
    uint64_t need() { return volatileNeed(disk, op.offset, op.bytes.size()); }
    void wakeRefill() { assert(false); }  // pool starts with the entire budget
    bool sleep() { assert(false); return false; }
  };
  Operation write(true, 0, 3 * kBlock, 0x81);
  Operation read(false, 0, 3 * kBlock);
  Operation overflow(true, 3 * kBlock, kBlock, 0x82);
  bool noMemory = false;
  RamIO put{pool, disk, write}, get{pool, disk, read}, reject{pool, disk, overflow};
  assert(volatileAdmit(pool, put) == VolatileAdmit::Ok);
  assert(volatileDiskIO(seed, disk, true, 0, write.bytes.size(), write, put, noMemory) == write.bytes.size());
  assert(!noMemory && pool.inUse == budget && disk.resident == 3 * kBlock && disk.leaves == 1);
  assert(volatileAdmit(pool, reject) == VolatileAdmit::NoSpace);
  assert(volatileDiskIO(seed, disk, false, 0, read.bytes.size(), read, get, noMemory) == read.bytes.size());
  assert(!noMemory && read.bytes == write.bytes && pool.peak == budget && !disk.find(3));
  auto give = [&](uint8_t *p) { pool.give(p); };
  assert(volatileUnmap(disk, 0, kBlock, give) == 1);
  assert(volatileAdmit(pool, reject) == VolatileAdmit::Ok);
  assert(volatileDiskIO(seed, disk, true, overflow.offset, kBlock, overflow, reject, noMemory) == kBlock);
  assert(!noMemory && pool.inUse == budget && pool.peak == budget);
  volatileDiskRelease(disk, give);
  assert(!pool.inUse && !disk.resident && !disk.leaves);
  assert(!gStoreOpen && !controller.submissions && controller.durable == baseline);
}
}  // namespace

int main()
{
  checkWindow();
  checkCallerThreads(stress(false));
  const auto reference = stress(true);
  checkFault(Fault::Timeout, true, reference);
  checkFault(Fault::Fatal, false, reference);
  checkVolatileBoot();
  puts("PASS timeout/fatal DMA quarantine, fresh-boot recovery, partition edges and volatile budget");
}
