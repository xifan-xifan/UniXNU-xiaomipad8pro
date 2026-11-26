// SPDX-License-Identifier: BSD-2-Clause-Patent
// The macOS side of piano's Android/macOS boot flag (T-16 UEFI OS switch): the read-only SDAM survey and the flag request, over
// the SPMI arbiter v7 as EE 0. Portable: the kext glue (PianoDwc3's sysctls machdep.piano_sdam_survey / machdep.piano_bootflag) passes
// the channel accessors; package/PianoDwc3/tests/bootflag_sdam_host.cpp drives it against an arbiter + SDAM model.
//   flag     SID 0 SDAM 0x7100 (PPID 0x071) byte 0x49, the encoding and the requests of firmware/selector/bootflag.h (shared with the
//            pre-UEFI selector and the FD);
//   survey   bytes 0x40..0xBF of the same SDAM, observer reads only (8 bytes each), never a write;
//   request  read the byte, piano_bootflag_apply (refused when the byte is not ours), write it through the EE-0 owner channel only
//            when it changes, read it back. Writes need the caller's explicit permission (the boot argument piano-bootflag-write=1,
//            default 0): without it a request is refused before any channel store.
#ifndef PIANO_BOOTFLAG_SDAM_H
#define PIANO_BOOTFLAG_SDAM_H
#include <stdint.h>
#include "PianoSpmi.h"
#include "../../firmware/selector/bootflag.h"

namespace pianobootflag {

constexpr uint16_t kSdamPpid = PIANO_BOOTFLAG_PPID;
constexpr uint8_t kFlagOffset = PIANO_BOOTFLAG_OFFSET, kSurveyFirst = 0x40, kSurveyBytes = 0x80;

enum class Status : int8_t {
  ok = 0,
  unchanged = 1,      // the request left the byte as it was: nothing written
  readFailed = -1,
  notOurs = -2,       // the byte holds a value no writer of ours stores: never written
  badRequest = -3,
  writeDisabled = -4, // piano-bootflag-write is not 1
  notOwner = -5,      // the SDAM's APID is not EE 0's: its owner channel cannot write
  writeFailed = -6,
  readBackMismatch = -7,
};

inline const char *statusName(Status s)
{
  switch (s) {
    case Status::ok: return "ok";
    case Status::unchanged: return "unchanged";
    case Status::readFailed: return "read failed";
    case Status::notOurs: return "not ours";
    case Status::badRequest: return "bad request";
    case Status::writeDisabled: return "writes disabled (piano-bootflag-write=0)";
    case Status::notOwner: return "SDAM APID not owned by EE 0";
    case Status::writeFailed: return "write failed";
    case Status::readBackMismatch: return "read back mismatch";
  }
  return "?";
}

// The survey: kSurveyBytes bytes from kSurveyFirst into out[], 8 per observer read. Returns the bytes read (a failed read ends it).
template <class ObsRd, class ObsWr, class Delay>
unsigned survey(ObsRd &&obsRd, ObsWr &&obsWr, Delay &&delayUs, uint8_t *out)
{
  unsigned got = 0;
  for (unsigned off = 0; off < kSurveyBytes; off += 8) {
    if (pianospmi::read(obsRd, obsWr, delayUs, uint8_t(kSurveyFirst + off), out + off, 8) != pianospmi::Result::ok) break;
    got = off + 8;
  }
  return got;
}

struct Outcome {
  Status status = Status::readFailed;
  uint8_t before = 0, after = 0;
};

// A request (bootflag.h piano_bootflag_req, 1..6) on the flag byte. ownerEe0: the APID's owner is EE 0 (checked by the caller from
// the arbiter's owner table). allowWrite: the boot argument.
template <class ObsRd, class ObsWr, class ChnRd, class ChnWr, class Delay>
Outcome request(ObsRd &&obsRd, ObsWr &&obsWr, ChnRd &&chnRd, ChnWr &&chnWr, Delay &&delayUs, int req, bool ownerEe0, bool allowWrite)
{
  Outcome o;
  uint8_t v = 0;
  if (req < PIANO_REQ_ONCE_ANDROID || req > PIANO_REQ_RESET) { o.status = Status::badRequest; return o; }
  if (!allowWrite) { o.status = Status::writeDisabled; return o; }
  if (pianospmi::readStable(obsRd, obsWr, delayUs, kFlagOffset, &v, 1) != pianospmi::Result::ok) return o;
  o.before = o.after = v;
  unsigned nv = 0;
  if (piano_bootflag_apply(v, req, &nv) != 0) { o.status = Status::notOurs; return o; }
  if (nv == v) { o.status = Status::unchanged; return o; }
  if (!ownerEe0) { o.status = Status::notOwner; return o; }
  const uint8_t w = uint8_t(nv);
  if (pianospmi::write(chnRd, chnWr, delayUs, kFlagOffset, &w, 1) != pianospmi::Result::ok) { o.status = Status::writeFailed; return o; }
  uint8_t back = 0;
  if (pianospmi::read(obsRd, obsWr, delayUs, kFlagOffset, &back, 1) != pianospmi::Result::ok) { o.status = Status::readFailed; return o; }
  o.after = back;
  o.status = back == w ? Status::ok : Status::readBackMismatch;
  return o;
}

// The survey as text for the sysctl: "sdam 0x7100 0x40..0xbf (N bytes): xx xx ...", 16 a line, then the flag line.
inline unsigned formatSurvey(char *buf, unsigned size, const uint8_t *bytes, unsigned got)
{
  static const char hex[] = "0123456789abcdef";
  unsigned n = 0;
  auto put = [&](char c) { if (n + 1 < size) buf[n++] = c; };
  auto puts_ = [&](const char *s) { while (*s) put(*s++); };
  auto byte = [&](uint8_t b) { put(hex[b >> 4]); put(hex[b & 15]); };
  puts_("sdam 0x7100 0x40..0xbf: ");
  byte(uint8_t(got));
  puts_(" bytes read (hex count)\n");
  for (unsigned i = 0; i < got; ++i) {
    if (i % 16 == 0) { byte(uint8_t(kSurveyFirst + i)); puts_(":"); }
    put(' ');
    byte(bytes[i]);
    if (i % 16 == 15) put('\n');
  }
  if (got > kFlagOffset - kSurveyFirst) {
    const uint8_t f = bytes[kFlagOffset - kSurveyFirst];
    puts_("flag 0x49 = ");
    byte(f);
    puts_(f == 0 ? " (baseline)\n" : piano_bootflag_ours(f) ? " (ours)\n" : " (NOT ours)\n");
  } else {
    puts_("flag 0x49 not read\n");
  }
  if (size) buf[n < size ? n : size - 1] = 0;
  return n;
}

}  // namespace pianobootflag
#endif
