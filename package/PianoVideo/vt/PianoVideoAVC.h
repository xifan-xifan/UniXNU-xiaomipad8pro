// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Bounded progressive 8-bit 4:2:0 AVC admission, through 1920x1080 High.
// This parser validates geometry, parameter sets and
// picture identity before any caller bytes can reach the Iris DMA buffers.
#pragma once
#include <stdint.h>
#include <string.h>
namespace pianovideo { namespace avc {
constexpr uint32_t MaxAu = 1u << 20, MaxParameter = 256;
struct Bits {
  const uint8_t *p; uint32_t n, byte = 0, bit = 0, zeros = 0; bool good = true;
  uint32_t read(unsigned count) {
    if (count > 32) { good = false; return 0; }
    uint32_t v = 0;
    for (unsigned i = 0; i < count; ++i) {
      if (!bit && byte < n && zeros >= 2 && p[byte] == 3) { ++byte; zeros = 0; }
      if (byte >= n) { good = false; return 0; }
      v = (v << 1) | ((p[byte] >> (7 - bit)) & 1);
      if (++bit == 8) { zeros = p[byte] ? 0 : zeros + 1; ++byte; bit = 0; }
    }
    return v;
  }
  uint32_t ue() {
    unsigned k = 0;
    while (good && !read(1)) if (++k > 20) { good = false; return 0; }
    return good ? (1u << k) - 1 + read(k) : 0;
  }
  int32_t se() { uint32_t v = ue(); return (v & 1) ? int32_t((v + 1) / 2) : -int32_t(v / 2); }
  bool end() {
    if (!good || read(1) != 1) return false;
    while (bit) if (read(1)) return false;
    return good && byte == n;
  }
};
// Validate EBSP separately: forbidden start codes and invalid emulation bytes
// cannot be hidden from the length-delimited AU walker.
inline bool ebsp(const uint8_t *p, uint32_t n) {
  if (!p || n < 2 || (p[0] & 0x80)) return false;
  unsigned zeros = 0;
  for (uint32_t i = 1; i < n; ++i) {
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
struct Sps {
  uint32_t id = 0, profile = 0, level = 0, constraints = 0, refs = 0, pocType = 0, frameBits = 0, pocBits = 0;
  uint32_t maxReorder = 0, maxDpb = 0;
  uint32_t codedWidth = 0, codedHeight = 0, width = 0, height = 0;
  uint32_t left = 0, top = 0, right = 0, bottom = 0;
  uint32_t color = 0x14000003; // unspecified video signal, limited-range
  uint32_t macroblocks() const { return (codedWidth / 16) * (codedHeight / 16); }
};
struct Pps { uint32_t id = 0, spsId = 0; bool bottomPoc = false, entropy = false; };
// Parameter-set scaling lists are syntax, not a host pixel operation. Parse
// them completely so transform8x8/scaling presence cannot shift picture fields.
inline bool scaling(Bits &b, unsigned count) {
  int last = 8, next = 8;
  for (unsigned i = 0; i < count; ++i) {
    if (next) { const int delta = b.se(); if (!b.good || delta < -128 || delta > 127) return false; next = (last + delta + 256) % 256; }
    if (next) last = next;
  }
  return b.good;
}
inline bool hrd(Bits &b) {
  const uint32_t count = b.ue(); if (!b.good || count > 31) return false;
  b.read(4); b.read(4);
  for (uint32_t i = 0; i <= count; ++i) { b.ue(); b.ue(); b.read(1); }
  b.read(5); b.read(5); b.read(5); b.read(5); return b.good;
}
inline bool sps(const uint8_t *p, uint32_t n, Sps &s) {
  if (n > MaxParameter || !ebsp(p, n) || (p[0] & 31) != 7) return false;
  Bits b{p + 1, n - 1}; s = Sps{};
  s.profile = b.read(8); s.constraints = b.read(8); s.level = b.read(8); s.id = b.ue();
  if ((s.profile != 66 && s.profile != 77 && s.profile != 100) || (s.constraints & 3) || !s.level || s.level > 42 || s.id > 31) return false;
  if (s.profile == 100) {
    if (b.ue() != 1 || b.ue() != 0 || b.ue() != 0 || b.read(1)) return false; // 8-bit 420, no transform bypass
    if (b.read(1)) for (unsigned i = 0; i < 8; ++i) if (b.read(1) && !scaling(b, i < 6 ? 16 : 64)) return false;
  }
  s.frameBits = b.ue() + 4; s.pocType = b.ue();
  if (s.frameBits > 16 || (s.pocType != 0 && s.pocType != 2)) return false;
  if (!s.pocType) { s.pocBits = b.ue() + 4; if (s.pocBits > 16) return false; }
  s.refs = b.ue();
  if (s.refs > 4 || b.read(1)) return false; // no frame_num gaps
  const uint32_t mbw = b.ue() + 1, mbh = b.ue() + 1;
  if (!b.good || !mbw || !mbh || mbw > 120 || mbh > 68 || !b.read(1)) return false; // progressive only
  s.codedWidth = mbw * 16; s.codedHeight = mbh * 16;
  b.read(1); // direct_8x8_inference_flag
  if (b.read(1)) { s.left = b.ue()*2; s.right = b.ue()*2; s.top = b.ue()*2; s.bottom = b.ue()*2; }
  if (!b.good || s.left + s.right >= s.codedWidth || s.top + s.bottom >= s.codedHeight) return false;
  s.width = s.codedWidth - s.left - s.right; s.height = s.codedHeight - s.top - s.bottom;
  if (s.width < 96 || s.height < 96 || s.width > 1920 || s.height > 1080) return false;
  s.maxReorder = s.pocType ? 0 : s.refs; s.maxDpb = s.refs;
  if (b.read(1)) {
    if (b.read(1)) { const uint32_t aspect = b.read(8); if (aspect == 255) { if (!b.read(16) || !b.read(16)) return false; } else if (!aspect || aspect > 16) return false; }
    if (b.read(1)) b.read(1);
    if (b.read(1)) {
      const uint32_t format=b.read(3); if (b.read(1)) return false; // 420v is video-range
      const bool description=b.read(1);uint32_t primaries=0,transfer=0,matrix=3;
      if(description){primaries=b.read(8);transfer=b.read(8);matrix=b.read(8);}
      s.color=matrix|(transfer<<8)|(primaries<<16)|(uint32_t(description)<<24)|(format<<26)|(1u<<29);
    }
    if (b.read(1)) { if (b.ue() > 5 || b.ue() > 5) return false; }
    if (b.read(1)) { uint32_t tick = b.read(32), scale = b.read(32); b.read(1); if (!tick || !scale) return false; }
    const bool nalHrd = b.read(1); if (nalHrd && !hrd(b)) return false;
    const bool vclHrd = b.read(1); if (vclHrd && !hrd(b)) return false;
    if (nalHrd || vclHrd) b.read(1);
    b.read(1);
    if (b.read(1)) {
      b.read(1);
      if (b.ue() > 16 || b.ue() > 16 || b.ue() > 16 || b.ue() > 16) return false;
      s.maxReorder = b.ue(); s.maxDpb = b.ue();
    }
  }
  return b.end() && s.maxReorder <= 4 && s.maxDpb >= s.refs && s.maxDpb <= 8 && s.maxReorder <= s.maxDpb;
}
inline bool pps(const uint8_t *p, uint32_t n, const Sps &s, Pps &v) {
  if (n > MaxParameter || !ebsp(p, n) || (p[0] & 31) != 8) return false;
  Bits b{p + 1, n - 1}; v = Pps{};
  v.id = b.ue(); v.spsId = b.ue(); v.entropy = b.read(1); v.bottomPoc = b.read(1);
  if (v.id > 255 || v.spsId != s.id || (s.profile == 66 && v.entropy) || b.ue() != 0) return false; // no FMO
  if (b.ue() > 3 || b.ue() > 3) return false;
  const bool weighted = b.read(1); const uint32_t bipred = b.read(2);
  if (bipred > 2 || (s.profile == 66 && (weighted || bipred))) return false;
  const int qp = b.se(), qs = b.se(), chroma = b.se();
  if (qp < -26 || qp > 25 || qs < -26 || qs > 25 || chroma < -12 || chroma > 12) return false;
  b.read(1); b.read(1); if (b.read(1)) return false; // no redundant picture slices
  Bits tail = b;
  if (tail.end()) return true;
  if (s.profile != 100) return false;
  const bool transform8 = b.read(1);
  if (b.read(1)) for (unsigned i = 0; i < (transform8 ? 8u : 6u); ++i)
    if (b.read(1) && !scaling(b, i < 6 ? 16 : 64)) return false;
  const int secondChroma = b.se();
  return secondChroma >= -12 && secondChroma <= 12 && b.end();
}
struct Config {
  uint8_t sps[MaxParameter] = {}, pps[MaxParameter] = {};
  uint32_t spsBytes = 0, ppsBytes = 0;
  Sps s; Pps p;
  uint32_t pocType = 0;
  // Gen2 uses the V4L2 enumeration: Baseline0, ConstrainedBaseline1,
  // Main2 (UniXNU-sheng QcomIrisSession.h at the UPSTREAM reference pin).
  uint32_t hfiProfile() const { return s.profile == 100 ? 4 : s.profile == 77 ? 2 : 0; }
  uint32_t hfiLevel() const {
    switch (s.level) { case 10: return 0; case 11: return (s.constraints & 0x10) ? 1 : 2; case 12: return 3; case 13: return 4;
      case 20: return 5; case 21: return 6; case 22: return 7; case 30: return 8; case 31: return 9; case 32: return 10; case 40: return 11; case 41: return 12; case 42: return 13; default: return UINT32_MAX; }
  }
  bool valid() const { return spsBytes && ppsBytes; }
  bool same(const Config &v) const { return valid() && v.valid() && spsBytes == v.spsBytes && ppsBytes == v.ppsBytes &&
    !memcmp(sps, v.sps, spsBytes) && !memcmp(pps, v.pps, ppsBytes); }
  uint32_t parameterBytes() const { return 8 + spsBytes + ppsBytes; }
};
inline bool config(const uint8_t *p, uint32_t n, Config &c) {
  c = Config{};
  if (!p || n < 11 || p[0] != 1 || p[4] != 0xff || p[5] != 0xe1) return false;
  const uint32_t ns = uint32_t(p[6]) * 256 + p[7];
  if (!ns || ns > MaxParameter || ns > n - 11 || p[8 + ns] != 1) return false;
  const uint32_t np = uint32_t(p[9 + ns]) * 256 + p[10 + ns];
  if (!np || np > MaxParameter || n < 11 + ns + np || !sps(p + 8, ns, c.s) || !pps(p + 11 + ns, np, c.s, c.p)) return false;
  const uint32_t base = 11 + ns + np;
  if (n != base && (c.s.profile != 100 || n != base + 4 || p[base] != 0xfd || p[base+1] != 0xf8 || p[base+2] != 0xf8 || p[base+3])) return false;
  if (p[1] != c.s.profile || p[2] != p[10] || p[3] != c.s.level) return false;
  memcpy(c.sps, p + 8, ns); memcpy(c.pps, p + 11 + ns, np); c.spsBytes = ns; c.ppsBytes = np; c.pocType = c.s.pocType;
  return c.hfiLevel() != UINT32_MAX;
}
struct Picture {
  uint32_t firstMb = 0, type = 0, pps = 0, frame = 0, poc = 0, id = 0;
  int32_t deltaPoc = 0;
  bool idr = false, reference = false;
  bool same(const Picture &p) const { return type == p.type && pps == p.pps && frame == p.frame && poc == p.poc && id == p.id &&
    deltaPoc == p.deltaPoc && idr == p.idr && reference == p.reference; }
};
inline bool slice(const uint8_t *p, uint32_t n, const Config &c, Picture &v) {
  if (!c.valid() || !ebsp(p, n)) return false;
  const unsigned nal = p[0] & 31;
  if (nal != 1 && nal != 5) return false;
  Bits b{p + 1, n - 1}; v = Picture{};
  v.firstMb = b.ue(); const auto kind = b.ue(); v.type = kind % 5; v.pps = b.ue();
  if (v.firstMb >= c.s.macroblocks() || kind > 9 || v.type > 2 || (c.s.profile == 66 && v.type == 1) || v.pps != c.p.id) return false;
  v.frame = b.read(c.s.frameBits); v.idr = nal == 5; v.reference = (p[0] & 0x60) != 0;
  if (v.idr) { v.id = b.ue(); if (v.id > 65535 || v.type != 2 || !v.reference) return false; }
  if (!c.s.pocType) { v.poc = b.read(c.s.pocBits); if (c.p.bottomPoc) v.deltaPoc = b.se(); }
  return b.good;
}
inline bool accessUnit(const uint8_t *p, uint32_t n, const Config &c, Picture *picture = nullptr) {
  if (!p || !c.valid() || n < 6 || n > MaxAu - c.parameterBytes()) return false;
  uint32_t at = 0, nalCount = 0, previousMb = 0; bool have = false;
  Picture first;
  while (at != n) {
    if (n - at < 4 || ++nalCount > 64) return false;
    const uint32_t len = uint32_t(p[at]) << 24 | uint32_t(p[at+1]) << 16 | uint32_t(p[at+2]) << 8 | p[at+3]; at += 4;
    if (!len || len > n - at || !ebsp(p + at, len)) return false;
    const uint32_t kind = p[at] & 31;
    if (kind == 1 || kind == 5) {
      Picture current;
      if (!slice(p + at, len, c, current)) return false;
      if (!have) { if (current.firstMb) return false; first = current; have = true; }
      else if (!first.same(current) || current.firstMb <= previousMb) return false;
      previousMb = current.firstMb;
    } else if (kind == 7 || kind == 8) {
      const uint8_t *expected = kind == 7 ? c.sps : c.pps;
      const uint32_t bytes = kind == 7 ? c.spsBytes : c.ppsBytes;
      if (have || len != bytes || memcmp(p + at, expected, bytes)) return false;
    } else if (kind == 9) {
      if (have || len != 2 || !(p[at + 1] & 0x10) || (p[at + 1] & 15)) return false;
    } else if (kind != 6 || have) return false;
    at += len;
  }
  if (have && picture) *picture = first;
  return have;
}
inline uint32_t normalize(const uint8_t *p, uint32_t n, uint8_t *out, uint32_t cap, const Config &c, bool prepend) {
  if (!out || !accessUnit(p, n, c)) return 0;
  const uint32_t prefix = prepend ? c.parameterBytes() : 0;
  if (n > cap || prefix > cap - n || prefix + n > MaxAu) return 0;
  uint32_t at = 0;
  const uint8_t start[4] = {0,0,0,1};
  if (prepend) {
    memcpy(out, start, 4); memcpy(out + 4, c.sps, c.spsBytes);
    at = c.spsBytes + 4; memcpy(out + at, start, 4); memcpy(out + at + 4, c.pps, c.ppsBytes); at += c.ppsBytes + 4;
  }
  uint32_t in = 0;
  while (in < n) {
    const uint32_t len = uint32_t(p[in]) << 24 | uint32_t(p[in+1]) << 16 | uint32_t(p[in+2]) << 8 | p[in+3];
    memcpy(out + at, start, 4); memcpy(out + at + 4, p + in + 4, len); in += len + 4; at += len + 4;
  }
  return at;
}
inline bool normalized(const uint8_t *p, uint32_t n, const Config &c, Picture *picture = nullptr) {
  if (!p || !c.valid() || n < 6 || n > MaxAu) return false;
  uint32_t at = 0, count = 0, previousMb = 0; bool have = false;
  Picture first;
  while (at != n) {
    if (n - at < 6 || p[at] || p[at+1] || p[at+2] || p[at+3] != 1 || ++count > 66) return false;
    const uint32_t begin = at + 4; uint32_t end = begin;
    while (end + 4 <= n && (p[end] || p[end+1] || p[end+2] || p[end+3] != 1)) ++end;
    if (end + 4 > n) end = n;
    const uint32_t len = end - begin, kind = p[begin] & 31;
    if (!ebsp(p + begin, len)) return false;
    if (kind == 1 || kind == 5) {
      Picture current; if (!slice(p + begin, len, c, current)) return false;
      if (!have) { if (current.firstMb) return false; first = current; have = true; }
      else if (!first.same(current) || current.firstMb <= previousMb) return false;
      previousMb = current.firstMb;
    } else if (kind == 7 || kind == 8) {
      const uint8_t *expected = kind == 7 ? c.sps : c.pps;
      const uint32_t bytes = kind == 7 ? c.spsBytes : c.ppsBytes;
      if (have || len != bytes || memcmp(p + begin, expected, bytes)) return false;
    } else if (kind == 9) {
      if (have || len != 2 || !(p[begin+1] & 0x10) || (p[begin+1] & 15)) return false;
    } else if (kind != 6 || have) return false;
    at = end;
  }
  if (have && picture) *picture = first;
  return have;
}
} }
