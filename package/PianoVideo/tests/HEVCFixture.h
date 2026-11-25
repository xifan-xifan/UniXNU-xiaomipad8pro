// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Original synthetic HEVC syntax builder for host admission tests.
#pragma once
#include <stdint.h>
#include <vector>
namespace hevcfixture {
using Bytes = std::vector<uint8_t>;
struct Writer {
  Bytes data; unsigned bit = 0;
  void put(uint32_t value, unsigned count) {
    for (unsigned i = count; i; --i) { if (!bit) data.push_back(0); data.back() |= ((value >> (i - 1)) & 1) << (7 - bit); bit = (bit + 1) & 7; }
  }
  void ue(uint32_t value) { unsigned bits = 0; for (uint32_t v = value + 1; v >>= 1;) ++bits; put(0, bits); put(value + 1, bits + 1); }
  void se(int value) { ue(value <= 0 ? uint32_t(-2 * value) : uint32_t(2 * value - 1)); }
  void end() { put(1, 1); while (bit) put(0, 1); }
  Bytes nal(uint32_t type, uint32_t temporal = 0) {
    Bytes out{uint8_t(type << 1), uint8_t(temporal + 1)}; unsigned zeros = 0;
    for (uint8_t value : data) { if (zeros >= 2 && value <= 3) { out.push_back(3); zeros = 0; } out.push_back(value); zeros = value ? 0 : zeros + 1; }
    return out;
  }
};
static void ptl(Writer &w, unsigned depth, unsigned level = 120) {
  w.put(0, 2); w.put(0, 1); w.put(depth == 10 ? 2 : 1, 5); w.put(depth == 10 ? 0x20000000 : 0x40000000, 32);
  w.put(0x9000, 16); w.put(0, 32); w.put(level, 8);
}
static void ordering(Writer &w) { w.put(0, 1); w.ue(5); w.ue(2); w.ue(0); }
struct Fixture {
  Bytes vps, sps, pps, hvcc; unsigned depth; bool wpp;
  Fixture(unsigned bits = 8, bool wave = true, unsigned width = 1920, unsigned height = 1080, unsigned level = 120) : depth(bits), wpp(wave) {
    Writer v; v.put(0, 4); v.put(3, 2); v.put(0, 6); v.put(0, 3); v.put(1, 1); v.put(0xffff, 16); ptl(v, depth, level);
    ordering(v); v.put(0, 6); v.ue(0); v.put(0, 1); v.put(0, 1); v.end(); vps = v.nal(32);
    Writer s; s.put(0, 4); s.put(0, 3); s.put(1, 1); ptl(s, depth, level); s.ue(0); s.ue(1); s.ue(width); s.ue(height); s.put(0, 1);
    s.ue(depth - 8); s.ue(depth - 8); s.ue(4); ordering(s); s.ue(0); s.ue(3); s.ue(0); s.ue(3); s.ue(0); s.ue(0);
    s.put(0, 1); s.put(1, 1); s.put(0, 1); s.put(0, 1); s.ue(1); s.ue(1); s.ue(0); s.ue(0); s.put(1, 1);
    s.put(0, 1); s.put(0, 1); s.put(1, 1); s.put(0, 1); s.put(0, 1); s.end(); sps = s.nal(33);
    Writer p; p.ue(0); p.ue(0); p.put(0, 1); p.put(0, 1); p.put(0, 3); p.put(0, 1); p.put(0, 1); p.ue(0); p.ue(0); p.se(0);
    p.put(0, 1); p.put(0, 1); p.put(0, 1); p.se(0); p.se(0); p.put(0, 1); p.put(0, 1); p.put(0, 1); p.put(0, 1);
    p.put(0, 1); p.put(wpp, 1); p.put(0, 1); p.put(0, 1); p.put(0, 1); p.put(0, 1); p.ue(0); p.put(0, 1); p.put(0, 1); p.end(); pps = p.nal(34);
    hvcc = {1, uint8_t(depth == 10 ? 2 : 1), uint8_t(depth == 10 ? 0x20 : 0x40), 0, 0, 0, 0x90, 0, 0, 0, 0, 0, uint8_t(level),
      0xf0, 0, 0xfc, 0xfd, uint8_t(0xf8 + depth - 8), uint8_t(0xf8 + depth - 8), 0, 0, 0x0f, 3};
    for (const auto &n : {vps, sps, pps}) { hvcc.push_back(0x80 | (n[0] >> 1)); hvcc.push_back(0); hvcc.push_back(1); hvcc.push_back(n.size() >> 8); hvcc.push_back(n.size()); hvcc.insert(hvcc.end(), n.begin(), n.end()); }
  }
  Bytes slice(unsigned type = 19, unsigned kind = 2, unsigned temporal = 0, unsigned entryBytes = 0, unsigned payloadBytes = 1) const {
    Writer w; w.put(1, 1); if (type >= 16) w.put(0, 1); w.ue(0); w.ue(kind);
    if (type != 19 && type != 20) { w.put(1, 8); w.put(1, 1); }
    if (kind != 2) { w.put(0, 1); if (kind == 0) w.put(0, 1); w.ue(0); }
    w.se(0);
    if (wpp) { w.ue(entryBytes ? 1 : 0); if (entryBytes) { w.ue(31); w.put(entryBytes - 1, 32); } }
    w.end(); for (unsigned i = 0; i < payloadBytes; ++i) w.put(0x80, 8); return w.nal(type, temporal);
  }
};
static void append(Bytes &out, const Bytes &nal) {
  const uint32_t size = nal.size(); out.push_back(size >> 24); out.push_back(size >> 16); out.push_back(size >> 8); out.push_back(size); out.insert(out.end(), nal.begin(), nal.end());
}
}
