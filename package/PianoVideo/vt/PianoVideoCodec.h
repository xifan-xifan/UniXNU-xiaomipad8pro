// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Shared, codec-aware native admission. The proven AVC parser stays unchanged.
#pragma once
#include "PianoVideoAVC.h"
#include "PianoVideoHEVC.h"
namespace pianovideo { namespace codec {
// Experimental native envelope. Keep the wider parser for offline analysis;
// unqualified 4K cold reopen must be declined before a hardware lease starts.
constexpr uint32_t kHevcMaxLevel = 120, kHevcMaxWidth = 1920, kHevcMaxHeight = 1080, kHevcMaxCodedHeight = 1088;
struct Config {
  avc::Config avc; hevc::Config hevc; avc::Sps s; bool hevcMode = false; uint32_t depth = 8;
  bool isHevc() const { return hevcMode; }
  bool valid() const { return hevcMode ? hevc.valid() : avc.valid(); }
  bool same(const Config &c) const { return hevcMode == c.hevcMode && (hevcMode ? hevc.same(c.hevc) : avc.same(c.avc)); }
  uint32_t parameterBytes() const { return hevcMode ? hevc.parameterBytes() : avc.parameterBytes(); }
  uint32_t hfiProfile() const { return hevcMode ? hevc.hfiProfile() : avc.hfiProfile(); }
  uint32_t hfiLevel() const { return hevcMode ? hevc.hfiLevel() : avc.hfiLevel(); }
  uint32_t hfiTier() const { return hevcMode ? hevc.hfiTier() : 0; }
  uint32_t format() const { return hevcMode ? 0x68766331u : 0x61766331u; } // hvc1 / avc1
  uint32_t pixelFormat() const { return depth == 10 ? 0x78343230u : 0x34323076u; } // x420 / 420v
};
inline bool nativeAdmitted(const Config &c) {
  return c.valid() && (!c.isHevc() || (c.s.width <= kHevcMaxWidth && c.s.height <= kHevcMaxHeight &&
    c.s.codedWidth <= kHevcMaxWidth && c.s.codedHeight <= kHevcMaxCodedHeight && c.s.level <= kHevcMaxLevel));
}
inline bool parseAvc(const uint8_t *p, uint32_t n, Config &c) {
  memset(&c, 0, sizeof(c)); c.depth = 8;
  if (!avc::config(p, n, c.avc)) return false; c.s = c.avc.s; return true;
}
inline bool parseHevc(const uint8_t *p, uint32_t n, Config &c) {
  memset(&c, 0, sizeof(c)); c.hevcMode = true; c.depth = 8;
  if (!hevc::config(p, n, c.hevc)) return false;
  const hevc::Sps &s = c.hevc.s; c.depth = s.depth;
  c.s.id = s.id; c.s.width = s.width; c.s.height = s.height; c.s.codedWidth = s.codedWidth; c.s.codedHeight = s.codedHeight;
  c.s.left = s.left; c.s.top = s.top; c.s.right = s.right; c.s.bottom = s.bottom;
  c.s.profile = s.pt.profile; c.s.level = s.pt.level; c.s.color = s.color; c.s.maxReorder = s.maxReorder;
  c.s.maxDpb = s.maxDpb; c.s.refs = s.maxDpb - 1; c.s.pocType = 0; c.s.pocBits = s.pocBits; return true;
}
struct Picture { bool idr = false, reference = false; uint32_t nalType = 0, temporalId = 0; };
inline bool accessUnit(const uint8_t *p, uint32_t n, const Config &c, Picture *picture = nullptr) {
  Picture out;
  if (c.isHevc()) {
    hevc::Picture v; if (!hevc::accessUnit(p, n, c.hevc, &v)) return false;
    out.idr = v.idr; out.reference = v.reference; out.nalType = v.nalType; out.temporalId = v.temporalId;
  } else {
    avc::Picture v; if (!avc::accessUnit(p, n, c.avc, &v)) return false;
    out.idr = v.idr; out.reference = v.reference; out.nalType = v.idr ? 5 : 1;
  }
  if (picture) *picture = out; return true;
}
inline bool normalized(const uint8_t *p, uint32_t n, const Config &c, Picture *picture = nullptr) {
  Picture out;
  if (c.isHevc()) {
    hevc::Picture v; if (!hevc::normalized(p, n, c.hevc, &v)) return false;
    out.idr = v.idr; out.reference = v.reference; out.nalType = v.nalType; out.temporalId = v.temporalId;
  } else {
    avc::Picture v; if (!avc::normalized(p, n, c.avc, &v)) return false;
    out.idr = v.idr; out.reference = v.reference; out.nalType = v.idr ? 5 : 1;
  }
  if (picture) *picture = out; return true;
}
inline uint32_t normalize(const uint8_t *p, uint32_t n, uint8_t *out, uint32_t cap, const Config &c, bool prepend) {
  return c.isHevc() ? hevc::normalize(p, n, out, cap, c.hevc, prepend) : avc::normalize(p, n, out, cap, c.avc, prepend);
}
} }
