// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of package/include/PianoBootFlagSdam.h against a model of one SPMI channel pair (observer + owner) on a 256-byte SDAM:
// the survey never stores to the owner channel and reads 0x40..0xbf; a request is refused before any store when writes are off,
// the request is unknown, the byte is foreign, or the APID is not EE 0's; a write changes byte 0x49 only and is read back; channel
// failures, a denied write and a lying read-back are reported; every request over all 256 bytes agrees with bootflag.h.
//   c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined -I.. bootflag_sdam_host.cpp -o /tmp/bootflag_sdam_host && /tmp/bootflag_sdam_host
#include <cstdio>
#include <cstring>
#include "../../include/PianoBootFlagSdam.h"

static int fails, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

namespace pb = pianobootflag;
namespace ps = pianospmi;

struct Model {
  uint8_t sdam[256];
  uint32_t obsCmd = 0, obsStatus = 0, obsR0 = 0, obsR1 = 0;
  uint32_t chnCmd = 0, chnStatus = 0, chnW0 = 0, chnW1 = 0;
  unsigned ownerStores = 0, ownerWrites = 0;
  int failObsReads = 0;         // the next N observer reads fail
  bool denyWrite = false, dropWrite = false, corruptReadBack = false;
  unsigned writesDone = 0;

  Model() { for (int i = 0; i < 256; ++i) sdam[i] = uint8_t(i * 7 + 3); sdam[0x49] = 0; }

  void obsCommand(uint32_t v)
  {
    const unsigned op = v >> 27, reg = (v >> 4) & 0xff, n = (v & 7) + 1;
    obsR0 = obsR1 = 0;
    if (op != ps::kOpExtReadL || reg + n > 256) { obsStatus = ps::kDone | ps::kFailure; return; }
    if (failObsReads > 0) { --failObsReads; obsStatus = ps::kDone | ps::kFailure; return; }
    for (unsigned b = 0; b < n; ++b) {
      uint8_t x = sdam[reg + b];
      if (corruptReadBack && writesDone && reg + b == 0x49) x ^= 0x10;
      (b < 4 ? obsR0 : obsR1) |= uint32_t(x) << (8 * (b & 3));
    }
    obsStatus = ps::kDone;
  }
  void chnCommand(uint32_t v)
  {
    const unsigned op = v >> 27, reg = (v >> 4) & 0xff, n = (v & 7) + 1;
    ++ownerWrites;
    if (op != ps::kOpExtWriteL || reg + n > 256) { chnStatus = ps::kDone | ps::kFailure; return; }
    if (denyWrite) { chnStatus = ps::kDone | ps::kDenied; return; }
    if (!dropWrite)
      for (unsigned b = 0; b < n; ++b) sdam[reg + b] = uint8_t((b < 4 ? chnW0 : chnW1) >> (8 * (b & 3)));
    ++writesDone;
    chnStatus = ps::kDone;
  }
};

struct Bound {
  Model &m;
  uint32_t obsRd(uint32_t off) { return off == ps::kStatus ? m.obsStatus : off == ps::kRdata0 ? m.obsR0 : off == ps::kRdata1 ? m.obsR1 : 0; }
  void obsWr(uint32_t off, uint32_t v) { if (off == ps::kCmd) m.obsCommand(v); }
  uint32_t chnRd(uint32_t off) { return off == ps::kStatus ? m.chnStatus : 0; }
  void chnWr(uint32_t off, uint32_t v)
  {
    ++m.ownerStores;
    if (off == ps::kWdata0) m.chnW0 = v;
    else if (off == ps::kWdata1) m.chnW1 = v;
    else if (off == ps::kCmd) m.chnCommand(v);
  }
};

static pb::Outcome req(Model &m, int r, bool owner = true, bool allow = true)
{
  Bound b{m};
  return pb::request([&](uint32_t o) { return b.obsRd(o); }, [&](uint32_t o, uint32_t v) { b.obsWr(o, v); },
                     [&](uint32_t o) { return b.chnRd(o); }, [&](uint32_t o, uint32_t v) { b.chnWr(o, v); }, [](unsigned) {}, r, owner,
                     allow);
}

static bool othersKept(const Model &m, const Model &ref)
{
  for (int i = 0; i < 256; ++i)
    if (i != 0x49 && m.sdam[i] != ref.sdam[i]) return false;
  return true;
}

int main()
{
  // survey
  {
    Model m;
    Bound b{m};
    uint8_t out[128] = {};
    const unsigned got = pb::survey([&](uint32_t o) { return b.obsRd(o); }, [&](uint32_t o, uint32_t v) { b.obsWr(o, v); }, [](unsigned) {}, out);
    CHECK(got == 128, "survey read %u bytes", got);
    CHECK(std::memcmp(out, m.sdam + 0x40, 128) == 0, "survey bytes = SDAM 0x40..0xbf");
    CHECK(m.ownerStores == 0, "survey: no owner-channel store (%u)", m.ownerStores);
    char text[1024];
    pb::formatSurvey(text, sizeof text, out, got);
    CHECK(std::strstr(text, "flag 0x49 = 00 (baseline)") != nullptr, "survey text names the baseline flag: %s", text);
    CHECK(std::strstr(text, "40: ") == text + std::strlen("sdam 0x7100 0x40..0xbf: 80 bytes read (hex count)\n"), "survey text first row");
    m.sdam[0x49] = 0x5c;
    pb::survey([&](uint32_t o) { return b.obsRd(o); }, [&](uint32_t o, uint32_t v) { b.obsWr(o, v); }, [](unsigned) {}, out);
    pb::formatSurvey(text, sizeof text, out, 128);
    CHECK(std::strstr(text, "flag 0x49 = 5c (NOT ours)") != nullptr, "survey text flags a foreign byte");
    Model f;
    Bound bf{f};
    f.failObsReads = 100;
    const unsigned g2 = pb::survey([&](uint32_t o) { return bf.obsRd(o); }, [&](uint32_t o, uint32_t v) { bf.obsWr(o, v); }, [](unsigned) {}, out);
    CHECK(g2 == 0, "survey with a dead channel reads nothing (%u)", g2);
    pb::formatSurvey(text, sizeof text, out, g2);
    CHECK(std::strstr(text, "flag 0x49 not read") != nullptr, "survey text without data");
    char tiny[8];
    pb::formatSurvey(tiny, sizeof tiny, out, 128);
    CHECK(std::strlen(tiny) == 7, "survey text truncates in bounds");
  }
  // refusals before any store
  {
    Model m;
    pb::Outcome o = req(m, PIANO_REQ_ONCE_ANDROID, true, false);
    CHECK(o.status == pb::Status::writeDisabled && m.ownerStores == 0, "writes off: refused, no store");
    o = req(m, 0);
    CHECK(o.status == pb::Status::badRequest && m.ownerStores == 0, "request 0 refused");
    o = req(m, 7);
    CHECK(o.status == pb::Status::badRequest && m.ownerStores == 0, "request 7 refused");
    m.sdam[0x49] = 0x5c;
    o = req(m, PIANO_REQ_RESET);
    CHECK(o.status == pb::Status::notOurs && m.ownerStores == 0 && m.sdam[0x49] == 0x5c, "foreign byte: refused, untouched");
    m.sdam[0x49] = 0;
    o = req(m, PIANO_REQ_ONCE_ANDROID, false);
    CHECK(o.status == pb::Status::notOwner && m.ownerStores == 0, "not EE 0's APID: refused before a store");
    o = req(m, PIANO_REQ_ONCE_CLEAR);
    CHECK(o.status == pb::Status::unchanged && m.ownerStores == 0, "no change: nothing written");
    m.failObsReads = 100;
    o = req(m, PIANO_REQ_ONCE_ANDROID);
    CHECK(o.status == pb::Status::readFailed && m.ownerStores == 0, "unreadable byte: refused");
  }
  // writes
  {
    Model m;
    const Model ref;
    pb::Outcome o = req(m, PIANO_REQ_ONCE_ANDROID);
    CHECK(o.status == pb::Status::ok && o.before == 0 && o.after == 0xa1 && m.sdam[0x49] == 0xa1, "once-android: 0x00 -> 0xa1");
    CHECK(othersKept(m, ref), "only byte 0x49 changed");
    CHECK(m.ownerWrites == 1, "one owner-channel command (%u)", m.ownerWrites);
    Model d;
    d.denyWrite = true;
    o = req(d, PIANO_REQ_ONCE_ANDROID);
    CHECK(o.status == pb::Status::writeFailed && d.sdam[0x49] == 0, "denied write reported");
    Model dr;
    dr.dropWrite = true;
    o = req(dr, PIANO_REQ_ONCE_ANDROID);
    CHECK(o.status == pb::Status::readBackMismatch && o.after == 0, "a write that did not land: read-back mismatch");
    Model c;
    c.corruptReadBack = true;
    o = req(c, PIANO_REQ_DEFAULT_ANDROID);
    CHECK(o.status == pb::Status::readBackMismatch, "a lying read-back reported");
  }
  // every request on every byte agrees with bootflag.h
  for (unsigned v = 0; v < 256; ++v)
    for (int r = PIANO_REQ_ONCE_ANDROID; r <= PIANO_REQ_RESET; ++r) {
      Model m;
      const Model ref;
      m.sdam[0x49] = uint8_t(v);
      unsigned want = 0;
      const int a = piano_bootflag_apply(v, r, &want);
      const pb::Outcome o = req(m, r);
      if (a != 0) CHECK(o.status == pb::Status::notOurs && m.sdam[0x49] == v && m.ownerStores == 0, "%#x req %d refused", v, r);
      else if (want == v) CHECK(o.status == pb::Status::unchanged && m.ownerStores == 0, "%#x req %d unchanged", v, r);
      else CHECK(o.status == pb::Status::ok && m.sdam[0x49] == want && othersKept(m, ref), "%#x req %d -> %#x", v, r, want);
    }
  std::printf("%s: bootflag_sdam_host %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails != 0;
}
