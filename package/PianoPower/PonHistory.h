// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang. All rights reserved.
// Independently expressed Gen3 PON-history layout facts: PMK8550 SID0 SDAM
// 0x7400/0x7500, 117 FIFO bytes per page. No PMIC write or consume operation.
// See docs/drivers/pon-history.md for the reference pin and evidence limits.
#ifndef PIANO_PON_HISTORY_H
#define PIANO_PON_HISTORY_H
#include "SpmiObserver.h"

namespace pianopower {
constexpr unsigned kPonBanks = 2, kPonPageBytes = 128, kPonBankBytes = 117;
constexpr unsigned kPonBytes = kPonBanks * kPonBankBytes, kPonEntries = kPonBytes / 4;
constexpr unsigned kPonFirst = 0x4b;
struct PonEntry {
  uint8_t state, event, hi, lo;
  unsigned data() const { return unsigned(hi) * 256 + lo; }
  bool unused() const { return !(state | event | hi | lo); }
};
inline int ponCursor(const uint8_t *page)
{
  if (page[5] != kPonBanks - 1 || page[6] < kPonFirst || page[6] > 0xbf || page[7] >= kPonBanks) return -1;
  return int(page[7] * kPonBankBytes + page[6] - kPonFirst);
}
inline PonEntry ponEntry(const uint8_t pages[kPonBanks][kPonPageBytes], unsigned cursor, unsigned ordinal)
{
  uint8_t b[4];
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned pos = (cursor + kPonBytes - kPonEntries * 4 + ordinal * 4 + i) % kPonBytes;
    b[i] = pages[pos / kPonBankBytes][kPonFirst - 0x40 + pos % kPonBankBytes];
  }
  return {b[0], b[1], b[2], b[3]};
}
inline const char *ponState(unsigned state)
{
  switch (state) {
    case 0: case 6: return "FAULT";
    case 1: return "PON";
    case 2: return "POFF";
    case 3: return "ON";
    case 4: return "RESET";
    case 5: return "OFF";
    case 7: return "WARM_RESET";
    default: return "UNKNOWN";
  }
}
inline const char *ponTrigger(unsigned data, bool reset)
{
  if (reset) {
    switch (data) {
      case 0x0080: return "KPDPWR_S2";
      case 0x0081: return "RESIN_S2";
      case 0x0082: return "KPDPWR_RESIN_S2";
      case 0x0083: return "PMIC_WATCHDOG_S2";
      case 0x0084: return "PS_HOLD";
      case 0x0085: return "SW_RESET";
      case 0x0086: return "RESIN_DEBOUNCE";
      case 0x0087: return "KPDPWR_DEBOUNCE";
      case 0x21e3: return "SID2_BCL_ALARM";
      case 0x31f5: return "SID3_BCL_ALARM";
      case 0x11d0: return "SID1_OCP";
      case 0x21d0: return "SID2_OCP";
      case 0x41d0: return "SID4_OCP";
      case 0x51d0: return "SID5_OCP";
    }
  } else {
    switch (data) {
      case 0x0084: return "PS_HOLD";
      case 0x0085: return "HARD_RESET";
      case 0x0086: return "RESIN";
      case 0x0087: return "KPDPWR";
      case 0x0202: case 0x18a0: case 0x2763: case 0x31c2: case 0x71c2: case 0x8732: return "USB_CHARGER";
      case 0x0621: return "RTC_ALARM";
      case 0x0640: return "SMPL";
      case 0x18c0: return "SID1_GPIO5";
    }
  }
  return "UNKNOWN_TRIGGER";
}
// The masks and labels are hardware data, not copied driver implementation.
inline void ponBits(char *out, size_t size, unsigned bits, const char *const names[8])
{
  size_t used = 0;
  if (size) out[0] = 0;
  for (unsigned i = 0; i < 8 && used + 1 < size; ++i) {
    if (!(bits & (1u << i))) continue;
    const int n = snprintf(out + used, size - used, "%s%s", used ? "|" : "", names[i]);
    if (n < 0 || size_t(n) >= size - used) return;
    used += size_t(n);
  }
  if (!used && size) snprintf(out, size, "none");
}
inline void ponDescribe(const PonEntry &e, char *out, size_t size)
{
  static const char *const fault1[8] = {"GP_FAULT0", "GP_FAULT1", "GP_FAULT2", "GP_FAULT3", "MBG_FAULT", "OVLO", "UVLO", "AVDD_RB"};
  static const char *const fault2[8] = {"UNKNOWN_BIT0", "UNKNOWN_BIT1", "PMIC_RB", "FAULT_N", "PMIC_FAULT_WATCHDOG", "PBS_NACK", "RESTART_PON", "OTST3"};
  static const char *const fault3[8] = {"GP_FAULT4", "GP_FAULT5", "GP_FAULT6", "GP_FAULT7", "GP_FAULT8", "GP_FAULT9", "GP_FAULT10", "GP_FAULT11"};
  static const char *const s3[8] = {"UNKNOWN_BIT0", "UNKNOWN_BIT1", "UNKNOWN_BIT2", "UNKNOWN_BIT3", "FAULT_N", "PMIC_FAULT_WATCHDOG", "PBS_NACK", "KPDPWR_OR_RESIN"};
  static const char *const pbl[8] = {"UNKNOWN_BIT0", "UNKNOWN_BIT1", "UNKNOWN_BIT2", "UNKNOWN_BIT3", "UNKNOWN_BIT4", "UNKNOWN_BIT5", "XVDD", "DVDD"};
  char a[160], b[160];
  if (e.event == 9 || (e.event >= 0x10 && e.event <= 0x1c)) {
    ponBits(a, sizeof a, e.lo, fault1); ponBits(b, sizeof b, e.hi, fault2);
    snprintf(out, size, "fault-sid=%u fault1=%02x[%s] fault2=%02x[%s]", e.event == 9 ? 0 : e.event - 15, e.lo, a, e.hi, b);
    return;
  }
  switch (e.event) {
    case 1: case 6:
      snprintf(out, size, "%s=%s sid=%u pid=%02x irq=%u", e.event == 1 ? "pon-trigger" : "reset-trigger",
               ponTrigger(e.data(), e.event == 6), e.hi >> 4, (e.data() >> 4) & 255, e.lo & 7); break;
    case 2: snprintf(out, size, "OTP-copy last-address=%04x", e.data()); break;
    case 3: snprintf(out, size, "trim bytes=%u", e.data()); break;
    case 4: snprintf(out, size, "XVLO-check-complete"); break;
    case 5: snprintf(out, size, "PMIC-check sid-mask=%04x", e.data()); break;
    case 7: snprintf(out, size, "reset-type=%s", e.lo == 1 ? "WARM" : e.lo == 4 ? "SHUTDOWN" : e.lo == 7 ? "HARD" : "UNKNOWN"); break;
    case 8: snprintf(out, size, "warm-count=%u", e.data()); break;
    case 10: ponBits(a, sizeof a, e.lo, fault3); snprintf(out, size, "fault3=%02x[%s]", e.lo, a); break;
    case 11: snprintf(out, size, "PBS-PC=%04x", e.data()); break;
    case 12:
      ponBits(a, sizeof a, e.lo, s3); ponBits(b, sizeof b, e.hi, pbl);
      snprintf(out, size, "fundamental S3=%02x[%s] PBL=%02x[%s]", e.lo, a, e.hi, b); break;
    case 13: snprintf(out, size, "PON-sequence-start"); break;
    case 14: snprintf(out, size, "PON-success"); break;
    case 15: snprintf(out, size, "waiting-PS_HOLD"); break;
    case 0x20: snprintf(out, size, "VREG-ready-check fault=%u", e.data() != 0); break;
    default: snprintf(out, size, "%s", e.unused() ? "unused" : "UNKNOWN_EVENT"); break;
  }
}

// Every transaction uses the existing shared lock; no logging while it is held.
// Two full copies, each bracketed by metadata, detect moving FIFO and bad reads.
inline bool readPonHistory(const Io &io, const Arbiter &arb, void (*log)(const char *, ...))
{
  log("pon-history-v1: begin banks=2 sid=0 read-only=1");
  if (!arb.v7) { log("pon-history-v1: end valid=0 reason=not-v7"); return false; }
  Apid apids[2] = {spmiFindApid(io, arb, 0x074), spmiFindApid(io, arb, 0x075)};
  for (unsigned bank = 0; bank < kPonBanks; ++bank) {
    log("pon-history-v1: page=%u ppid=%03x apid=%d owner=%u", bank, 0x074 + bank, apids[bank].apid, apids[bank].owner);
    if (apids[bank].apid < 0) { log("pon-history-v1: end valid=0 reason=missing-page"); return false; }
  }
  uint8_t raw[2][kPonBanks][kPonPageBytes] = {}, ids[2][kPonBanks][2] = {};
  bool good = true;
  for (unsigned sample = 0; sample < 2; ++sample) {
    uint8_t before[3] = {}, after[3] = {};
    auto read = [&](unsigned bank, unsigned reg, uint8_t *buf, unsigned bytes) {
      uint32_t status = 0;
      const ReadResult r = spmiRead(io, apids[bank].apid, uint8_t(reg), buf, bytes, &status);
      log("pon-history-v1: read sample=%u page=%u reg=%02x bytes=%u result=%s status=%08x", sample, bank, reg, bytes, resultName(r), status);
      good = good && r == ReadResult::ok;
    };
    read(0, 0x45, before, 3);
    for (unsigned bank = 0; bank < kPonBanks; ++bank) {
      read(bank, 4, ids[sample][bank], 2);
      for (unsigned off = 0; off < kPonPageBytes; off += 8) {
        uint8_t *v = &raw[sample][bank][off];
        read(bank, 0x40 + off, v, 8);
        log("pon-history-v1: raw sample=%u page=%u reg=%02x data=%02x%02x%02x%02x%02x%02x%02x%02x",
            sample, bank, 0x40 + off, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
      }
      log("pon-history-v1: identity sample=%u page=%u type=%02x subtype=%02x map-id=%02x map-version=%02x size=%02x",
          sample, bank, ids[sample][bank][0], ids[sample][bank][1], raw[sample][bank][0], raw[sample][bank][1], raw[sample][bank][4]);
    }
    read(0, 0x45, after, 3);
    for (unsigned i = 0; i < 3; ++i) good = good && before[i] == after[i] && before[i] == raw[sample][0][5 + i];
  }
  for (unsigned bank = 0; bank < kPonBanks; ++bank) {
    for (unsigned i = 0; i < 2; ++i) good = good && ids[0][bank][i] == ids[1][bank][i];
    for (unsigned i = 0; i < kPonPageBytes; ++i) good = good && raw[0][bank][i] == raw[1][bank][i];
  }
  const int cursor = ponCursor(raw[0][0]);
  if (!good || cursor < 0) {
    log("pon-history-v1: end valid=0 reason=%s", good ? "layout" : "read-or-unstable");
    return false;
  }
  log("pon-history-v1: snapshot cursor=%u count=%u push=%02x bank=%u entries=58", unsigned(cursor), raw[0][0][5], raw[0][0][6], raw[0][0][7]);
  unsigned used = 0, success = 0;
  for (unsigned i = 0; i < kPonEntries; ++i) {
    const PonEntry e = ponEntry(raw[0], unsigned(cursor), i);
    char desc[384]; ponDescribe(e, desc, sizeof desc);
    log("pon-history-v1: entry=%02u offset=%03u raw=%02x%02x%02x%02x state=%s(%02x) event=%02x data=%04x %s",
        i, (unsigned(cursor) + kPonBytes - kPonEntries * 4 + i * 4) % kPonBytes,
        e.state, e.event, e.hi, e.lo, ponState(e.state), e.state, e.event, e.data(), desc);
    used += !e.unused(); success += e.event == 14;
  }
  log("pon-history-v1: end valid=%u used=%u pon-success=%u reason=%s", used != 0, used, success, used ? "stable" : "empty");
  return used != 0;
}
} // namespace pianopower
#endif
