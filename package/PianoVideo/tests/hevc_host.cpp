// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Host admission only: synthetic syntax is not a firmware decode fixture.
#include "../vt/PianoVideoCodec.h"
#include <assert.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include "HEVCFixture.h"
using namespace hevcfixture;
namespace h = pianovideo::hevc;
namespace c = pianovideo::codec;
static Bytes readFile(const char *path) { std::ifstream in(path, std::ios::binary); assert(in); return Bytes(std::istreambuf_iterator<char>(in), {}); }
static void unit() {
  for (unsigned depth : {8u, 10u}) for (bool wave : {false, true}) {
    Fixture f(depth, wave); c::Config cfg; assert(c::parseHevc(f.hvcc.data(), f.hvcc.size(), cfg));
    assert(cfg.valid() && cfg.isHevc() && cfg.depth == depth && cfg.s.width == 1920 && cfg.s.height == 1080 && cfg.s.maxDpb == 6 && cfg.s.maxReorder == 2);
    assert(cfg.hfiProfile() == (depth == 10 ? 2u : 0u) && cfg.hfiLevel() == 5 && !cfg.hfiTier());
    assert(cfg.pixelFormat() == (depth == 10 ? 0x78343230u : 0x34323076u) && cfg.format() == 0x68766331u);
    c::Config same; assert(c::parseHevc(f.hvcc.data(), f.hvcc.size(), same) && cfg.same(same));
    Bytes temporalVps = f.vps, temporalSps = f.sps, temporalPps = f.pps;
    temporalVps[1] = temporalSps[1] = temporalPps[1] = 2;
    h::Vps v; h::Sps s; h::Pps p;
    assert(!h::vps(temporalVps.data(), temporalVps.size(), v));
    assert(!h::sps(temporalSps.data(), temporalSps.size(), cfg.hevc.v, s));
    assert(!h::pps(temporalPps.data(), temporalPps.size(), cfg.hevc.s, p));
    for (unsigned size = 0; size < f.hvcc.size(); ++size) { c::Config bad; assert(!c::parseHevc(f.hvcc.data(), size, bad) && !bad.valid()); }
    for (unsigned at : {0u, 13u, 15u, 16u, 17u, 18u, 21u, 22u, 23u}) {
      Bytes bad = f.hvcc; bad[at] ^= at == 23 ? 0x40 : at == 21 ? 1 : 0x80; c::Config out; assert(!c::parseHevc(bad.data(), bad.size(), out));
    }
    for (unsigned kind : {0u, 1u, 2u}) {
      Bytes au; append(au, f.slice(kind == 2 ? 19 : 1, kind)); c::Picture picture;
      assert(c::accessUnit(au.data(), au.size(), cfg, &picture) && picture.idr == (kind == 2));
      Bytes normalized(h::MaxAu); const uint32_t size = c::normalize(au.data(), au.size(), normalized.data(), normalized.size(), cfg, true);
      assert(size == au.size() + cfg.parameterBytes() && c::normalized(normalized.data(), size, cfg));
      assert(!c::normalize(au.data(), au.size(), normalized.data(), size - 1, cfg, true));
      Bytes bad = au; bad[4] |= 0x80; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad[5] = 0; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad[5] |= 8; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad[5] = 2; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad[6] &= 0x7f; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; append(bad, f.slice()); assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad[3] = 255; assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; bad.push_back(0); assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad = au; append(bad, Bytes{80, 1, 0x80}); assert(c::accessUnit(bad.data(), bad.size(), cfg)); // suffix SEI
      bad = au; append(bad, f.sps); assert(!c::accessUnit(bad.data(), bad.size(), cfg));
      bad.clear(); append(bad, Bytes{78, 1, 5, 1, 0, 0x80}); bad.insert(bad.end(), au.begin(), au.end()); assert(c::accessUnit(bad.data(), bad.size(), cfg)); // opaque prefix SEI
      bad = au; append(bad, Bytes{80, 1, 5, 10, 0, 0x80}); assert(!c::accessUnit(bad.data(), bad.size(), cfg));
    }
  }
  for (auto size : {std::pair<unsigned, unsigned>{3848, 2160}, {3840, 2184}, {3840, 2176}, {1920, 1081}}) {
    Fixture f(10, true, size.first, size.second); c::Config cfg; assert(!c::parseHevc(f.hvcc.data(), f.hvcc.size(), cfg));
  }
  Fixture f; c::Config cfg; assert(c::parseHevc(f.hvcc.data(), f.hvcc.size(), cfg));
  for (unsigned payload : {0u, 1u, 2u}) {
    Bytes au; append(au, f.slice(19, 2, 0, 1, payload));
    assert(c::accessUnit(au.data(), au.size(), cfg) == (payload == 2));
  }
  Bytes enormous; append(enormous, f.slice(19, 2, 0, UINT32_MAX, 2));
  assert(!c::accessUnit(enormous.data(), enormous.size(), cfg));
  Bytes excessive(h::MaxAu + 1); assert(!c::accessUnit(excessive.data(), excessive.size(), cfg));
  Bytes many;
  for (unsigned i = 0; i < 64; ++i) append(many, Bytes{78, 1, 5, 1, 0, 0x80});
  append(many, f.slice()); assert(!c::accessUnit(many.data(), many.size(), cfg));
  // Mutations cover every bit of independently constructed parameter/slice
  // syntax. Accepted variants must still satisfy the allocation and complete
  // normalization contract; ASan/UBSan watch every rejected parse as well.
  for (size_t bit = 0; bit < f.hvcc.size() * 8; ++bit) {
    Bytes bytes = f.hvcc; bytes[bit / 8] ^= 1u << (bit & 7); c::Config candidate;
    if (c::parseHevc(bytes.data(), bytes.size(), candidate)) {
      assert(candidate.valid() && (candidate.depth == 8 || candidate.depth == 10));
      assert(candidate.s.width <= 3840 && candidate.s.height <= 2160 && candidate.s.codedHeight <= 2176 && candidate.s.maxDpb <= h::MaxDpb);
    } else assert(!candidate.valid());
  }
  Bytes original; append(original, f.slice(1, 0));
  for (size_t bit = 0; bit < original.size() * 8; ++bit) {
    Bytes bytes = original; bytes[bit / 8] ^= 1u << (bit & 7);
    Bytes output(bytes.size() + cfg.parameterBytes() + 16, 0xa5); const uint32_t cap = output.size() - 16;
    const bool valid = c::accessUnit(bytes.data(), bytes.size(), cfg);
    const uint32_t written = c::normalize(bytes.data(), bytes.size(), output.data(), cap, cfg, true);
    assert(bool(written) == valid); if (valid) assert(c::normalized(output.data(), written, cfg));
    for (size_t i = cap; i < output.size(); ++i) assert(output[i] == 0xa5);
  }
  const uint8_t illegal[][7] = {{2,1,0,0,0,0x80,0x80},{2,1,0,0,1,0x80,0x80},{2,1,0,0,3,4,0x80},{2,1,0,0,3,0x80,0x80}};
  for (const auto &nal : illegal) assert(!h::ebsp(nal, sizeof(nal)));
  const uint8_t valid[] = {2,1,0,0,3,0,0x80}; assert(h::ebsp(valid, sizeof(valid)));
  const uint8_t overflow[] = {0,0,3,0,0,3,0,0,3,0}; h::Bits bits{overflow, sizeof(overflow)}; bits.ue(); assert(!bits.good);
  assert(h::level(183) == 11 && h::level(186) == UINT32_MAX);
  std::cout << "HEVC_UNIT_PASS main8=1 main10=1 WPP_B_RPS=1 malformed_rejected=1 config_bytes=" << sizeof(c::Config) << "\n";
}
static void corpus(const char *configPath, const char *samplesPath) {
  const Bytes hvcc = readFile(configPath), samples = readFile(samplesPath); c::Config cfg;
  assert(c::parseHevc(hvcc.data(), hvcc.size(), cfg));
  Bytes out(h::MaxAu); uint32_t count = 0, idrs = 0, maxAu = 0, tids = 0, types = 0; size_t at = 0;
  while (at < samples.size()) {
    assert(samples.size() - at >= 4); const uint32_t size = uint32_t(samples[at]) << 24 | uint32_t(samples[at + 1]) << 16 | uint32_t(samples[at + 2]) << 8 | samples[at + 3]; at += 4;
    assert(size <= samples.size() - at); c::Picture p;
    if (!c::accessUnit(samples.data() + at, size, cfg, &p)) { std::cerr << "rejected AU " << count << " bytes=" << size << "\n"; std::abort(); }
    const uint32_t written = c::normalize(samples.data() + at, size, out.data(), out.size(), cfg, true);
    assert(written == size + cfg.parameterBytes() && c::normalized(out.data(), written, cfg));
    const uint32_t without = c::normalize(samples.data() + at, size, out.data(), out.size(), cfg, false);
    assert(without == size && c::normalized(out.data(), without, cfg));
    size_t source = at; uint32_t dest = 0;
    while (source < at + size) {
      const uint32_t length = uint32_t(samples[source]) << 24 | uint32_t(samples[source + 1]) << 16 | uint32_t(samples[source + 2]) << 8 | samples[source + 3]; source += 4;
      assert(!out[dest] && !out[dest + 1] && !out[dest + 2] && out[dest + 3] == 1 && !memcmp(samples.data() + source, out.data() + dest + 4, length));
      source += length; dest += length + 4;
    }
    if (!count) assert(p.idr); ++count; if (p.idr) ++idrs; tids |= 1u << p.temporalId; types |= 1u << p.nalType;
    if (size > maxAu) maxAu = size; at += size;
  }
  std::cout << "HEVC_CORPUS_PASS samples=" << count << " idrs=" << idrs << " max_au=" << maxAu << " temporal_mask=" << tids << " nal_mask=" << types
    << " width=" << cfg.s.width << " height=" << cfg.s.height << " depth=" << cfg.depth << " profile=" << cfg.s.profile << " level=" << cfg.s.level
    << " tier=" << cfg.hfiTier() << " RPS=" << cfg.hevc.s.numRps << " WPP=" << cfg.hevc.p.wpp << " dpb=" << cfg.s.maxDpb << " reorder=" << cfg.s.maxReorder << "\n";
}
int main(int argc, char **argv) { unit(); if (argc == 3) corpus(argv[1], argv[2]); else assert(argc == 1); }
