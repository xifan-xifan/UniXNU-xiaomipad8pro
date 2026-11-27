// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace pianousb {

// Console, user pipe, HID commands and notes. The owner serializes access and supplies a power-of-two buffer size.
struct ByteRing {
  uint8_t *buf;
  uint32_t size, head, tail;
  uint64_t dropped;

  uint32_t used() const { return head - tail; }
  uint32_t space() const { return size - used(); }
  uint32_t put(const uint8_t *s, uint32_t n)
  {
    if (n > space()) { dropped += n - space(); n = space(); }
    for (uint32_t i = 0; i < n; ++i) buf[(head + i) & (size - 1)] = s[i];
    head += n;
    return n;
  }
  uint32_t get(uint8_t *d, uint32_t n)
  {
    if (n > used()) n = used();
    for (uint32_t i = 0; i < n; ++i) d[i] = buf[(tail + i) & (size - 1)];
    tail += n;
    return n;
  }
};

}  // namespace pianousb
