// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Independently implemented, bounded HEVC Main/Main10 hvc1 admission.
// Parameter sets are immutable for a session. The caller must start at IDR;
// CRA/BLA are admitted only within an already established reference history.
#pragma once
#include <stdint.h>
#include <string.h>
namespace pianovideo { namespace hevc {
constexpr uint32_t MaxAu = 1u << 20, MaxParameter = 1024, MaxDpb = 10;
struct Bits {
  const uint8_t *p; uint32_t n, byte = 0, bit = 0, zeros = 0; bool good = true;
  uint32_t read(unsigned count) {
    if (!good || count > 32) { good = false; return 0; }
    uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) {
      if (!bit && byte < n && zeros >= 2 && p[byte] == 3) { ++byte; zeros = 0; }
      if (byte >= n) { good = false; return 0; }
      value = (value << 1) | ((p[byte] >> (7 - bit)) & 1);
      if (++bit == 8) { zeros = p[byte] ? 0 : zeros + 1; ++byte; bit = 0; }
    }
    return value;
  }
  uint32_t ue(uint32_t maximum = 0xfffffffeu) {
    unsigned count = 0;
    while (good && !read(1)) if (++count > 31) { good = false; return 0; }
    if (!good) return 0;
    const uint32_t value = ((1u << count) - 1) + read(count);
    if (value > maximum) { good = false; return 0; }
    return value;
  }
  int32_t se(int32_t low = INT32_MIN + 1, int32_t high = INT32_MAX) {
    const uint32_t code = ue();
    const int32_t value = (code & 1) ? int32_t((code + 1) / 2) : -int32_t(code / 2);
    if (value < low || value > high) good = false;
    return value;
  }
  bool align() {
    if (!good || read(1) != 1) return false;
    while (bit) if (read(1)) return false;
    return good;
  }
  bool end() { return align() && byte == n; }
};
inline unsigned ceilLog2(uint32_t value) {
  unsigned bits = 0; for (uint32_t v = value - 1; v; v >>= 1) ++bits; return bits;
}
struct Nal { uint32_t type = 0, temporalId = 0; };
inline bool header(const uint8_t *p, uint32_t n, Nal &v) {
  if (!p || n < 3 || (p[0] & 0x81) || (p[1] & 0xf8) || !(p[1] & 7)) return false;
  v.type = (p[0] >> 1) & 63; v.temporalId = (p[1] & 7) - 1;
  // Temporal layer bounds for pictures are checked against SPS. Deployed hvc1
  // streams can carry an IDR on a higher layer; do not erase that layer or
  // confuse it with the zero temporal-id-plus1 encoding, which is invalid.
  if (v.type >= 32 && v.type <= 37)
    if (v.temporalId) return false;
  return true;
}
inline bool ebsp(const uint8_t *p, uint32_t n) {
  Nal v; if (!header(p, n, v)) return false;
  unsigned zeros = 0;
  for (uint32_t i = 2; i < n; ++i) {
    if (zeros >= 2) {
      if (p[i] < 3) return false;
      if (p[i] == 3) {
        if (i + 1 == n || p[i + 1] > 3) return false;
        zeros = 0; continue;
      }
    }
    zeros = p[i] ? 0 : zeros + 1;
  }
  return true;
}
struct Ptl {
  uint32_t profile = 0, tier = 0, level = 0, compatibility = 0;
  uint64_t constraints = 0;
  bool same(const Ptl &p) const { return profile == p.profile && tier == p.tier && level == p.level &&
    compatibility == p.compatibility && constraints == p.constraints; }
};
inline uint32_t level(uint32_t value) {
  switch (value) {
    case 30: return 0; case 60: return 1; case 63: return 2; case 90: return 3; case 93: return 4;
    case 120: return 5; case 123: return 6; case 150: return 7; case 153: return 8; case 156: return 9;
    case 180: return 10; case 183: return 11; default: return UINT32_MAX;
  }
}
inline bool profile(Bits &b, Ptl &p) {
  const uint32_t space = b.read(2); p.tier = b.read(1); p.profile = b.read(5);
  p.compatibility = b.read(32); p.constraints = uint64_t(b.read(16)) << 32; p.constraints |= b.read(32);
  // No interlaced source or profile extension. Main10 reserves one additional
  // one-picture constraint bit; the final inbld flag is defined for both.
  const uint64_t reserved = p.profile == 2 ? 0x0feffffffffeull : 0x0ffffffffffeull;
  return b.good && !space && (p.profile == 1 || p.profile == 2) && !(p.constraints & reserved) &&
    !(p.constraints & (1ull << 46));
}
inline bool ptl(Bits &b, uint32_t subLayers, Ptl &p) {
  if (subLayers < 1 || subLayers > 7 || !profile(b, p)) return false;
  p.level = b.read(8);
  if (level(p.level) == UINT32_MAX || (p.tier && p.level < 120)) return false;
  bool profiles[7] = {}, levels[7] = {};
  for (uint32_t i = 0; i + 1 < subLayers; ++i) { profiles[i] = b.read(1); levels[i] = b.read(1); }
  if (subLayers > 1) for (uint32_t i = subLayers - 1; i < 8; ++i) if (b.read(2)) return false;
  for (uint32_t i = 0; i + 1 < subLayers; ++i) {
    Ptl sub;
    if (profiles[i] && (!profile(b, sub) || sub.profile != p.profile || sub.tier > p.tier)) return false;
    if (levels[i]) { const uint32_t l = b.read(8); if (level(l) == UINT32_MAX || l > p.level) return false; }
  }
  return b.good;
}
inline bool ordering(Bits &b, uint32_t layers, uint32_t &dpb, uint32_t &reorder) {
  const bool all = b.read(1); uint32_t previousDpb = 0, previousReorder = 0;
  for (uint32_t i = all ? 0 : layers - 1; i < layers; ++i) {
    dpb = b.ue(MaxDpb - 1) + 1; reorder = b.ue(MaxDpb - 1); b.ue();
    if (!b.good || reorder >= dpb || dpb < previousDpb || reorder < previousReorder) return false;
    previousDpb = dpb; previousReorder = reorder;
  }
  return b.good;
}
inline bool subHrd(Bits &b, uint32_t count, bool subPic) {
  for (uint32_t i = 0; i <= count; ++i) {
    b.ue(); b.ue(); if (subPic) { b.ue(); b.ue(); } b.read(1);
  }
  return b.good;
}
inline bool hrd(Bits &b, uint32_t layers) {
  const bool nal = b.read(1), vcl = b.read(1); bool subPic = false;
  if (nal || vcl) {
    subPic = b.read(1);
    if (subPic) { b.read(8); b.read(5); b.read(1); b.read(5); }
    b.read(4); b.read(4); if (subPic) b.read(4);
    b.read(5); b.read(5); b.read(5);
  }
  for (uint32_t i = 0; i < layers; ++i) {
    const bool fixedGeneral = b.read(1); const bool fixed = fixedGeneral || b.read(1);
    bool lowDelay = false;
    if (fixed) b.ue(); else lowDelay = b.read(1);
    const uint32_t count = lowDelay ? 0 : b.ue(31);
    if (!b.good || (nal && !subHrd(b, count, subPic)) || (vcl && !subHrd(b, count, subPic))) return false;
  }
  return b.good;
}
struct Vps {
  uint32_t id = 0, subLayers = 0, maxDpb = 0, maxReorder = 0; bool nested = false; Ptl pt;
};
inline bool vps(const uint8_t *p, uint32_t n, Vps &v) {
  Nal h; if (n > MaxParameter || !ebsp(p, n) || !header(p, n, h) || h.type != 32) return false;
  Bits b{p + 2, n - 2}; v = Vps{}; v.id = b.read(4);
  if (b.read(2) != 3 || b.read(6)) return false; // one internal, available base layer
  v.subLayers = b.read(3) + 1; v.nested = b.read(1);
  if ((!v.nested && v.subLayers == 1) || b.read(16) != 0xffff || !ptl(b, v.subLayers, v.pt) ||
      !ordering(b, v.subLayers, v.maxDpb, v.maxReorder) || b.read(6) || b.ue() != 0) return false;
  if (b.read(1)) {
    if (!b.read(32) || !b.read(32)) return false;
    if (b.read(1)) b.ue();
    const uint32_t count = b.ue(1);
    if (!b.good) return false;
    for (uint32_t i = 0; i < count; ++i) if (b.ue() != 0 || !hrd(b, v.subLayers)) return false;
  }
  return !b.read(1) && b.end(); // multilayer extensions are not this contract
}
inline bool scaling(Bits &b) {
  for (unsigned size = 0; size < 4; ++size) for (unsigned matrix = 0; matrix < 6; matrix += size == 3 ? 3 : 1) {
    if (!b.read(1)) { if (b.ue() > matrix) return false; }
    else {
      if (size > 1) b.se(-7, 247);
      for (unsigned i = 0; i < (size ? 64u : 16u); ++i) b.se(-128, 127);
    }
    if (!b.good) return false;
  }
  return true;
}
// A compact, derived RPS is retained for validating slice-selected and predicted
// sets. No compressed coefficient or third-party parser code is retained.
struct Rps { int16_t delta[16] = {}; uint16_t used = 0; uint8_t count = 0, usedCount = 0; };
inline bool addRps(Rps &r, int32_t delta, bool used, uint32_t maximum) {
  if (!delta) return true;
  if (delta < -32767 || delta > 32767 || r.count >= maximum || r.count >= 16) return false;
  uint32_t at = 0;
  // Negative POCs nearest first, followed by positive POCs nearest first.
  while (at < r.count && ((r.delta[at] < 0 && delta > 0) ||
      ((r.delta[at] < 0) == (delta < 0) && (delta < 0 ? r.delta[at] > delta : r.delta[at] < delta)))) ++at;
  if (at < r.count && r.delta[at] == delta) return false;
  for (uint32_t j = r.count; j > at; --j) r.delta[j] = r.delta[j - 1];
  const uint32_t low = (1u << at) - 1;
  r.used = uint16_t((r.used & low) | ((r.used & ~low) << 1) | (uint32_t(used) << at));
  r.delta[at] = int16_t(delta); ++r.count; if (used) ++r.usedCount;
  return true;
}
inline bool shortRps(Bits &b, uint32_t index, uint32_t stored, const Rps *sets, uint32_t maximum, Rps &r) {
  r = Rps{};
  if (index && b.read(1)) {
    const uint32_t back = index == stored ? b.ue(index - 1) + 1 : 1;
    if (!b.good || back > index) return false;
    const Rps &ref = sets[index - back];
    const bool negative = b.read(1); const int32_t distance = int32_t(b.ue(32766) + 1);
    const int32_t shift = negative ? -distance : distance;
    for (uint32_t j = 0; j <= ref.count; ++j) {
      const bool used = b.read(1); const bool keep = used || b.read(1);
      if (keep && !addRps(r, (j == ref.count ? 0 : ref.delta[j]) + shift, used, maximum)) return false;
    }
  } else {
    const uint32_t negative = b.ue(maximum), positive = b.ue(maximum);
    if (!b.good || negative + positive > maximum) return false;
    int32_t delta = 0;
    for (uint32_t j = 0; j < negative; ++j) { delta -= int32_t(b.ue(32766) + 1); const bool used = b.read(1); if (!addRps(r, delta, used, maximum)) return false; }
    delta = 0;
    for (uint32_t j = 0; j < positive; ++j) { delta += int32_t(b.ue(32766) + 1); const bool used = b.read(1); if (!addRps(r, delta, used, maximum)) return false; }
  }
  return b.good;
}
struct Sps {
  uint32_t id = 0, vpsId = 0, subLayers = 0, depth = 0, pocBits = 0, maxDpb = 0, maxReorder = 0;
  uint32_t width = 0, height = 0, codedWidth = 0, codedHeight = 0, left = 0, top = 0, right = 0, bottom = 0;
  uint32_t color = 0x14000003, minCbLog2 = 0, ctbLog2 = 0, numRps = 0, numLongTerm = 0;
  uint32_t longTermPoc[16] = {}; uint16_t longTermUsed = 0;
  bool nested = false, sao = false, longTerm = false, temporalMvp = false; Ptl pt; Rps rps[64];
  uint32_t ctbWidth() const { return (codedWidth + (1u << ctbLog2) - 1) >> ctbLog2; }
  uint32_t ctbHeight() const { return (codedHeight + (1u << ctbLog2) - 1) >> ctbLog2; }
  uint32_t ctbs() const { return ctbWidth() * ctbHeight(); }
};
inline bool vui(Bits &b, Sps &s) {
  if (b.read(1)) { const uint32_t aspect = b.read(8); if (aspect == 255) { if (!b.read(16) || !b.read(16)) return false; } else if (!aspect || aspect > 16) return false; }
  if (b.read(1)) b.read(1);
  if (b.read(1)) {
    const uint32_t format = b.read(3); if (format > 5 || b.read(1)) return false; // video-range output
    const bool description = b.read(1); uint32_t primaries = 0, transfer = 0, matrix = 3;
    if (description) { primaries = b.read(8); transfer = b.read(8); matrix = b.read(8); }
    s.color = matrix | (transfer << 8) | (primaries << 16) | (uint32_t(description) << 24) | (format << 26) | (1u << 29);
  }
  if (b.read(1)) { b.ue(5); b.ue(5); }
  b.read(1); if (b.read(1)) return false; // no field sequence
  b.read(1);
  if (b.read(1)) { // default display window is additional presentation metadata
    const uint32_t l = b.ue(1920), r = b.ue(1920), t = b.ue(1088), d = b.ue(1088);
    if (!b.good || (l + r) * 2 >= s.width || (t + d) * 2 >= s.height) return false;
    // A distinct display crop needs a separate output contract.
    if (l || r || t || d) return false;
  }
  if (b.read(1)) {
    if (!b.read(32) || !b.read(32)) return false;
    if (b.read(1)) b.ue();
    if (b.read(1) && !hrd(b, s.subLayers)) return false;
  }
  if (b.read(1)) { b.read(1); b.read(1); b.read(1); b.ue(4095); b.ue(16); b.ue(16); b.ue(15); b.ue(15); }
  return b.good;
}
inline bool sps(const uint8_t *p, uint32_t n, const Vps &v, Sps &s) {
  Nal h; if (n > MaxParameter || !ebsp(p, n) || !header(p, n, h) || h.type != 33) return false;
  Bits b{p + 2, n - 2}; memset(&s, 0, sizeof(s)); s.color = 0x14000003;
  s.vpsId = b.read(4); s.subLayers = b.read(3) + 1; s.nested = b.read(1);
  if (s.vpsId != v.id || s.subLayers != v.subLayers || s.nested != v.nested || !ptl(b, s.subLayers, s.pt) || !s.pt.same(v.pt)) return false;
  s.id = b.ue(15); if (b.ue() != 1) return false; // 4:2:0, no separate color plane
  s.codedWidth = b.ue(3840); s.codedHeight = b.ue(2176);
  if (b.read(1)) { s.left = b.ue(1920) * 2; s.right = b.ue(1920) * 2; s.top = b.ue(1088) * 2; s.bottom = b.ue(1088) * 2; }
  if (!b.good || s.left + s.right >= s.codedWidth || s.top + s.bottom >= s.codedHeight) return false;
  s.width = s.codedWidth - s.left - s.right; s.height = s.codedHeight - s.top - s.bottom;
  if (s.width < 96 || s.height < 96 || s.width > 3840 || s.height > 2160) return false;
  s.depth = b.ue(2) + 8; if ((s.depth != 8 && s.depth != 10) || b.ue() + 8 != s.depth || (s.pt.profile == 1 && s.depth != 8)) return false;
  s.pocBits = b.ue(12) + 4;
  if (!ordering(b, s.subLayers, s.maxDpb, s.maxReorder) || s.maxDpb > v.maxDpb || s.maxReorder > v.maxReorder) return false;
  s.minCbLog2 = b.ue(3) + 3; s.ctbLog2 = s.minCbLog2 + b.ue(3);
  const uint32_t minTransform = b.ue(3) + 2, maxTransform = minTransform + b.ue(3);
  if (!b.good || s.ctbLog2 > 6 || minTransform >= s.minCbLog2 || maxTransform > 5 || maxTransform > s.ctbLog2 ||
      (s.codedWidth & ((1u << s.minCbLog2) - 1)) || (s.codedHeight & ((1u << s.minCbLog2) - 1))) return false;
  b.ue(s.ctbLog2 - minTransform); b.ue(s.ctbLog2 - minTransform);
  if (b.read(1) && b.read(1) && !scaling(b)) return false;
  b.read(1); s.sao = b.read(1);
  if (b.read(1)) {
    if (b.read(4) + 1 > s.depth || b.read(4) + 1 > s.depth) return false;
    const uint32_t minPcm = b.ue(2) + 3, maxPcm = minPcm + b.ue(2);
    if (minPcm < s.minCbLog2 || maxPcm > s.ctbLog2 || maxPcm > 5) return false;
    b.read(1);
  }
  s.numRps = b.ue(64); if (!b.good) return false;
  for (uint32_t i = 0; i < s.numRps; ++i) if (!shortRps(b, i, s.numRps, s.rps, s.maxDpb - 1, s.rps[i])) return false;
  s.longTerm = b.read(1);
  if (s.longTerm) {
    s.numLongTerm = b.ue(s.maxDpb - 1); if (!b.good) return false;
    for (uint32_t i = 0; i < s.numLongTerm; ++i) { s.longTermPoc[i] = b.read(s.pocBits); if (b.read(1)) s.longTermUsed |= uint16_t(1u << i); }
  }
  s.temporalMvp = b.read(1); b.read(1);
  if (b.read(1) && !vui(b, s)) return false;
  if (b.read(1) && b.read(8)) return false; // no range, multilayer, 3D, SCC extensions
  return b.end();
}
struct Pps {
  uint32_t id = 0, spsId = 0, extraBits = 0, refsL0 = 0, refsL1 = 0, tileColumns = 1, tileRows = 1;
  int32_t qp = 0, cbQp = 0, crQp = 0;
  bool dependent = false, outputFlag = false, cabac = false, chromaQp = false, weighted = false, weightedBi = false;
  bool tiles = false, wpp = false, loopAcross = false, deblockOverride = false, deblockDisabled = false;
  bool listsMod = false, headerExtension = false;
};
inline bool pps(const uint8_t *p, uint32_t n, const Sps &s, Pps &v) {
  Nal h; if (n > MaxParameter || !ebsp(p, n) || !header(p, n, h) || h.type != 34) return false;
  Bits b{p + 2, n - 2}; v = Pps{}; v.id = b.ue(63); v.spsId = b.ue(15);
  if (v.spsId != s.id) return false;
  v.dependent = b.read(1); v.outputFlag = b.read(1); v.extraBits = b.read(3); b.read(1); v.cabac = b.read(1);
  v.refsL0 = b.ue(14) + 1; v.refsL1 = b.ue(14) + 1; v.qp = b.se(-26 - 6 * int32_t(s.depth - 8), 25);
  b.read(1); b.read(1); if (b.read(1)) b.ue(s.ctbLog2 - s.minCbLog2);
  v.cbQp = b.se(-12, 12); v.crQp = b.se(-12, 12); v.chromaQp = b.read(1);
  v.weighted = b.read(1); v.weightedBi = b.read(1); b.read(1); v.tiles = b.read(1); v.wpp = b.read(1);
  if (v.tiles) {
    v.tileColumns = b.ue(19) + 1; v.tileRows = b.ue(21) + 1;
    if (!b.good || v.tileColumns > s.ctbWidth() || v.tileRows > s.ctbHeight()) return false;
    if (!b.read(1)) {
      uint32_t width = 0, height = 0;
      for (uint32_t i = 1; i < v.tileColumns; ++i) width += b.ue(s.ctbWidth() - 1) + 1;
      for (uint32_t i = 1; i < v.tileRows; ++i) height += b.ue(s.ctbHeight() - 1) + 1;
      if (width >= s.ctbWidth() || height >= s.ctbHeight()) return false;
    }
    b.read(1);
  }
  v.loopAcross = b.read(1);
  if (b.read(1)) { v.deblockOverride = b.read(1); v.deblockDisabled = b.read(1); if (!v.deblockDisabled) { b.se(-6, 6); b.se(-6, 6); } }
  if (b.read(1) && !scaling(b)) return false;
  v.listsMod = b.read(1); b.ue(s.ctbLog2 - 2); v.headerExtension = b.read(1);
  if (b.read(1) && b.read(8)) return false;
  return b.end();
}
struct Config {
  uint8_t vps[MaxParameter] = {}, sps[MaxParameter] = {}, pps[MaxParameter] = {};
  uint32_t vpsBytes = 0, spsBytes = 0, ppsBytes = 0; Vps v; Sps s; Pps p;
  bool valid() const { return vpsBytes && spsBytes && ppsBytes; }
  bool same(const Config &c) const { return valid() && c.valid() && vpsBytes == c.vpsBytes && spsBytes == c.spsBytes && ppsBytes == c.ppsBytes &&
    !memcmp(vps, c.vps, vpsBytes) && !memcmp(sps, c.sps, spsBytes) && !memcmp(pps, c.pps, ppsBytes); }
  uint32_t parameterBytes() const { return 12 + vpsBytes + spsBytes + ppsBytes; }
  uint32_t hfiProfile() const { return s.pt.profile == 2 ? 2 : 0; }
  uint32_t hfiLevel() const { return level(s.pt.level); }
  uint32_t hfiTier() const { return s.pt.tier; }
};
inline bool config(const uint8_t *p, uint32_t n, Config &c) {
  // Clear the output in place: an aggregate assignment materializes another
  // complete parameter/RPS store on the 16 KiB kernel stack with some compilers.
  memset(&c, 0, sizeof(c)); c.s.color = 0x14000003;
  if (!p || n < 23 || n > 3 * MaxParameter + 38 || p[0] != 1 || (p[13] & 0xf0) != 0xf0 ||
      (p[15] & 0xfc) != 0xfc || (p[16] & 0xfc) != 0xfc || (p[17] & 0xf8) != 0xf8 ||
      (p[18] & 0xf8) != 0xf8 || (p[21] & 3) != 3 || p[22] != 3) return false;
  uint32_t at = 23; const uint8_t *sets[3] = {}; uint32_t lengths[3] = {};
  for (uint32_t i = 0; i < p[22]; ++i) {
    if (n - at < 5 || (p[at] & 0x40)) return false;
    const uint32_t type = p[at++] & 63;
    if (type < 32 || type > 34 || sets[type - 32] || p[at] || p[at + 1] != 1) return false;
    at += 2; const uint32_t bytes = uint32_t(p[at]) * 256 + p[at + 1]; at += 2;
    if (!bytes || bytes > MaxParameter || bytes > n - at) return false;
    sets[type - 32] = p + at; lengths[type - 32] = bytes; at += bytes;
  }
  if (at != n || !sets[0] || !sets[1] || !sets[2] || !vps(sets[0], lengths[0], c.v) ||
      !sps(sets[1], lengths[1], c.v, c.s) || !pps(sets[2], lengths[2], c.s, c.p)) return false;
  Ptl hv; hv.profile = p[1] & 31; hv.tier = (p[1] >> 5) & 1;
  hv.compatibility = uint32_t(p[2]) << 24 | uint32_t(p[3]) << 16 | uint32_t(p[4]) << 8 | p[5];
  for (unsigned i = 6; i < 12; ++i) hv.constraints = (hv.constraints << 8) | p[i]; hv.level = p[12];
  if ((p[1] >> 6) || !hv.same(c.s.pt) || (p[16] & 3) != 1 || (p[17] & 7) + 8 != c.s.depth ||
      (p[18] & 7) + 8 != c.s.depth || ((p[21] >> 3) & 7) != c.s.subLayers ||
      bool(p[21] & 4) != c.s.nested) return false;
  memcpy(c.vps, sets[0], lengths[0]); memcpy(c.sps, sets[1], lengths[1]); memcpy(c.pps, sets[2], lengths[2]);
  c.vpsBytes = lengths[0]; c.spsBytes = lengths[1]; c.ppsBytes = lengths[2]; return true;
}
struct Picture {
  bool idr = false, reference = false, first = false, dependent = false, output = true;
  uint32_t nalType = 0, temporalId = 0, pps = 0, address = 0, type = 0, poc = 0;
  bool same(const Picture &p) const { return nalType == p.nalType && temporalId == p.temporalId && pps == p.pps &&
    (dependent || p.dependent || (type == p.type && poc == p.poc && output == p.output)); }
};
inline bool weights(Bits &b, uint32_t l0, uint32_t l1, bool bi) {
  const uint32_t luma = b.ue(7); const int32_t chroma = int32_t(luma) + b.se(-7, 7);
  if (chroma < 0 || chroma > 7) return false;
  for (unsigned list = 0; list < (bi ? 2u : 1u); ++list) {
    bool y[15] = {}, c[15] = {}; const uint32_t count = list ? l1 : l0;
    for (uint32_t i = 0; i < count; ++i) y[i] = b.read(1);
    for (uint32_t i = 0; i < count; ++i) c[i] = b.read(1);
    for (uint32_t i = 0; i < count; ++i) {
      if (y[i]) { b.se(-128, 127); b.se(-128, 127); }
      if (c[i]) for (unsigned j = 0; j < 2; ++j) { b.se(-128, 127); b.se(-512, 511); }
    }
  }
  return b.good;
}
inline bool slice(const uint8_t *p, uint32_t n, const Config &c, Picture &v) {
  Nal h; if (!c.valid() || !header(p, n, h) || !((h.type <= 9) || (h.type >= 16 && h.type <= 21)) || h.temporalId >= c.s.subLayers) return false;
  Bits b{p + 2, n - 2}; v = Picture{}; v.nalType = h.type; v.temporalId = h.temporalId;
  v.idr = h.type == 19 || h.type == 20; v.reference = h.type >= 16 || (h.type & 1); v.first = b.read(1);
  if (h.type >= 16) b.read(1); v.pps = b.ue(63); if (v.pps != c.p.id) return false;
  if (!v.first) {
    if (c.p.dependent) v.dependent = b.read(1);
    v.address = b.read(ceilLog2(c.s.ctbs())); if (!v.address || v.address >= c.s.ctbs()) return false;
  }
  if (!v.dependent) {
    if (b.read(c.p.extraBits)) return false;
    v.type = b.ue(2); if (c.p.outputFlag) v.output = b.read(1);
    uint32_t used = 0; bool temporalMvp = false;
    if (!v.idr) {
      v.poc = b.read(c.s.pocBits);
      if (b.read(1)) {
        if (!c.s.numRps) return false;
        const uint32_t index = c.s.numRps > 1 ? b.read(ceilLog2(c.s.numRps)) : 0;
        if (index >= c.s.numRps) return false; used = c.s.rps[index].usedCount;
      } else {
        Rps local; if (!shortRps(b, c.s.numRps, c.s.numRps, c.s.rps, c.s.maxDpb - 1, local)) return false; used = local.usedCount;
      }
      if (c.s.longTerm) {
        const uint32_t fromSps = c.s.numLongTerm ? b.ue(c.s.numLongTerm) : 0, extra = b.ue(c.s.maxDpb - 1);
        if (!b.good || fromSps + extra > c.s.maxDpb - 1) return false;
        for (uint32_t i = 0; i < fromSps + extra; ++i) {
          if (i < fromSps) {
            const uint32_t index = c.s.numLongTerm > 1 ? b.read(ceilLog2(c.s.numLongTerm)) : 0;
            if (index >= c.s.numLongTerm) return false; if (c.s.longTermUsed & (1u << index)) ++used;
          } else { b.read(c.s.pocBits); if (b.read(1)) ++used; }
          if (b.read(1)) b.ue();
        }
      }
      if (c.s.temporalMvp) temporalMvp = b.read(1);
    } else if (v.type != 2) return false;
    const bool saoY = c.s.sao && b.read(1), saoC = c.s.sao && b.read(1);
    if (v.type != 2) {
      if (!used || used >= c.s.maxDpb) return false;
      uint32_t l0 = c.p.refsL0, l1 = c.p.refsL1;
      if (b.read(1)) { l0 = b.ue(14) + 1; if (v.type == 0) l1 = b.ue(14) + 1; }
      if (c.p.listsMod && used > 1) {
        if (b.read(1)) for (uint32_t i = 0; i < l0; ++i) if (b.read(ceilLog2(used)) >= used) return false;
        if (v.type == 0 && b.read(1)) for (uint32_t i = 0; i < l1; ++i) if (b.read(ceilLog2(used)) >= used) return false;
      }
      if (v.type == 0) b.read(1); if (c.p.cabac) b.read(1);
      if (temporalMvp) { const bool fromL0 = v.type != 0 || b.read(1); const uint32_t count = fromL0 ? l0 : l1; if (count > 1) b.ue(count - 1); }
      if (((v.type == 1 && c.p.weighted) || (v.type == 0 && c.p.weightedBi)) && !weights(b, l0, l1, v.type == 0)) return false;
      b.ue(4);
    }
    const int32_t qp = 26 + c.p.qp + b.se(-128, 127);
    if (qp < -6 * int32_t(c.s.depth - 8) || qp > 51) return false;
    if (c.p.chromaQp) { const int32_t cb = b.se(-12, 12) + c.p.cbQp, cr = b.se(-12, 12) + c.p.crQp; if (cb < -12 || cb > 12 || cr < -12 || cr > 12) return false; }
    bool disabled = c.p.deblockDisabled;
    if (c.p.deblockOverride && b.read(1)) { disabled = b.read(1); if (!disabled) { b.se(-6, 6); b.se(-6, 6); } }
    if (c.p.loopAcross && (saoY || saoC || !disabled)) b.read(1);
  }
  uint64_t entryExtent = 0;
  if (c.p.tiles || c.p.wpp) {
    const uint32_t entries = b.ue(c.s.ctbs() - 1); if (!b.good || entries > 8192) return false;
    if (entries) {
      const uint32_t bits = b.ue(31) + 1;
      for (uint32_t i = 0; i < entries; ++i) { entryExtent += uint64_t(b.read(bits)) + 1; if (entryExtent > n) return false; }
    }
  }
  if (c.p.headerExtension) { const uint32_t bytes = b.ue(256); if (!b.good) return false; for (uint32_t i = 0; i < bytes; ++i) b.read(8); }
  // Slice data stays opaque to the host, but a truncated header or empty CABAC
  // payload is never admitted. The firmware validates coding-tree syntax.
  return b.align() && b.byte < b.n && entryExtent < b.n - b.byte;
}
inline bool sei(const uint8_t *p, uint32_t n) {
  Bits b{p + 2, n - 2};
  while (b.good && b.byte < b.n) {
    Bits tail = b; if (tail.end()) return true;
    uint32_t type = 0, size = 0, value;
    do { value = b.read(8); if (type > MaxAu - value) return false; type += value; } while (b.good && value == 255);
    do { value = b.read(8); if (size > MaxAu - value) return false; size += value; } while (b.good && value == 255);
    if (!b.good || size > b.n - b.byte) return false;
    for (uint32_t i = 0; i < size; ++i) b.read(8);
  }
  return false;
}
inline bool walk(const uint8_t *p, uint32_t n, const Config &c, bool annexB, Picture *picture) {
  if (!p || !c.valid() || n < 7 || n > MaxAu || (!annexB && n > MaxAu - c.parameterBytes())) return false;
  uint32_t at = 0, count = 0, previous = 0; bool have = false, terminal = false; Picture first;
  while (at != n) {
    if (n - at < 7 || ++count > (annexB ? 67u : 64u)) return false;
    uint32_t length;
    if (annexB) {
      if (p[at] || p[at + 1] || p[at + 2] || p[at + 3] != 1) return false;
      const uint32_t begin = at + 4; uint32_t end = begin;
      while (end + 4 <= n && (p[end] || p[end + 1] || p[end + 2] || p[end + 3] != 1)) ++end;
      if (end + 4 > n) end = n; length = end - begin;
    } else length = uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | p[at + 3];
    at += 4; Nal h;
    if (length > n - at || !ebsp(p + at, length) || !header(p + at, length, h) || h.temporalId >= c.s.subLayers) return false;
    if (h.type <= 31) {
      Picture current; if (terminal || !slice(p + at, length, c, current)) return false;
      if (!have) { if (!current.first || current.dependent) return false; first = current; have = true; }
      else if (current.first || !first.same(current) || current.address <= previous) return false;
      previous = current.address;
    } else if (h.type >= 32 && h.type <= 34) {
      const uint8_t *expected = h.type == 32 ? c.vps : h.type == 33 ? c.sps : c.pps;
      const uint32_t bytes = h.type == 32 ? c.vpsBytes : h.type == 33 ? c.spsBytes : c.ppsBytes;
      if (have || length != bytes || memcmp(p + at, expected, bytes)) return false;
    } else if (h.type == 35) {
      if (have) return false; Bits b{p + at + 2, length - 2}; if (b.read(3) > 2 || !b.end()) return false;
    } else if (h.type == 39 || h.type == 40) {
      if ((h.type == 39 && have) || (h.type == 40 && !have) || !sei(p + at, length)) return false;
    } else if (h.type == 36 || h.type == 37) {
      if (!have || length != 3 || p[at + 2] != 0x80) return false; terminal = true;
    } else if (h.type == 38) {
      Bits b{p + at + 2, length - 2}; while (b.good && b.byte + 1 < b.n) if (b.read(8) != 255) return false;
      if (!b.end()) return false;
    } else return false;
    at += length;
  }
  if (have && picture) *picture = first;
  return have;
}
inline bool accessUnit(const uint8_t *p, uint32_t n, const Config &c, Picture *picture = nullptr) { return walk(p, n, c, false, picture); }
inline bool normalized(const uint8_t *p, uint32_t n, const Config &c, Picture *picture = nullptr) { return walk(p, n, c, true, picture); }
inline uint32_t normalize(const uint8_t *p, uint32_t n, uint8_t *out, uint32_t cap, const Config &c, bool prepend) {
  if (!out || !accessUnit(p, n, c)) return 0;
  const uint32_t prefix = prepend ? c.parameterBytes() : 0;
  if (n > cap || prefix > cap - n || prefix + n > MaxAu) return 0;
  const uint8_t start[4] = {0, 0, 0, 1}; uint32_t at = 0;
  if (prepend) {
    const uint8_t *sets[3] = {c.vps, c.sps, c.pps}; const uint32_t lengths[3] = {c.vpsBytes, c.spsBytes, c.ppsBytes};
    for (unsigned i = 0; i < 3; ++i) { memcpy(out + at, start, 4); memcpy(out + at + 4, sets[i], lengths[i]); at += 4 + lengths[i]; }
  }
  for (uint32_t in = 0; in < n;) {
    const uint32_t length = uint32_t(p[in]) << 24 | uint32_t(p[in + 1]) << 16 | uint32_t(p[in + 2]) << 8 | p[in + 3];
    memcpy(out + at, start, 4); memcpy(out + at + 4, p + in + 4, length); in += 4 + length; at += 4 + length;
  }
  return at;
}
} }
