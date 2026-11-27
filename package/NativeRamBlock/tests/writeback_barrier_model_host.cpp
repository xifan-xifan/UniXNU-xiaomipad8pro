// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: clang++ -std=c++17 -O1 -g -UNDEBUG -Wall -Wextra -Werror -DNATIVE_UFS_SM8750=1 -fsanitize=address,undefined -fno-sanitize-recover=all writeback_barrier_model_host.cpp -o /tmp/writeback_barrier_model_host && /tmp/writeback_barrier_model_host
// Disk M5 offline. The harness models the provider entry points and a UFS
// medium with atomic 4 KiB blocks. The driver headers issue and check the
// commands. APFS ioctl routing and physical UFS power loss require hardware.
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

// The shared headers also define private functions for other driver paths.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../DirectAsync.h"
#include "../VolatileDisk.h"
#include "../VolatilePool.h"
#include "../../NativeUfs/tests/UfsModel.h"
#include "../../NativeUfs/RootWindowRead.h"
#pragma GCC diagnostic pop

namespace {
constexpr unsigned kBlock = persist::kBlock;
constexpr unsigned kPages = 6;
constexpr uint64_t kWindowBlocks = kPages + 2;  // header, bitmap, data
using Block = std::array<uint8_t, kBlock>;
using Image = std::map<uint64_t, Block>;

Block block(uint8_t value)
{
  Block bytes;
  bytes.fill(value);
  return bytes;
}

struct Window {
  uint64_t first, blocks;
  constexpr uint64_t end() const { return first + blocks; }
};
constexpr Window kOwner{0x120, kWindowBlocks}, kScratch{0x280, kWindowBlocks};
constexpr uint64_t kAndroidLbas[] = {kOwner.first - 1, kOwner.end(), kScratch.first - 1, kScratch.end()};

enum class Event {
  Start, Waiting, Cached, PreComplete, PostWaiting, Drained, Sync, SectorStable, SyncReply,
  FullComplete, BarrierComplete, DataFua, BitmapFua, PostComplete
};
struct Cut {
  Event event;
  Image stable;
};

struct Medium {
  Image stable, pending;
  std::vector<Cut> cuts;
  Window writable{};
  bool recording = false, reverseFlush = false;
  unsigned flushLimit = UINT32_MAX, writes = 0;

  void record(Event event) {
    if (recording) cuts.push_back({event, stable});
  }

  void command(uint8_t lun, const uint8_t *cdb, uint8_t *data) {
    assert(lun == 0);
    if (cdb[0] == 0x35) {
      // Whole-LUN, non-immediate SYNCHRONIZE CACHE(10).
      assert(std::all_of(cdb + 1, cdb + 10, [](uint8_t byte) { return byte == 0; }));
      record(Event::Sync);
      for (unsigned flushed = 0; flushed < flushLimit && !pending.empty(); ++flushed) {
        auto next = reverseFlush ? std::prev(pending.end()) : pending.begin();
        stable[next->first] = next->second;
        pending.erase(next);
        record(Event::SectorStable);
      }
      record(Event::SyncReply);
      return;
    }
    const uint64_t lba = loadBe32(cdb + 2);
    const uint32_t count = uint32_t(cdb[7]) << 8 | cdb[8];
    assert(count && count <= kDataMax / kBlock);
    if (cdb[0] == 0x28) {
      static constexpr Block zero{};
      for (uint32_t i = 0; i < count; ++i) {
        const auto cached = pending.find(lba + i), durable = stable.find(lba + i);
        const auto &bytes = cached != pending.end() ? cached->second : durable != stable.end() ? durable->second : zero;
        memcpy(data + i * kBlock, bytes.data(), kBlock);
      }
      return;
    }
    assert(cdb[0] == 0x2a);
    // Independent check on the submitted CDB, including commands during setup.
    assert(lba >= writable.first && lba + count <= writable.end());
    const bool fua = cdb[1] & 8;
    ++writes;
    for (uint32_t i = 0; i < count; ++i) {
      auto &target = fua ? stable : pending;
      memcpy(target[lba + i].data(), data + i * kBlock, kBlock);
      if (fua) pending.erase(lba + i);
      const auto fuaEvent = lba + i == writable.first + 1 ? Event::BitmapFua : Event::DataFua;
      record(fua ? fuaEvent : Event::Cached);
    }
  }

  void reboot() {
    pending.clear();
    writable = {};
    recording = false;
    flushLimit = UINT32_MAX;
    reset();
    m.onBlockCommand = [this](uint8_t lun, const uint8_t *cdb, uint8_t *data) { command(lun, cdb, data); };
  }
};

enum class Persist { Ram = 0, Ufs = 2 };

struct Store {
  Window window;
  Persist mode;
  DirectWriteState state{};

  bool open(Medium &medium) {
    if (mode != Persist::Ufs || !hostStoreOpen(window.first, window.blocks)) return false;
    medium.writable = window;
    return true;
  }

  bool read(uint64_t rel, uint32_t count, void *data) {
    return mode == Persist::Ufs && count && rel < window.blocks && count <= window.blocks - rel &&
           hostRead4k(0, window.first + rel, count, data) == kIOReturnSuccess;
  }

  // The transport accepts a block count. Validate byte lengths before converting
  // in the host adapter. Data disk requests can still cover partial 4 KiB pages.
  bool writeBytes(uint64_t rel, uint64_t bytes, const void *data, bool fua) {
    if (mode != Persist::Ufs || state.failed() || !bytes || bytes % kBlock || rel >= window.blocks ||
        bytes / kBlock > window.blocks - rel) return false;
    if (!fua) state.markCached();
    if (hostWrite4k(0, window.first + rel, static_cast<uint32_t>(bytes / kBlock), data, fua) == kIOReturnSuccess)
      return true;
    state.fail();
    return false;
  }

  bool write(uint64_t rel, uint32_t count, const void *data) {
    return writeBytes(rel, uint64_t(count) * kBlock, data, true);
  }
  bool writeCached(uint64_t rel, uint32_t count, const void *data) {
    return writeBytes(rel, uint64_t(count) * kBlock, data, false);
  }
  bool sync() { return mode == Persist::Ufs && hostSyncCache() == kIOReturnSuccess; }
  void published(uint64_t) {}
};

struct Buffer {
  Block bytes{};
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

// One host thread plays workers at wait points, where the kext releases its gate.
struct Schedule {
  std::function<void()> onWait;
  void wait() { assert(onWait); onWait(); }
  void wake() {}
  void lock() {}
  void unlock() {}
  void lockCommit() {}
  void unlockCommit() {}
};

// Provider sync requests for F_FULLFSYNC and F_BARRIERFSYNC take the same
// whole-device flush path. The latter also sets the barrier option.
enum class Sync { Full, Barrier };

struct Disk {
  Medium &medium;
  Store store;
  SeedDisk seed{kPages * kBlock};
  Block bits{}, stage{};
  persist::Overlay overlay;
  DirectAdmission admission;
  DirectCommitQueue commits;
  Schedule schedule;
  const bool requestWriteBack;
  bool writeBack = false;

  Disk(Medium &device, Window window, bool cache = false, Persist mode = Persist::Ufs)
    : medium(device), store{window, mode}, requestWriteBack(cache) {}

  persist::Open open(bool startParallel = true) {
    if (!store.open(medium)) return persist::Open::Failed;
    assert(store.window.blocks == persist::windowBlocks(kPages));
    const uint8_t digest[32] = {0x35};
    const auto result = overlay.open(store, kPages, digest, false, bits.data(), false, 0, store.window.first);
    if (result == persist::Open::Fresh || result == persist::Open::Resumed) {
      admission.configure(2, startParallel);
      writeBack = requestWriteBack;
    }
    return result;
  }

  bool transfer(DirectAdmission::Request &request, Buffer &buffer) {
    const auto offset = request.first * kBlock;
    const auto length = (request.last - request.first + 1) * kBlock;
    bool error = false;
    if (request.write && store.state.failed()) return false;
    CachedOverwrite<Store> cached{overlay, store};
    ConcurrentDirect<Schedule, Store> fua{overlay, commits, schedule, store, stage.data()};
    // A single stage is enough for these one-block commands.
    const auto serve = [&](auto &policy) {
      return directDiskIOWithPolicy(seed, overlay, store, stage.data(), 1, request.write, offset, length, buffer, &error, policy);
    };
    uint64_t actual;
    if (writeBack && cached.eligible(request)) actual = serve(cached);
    else if (admission.parallel) actual = serve(fua);
    else actual = directDiskIO(seed, overlay, store, stage.data(), 1, request.write, offset, length, buffer, &error);
    return !error && actual == length;
  }

  bool write(unsigned page, uint8_t value) {
    DirectAdmission::Request request(page * kBlock, kBlock, true);
    Buffer buffer{block(value)};
    return transfer(request, buffer);
  }

  Block read(unsigned page) {
    DirectAdmission::Request request(page * kBlock, kBlock, false);
    Buffer buffer;
    assert(transfer(request, buffer));
    return buffer.bytes;
  }

  bool synchronize(Sync kind) {
    return admission.beginFence(schedule) && finishFence(kind);
  }

  bool enableConcurrent() {
    if (!admission.beginFence(schedule)) return false;
    const bool ok = !store.state.failed() && admission.modeChangeAllowed(true, false) &&
                    (admission.parallel || !writeBack || !store.state.dirty() || store.state.flush(store));
    if (ok) admission.parallel = true;
    admission.endFence(schedule);
    return ok;
  }

  bool finishFence(Sync kind) {
    assert(admission.fenced && !admission.inflight && !admission.queued);
    medium.record(Event::Drained);
    const bool ok = !store.state.failed() && (!writeBack || admission.held || store.state.flush(store));
    admission.endFence(schedule);
    if (ok) medium.record(kind == Sync::Full ? Event::FullComplete : Event::BarrierComplete);
    return ok;
  }
};

void checkBarrier(Sync kind, bool reverse)
{
  Medium medium;
  medium.reboot();
  Disk disk(medium, kScratch, true);
  assert(disk.open() == persist::Open::Fresh);
  // First-touch writes establish two durable pages. The post-barrier write
  // uses an absent page so recovery must follow its FUA bitmap publication.
  assert(disk.write(0, 0x10) && disk.write(1, 0x11));
  assert(!disk.store.state.dirty() && medium.pending.empty() && !disk.overlay.has(2));
  medium.reverseFlush = reverse;
  medium.recording = true;
  medium.record(Event::Start);

  DirectAdmission::Request first(0, kBlock, true), second(kBlock, kBlock, true);
  auto &admission = disk.admission;
  auto &schedule = disk.schedule;
  admission.accept(schedule, first);
  DirectAdmission::Request *request = nullptr;
  const int firstSlot = admission.dispatch(schedule, true, 3, request);
  assert(firstSlot >= 0 && request == &first);
  admission.accept(schedule, second);
  unsigned waits = 0;
  schedule.onWait = [&] {
    assert(admission.fenced);
    medium.record(Event::Waiting);
    if (waits++ == 0) {
      assert(admission.inflight == 1 && admission.queued == 1);
      Buffer buffer{block(0x20)};
      assert(disk.transfer(first, buffer));
      admission.give(schedule, firstSlot, false);
    } else {
      assert(waits == 2 && admission.inflight == 0 && admission.queued == 1);
      const int slot = admission.dispatch(schedule, true, 3, request);
      assert(slot >= 0 && request == &second);
      Buffer buffer{block(0x21)};
      assert(disk.transfer(second, buffer));
      admission.give(schedule, slot, false);
    }
    assert(disk.store.state.dirty() && medium.pending.size() == waits);
    assert(disk.read(waits - 1) == block(0x1f + waits));
    medium.record(Event::PreComplete);
  };

  assert(admission.beginFence(schedule) && waits == 2);
  // A later caller must wait for the flush even though the stages are idle.
  schedule.onWait = [&] {
    assert(admission.fenced && admission.waiting == 1 && !admission.inflight);
    assert(disk.store.state.dirty() && medium.pending.size() == 2);
    medium.record(Event::PostWaiting);
    assert(disk.finishFence(kind));
  };
  DirectAdmission::Request post(2 * kBlock, kBlock, true);
  const int postSlot = admission.take(schedule, post);
  assert(postSlot >= 0 && !admission.fenced && !disk.store.state.dirty() && medium.pending.empty());
  Buffer postBuffer{block(0x30)};
  assert(disk.transfer(post, postBuffer));
  admission.give(schedule, postSlot, false);
  medium.record(Event::PostComplete);

  const Event completed = kind == Sync::Full ? Event::FullComplete : Event::BarrierComplete;
  const unsigned firstStable = reverse ? 2 : 1;
  const struct { Event event; unsigned flushed; bool checkpoint; } expected[] = {
    {Event::Start, 0, false},
    {Event::Waiting, 0, false}, {Event::Cached, 0, false}, {Event::PreComplete, 0, false},
    {Event::Waiting, 0, false}, {Event::Cached, 0, false}, {Event::PreComplete, 0, false},
    {Event::PostWaiting, 0, false}, {Event::Drained, 0, false}, {Event::Sync, 0, false},
    {Event::SectorStable, firstStable, false}, {Event::SectorStable, 3, false},
    {Event::SyncReply, 3, false}, {completed, 3, false}, {Event::DataFua, 3, false},
    {Event::BitmapFua, 3, true}, {Event::PostComplete, 3, true},
  };
  assert(medium.cuts.size() == std::size(expected));
  for (unsigned i = 0; i < std::size(expected); ++i) {
    // Lose the controller cache, the live bitmap, and the admission state.
    medium.reboot();
    medium.stable = medium.cuts[i].stable;
    assert(medium.cuts[i].event == expected[i].event);
    const auto writes = medium.writes;
    Disk recovered(medium, kScratch);
    assert(recovered.open() == persist::Open::Resumed);
    assert(recovered.overlay.generation == 1 && recovered.overlay.boundLba == kScratch.first);
    assert(recovered.overlay.present == (expected[i].checkpoint ? 3 : 2));
    assert(recovered.read(0) == block(expected[i].flushed & 1 ? 0x20 : 0x10));
    assert(recovered.read(1) == block(expected[i].flushed & 2 ? 0x21 : 0x11));
    assert(recovered.read(2) == block(expected[i].checkpoint ? 0x30 : 0));
    assert(medium.writes == writes);
  }
}

void checkSyncFailure(Sync kind)
{
  Medium medium;
  medium.reboot();
  Disk disk(medium, kScratch, true);
  assert(disk.open() == persist::Open::Fresh);
  assert(disk.write(0, 0x10) && disk.write(1, 0x11));
  assert(disk.write(0, 0x20) && disk.write(1, 0x21));
  medium.flushLimit = 1;
  m.fault = Fault::SyncCheck;
  const auto syncs = m.syncs;
  assert(!disk.synchronize(kind));
  assert(disk.store.state.dirty() && disk.store.state.failed() && medium.pending.size() == 1);
  assert(!disk.admission.fenced && m.syncs == syncs + 1);
  const auto writes = medium.writes;
  assert(!disk.write(2, 0x30) && !disk.synchronize(kind));
  assert(medium.writes == writes && m.syncs == syncs + 1);
  medium.reboot();
  Disk recovered(medium, kScratch);
  assert(recovered.open() == persist::Open::Resumed);
  assert(recovered.read(0) == block(0x20) && recovered.read(1) == block(0x11));
  assert(!recovered.overlay.has(2) && recovered.read(2) == block(0));
}

void checkDailyModeSwitch(bool failSync)
{
  Medium medium;
  medium.reboot();
  Disk initial(medium, kOwner);
  assert(initial.open() == persist::Open::Fresh && initial.write(0, 0x10));
  medium.reboot();
  Disk daily(medium, kOwner, true);
  assert(daily.open(false) == persist::Open::Resumed && !daily.admission.parallel && daily.writeBack);
  const uint64_t lba = kOwner.first + persist::pageBlock(kPages, 0);
  DirectAdmission::Request overwrite(0, kBlock, true);
  assert(daily.admission.take(daily.schedule, overwrite) == DirectAdmission::kSerial);
  Buffer beforeMount{block(0x20)};
  assert(daily.transfer(overwrite, beforeMount) && daily.store.state.dirty());
  assert(medium.stable.at(lba) == block(0x10) && medium.pending.at(lba) == beforeMount.bytes);

  if (failSync) {
    m.fault = Fault::SyncCheck;
    medium.flushLimit = 0;
  }
  const auto syncs = m.syncs;
  assert(daily.enableConcurrent() == !failSync);
  assert(m.syncs == syncs + 1 && !daily.admission.fenced && daily.admission.parallel == !failSync);
  if (failSync) {
    assert(daily.store.state.failed() && daily.store.state.dirty());
    assert(medium.stable.at(lba) == block(0x10));
    assert(!daily.enableConcurrent() && !daily.write(1, 0x30) && m.syncs == syncs + 1);
    return;
  }

  assert(!daily.store.state.dirty() && medium.pending.empty() && medium.stable.at(lba) == beforeMount.bytes);
  const int slot = daily.admission.take(daily.schedule, overwrite);
  assert(slot >= 0 && daily.admission.inflight == 1);
  Buffer afterMount{block(0x30)};
  assert(daily.transfer(overwrite, afterMount) && daily.store.state.dirty());
  daily.admission.give(daily.schedule, slot, false);
  assert(daily.read(0) == afterMount.bytes && medium.stable.at(lba) == beforeMount.bytes);
  assert(daily.enableConcurrent() && daily.store.state.dirty() && m.syncs == syncs + 1);
  assert(daily.synchronize(Sync::Barrier) && !daily.store.state.dirty());
  medium.reboot();
  Disk resumed(medium, kOwner);
  assert(resumed.open() == persist::Open::Resumed && resumed.read(0) == afterMount.bytes);
}

void checkWindow(Window window, Window other)
{
  Medium medium;
  for (const auto lba : kAndroidLbas) medium.stable[lba] = block(0xad);
  medium.stable[other.first] = block(0xc1);
  medium.reboot();
  const auto protectedImage = medium.stable;
  assert(!hostStoreOpen(UINT64_MAX - 1, 3));
  assert(!hostStoreOpen(window.first, 0));
  assert(!hostStoreOpen(UINT32_MAX - 1, 3));
  Store store{window, Persist::Ufs};
  assert(store.open(medium));
  assert(!hostLogOpen(window.first - 1, 2));
  assert(!hostLogOpen(window.end() - 1, 2));
  assert(!hostStoreOpen(other.first, other.blocks));

  const auto payload = block(0x54);
  for (const bool fua : {false, true}) {
    const auto before = m.doorbells;
    const struct { uint64_t lba; uint32_t count; uint8_t lun = 0; } invalid[] = {
      {window.first - 1, 1}, {window.first - 1, 2}, {window.end(), 1}, {window.end() - 1, 2},
      {UINT64_MAX - 1, 3}, {UINT32_MAX, 2}, {window.first, UINT32_MAX},
      {window.first, 0}, {window.first, 1, 1}, {other.first, 1},
    };
    for (const auto &request : invalid)
      assert(hostWrite4k(request.lun, request.lba, request.count, payload.data(), fua) == kIOReturnBadArgument);
    assert(hostWrite4k(0, window.first, 1, nullptr, fua) == kIOReturnBadArgument);
    for (const uint64_t bytes : {0U, 1U, kBlock - 512, kBlock + 512, 2 * kBlock - 1})
      assert(!store.writeBytes(0, bytes, payload.data(), fua));
    assert(!store.writeBytes(UINT64_MAX - 1, 3 * kBlock, payload.data(), fua));
    assert(!store.writeBytes(window.blocks - 1, 2 * kBlock, payload.data(), fua));
    assert(m.doorbells == before && medium.writes == 0 && medium.stable == protectedImage && medium.pending.empty());
  }
  assert(store.write(0, 1, payload.data()));
  assert(store.writeCached(window.blocks - 1, 1, payload.data()));
  assert(store.state.flush(store));
  auto expected = protectedImage;
  expected[window.first] = expected[window.end() - 1] = payload;
  assert(medium.stable == expected && medium.pending.empty() && medium.writes == 2);
}

void checkQueue()
{
  using Queue = DirectAsync<unsigned, std::function<void(int, uint64_t)>>;
  constexpr unsigned perDirection = Queue::kRecordsPerDirection;
  static_assert(perDirection == 64);
  Queue queue;
  queue.initialize();
  DirectAdmission admission;
  admission.configure(DirectAdmission::kDepth, true);
  Schedule schedule;
  std::array<unsigned, 2 * perDirection> buffers{};
  const unsigned stageMask = (1U << admission.depth) - 1;
  for (unsigned round = 0; round < 2; ++round) {
    std::array<unsigned, 2 * perDirection> calls{};
    for (unsigned id = 0; id < buffers.size(); ++id) {
      const bool write = id % 2;
      auto *record = queue.reserve(write);
      assert(record && record->write == write);
      static_cast<DirectAdmission::Request &>(*record) = DirectAdmission::Request(id * kBlock, kBlock, write);
      record->buffer = &buffers[id];
      record->completion = [&, id](int status, uint64_t actual) {
        assert(++calls[id] == 1);
        assert(status == (id % 7 == 0 ? -1 : 0) && actual == (status ? 0 : kBlock));
      };
      admission.accept(schedule, *record);
    }
    assert(admission.queued == 2 * perDirection && !queue.reserve(false) && !queue.reserve(true));

    // Workers fill every stage before any finish. Completion delivery is paused
    // until all accepted requests finish, so callback backlog pins the records.
    while (admission.queued) {
      std::array<Queue::Record *, DirectAdmission::kDepth> active{};
      for (unsigned i = 0; i < active.size(); ++i) {
        DirectAdmission::Request *request = nullptr;
        const int slot = admission.dispatch(schedule, i % 2, stageMask, request);
        assert(slot >= 0 && unsigned(slot) < active.size() && !active[slot] && request);
        active[slot] = static_cast<Queue::Record *>(request);
      }
      assert(admission.inflight == active.size());
      DirectAdmission::Request *extra = nullptr;
      const auto queued = admission.queued;
      assert(admission.dispatch(schedule, true, stageMask, extra) == DirectAdmission::kNoStage && !extra);
      assert(admission.queued == queued);
      for (unsigned slot = 0; slot < active.size(); ++slot) {
        auto &record = *active[slot];
        const auto id = record.first;
        assert(id < buffers.size() && record.buffer == &buffers[id]);
        record.status = id % 7 == 0 ? -1 : 0;
        record.actual = record.status ? 0 : kBlock;
        admission.give(schedule, slot, false);
        queue.complete(record);
      }
      assert(!queue.reserve(false) && !queue.reserve(true));
    }

    for (unsigned i = 0; i < buffers.size(); ++i) {
      auto *record = queue.nextCompletion();
      assert(record);
      // Even a callback currently being delivered still owns its record.
      if (i == 0) assert(!queue.reserve(record->write));
      record->completion(record->status, record->actual);
      queue.reclaim(*record);
    }
    assert(!queue.nextCompletion() && !admission.queued && !admission.inflight && admission.peak == admission.depth);
    for (const unsigned count : calls) assert(count == 1);
  }
}

void checkVolatileAndPersistent()
{
  Medium medium;
  medium.reboot();
  Disk persistent(medium, kOwner);  // data-persist=2, write-back absent
  assert(persistent.open() == persist::Open::Fresh && persistent.write(0, 0x72));
  const auto syncs = m.syncs;
  const auto writes = medium.writes;
  assert(persistent.write(0, 0x73) && medium.writes == writes + 1);  // FUA overwrite
  assert(persistent.synchronize(Sync::Full) && persistent.synchronize(Sync::Barrier));
  assert(m.syncs == syncs && !persistent.store.state.dirty() && medium.pending.empty());
  const auto ownerImage = medium.stable;

  medium.reboot();
  Disk ram(medium, kOwner, true, Persist::Ram);  // Carry the disk flags into data-persist=0.
  assert(ram.open(false) == persist::Open::Failed && !gStoreOpen);
  assert(!ram.writeBack && !ram.enableConcurrent() && !ram.admission.parallel && !ram.admission.depth);
  const auto readBack = block(0x90);
  assert(!ram.store.write(0, 1, readBack.data()) && !ram.store.writeCached(0, 1, readBack.data()) && !ram.store.sync());
  auto ramBoot = [&](bool afterReset) {
    // Three budgeted pages, only two ready on boot. The first write takes
    // one leaf and one data page. Its seed is shared with the persistent boot.
    alignas(kBlock) std::array<uint8_t, 3 * kBlock> storage{};
    VolatileChunk chunks[2]{};
    VolatilePool pool;
    pool.budgetPages = storage.size() / kBlock;
    pool.chunks = chunks;
    pool.chunkCapacity = std::size(chunks);
    assert(pool.addChunk(storage.data(), 2, 0));
    uint8_t **directory[1]{};
    VolatileDisk disk{persistent.seed.bytes, 0, 0, directory};
    struct Allocator {
      VolatilePool &pool;
      uint8_t *allocate() { return pool.take(); }
    } allocator{pool};
    auto transfer = [&](bool write, uint64_t offset, uint64_t bytes, Buffer &buffer) {
      bool noMemory = false;
      const auto actual = volatileDiskIO(persistent.seed, disk, write, offset, bytes, buffer, allocator, noMemory);
      assert(!noMemory && actual == bytes);
    };
    auto read = [&](uint64_t page) {
      Buffer output{};
      transfer(false, page * kBlock, kBlock, output);
      return output.bytes;
    };
    assert(read(0) == block(0) && !disk.resident && !pool.inUse);
    if (afterReset) return;

    unsigned sleeps = 0;
    struct Admit {
      VolatileDisk &disk;
      uint64_t offset, bytes;
      unsigned &sleeps;
      uint64_t need() { return volatileNeed(disk, offset, bytes); }
      void wakeRefill() {}
      bool sleep() { ++sleeps; return false; }
    };
    auto write = [&](uint64_t offset, uint64_t length, Buffer &buffer) {
      Admit env{disk, offset, length, sleeps};
      const auto result = volatileAdmit(pool, env);
      if (result == VolatileAdmit::Ok) transfer(true, offset, length, buffer);
      return result;
    };

    Buffer input{readBack};
    assert(write(0, kBlock, input) == VolatileAdmit::Ok && read(0) == readBack);
    assert(disk.resident == kBlock && disk.leaves == 1 && pool.inUse == 2 && pool.heldBytes() == 2 * kBlock);

    // The same failed request could modify its first page if it entered the
    // I/O loop. Admission refuses the whole write before it can change RAM.
    Buffer other{block(0x91)};
    for (unsigned retry = 0; retry < 2; ++retry) {
      assert(write(0, 3 * kBlock, other) == VolatileAdmit::NoSpace);
      assert(write(kBlock, kBlock, other) == VolatileAdmit::NoMemory);
      assert(read(0) == readBack && read(1) == block(0) && read(2) == block(0));
      assert(disk.resident == kBlock && pool.inUse == 2 && pool.heldBytes() == 2 * kBlock && !pool.wanted);
    }
    assert(sleeps == 2 && pool.refusedNoSpace == 2 && pool.refusedNoMemory == 2);
    assert(pool.addChunk(storage.data() + 2 * kBlock, 1, 0));
    assert(!pool.addChunk(storage.data(), 1, 0));
    assert(write(kBlock, kBlock, other) == VolatileAdmit::Ok);
    assert(disk.resident == 2 * kBlock && pool.inUse == 3 && pool.peak == 3 && pool.heldBytes() == 3 * kBlock);

    // The RAM disk returns bytes. Its verifier is the caller: a mismatch must
    // fail verification without modifying the RAM page or the persistent store.
    auto verify = [&](const Block &expected) { return read(0) == expected; };
    Block bad = readBack;
    bad.back() ^= 1;
    assert(verify(readBack) && !verify(bad) && verify(readBack) && read(1) == other.bytes);
  };
  ramBoot(false);
  ramBoot(true);
  assert(m.doorbells == 0 && m.syncs == 0 && medium.writes == writes + 1 && medium.stable == ownerImage);

  medium.reboot();
  Disk recovered(medium, kOwner);
  assert(recovered.open() == persist::Open::Resumed);
  assert(recovered.read(0) == block(0x73) && recovered.read(1) == block(0));
  assert(medium.stable == ownerImage);
}

void checkVerifiedRead()
{
  Medium medium;
  medium.reboot();
  Store store{kOwner, Persist::Ufs};
  assert(store.open(medium));
  const auto written = block(0x67);
  assert(store.write(0, 1, written.data()));

  struct Verification {
    Block expected, out;
    bool locked = false;
    unsigned copies = 0;
    void lock() { assert(!locked); locked = true; }
    void unlock() { assert(locked); locked = false; }
    IOReturn read(uint64_t lba, uint32_t count, void *bounce) {
      assert(locked);
      return hostRead4k(0, lba, count, bounce);
    }
    IOReturn verify(uint64_t index, const void *bytes, size_t size) {
      assert(locked && index == 0 && size == kBlock);
      return memcmp(bytes, expected.data(), size) ? kIOReturnIOError : kIOReturnSuccess;
    }
    uint64_t copy(uint64_t offset, const void *bytes, uint64_t size) {
      assert(locked && offset == 0 && size == 512);
      ++copies;
      memcpy(out.data(), bytes, size);
      return size;
    }
  } verify{written, block(0xa5)};
  Block bounce{};
  auto read = [&] {
    return ufsReadWindow(verify, kOwner.first * kBlock, kBlock, 0, 1, bounce.data(), kBlock);
  };
  // Corrupt a byte outside the requested sector. The whole 4 KiB bounce
  // must be verified before the requested bytes become visible.
  medium.stable[kOwner.first].back() ^= 1;
  const auto failed = read();
  assert(failed.status == kIOReturnIOError && failed.done == 0 && !verify.locked && !verify.copies);
  assert(verify.out == block(0xa5));
  medium.stable[kOwner.first] = written;
  const auto recovered = read();
  assert(recovered.status == kIOReturnSuccess && recovered.done == 512 && !verify.locked && verify.copies == 1);
  assert(memcmp(verify.out.data(), written.data(), 512) == 0 && medium.writes == 1);
}

// Boot-argument parsing lives in the two kernel translation units. Guard the
// production defaults separately from the modeled dispatch and media tests.
std::string source(const std::filesystem::path &path)
{
  std::ifstream input(path);
  assert(input);
  std::string text{std::istreambuf_iterator<char>(input), {}};
  text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }), text.end());
  return text;
}

void checkBootDefaults()
{
  const auto dir = std::filesystem::path(__FILE__).parent_path();
  const auto ram = source(dir / "../NativeRamBlock.cpp");
  assert(ram.find("writeBack=false;") != std::string::npos);
  const auto start = ram.find("voidNativeRamBlock::configureDirect(){");
  const auto end = ram.find("voidNativeRamBlock::configureAsync(){", start);
  assert(start != std::string::npos && end != std::string::npos);
  const auto setup = ram.substr(start, end - start);
  assert(setup.find("voidNativeRamBlock::configureDirect(){if(!direct)return;") == 0);
  assert(ram.find("if(commitGate)install(modeOid,") != std::string::npos);
  assert(setup.find("cache=0;") != std::string::npos);
  assert(setup.find(R"(PE_parse_boot_argn("data-store-write-back",&cache,sizeofcache);)") != std::string::npos);
  assert(setup.find("writeBack=cache==1;") != std::string::npos);
  assert(setup.find(R"(queue=0;if(PE_parse_boot_argn("data-store-queue",&queue,sizeofqueue)&&queue==1)configureAsync();)") != std::string::npos);
  const auto ufs = source(dir / "../../NativeUfs/NativeUfs.cpp");
  assert(ufs.find(R"(value=0;if(!PE_parse_boot_argn("data-persist",&value,sizeof(value))||value>2)value=0;)") != std::string::npos);
}
}  // namespace

int main()
{
  checkBootDefaults();
  for (const auto sync : {Sync::Full, Sync::Barrier}) {
    checkBarrier(sync, false);
    checkBarrier(sync, true);
    checkSyncFailure(sync);
  }
  checkQueue();
  checkDailyModeSwitch(false);
  checkDailyModeSwitch(true);
  checkWindow(kOwner, kScratch);
  checkWindow(kScratch, kOwner);
  checkVolatileAndPersistent();
  checkVerifiedRead();
  puts("PASS Disk M5 offline: provider barriers, crash recovery, queue completions, windows and volatile isolation");
}
