// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// PianoVideo E1 (piano-video-enc=1): the portable half of one bounded native H.264 encode session on Iris (VPU35, HFI gen2).
// Portable (host tests in tests/enc_host.cpp).
//
// The specification (GPL-2.0, read only; nothing copied): Linux Iris at b5100d5f2e0b in linux-sm8750-piano
// (iris_venc.c streamon order, iris_hfi_gen2_command.c set_codec / set_config_params / get_port_from_buf_type / set_num_comv,
// iris_hfi_gen2.c inst_fw_cap_sm8550_enc / sm8550_venc_*_config_params / sm8550_enc_*_int_buf_tbl, iris_ctrls.c setters,
// iris_buffer.c NV12 and bitstream sizes, iris_vpu_buffer.c iris_vpu33_buf_size for the internal buffers). The internal buffer
// sizes below are numbers produced by running that function on the host at the reference pin;
// they are facts about the firmware's needs, not code. Property and buffer ids are the gen2 interface's numbers.
#pragma once
#include <stdint.h>
#include "VideoBuffers.h"

namespace pianovideo
{
namespace enc
{
// Require one explicit enable token on the merged boot line. Bare or conflicting keys leave E1 off.
inline bool bootEnabled(const char *args)
{
  constexpr char key[] = "piano-video-enc";
  constexpr int keyLength = sizeof key - 1;
  bool enabled = false;
  while (*args) {
    while (*args == ' ' || *args == '\t') ++args;
    const char *end = args;
    while (*end && *end != ' ' && *end != '\t') ++end;
    const auto length = end - args;
    if (length >= keyLength && __builtin_memcmp(args, key, keyLength) == 0 &&
        (length == keyLength || args[keyLength] == '=')) {
      if (enabled || length != keyLength + 2 || args[keyLength + 1] != '1') return false;
      enabled = true;
    }
    args = end;
  }
  return enabled;
}

// ------------------------------------------------------------------------------------------------ the interface's numbers
enum : uint32_t {
  PropCodec = 0x03000100, PropColorFormat = 0x03000101, PropBitstreamResolution = 0x03000103, PropStride = 0x03000104,
  PropCrop = 0x03000105, PropProfile = 0x03000107, PropLevel = 0x03000108, PropStage = 0x0300010a, PropFrameRate = 0x0300010c,
  PropCabac = 0x03000121, PropHostMaxCount = 0x03000123, PropDeblockingMode = 0x03000129, PropRateControl = 0x0300012a,
  PropTimeDeltaBasedRateControl = 0x0300012b, PropIrRandomPeriod = 0x03000131,
  PropTotalBitrate = 0x0300013b, PropRequestSyncFrame = 0x03000145, PropMaxGopFrames = 0x03000146, PropMaxBFrames = 0x03000147,
  PropSeqHeaderMode = 0x03000149, PropFlip = 0x0300014c, PropColorInfo = 0x03000155, PropPictureType = 0x03000162,
  PropRawResolution = 0x03000178, PropComvCount = 0x03000193,
};
enum : uint32_t { PayloadU32 = 1, PayloadStructure = 5, PayloadQ16 = 8, PayloadEnum = 9, Payload32Packed = 10, Payload64Packed = 13 };
enum : uint32_t { PortNone = 0, PortBitstream = 1, PortRaw = 2 };
enum : uint32_t { CodecEncodeAvc = 2, ColorNv12 = 1, HeaderJoined = 2, RcVbrCfr = 0, SyncWithoutHeader = 1 };
enum : uint32_t { ProfileHigh = 4, Level50 = 14, Stage2 = 2, HostMaxCount = 64, ComvCount = 4 };
// HFI encoder buffer types (DPB is the SCRATCH_2 reconstruction allocation).
enum : uint32_t { BufBitstream = 1, BufRaw = 2, BufDpb = 6, BufBin = 7, BufLine = 8, BufArp = 9, BufComv = 10, BufNonComv = 11, BufVpss = 13 };
// Picture types of HFI_PROP_PICTURE_TYPE (gen2): IDR 1, P 2, B 4, I 8.
enum : uint32_t { PictureIdr = 1, PictureP = 2, PictureB = 4, PictureI = 8 };
constexpr uint32_t kColorInfoUnspecified = 0x14000003;   // matrix reserved (3), video format unspecified (5 << 26)
constexpr uint32_t kFlagLast = 0x10000000, kFlagReadonly = 0x10, kFlagReleaseDone = 1, kHostNonSecure = 0x200;

// ------------------------------------------------------------------------- VPU35 internal buffer sizes (H.264, VBR, stage 2)
struct Internal {
  uint32_t width, height, bin, comv, nonComv, line, dpb, vpss;
};
constexpr Internal kInternal[] = {
    {320, 240, 5969408, 41472, 274176, 619776, 626688, 131072},
    {640, 480, 23787008, 147456, 313856, 700928, 1921024, 0},
    {1280, 720, 9563648, 436224, 392448, 834048, 5804032, 1449984},
    // Both padded formats are 1920x1088. Size BIN for the coded height and skip standalone VPSS.
    {1920, 1080, 21661184, 983040, 542720, 984576, 12636160, 0},
};
constexpr unsigned kInternalCount = sizeof kInternal / sizeof kInternal[0];

constexpr uint32_t align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// ---------------------------------------------------------------------------------------------------------- the geometry
// Input NV12 as iris_venc_s_fmt_input makes it (stride ALIGN(w,128), scanlines ALIGN(h,32), UV scanlines ALIGN(h/2,16)); the
// coded size the bitstream port is told (16-aligned for H.264) and the crop that brings it back to w x h.
struct Geometry {
  uint32_t width = 0, height = 0, stride = 0, scanlines = 0, uvScanlines = 0, uvOffset = 0, rawBytes = 0, bitstreamBytes = 0;
  uint32_t codedWidth = 0, codedHeight = 0;
  Internal internal = {};
  bool configure(uint32_t w, uint32_t h)
  {
    *this = Geometry{};
    unsigned i = 0;
    for (; i < kInternalCount; ++i)
      if (kInternal[i].width == w && kInternal[i].height == h) break;
    if (i == kInternalCount) return false;   // E1: only sizes whose internal buffers were computed at the pin
    internal = kInternal[i];
    width = w;
    height = h;
    stride = align(w, 128);
    scanlines = align(h, 32);
    uvScanlines = align((h + 1) / 2, 16);
    uvOffset = stride * scanlines;
    rawBytes = align(stride * scanlines + stride * uvScanlines, 4096);
    const uint32_t aw = align(w, 32), ah = align(h, 32);
    uint32_t bits = aw * ah * 3;
    if (aw * ah > 4096u * 2176u) bits >>= 3;
    else if (aw * ah > 1280u * 720u) bits >>= 2;
    bitstreamBytes = align(bits, 4096);
    codedWidth = align(w, 16);
    codedHeight = align(h, 16);
    return true;
  }
  uint32_t rawResolution() const { return width << 16 | height; }
  uint32_t codedResolution() const { return codedWidth << 16 | codedHeight; }
  uint32_t codedCrop() const { return (codedWidth - width) << 16 | (codedHeight - height); }
};

struct Settings {
  uint32_t fps = 30, bitrate = 2000000, gop = 9, profile = ProfileHigh, level = Level50;
  unsigned frames = 10;
};
constexpr unsigned kMaxFrames = 120;
constexpr uint32_t kMaxFps = 60;
constexpr uint64_t kNsPerSecond = 1000000000ULL;

constexpr uint64_t frameNs(unsigned frame, uint32_t fps) { return uint64_t(frame) * kNsPerSecond / fps; }

inline unsigned frameIndex(uint64_t ns, uint32_t fps)
{
  // Bound the timestamp before multiplying. Invert floor(frame * 1e9 / fps)
  // with a ceiling division, then reject timestamps outside the exact schedule.
  if (!fps || fps > kMaxFps || ns > frameNs(kMaxFrames - 1, fps)) return kMaxFrames;
  const unsigned frame = unsigned((ns * fps + kNsPerSecond - 1) / kNsPerSecond);
  return frameNs(frame, fps) == ns ? frame : kMaxFrames;
}

// ------------------------------------------------------------------------------------------------------- the properties
struct Prop {
  uint32_t id = 0, port = 0, payload = 0, words = 0, v[2] = {0, 0};
};
struct Script {
  Prop p[24];
  unsigned n = 0;
  void add(uint32_t id, uint32_t port, uint32_t payload, uint32_t a, uint32_t words = 1, uint32_t b = 0)
  {
    if (n < sizeof p / sizeof p[0]) p[n++] = Prop{id, port, payload, words, {a, b}};
  }
};
// sm8550_venc_input_config_params, on the raw port.
inline Script inputConfig(const Geometry &g)
{
  Script s;
  s.add(PropColorFormat, PortRaw, PayloadEnum, ColorNv12);
  s.add(PropRawResolution, PortRaw, Payload32Packed, g.rawResolution());
  s.add(PropCrop, PortRaw, Payload64Packed, 0, 2, 0);
  s.add(PropStride, PortRaw, Payload64Packed, g.stride << 16 | g.scanlines, 2, g.stride << 16 | g.scanlines / 2);
  s.add(PropColorInfo, PortRaw, Payload32Packed, kColorInfoUnspecified);
  return s;
}
// sm8550_venc_output_config_params, on the bitstream port.
inline Script outputConfig(const Geometry &g, const Settings &c)
{
  Script s;
  s.add(PropBitstreamResolution, PortBitstream, Payload32Packed, g.codedResolution());
  s.add(PropCrop, PortBitstream, Payload64Packed, 0, 2, g.codedCrop());
  s.add(PropFrameRate, PortBitstream, PayloadQ16, c.fps << 16);
  return s;
}
// iris_set_properties' capability loop for H.264 (cap enum order; each distinct property once, with the port its flags select:
// INPUT_PORT first, then OUTPUT_PORT, else none). Setters that send nothing with these values are left out (min/max/frame QP
// without a client value under VBR, peak bitrate outside CBR, rotation 0, LTR count 0, layers off).
inline Script capabilities(const Settings &c)
{
  Script s;
  s.add(PropProfile, PortBitstream, PayloadEnum, c.profile);
  s.add(PropLevel, PortBitstream, PayloadEnum, c.level);
  s.add(PropHostMaxCount, PortRaw, PayloadU32, HostMaxCount);
  s.add(PropHostMaxCount, PortBitstream, PayloadU32, HostMaxCount);
  s.add(PropStage, PortNone, PayloadU32, Stage2);
  s.add(PropSeqHeaderMode, PortBitstream, PayloadEnum, HeaderJoined);
  s.add(PropTotalBitrate, PortRaw, PayloadU32, c.bitrate);
  s.add(PropRateControl, PortBitstream, PayloadEnum, RcVbrCfr);
  s.add(PropMaxGopFrames, PortRaw, PayloadU32, c.gop);
  s.add(PropCabac, PortBitstream, PayloadU32, 1);
  s.add(PropDeblockingMode, PortBitstream, Payload32Packed, 0x00060600);   // all boundaries, alpha/beta zero with +6 bias
  s.add(PropFlip, PortRaw, PayloadEnum, 0);
  s.add(PropIrRandomPeriod, PortBitstream, PayloadU32, 0);
  s.add(PropMaxBFrames, PortBitstream, PayloadU32, 0);
  s.add(PropRequestSyncFrame, PortRaw, PayloadEnum, SyncWithoutHeader);
  s.add(PropTimeDeltaBasedRateControl, PortBitstream, PayloadU32, 0);
  return s;
}
inline bool admitted(const Settings &c)
{
  return c.fps >= 1 && c.fps <= kMaxFps && c.bitrate >= 100000 && c.bitrate <= 40000000 && c.gop >= 1 && c.gop <= 300 &&
         c.frames >= 2 && c.frames <= kMaxFrames;
}

// ------------------------------------------------------------------------------------------------------------- the arena
// The shared V4 arena maps four buffers per external queue, session ARP, and both per-port internal tables.
constexpr unsigned kRaws = 4, kBitstreams = 4;
constexpr uint32_t kArpBytes = 204800;
inline bool buildLayout(session::Layout &l, const Geometry &g)
{
  l.used = l.bytes = 0;
  l.general = l.hevc = false;
  if (!g.width) return false;
  for (unsigned i = 0; i < kRaws; ++i)
    if (!l.add(BufRaw, i, g.rawBytes, PortRaw)) return false;
  for (unsigned i = 0; i < kBitstreams; ++i)
    if (!l.add(BufBitstream, i, g.bitstreamBytes, PortBitstream)) return false;
  if (!l.add(BufArp, 0, kArpBytes, PortNone)) return false;
  if (g.internal.vpss && !l.add(BufVpss, 0, g.internal.vpss, PortRaw)) return false;
  return l.add(BufBin, 0, g.internal.bin, PortBitstream) && l.add(BufComv, 0, g.internal.comv, PortBitstream) &&
         l.add(BufNonComv, 0, g.internal.nonComv, PortBitstream) && l.add(BufLine, 0, g.internal.line, PortBitstream) &&
         l.add(BufDpb, 0, g.internal.dpb, PortBitstream);
}
// The internal buffers in the order the session queues them (input first, then output).
constexpr uint32_t kInternalOrder[] = {BufVpss, BufBin, BufComv, BufNonComv, BufLine, BufDpb};

// ---------------------------------------------------------------------------------------------------- the test pattern
// A moving diagonal ramp in Y and slow ramps in Cb/Cr: the same integer formulas in tests/enc_stream_check.py.
inline uint8_t patternY(uint32_t x, uint32_t y, uint32_t f) { return uint8_t(16 + (x / 3 + y / 2 + 6 * f) % 220); }
inline uint8_t patternU(uint32_t, uint32_t y, uint32_t f) { return uint8_t(96 + (y / 8 + f) % 64); }
inline uint8_t patternV(uint32_t x, uint32_t, uint32_t f) { return uint8_t(96 + (x / 10 + 2 * f) % 64); }
// Writes frame f into an NV12 buffer of g's layout; padding bytes are 0.
template <class Store> void pattern(const Geometry &g, uint32_t f, Store store)
{
  for (uint32_t y = 0; y < g.scanlines; ++y)
    for (uint32_t x = 0; x < g.stride; ++x) store(y * g.stride + x, x < g.width && y < g.height ? patternY(x, y, f) : 0);
  for (uint32_t y = 0; y < g.uvScanlines; ++y)
    for (uint32_t x = 0; x < g.stride; x += 2) {
      const bool in = x < g.width && y < g.height / 2;
      store(g.uvOffset + y * g.stride + x, in ? patternU(x / 2, y, f) : 0);
      store(g.uvOffset + y * g.stride + x + 1, in ? patternV(x / 2, y, f) : 0);
    }
}

// --------------------------------------------------------------------------------------------------- the bitstream scan
// Annex B NAL units (3- or 4-byte start codes). Returns the number found (at most cap kept); *bad when the data does not start
// with a start code or a NAL is empty.
struct Nal {
  uint32_t offset = 0, size = 0;
  uint8_t type = 0;
};
inline unsigned scanAnnexB(const uint8_t *b, uint32_t n, Nal *out, unsigned cap, bool *bad)
{
  unsigned count = 0;
  *bad = false;
  auto start = [&](uint32_t i, uint32_t *len) {
    const uint32_t remaining = n - i;
    if (remaining >= 3 && !b[i] && !b[i + 1] && b[i + 2] == 1) { *len = 3; return true; }
    if (remaining >= 4 && !b[i] && !b[i + 1] && !b[i + 2] && b[i + 3] == 1) { *len = 4; return true; }
    return false;
  };
  uint32_t len = 0;
  if (!n || !start(0, &len)) { *bad = n != 0; return 0; }
  uint32_t i = len;
  while (i <= n) {
    uint32_t j = i, next = 0;
    while (j < n && !start(j, &next)) ++j;
    if (j == i) { *bad = true; break; }
    if (count < cap) out[count] = Nal{i, j - i, uint8_t(b[i] & 31)};
    ++count;
    if (j >= n) break;
    i = j + next;
  }
  return count;
}
// What a frame's NAL list says: 7 SPS, 8 PPS, 5 IDR slice, 1 non-IDR slice.
struct FrameNals {
  unsigned sps = 0, pps = 0, idr = 0, slice = 0, other = 0;
  bool bad = false;
};
inline FrameNals classify(const uint8_t *b, uint32_t n)
{
  Nal nals[32];
  FrameNals f;
  const unsigned count = scanAnnexB(b, n, nals, 32, &f.bad);
  for (unsigned i = 0; i < count && i < 32; ++i) {
    switch (nals[i].type) {
    case 7: ++f.sps; break;
    case 8: ++f.pps; break;
    case 5: ++f.idr; break;
    case 1: ++f.slice; break;
    default: ++f.other; break;
    }
  }
  if (count > 32) f.other += count - 32;
  return f;
}
} // namespace enc
} // namespace pianovideo
