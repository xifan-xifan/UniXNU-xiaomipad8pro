// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "DirectDisk.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <map>

using Block = std::array<uint8_t, persist::kBlock>;
struct Store {
  std::map<uint64_t, Block> media;
  uint64_t fail = UINT64_MAX;
  bool read(uint64_t b, uint32_t n, void *dst) {
    printf("r %llu %u\n", static_cast<unsigned long long>(b), n);
    for (unsigned i = 0; i < n; ++i) memcpy(static_cast<uint8_t *>(dst) + i * 4096, media[b + i].data(), 4096);
    return true;
  }
  bool write(uint64_t b, uint32_t n, const void *src) {
    printf("w %llu %u %d\n", static_cast<unsigned long long>(b), n, b != fail);
    if (b == fail) return false;
    for (unsigned i = 0; i < n; ++i) memcpy(media[b + i].data(), static_cast<const uint8_t *>(src) + i * 4096, 4096);
    return true;
  }
  bool sync() { puts("sync"); return true; }
};
struct Seed {
  uint64_t bytes = 32776ULL * 4096;
  Block first{};
  const uint8_t *find(uint64_t i) const { return i == 0 ? first.data() : nullptr; }
};
struct Buffer {
  uint8_t data[34 * 4096] = {};
  uint64_t read(uint64_t at, void *p, uint64_t n) { memcpy(p, data + at, n); return n; }
  uint64_t copy(uint64_t at, const void *p, uint64_t n) { memcpy(data + at, p, n); return n; }
};
int main()
{
  Store store;
  Seed seed;
  seed.first.fill(17);
  Buffer buffer;
  uint8_t bits[8192], stage[32 * 4096], seedHash[32] = {7};
  persist::Overlay overlay;
  assert(overlay.open(store, seed.bytes / 4096, seedHash, false, bits, false, 0, 100) == persist::Open::Fresh);
  auto io = [&](bool write, uint64_t off, uint64_t len, uint64_t expected, bool expectedError = false) {
    bool error = false;
    auto done = directDiskIO(seed, overlay, store, stage, 32, write, off, len, buffer, &error);
    assert(done == expected && error == expectedError);
    printf("done %llu %d\n", static_cast<unsigned long long>(done), error);
    if (!write) fwrite(buffer.data, 1, done, stdout);
  };
  io(false, 0, 4096, 4096);
  memset(buffer.data, 3, sizeof(buffer.data));
  io(true, 512, 34 * 4096 - 512, 34 * 4096 - 512);
  io(true, 4096, 4096, 4096);
  io(true, 32767ULL * 4096, 8192, 8192);
  io(false, 0, 34 * 4096, 34 * 4096);
  store.fail = 2;
  io(true, 32769ULL * 4096, 4096, 0, true);
  for (const auto &entry : store.media) {
    printf("block %llu\n", static_cast<unsigned long long>(entry.first));
    fwrite(entry.second.data(), 1, entry.second.size(), stdout);
  }
}
