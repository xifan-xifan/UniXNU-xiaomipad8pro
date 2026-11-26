// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../ConcurrentDirect.h"
#include "../PowerHold.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
constexpr uint64_t kBoundary = 8 * persist::kBlock;
constexpr uint64_t kPages = kBoundary + 32;
constexpr uint64_t kLargePages = 200ULL * 1024 * 1024 * 1024 / persist::kBlock;
constexpr unsigned kBlock = persist::kBlock;
using Block = std::array<uint8_t, kBlock>;

template<class Test> static void until(Test test)
{
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!test()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
}

struct Env {
  std::mutex gate, commit;
  std::condition_variable changed;
  void lock() { gate.lock(); }
  void unlock() { gate.unlock(); }
  void wait() {
    std::unique_lock<std::mutex> held(gate, std::adopt_lock);
    changed.wait(held);
    held.release();
  }
  void wake() { changed.notify_all(); }
  void lockCommit() { commit.lock(); }
  void unlockCommit() { commit.unlock(); }
};

struct Medium {
  using Image = std::map<uint64_t, Block>;
  const uint64_t pages;
  explicit Medium(uint64_t pageCount = kPages) : pages(pageCount) {}
  Image durable, cache;
  std::mutex mutex;
  std::condition_variable changed;
  unsigned dataIn = 0, readIn = 0, bitmapIn = 0, syncs = 0, vetoes = 0;
  bool holdData = false, holdRead = false, holdBitmap = false, checkCuts = false, syncOk = true;
  uint64_t failBlock = UINT64_MAX, lateBlock = UINT64_MAX, tornBlock = UINT64_MAX;
  const void *lateDMA = nullptr;
  std::map<const void *, uint64_t> active;
  std::vector<uint64_t> publications;
  uint64_t cuts = 0;

  static void valid(const Image &image, uint64_t pages = kPages) {
    for (const auto &entry : image) {
      if (!entry.first || entry.first > persist::bitmapBlocks(pages)) continue;
      const uint64_t base = (entry.first - 1) * kBoundary;
      for (unsigned b = 0; b < kBlock; ++b)
        for (unsigned i = 0; entry.second[b] && i < 8; ++i)
          if (entry.second[b] & (1U << i)) {
            assert(base + 8 * b + i < pages);
            assert(image.count(persist::pageBlock(pages, base + 8 * b + i)));
          }
    }
  }
  void cut() { if (checkCuts) { valid(durable, pages); ++cuts; } }
  bool read(uint64_t block, uint32_t n, void *dst, bool &live) {
    std::unique_lock<std::mutex> lock(mutex);
    live = false;
    if (block > persist::bitmapBlocks(pages)) {
      ++readIn;
      assert(active.emplace(dst, block).second);
      changed.notify_all();
      changed.wait(lock, [&] { return !holdRead; });
      active.erase(dst);
      --readIn;
      if (block == failBlock) return false;
    }
    for (unsigned i = 0; i < n; ++i) {
      auto *out = static_cast<uint8_t *>(dst) + i * kBlock;
      const auto pos = durable.find(block + i);
      if (pos == durable.end()) memset(out, 0, kBlock);
      else memcpy(out, pos->second.data(), kBlock);
    }
    return true;
  }
  bool write(uint64_t block, uint32_t n, const void *src, bool &live, bool fua = true) {
    std::unique_lock<std::mutex> lock(mutex);
    live = false;
    const bool isData = block > persist::bitmapBlocks(pages);
    cut();   // submission, possibly before any other completed command
    if (isData) {
      ++dataIn;
      assert(active.emplace(src, block).second);
      changed.notify_all();
      changed.wait(lock, [&] { return !holdData; });
      active.erase(src);
      --dataIn;
    } else if (block && checkCuts) {
      ++bitmapIn;
      changed.notify_all();
      changed.wait(lock, [&] { return !holdBitmap; });
      --bitmapIn;
    }
    if (block == failBlock || block == lateBlock) {
      live = block == lateBlock;
      if (live) lateDMA = src;
      cut();
      return false;
    }
    for (unsigned i = 0; i < n; ++i) {
      auto &out = (fua ? durable : cache)[block + i];
      const auto *in = static_cast<const uint8_t *>(src) + i * kBlock;
      if (block && !isData && checkCuts) {
        // A torn sector/block can contain either the old or new value in each byte.
        // Exercise every prefix; a merge only adds bits naming completed data.
        const auto old = out;
        for (unsigned b = 0; b < kBlock; ++b) {
          if (out[b] == in[b]) continue;
          assert((out[b] & in[b]) == out[b]);
          out[b] = in[b];
          cut();
          if (block == tornBlock) return false;
        }
        out = old;
        publications.push_back(block + i);
      }
      memcpy(out.data(), in, kBlock);
    }
    cut();   // command completed
    return true;
  }
  bool sync() {
    std::lock_guard<std::mutex> lock(mutex);
    assert(!dataIn && !bitmapIn && !readIn);
    ++syncs;
    if (!syncOk) return false;
    durable.merge(cache);
    for (const auto &entry : cache) durable[entry.first] = entry.second;
    cache.clear();
    return true;
  }
  void veto(const char *) { ++vetoes; }
  void hold(bool &flag, bool value) {
    std::lock_guard<std::mutex> lock(mutex);
    flag = value;
    changed.notify_all();
  }
  unsigned pending(const unsigned &n) {
    std::lock_guard<std::mutex> lock(mutex);
    return n;
  }
};

struct Store {
  Medium &medium;
  DirectWriteState &state;
  bool writeRequest, liveDMA = false;
  uint64_t publications = 0;
  bool read(uint64_t block, uint32_t n, void *dst) {
    const bool ok = medium.read(block, n, dst, liveDMA);
    if (!ok && writeRequest) state.fail();
    return ok;
  }
  bool write(uint64_t block, uint32_t n, const void *src) {
    if (state.failed()) return false;
    const bool ok = medium.write(block, n, src, liveDMA);
    if (!ok) state.fail();
    return ok;
  }
  bool sync() { return medium.sync(); }
  void published(uint64_t) { ++publications; }
};

struct Seed {
  uint64_t bytes = kPages * kBlock;
  Block first;
  Seed() { first.fill(0x11); }
  const uint8_t *find(uint64_t i) const { return i == 0 ? first.data() : nullptr; }
};
struct Buffer {
  std::vector<uint8_t> data;
  uint64_t shortAt = UINT64_MAX;
  explicit Buffer(unsigned n, uint8_t value = 0) : data(n, value) {}
  uint64_t read(uint64_t at, void *to, uint64_t n) {
    if (shortAt <= at) return 0;
    memcpy(to, data.data() + at, n);
    return n;
  }
  uint64_t copy(uint64_t at, const void *from, uint64_t n) {
    if (shortAt <= at) return 0;
    memcpy(data.data() + at, from, n);
    return n;
  }
};

struct Disk {
  Medium medium;
  Seed seed;
  persist::Overlay overlay;
  DirectWriteState state;
  DirectAdmission admission;
  DirectCommitQueue commits;
  Env env;
  std::vector<uint8_t> bits;
  std::array<std::vector<uint8_t>, DirectAdmission::kDepth> stages;
  const uint8_t digest[32] = {1};
  std::atomic<unsigned> callbacks{0}, bitmapWrites{0};

  explicit Disk(unsigned depth = 8, uint64_t pages = kPages, bool startParallel = true)
    : medium(pages), bits(persist::bitmapBlocks(pages) * kBlock) {
    seed.bytes = pages * kBlock;
    Store store{medium, state, false};
    assert(overlay.open(store, pages, digest, false, bits.data(), false, 0, 1000) == persist::Open::Fresh);
    admission.configure(depth, startParallel);
    for (unsigned i = 0; i < depth; ++i) stages[i].resize(32 * kBlock);
    medium.checkCuts = true;
  }

  int64_t io(bool write, uint64_t offset, Buffer &buf, bool *failed = nullptr) {
    DirectAdmission::Request request(offset, buf.data.size(), write);
    env.lock();
    const int slot = admission.take(env, request);
    if (slot == DirectAdmission::kOffline) { env.unlock(); return -1; }
    Store store{medium, state, write};
    bool error = false;
    uint64_t actual;
    if (slot == DirectAdmission::kSerial) {
      actual = directDiskIO(seed, overlay, store, stages[0].data(), 32, write,
                            offset, buf.data.size(), buf, &error);
    } else if (slot >= 0) {
      env.unlock();
      auto *stage = stages[slot].data();
      ConcurrentDirect<Env, Store> path{overlay, commits, env, store, stage};
      actual = directDiskIOWithPolicy(seed, overlay, store, stage, 32, write,
                                      offset, buf.data.size(), buf, &error, path);
      env.lock();
      admission.give(env, slot, store.liveDMA);
    } else {
      actual = 0;
      error = true;
    }
    env.unlock();
    // A storage completion can reenter the driver.
    env.lock();
    ++callbacks;
    env.unlock();
    bitmapWrites += store.publications;
    if (failed) *failed = error;
    return actual;
  }

  bool fence(int op) {
    env.lock();
    if (!admission.beginFence(env)) { env.unlock(); return false; }
    bool ok = !state.failed();
    if (op == 1) {
      env.unlock();
      DataPowerHold power;
      ok = power.off(medium, true);
      env.lock();
      admission.held = ok;
    }
    if (op == 2) admission.parallel = false;
    if (op == 3) admission.parallel = true;
    admission.endFence(env);
    env.unlock();
    return ok;
  }

  unsigned waiters() {
    env.lock();
    const unsigned n = admission.waiting;
    env.unlock();
    return n;
  }
  bool fenced() {
    env.lock();
    const bool busy = admission.fenced;
    env.unlock();
    return busy;
  }

  void verifyReopen(uint64_t present) {
    auto reopenedBits = std::vector<uint8_t>(bits.size());
    persist::Overlay reopened;
    DirectWriteState healthy;
    Store store{medium, healthy, false};
    assert(reopened.open(store, medium.pages, digest, true, reopenedBits.data(), false, 0, 1000) == persist::Open::Resumed);
    assert(reopened.present == present && reopenedBits == bits && reopened.boundLba == 1000);
    Medium::valid(medium.durable, medium.pages);
  }
};

static void armedSerialStart()
{
  Disk disk(2, kPages, false);
  DirectAdmission::Request first(0, kBlock, true), second(kBlock, kBlock, false);
  disk.env.lock();
  assert(disk.admission.online && !disk.admission.parallel && disk.admission.depth == 2);
  assert(disk.admission.take(disk.env, first) == DirectAdmission::kSerial);
  assert(disk.admission.inflight == 0);
  disk.env.unlock();

  Buffer original(kBlock, 0x42);
  assert(disk.io(true, 0, original) == kBlock);
  assert(disk.fence(3) && disk.admission.parallel);

  disk.env.lock();
  const int writer = disk.admission.take(disk.env, first);
  const int reader = disk.admission.take(disk.env, second);
  assert(writer >= 0 && reader >= 0 && writer != reader && disk.admission.peak == 2);
  disk.admission.give(disk.env, reader, false);
  disk.admission.give(disk.env, writer, false);
  disk.env.unlock();

  Buffer result(kBlock), added(kBlock, 0x84);
  assert(disk.io(false, 0, result) == kBlock && result.data == original.data);
  assert(disk.io(true, kBlock, added) == kBlock);
  assert(disk.fence(2) && !disk.admission.parallel);
  assert(disk.io(false, kBlock, result) == kBlock && result.data == added.data);
  disk.verifyReopen(2);
  assert(!memcmp(disk.medium.durable.at(persist::pageBlock(kPages, 0)).data(), original.data.data(), kBlock));
  assert(!memcmp(disk.medium.durable.at(persist::pageBlock(kPages, 1)).data(), added.data.data(), kBlock));
}

static void depthAndStop()
{
  Disk disk;
  for (unsigned i = 0; i < 9; ++i) {
    Buffer page(kBlock, 0x30 + i);
    assert(disk.io(true, i * kBlock, page) == kBlock);
  }
  disk.medium.hold(disk.medium.holdData, true);
  std::vector<std::thread> writers;
  for (unsigned i = 0; i < 8; ++i) {
    writers.emplace_back([&, i] {
      Buffer page(kBlock, 0x30 + i);
      assert(disk.io(true, i * kBlock, page) == kBlock);
    });
    if (i == 0) {
      until([&] { return disk.medium.pending(disk.medium.dataIn) == 1; });
      Buffer disjoint(kBlock);
      assert(disk.io(false, kBlock, disjoint) == kBlock && disjoint.data[0] == 0x31);
    }
  }
  until([&] { return disk.medium.pending(disk.medium.dataIn) == 8; });
  disk.medium.hold(disk.medium.holdData, false);
  for (auto &writer : writers) writer.join();
  disk.medium.hold(disk.medium.holdRead, true);
  std::vector<std::thread> readers;
  for (unsigned i = 0; i < 9; ++i) {
    readers.emplace_back([&, i] {
      Buffer page(kBlock);
      const auto done = disk.io(false, i * kBlock, page);
      if (done > 0) assert(i < 8 && done == kBlock && page.data[0] == 0x30 + i);
      else assert(done == -1 && i == 8);
    });
    if (i < 8) until([&] { return disk.medium.pending(disk.medium.readIn) == i + 1; });
  }
  until([&] { return disk.waiters() == 1; });
  std::atomic<bool> stopped{false};
  std::thread stop([&] {
    disk.env.lock();
    disk.admission.stop(disk.env);
    disk.env.unlock();
    stopped = true;
  });
  until([&] { return disk.waiters() == 0; });
  assert(!stopped);
  disk.medium.hold(disk.medium.holdRead, false);
  for (auto &reader : readers) reader.join();
  stop.join();
  assert(stopped && disk.admission.peak == 8 && disk.admission.waits && disk.callbacks == 26);
}

static void overlap()
{
  Disk disk;
  Buffer original(kBlock, 0x11);
  assert(disk.io(true, 0, original) == kBlock);
  disk.medium.hold(disk.medium.holdRead, true);
  std::thread shared[2];
  for (auto &reader : shared) reader = std::thread([&] {
    Buffer page(kBlock);
    assert(disk.io(false, 0, page) == kBlock && page.data == original.data);
  });
  until([&] { return disk.medium.pending(disk.medium.readIn) == 2; });
  std::thread first([&] {
    Buffer sector(512, 0x22);
    assert(disk.io(true, 0, sector) == 512);
  });
  until([&] { return disk.waiters() == 1; });
  // A later overlapping reader must see the first writer, even while another reader holds the page.
  std::thread after([&] {
    Buffer sector(512);
    assert(disk.io(false, 0, sector) == 512 && sector.data == Buffer(512, 0x22).data);
  });
  until([&] { return disk.waiters() == 2; });
  Buffer adjacent(kBlock, 0x33);
  assert(disk.io(true, kBlock, adjacent) == kBlock);
  disk.medium.hold(disk.medium.holdRead, false);
  for (auto &reader : shared) reader.join();
  first.join();
  after.join();

  std::vector<std::thread> workers;
  for (unsigned i = 0; i < 8; ++i) {
    workers.emplace_back([&, i] {
      for (unsigned pass = 0; pass < 32; ++pass) {
        Buffer sector(512, i + 0x40);
        assert(disk.io(true, i * 512, sector) == 512);
        Buffer read(512);
        assert(disk.io(false, i * 512, read) == 512 && read.data == sector.data);
      }
    });
  }
  for (auto &worker : workers) worker.join();
  Buffer page(kBlock);
  assert(disk.io(false, 0, page) == kBlock);
  for (unsigned i = 0; i < kBlock; ++i) assert(page.data[i] == 0x40 + i / 512);
  disk.verifyReopen(2);
}

static void mergeAndCut(bool reverse)
{
  Disk disk;
  disk.medium.hold(disk.medium.holdBitmap, true);
  std::thread first([&] {
    Buffer page(kBlock, 1);
    assert(disk.io(true, 0, page) == kBlock);
  });
  until([&] { return disk.medium.pending(disk.medium.bitmapIn) == 1; });
  assert(!disk.overlay.has(0));

  std::vector<std::thread> more;
  for (unsigned i = 1; i < 8; ++i) {
    const unsigned page = reverse ? 8 - i : i;
    more.emplace_back([&, page] {
      Buffer data(kBlock, page + 1);
      assert(disk.io(true, page * kBlock, data) == kBlock);
    });
    until([&] {
      disk.env.lock();
      const bool queued = disk.commits.pending == i;
      disk.env.unlock();
      return queued;
    });
  }
  assert(!disk.overlay.has(7));
  disk.medium.hold(disk.medium.holdBitmap, false);
  first.join();
  for (auto &thread : more) thread.join();
  assert(disk.bitmapWrites == 2);    // Seven runs shared the second FUA of this bitmap block.
  for (unsigned i = 0; i < 8; ++i) {
    Buffer page(kBlock);
    assert(disk.io(false, i * kBlock, page) == kBlock && page.data == Buffer(kBlock, i + 1).data);
  }
  Buffer cross(2 * kBlock, 0x91);
  assert(disk.io(true, (kBoundary - 1) * kBlock, cross) == 2 * kBlock);
  assert(disk.bitmapWrites == 4 && disk.medium.cuts > 25);
  disk.verifyReopen(10);
}

static void drains()
{
  Disk disk(1);
  disk.medium.hold(disk.medium.holdData, true);
  std::thread writer([&] {
    Buffer page(kBlock, 2);
    assert(disk.io(true, 0, page) == kBlock);
  });
  until([&] { return disk.medium.pending(disk.medium.dataIn) == 1; });
  std::atomic<bool> synced{false};
  std::thread sync([&] { assert(disk.fence(0)); synced = true; });
  until([&] { return disk.fenced(); });
  std::thread reader([&] {
    Buffer sector(512);
    assert(disk.io(false, 0, sector) == 512 && sector.data == Buffer(512, 2).data);
  });
  until([&] { return disk.waiters() == 1; });
  assert(!synced);
  disk.medium.hold(disk.medium.holdData, false);
  writer.join();
  sync.join();
  reader.join();
  assert(synced && disk.medium.syncs == 2 && disk.admission.peak == 1);  // Only open issued sync commands.

  disk.medium.hold(disk.medium.holdBitmap, true);
  std::thread bitmap([&] {
    Buffer page(kBlock, 3);
    assert(disk.io(true, kBlock, page) == kBlock);
  });
  until([&] { return disk.medium.pending(disk.medium.bitmapIn) == 1; });
  std::atomic<bool> slept{false};
  std::thread sleep([&] { assert(disk.fence(1)); slept = true; });
  until([&] { return disk.fenced(); });
  assert(!slept);
  disk.medium.hold(disk.medium.holdBitmap, false);
  bitmap.join();
  sleep.join();
  std::thread wakeReader([&] {
    Buffer page(kBlock);
    assert(disk.io(false, kBlock, page) == kBlock && page.data == Buffer(kBlock, 3).data);
  });
  until([&] { return disk.waiters() == 1; });
  disk.env.lock();
  disk.admission.held = false;
  disk.env.wake();
  disk.env.unlock();
  wakeReader.join();
  disk.medium.syncOk = false;
  assert(!disk.fence(1) && disk.medium.vetoes == 1 && !disk.admission.held);
  assert(disk.fence(2) && !disk.admission.parallel);
  Buffer serial(kBlock, 0x84);
  assert(disk.io(true, 2 * kBlock, serial) == kBlock);
  assert(disk.fence(3) && disk.admission.parallel);
  Buffer after(kBlock);
  assert(disk.io(false, 2 * kBlock, after) == kBlock && after.data == serial.data);
  disk.verifyReopen(3);
}

static void failure()
{
  Disk disk;
  Buffer page(kBlock, 1);
  assert(disk.io(true, 0, page) == kBlock);
  disk.medium.hold(disk.medium.holdBitmap, true);
  std::thread writer([&] {
    Buffer data(kBlock, 2);
    assert(disk.io(true, kBlock, data) == kBlock);   // This bitmap write was already issued.
  });
  until([&] { return disk.medium.pending(disk.medium.bitmapIn) == 1; });

  std::thread pending([&] {
    Buffer data(kBlock, 3);
    bool error = false;
    assert(disk.io(true, 2 * kBlock, data, &error) == 0 && error);
  });
  until([&] {
    disk.env.lock();
    const bool queued = disk.commits.pending == 1;
    disk.env.unlock();
    return queued;
  });
  {
    std::lock_guard<std::mutex> lock(disk.medium.mutex);
    disk.medium.lateBlock = persist::pageBlock(kPages, 3);
  }
  bool error = false;
  assert(disk.io(true, 3 * kBlock, page, &error) == 0 && error && disk.state.failed());
  assert(!disk.overlay.has(2) && !disk.overlay.has(3));
  Buffer read(kBlock);
  assert(disk.io(false, 0, read) == kBlock && read.data == page.data);  // A disjoint read continues.
  disk.medium.hold(disk.medium.holdBitmap, false);
  writer.join();
  pending.join();
  assert(!disk.fence(0));
  assert(disk.medium.lateDMA == disk.stages[2].data() && (disk.admission.quarantined & (1U << 2)));
  static_cast<uint8_t *>(const_cast<void *>(disk.medium.lateDMA))[0] = 99;  // Late DMA touches only the quarantined stage.
  assert(disk.io(true, 4 * kBlock, page, &error) == 0 && error);
  disk.verifyReopen(2);

  Disk torn;
  torn.medium.tornBlock = 2;
  Buffer cross(2 * kBlock, 7);
  assert(torn.io(true, (kBoundary - 1) * kBlock, cross, &error) == 0 && error);
  assert(torn.overlay.has(kBoundary - 1) && !torn.overlay.has(kBoundary));
  Medium::valid(torn.medium.durable);

  Disk shortBuffer;
  Buffer shortWrite(33 * kBlock, 5);
  shortWrite.shortAt = 32 * kBlock;
  assert(shortBuffer.io(true, 0, shortWrite, &error) == 32 * kBlock && !error);
  assert(!shortBuffer.state.failed() && !shortBuffer.overlay.has(32));
  shortBuffer.verifyReopen(32);

  Disk oneStage(1);
  oneStage.medium.lateBlock = persist::pageBlock(kPages, 0);
  assert(oneStage.io(true, 0, page, &error) == 0 && error && oneStage.admission.quarantined == 1);
  assert(oneStage.io(false, 0, read, &error) == 0 && error && oneStage.admission.inflight == 0);
  oneStage.verifyReopen(0);
}

static void stress()
{
  Disk disk;
  constexpr unsigned pages = 12;
  std::array<std::array<uint8_t, 8>, pages> expected{};
  std::atomic<unsigned> finished{0};
  std::thread snapshots([&] {
    while (finished != 8) {
      {
        std::lock_guard<std::mutex> lock(disk.medium.mutex);
        Medium::valid(disk.medium.durable);
      }
      std::this_thread::yield();
    }
  });
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < 8; ++i) {
    workers.emplace_back([&, i] {
      uint32_t random = 0x9e3779b9U * (i + 1);
      for (unsigned pass = 0; pass < 72; ++pass) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        const unsigned page = random % pages;
        const uint8_t value = (random >> 24) | 1;
        // Writers own sectors, and the driver still has to protect each shared 4 KiB RMW.
        const uint64_t offset = (1 + page) * kBlock + i * 512;
        Buffer sector(512, value), read(512);
        assert(disk.io(true, offset, sector) == 512);
        expected[page][i] = value;
        assert(disk.io(false, offset, read) == 512 && read.data == sector.data);
      }
      ++finished;
    });
  }
  for (auto &worker : workers) worker.join();
  snapshots.join();
  for (unsigned i = 0; i < pages; ++i) {
    Buffer read(kBlock);
    assert(disk.io(false, (1 + i) * kBlock, read) == kBlock);
    for (unsigned n = 0; n < kBlock; ++n) assert(read.data[n] == expected[i][n / 512]);
  }
  disk.verifyReopen(pages);
}

int main()
{
  Medium control;
  Block page{};
  bool live = false;
  const uint64_t block = persist::pageBlock(kPages, 1);
  assert(control.write(block, 1, page.data(), live, false));
  assert(control.cache.count(block) == 1 && control.durable.count(block) == 0);
  assert(control.sync() && control.durable.count(block) == 1);
  armedSerialStart();
  depthAndStop();
  overlap();
  mergeAndCut(false);
  mergeAndCut(true);
  drains();
  failure();
  stress();
  assert(persist::bitmapBlocks(kLargePages) == 1600);
  Disk large(8, kLargePages);
  Buffer last(kBlock, 0x94);
  assert(large.io(true, (kLargePages - 1) * kBlock, last) == kBlock);
  large.verifyReopen(1);
  puts("OK armed serial start/switch, depth/stop, shared/exclusive ranges and 512-byte RMW, merged bitmap FUA/cuts, drains/wake/veto, errors and quarantine");
}
