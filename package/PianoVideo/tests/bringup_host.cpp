// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoVideo levels 2-4 (V1-V3: VideoBringup.h and the headers under it) against one model of the SoC:
//   - GCC's video branches; VIDEO_CC (PLL0 that locks only when configured and running, the shared RCG whose update completes only
//     on a running source, GDSCs whose CFG reports power-up/down, branches that run only inside a powered GDSC on a running RCG, the
//     free-running clock's memory enable); the core's controller and codec registers, refused (a violation) whenever their GDSC or
//     clocks are off; VIDEO_CC refused while its rails (MXC/MMCX from our RPMh votes) or GCC's video AHB are off;
//   - apps DRV-2 (TCSes that complete or not, every access refused without the DRV-2 lock) and a cmd-db image; the boot display's
//     MMCX vote (NOM_L1) that a lower DRV-2 vote would take away;
//   - the secure world (PAS 9, MP_VIDEO_VAR, SET_REMOTE_STATE, PAS shutdown; the hold of the secure-call lock at each call; the
//     metadata blob and the carveout's contents checked at auth_and_reset);
//   - the apps SMMU (stream-match groups, banks, TLB maintenance) and a firmware that reaches the queue table and queues only by
//     walking our bank's table (SEP 0 faults every IOVA with bit 31 set, as this hypervisor family does), boots on CTRL_INIT when
//     the UC region is right, answers SYS_INIT / IMAGE_VERSION / PC_PREP in the message queue and writes the debug queue;
//   - every fault the sequences must survive: each wait made to fail, each secure call refused, no answer, a system error, a
//     malformed answer, no memory, the stop argument at every step. After each: everything off, the votes dropped, no violation.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. bringup_host.cpp QcomMdt.o -o /tmp/bringup_host && /tmp/bringup_host
//   (VIDEO_HOST_ECHO=1 prints every log line; VIDEO_HOST_FW=<path> also checks the owner's vpu35_4v.mbn against the real pin.)
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <CommonCrypto/CommonDigest.h>
#include "VideoBringup.h"

using namespace pianovideo;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------------------- the cmd-db
// ARC tables (vlvl per hlvl) and BCM addresses as piano's cmd-db has their shape.
static const uint16_t kMxcLevels[] = {0x0, 0x10, 0x30, 0x40, 0x80, 0xc0, 0x100, 0x140, 0x180};
static const uint16_t kMmcxLevels[] = {0x0, 0x10, 0x30, 0x38, 0x40, 0x80, 0xc0, 0x100, 0x140, 0x150, 0x180};
static const uint16_t kCxLevels[] = {0x0, 0x10, 0x40, 0x80, 0x100, 0x140, 0x180};
constexpr uint32_t kAddrCx = 0x30000, kAddrMxc = 0x30050, kAddrMmcx = 0x30060, kAddrSh0 = 0x50000, kAddrMm0 = 0x50030, kAddrMm1 = 0x50034;
constexpr uint32_t kMxcSvs = 4, kMxcLowSvs = 3, kMmcxLowSvs = 4, kMmcxNomL1 = 8;

struct DbEntry {
  const char *name;
  uint32_t addr;
  std::vector<uint8_t> aux;
};
static std::vector<uint8_t> levelsAux(const uint16_t *v, size_t n, size_t pad)
{
  std::vector<uint8_t> a;
  for (size_t i = 0; i < n + pad; ++i) {
    const uint16_t x = i < n ? v[i] : 0;
    a.push_back(uint8_t(x));
    a.push_back(uint8_t(x >> 8));
  }
  return a;
}
static std::vector<uint8_t> bcmAux(uint32_t unit, uint16_t width, uint8_t vcd)
{
  return {uint8_t(unit), uint8_t(unit >> 8), uint8_t(unit >> 16), uint8_t(unit >> 24), uint8_t(width), uint8_t(width >> 8), vcd, 0};
}
static void put16(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
  b[at] = uint8_t(v);
  b[at + 1] = uint8_t(v >> 8);
}
static void put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
  put16(b, at, v);
  put16(b, at + 2, v >> 16);
}
static std::vector<uint8_t> makeCmdDb(const std::vector<DbEntry> &arcs, const std::vector<DbEntry> &bcms)
{
  std::vector<uint8_t> db(kCmdDbBytes, 0);
  put32(db, 0, 1);
  put32(db, 4, rpmh::kMagic);
  uint32_t cursor = 0;   // from the data area (byte 144)
  const struct {
    unsigned id;
    const std::vector<DbEntry> *list;
  } slaves[] = {{3, &arcs}, {5, &bcms}};
  for (unsigned s = 0; s < 2; ++s) {
    const auto &list = *slaves[s].list;
    const uint32_t table = cursor;
    cursor += uint32_t(24 * list.size());
    const uint32_t data = cursor;
    uint32_t auxAt = 0;
    for (size_t i = 0; i < list.size(); ++i) {
      const size_t e = 144 + table + 24 * i;
      for (unsigned c = 0; c < 8 && list[i].name[c]; ++c) db[e + c] = uint8_t(list[i].name[c]);
      put32(db, e + 16, list[i].addr);
      put16(db, e + 20, uint32_t(list[i].aux.size()));
      put16(db, e + 22, auxAt);
      for (size_t k = 0; k < list[i].aux.size(); ++k) db[144 + data + auxAt + k] = list[i].aux[k];
      auxAt += uint32_t(list[i].aux.size());
    }
    cursor = data + auxAt;
    const size_t h = 8 + 16 * s;
    put16(db, h, slaves[s].id);
    put16(db, h + 2, table);
    put16(db, h + 4, data);
    put16(db, h + 6, uint32_t(list.size()));
  }
  return db;
}
static std::vector<uint8_t> pianoCmdDb()
{
  return makeCmdDb({{"cx.lvl", kAddrCx, levelsAux(kCxLevels, 7, 9)}, {"mxc.lvl", kAddrMxc, levelsAux(kMxcLevels, 9, 7)},
                    {"mmcx.lvl", kAddrMmcx, levelsAux(kMmcxLevels, 11, 5)}},
                   {{"SH0", kAddrSh0, bcmAux(1000, 16, 0)}, {"MM0", kAddrMm0, bcmAux(1000, 32, 1)}, {"MM1", kAddrMm1, bcmAux(1000, 32, 1)}});
}

// ------------------------------------------------------------------------------------------------------ the firmware image
// A small signed-image layout of the real one's shape: PT_NULL headers segment, relocatable PT_LOADs (one with a memory-only tail,
// one memory-only), the hash segment at the end of the file.
struct ImageSpec {
  bool noHash = false, outside = false, notElf = false;
};
constexpr uint32_t kImgMin = 0xf500000;
static std::vector<uint8_t> makeImage(const ImageSpec &s = {})
{
  const unsigned n = 5;
  std::vector<uint8_t> f(0x2100, 0);
  for (size_t i = 0x1000; i < f.size(); ++i) f[i] = uint8_t(i * 7 + 3);
  const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0};
  for (unsigned i = 0; i < 8; ++i) f[i] = ident[i];
  if (s.notElf) f[1] = 'X';
  put16(f, 16, 2);
  put16(f, 18, 0xa4);
  put32(f, 20, 1);
  put32(f, 24, kImgMin);
  put32(f, 28, 52);
  put16(f, 40, 52);
  put16(f, 42, 32);
  put16(f, 44, n);
  struct Ph {
    uint32_t type, off, pa, fsz, msz, flags;
  } ph[n] = {
      {0, 0, 0, 52 + 32 * n, 0, 0x7000000},
      {1, 0x1000, kImgMin, 0x800, 0x800, 0x8000005},
      {1, 0x1800, kImgMin + 0x1000, 0x400, 0x2000, 0x8000006},
      {1, 0, kImgMin + (s.outside ? 0x900000u : 0x100000u), 0, 0x1000, 0x8000006},
      {0, 0x2000, 0, 0x100, 0x100, s.noHash ? 0x7000000u : 0x2000000u},
  };
  for (unsigned i = 0; i < n; ++i) {
    const size_t p = 52 + 32 * i;
    put32(f, p, ph[i].type);
    put32(f, p + 4, ph[i].off);
    put32(f, p + 8, ph[i].pa);
    put32(f, p + 12, ph[i].pa);
    put32(f, p + 16, ph[i].fsz);
    put32(f, p + 20, ph[i].msz);
    put32(f, p + 24, ph[i].flags);
    put32(f, p + 28, 0x1000);
  }
  return f;
}
static void sha256(const uint8_t *p, uint64_t n, uint8_t out[32]) { CC_SHA256(p, CC_LONG(n), out); }

// ------------------------------------------------------------------------------------------------------------------ the SoC
struct Faults {
  bool gccStuck = false, gdscStuck = false, codecGdscStuck = false, pllNoLock = false, rcgStuck = false,
       branchStuck = false, wrongDivider = false, rpmhNoComplete = false, drv2Busy = false, rscReadback = false, lpiDeny = false,
       cpuLpiStuck = false, notIdle = false, noCmdDb = false, noFile = false, dmaFail = false, fwNoBoot = false, fwBadUc = false,
       fwNoReply = false, fwInitFail = false, fwSysError = false, fwNoVersion = false, fwNotIdle = false, smmuExisting = false,
       smmuNoBank = false, tlbStuck = false, sepFaults = true, s1BadTcr = false, s1BadSctlr = false,
       releaseSmrStuck = false, faultIgnored = false;
  std::string scmFail;   // the secure call to refuse ("init", "memsetup", "auth", "mpvideovar", "shutdown", "remote0", "remote1")
};

struct Model {
  // ---------------------------------------------------------------------------------------------------- bookkeeping
  uint64_t now = 0;
  bool echo = std::getenv("VIDEO_HOST_ECHO") != nullptr;
  std::vector<std::string> lines, events, violations;
  std::vector<int> stages;
  Faults f;
  unsigned writes = 0, reads = 0, cleans = 0;
  void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    lines.push_back(b);
    if (echo) std::printf("  | %s\n", b);
  }
  void violation(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (violations.size() < 50) violations.push_back(b);
    if (echo) std::printf("  ! %s\n", b);
  }
  void stage(int n, const char *what)
  {
    stages.push_back(n);
    if (echo) std::printf("  | stage %d: %s\n", n, what);
  }
  void event(const std::string &e) { events.push_back(e); }
  unsigned count(const std::string &e) const
  {
    unsigned n = 0;
    for (const auto &x : events) n += x == e;
    return n;
  }
  int first(const std::string &e) const
  {
    for (size_t i = 0; i < events.size(); ++i)
      if (events[i] == e) return int(i);
    return -1;
  }
  int last(const std::string &e) const
  {
    for (size_t i = events.size(); i-- > 0;)
      if (events[i] == e) return int(i);
    return -1;
  }
  bool logged(const char *text) const
  {
    for (const auto &l : lines)
      if (l.find(text) != std::string::npos) return true;
    return false;
  }
  void delayUs(uint32_t us) { now += us; }
  void sleepMs(uint32_t ms) { now += uint64_t(ms) * 1000; }
  uint64_t nowUs() { return now; }
  void barrier() {}
  void cleanToPoc(const void *, uint64_t) { ++cleans; }

  // ------------------------------------------------------------------------------------------------------- registers
  std::map<uint64_t, uint32_t> r;
  uint32_t &R(uint64_t pa) { return r[pa]; }
  static bool in(uint64_t pa, uint64_t base, uint64_t bytes) { return pa >= base && pa < base + bytes; }

  Model()
  {
    // The tablet's cold survey: AHB is enabled-capable but hardware-gated while idle,
    // XO is kept off until Linux's probe sets CLK_ENABLE, and both AXI branches are HW_CTL.
    R(kGcc + kGccVideoAhb) = kCbcrHwcg;
    R(kGcc + kGccVideoXo) = 0;
    R(kGcc + kGccVideoAxi0) = kCbcrHwcg;
    R(kGcc + kGccVideoAxi1) = kCbcrHwcg;
    R(kVideoCc + kCcMvs0cGdsc) = kGdscCollapse | 0x00222000;
    R(kVideoCc + kCcMvs0Gdsc) = kGdscCollapse | 0x00222000;
    R(kVideoCc + kCcMvs0Clk) = kCbcrHwcg;
    R(kVideoCc + kCcMvs0FreerunMem) = kMemEnable;
    R(kVideoCc + kCcAhbClk) = 1;
    R(kVideoCc + kCcXoClk) = 1;
    R(kVideoCc + kCcSleepClk) = 1;
    cmddb = pianoCmdDb();
    smmuInit();
    carve.assign(kCarveoutBytes, 0xaa);
    image = makeImage();
  }
  void coldAhbOff() { R(kGcc + kGccVideoAhb) = 0; }

  // --------------------------------------------------------------------------------------------------- rails, DRV-2
  uint32_t mxc = 0, mmcx = kMmcxNomL1, mm1 = kBcmMm1Off;   // the boot display holds MMCX at NOM_L1 (on DRV-2)
  bool allowMmcxDrop = false;
  bool drv2Held = false;
  unsigned drv2Takes = 0;
  bool tcsBusy[3] = {};
  uint32_t irqStatus = 0;
  std::vector<uint8_t> cmddb;
  const uint8_t *cmdDb(uint32_t *bytes)
  {
    *bytes = uint32_t(cmddb.size());
    return f.noCmdDb ? nullptr : cmddb.data();
  }
  bool drv2Take(uint32_t *waits)
  {
    if (f.drv2Busy) {
      now += 2000000;
      *waits = 200000;
      return false;
    }
    if (drv2Held) violation("the DRV-2 lock taken twice");
    drv2Held = true;
    ++drv2Takes;
    *waits = 0;
    return true;
  }
  void drv2Give()
  {
    if (!drv2Held) violation("the DRV-2 lock given without a hold");
    drv2Held = false;
  }
  // The shared multimedia votes' words (PianoA830's piano_mm_votes / piano_mm_votes_owner in the kernel): PianoVideo the only client
  // unless a test adds the camera's needs.
  volatile uint32_t mmWords_[PIANO_MM_WORDS] = {};
  volatile uint32_t mmLock_ = 0;
  volatile uint32_t *mmWords() { return mmWords_; }
  volatile uint32_t *mmLock() { return &mmLock_; }
  void vote(uint32_t addr, uint32_t data)
  {
    if (addr == kAddrMxc) {
      mxc = data;
      event("vote mxc " + std::to_string(data));
    } else if (addr == kAddrMmcx) {
      mmcx = data;
      event("vote mmcx " + std::to_string(data));
      if (data < kMmcxNomL1 && !allowMmcxDrop) violation("MMCX voted below the boot display's NOM_L1 (hlvl %u)", data);
    } else if (addr == kAddrMm1) {
      mm1 = data;
      event(data == kBcmMm1On ? "mm1 on" : data == kBcmMm1Off ? "mm1 off" : "mm1 ?");
    } else {
      violation("a vote for an RPMh address that is not the video's: %#x", addr);
    }
  }
  uint32_t rscRd(uint32_t o)
  {
    if (o == 0) return 0x00030000;                    // DRV id: version 3
    if (o == 0xc) return 16u << 27 | 6u << 12;        // NCMD 16, NTCS of DRV 2: 6
    if (o == rpmh::kIrqStatus) return irqStatus;
    for (unsigned t = 0; t < 3; ++t) {
      const uint32_t b = kRscTcsBlock + t * kRscTcsStride;
      if (o == b + 0x28) return tcsBusy[t] ? 0 : 1;   // STATUS
    }
    return R(kRscDrv2 + o);
  }
  void rscWr(uint32_t o, uint32_t v)
  {
    if (o == rpmh::kIrqClear) {
      irqStatus &= ~v;
      return;
    }
    uint32_t stored = v;
    if (f.rscReadback && o >= kRscTcsBlock + 0x34) {   // a command's DATA register that does not read back
      const uint32_t within = (o - kRscTcsBlock) % kRscTcsStride;
      if (within >= 0x34 && (within - 0x34) % 24 == 8) stored = v ^ 1;
    }
    R(kRscDrv2 + o) = stored;
    for (unsigned t = 0; t < 3; ++t) {
      const uint32_t b = kRscTcsBlock + t * kRscTcsStride;
      if (o != b + 0x24 || v != (rpmh::kAmcEnable | rpmh::kAmcTrigger)) continue;
      if (f.rpmhNoComplete) {
        tcsBusy[t] = true;
        return;
      }
      const uint32_t enable = R(kRscDrv2 + b + 0x2c);
      for (unsigned i = 0; i < 16; ++i) {
        if (!(enable & (1u << i))) continue;
        vote(R(kRscDrv2 + b + 0x38 + 24 * i), R(kRscDrv2 + b + 0x3c + 24 * i));
        R(kRscDrv2 + b + 0x40 + 24 * i) = rpmh::kCmdComplete;
      }
      irqStatus |= 1u << t;
    }
  }

  // ------------------------------------------------------------------------------------------------------------ GCC
  bool gccEn(uint32_t o) { return R(kGcc + o) & kCbcrEnable; }
  uint32_t gccRd(uint32_t o)
  {
    uint32_t v = R(kGcc + o);
    if (o == kGccVideoAhb || o == kGccVideoXo || o == kGccVideoAxi0 || o == kGccVideoAxi1) {
      const bool on = (v & kCbcrEnable) && !(f.gccStuck && o == kGccVideoAhb);
      // Linux's clk-branch halt check is skipped for HW_CTL branches: CLK_OFF remains set
      // while the hardware gate is idle, even though CLK_ENABLE is asserted.
      v = on && !(v & kCbcrHwcg) ? v & ~kCbcrOff : v | kCbcrOff;
    }
    return v;
  }
  void gccWr(uint32_t o, uint32_t v)
  {
    const uint32_t old = R(kGcc + o);
    if ((o == kGccVideoAxi0 || o == kGccVideoAxi1) && (v & kCbcrAres) && !(old & kCbcrAres)) {
      event(o == kGccVideoAxi0 ? "reset axi0" : "reset axi1");
      if (o == kGccVideoAxi0 ? mvs0On() : mvs0cOn()) violation("an ARES with its GDSC on");
    }
    if (o == kGccVideoAhb && !(v & 1)) violation("GCC video AHB turned off");
    R(kGcc + o) = v;
  }

  // ------------------------------------------------------------------------------------------------------- VIDEO_CC
  bool pcalDone = false, rcgPending = false;
  uint32_t rcgActive = 0;   // the CFG the RCG runs with
  bool vccOk()
  {
    const uint32_t v = gccRd(kGccVideoAhb);
    return (!(v & kCbcrOff) || ((v & (kCbcrEnable | kCbcrHwcg)) == (kCbcrEnable | kCbcrHwcg))) && mxc >= kMxcLowSvs &&
           mmcx >= kMmcxLowSvs;
  }
  bool pllLocked()
  {
    const uint64_t p = kVideoCc + kCcPll0;
    return R(p + kPllOpmode) == kPllRun && (R(p) & kPllResetN) && (R(p + kPllL) & 0xffff) == kPllConfigL && !f.pllNoLock;
  }
  bool pllOut() { return pllLocked() && (R(kVideoCc + kCcPll0) & kPllOutctrl) && (R(kVideoCc + kCcPll0 + kPllUserCtl) & 7) == 7; }
  bool rcgRunning() { return ((rcgActive >> 8) & 7) == 1 ? pllOut() : true; }
  bool mvs0cOn() { return !(R(kVideoCc + kCcMvs0cGdsc) & kGdscCollapse) && !f.gdscStuck; }
  bool mvs0On()
  {
    const uint32_t g = R(kVideoCc + kCcMvs0Gdsc);
    return mvs0cOn() && (!(g & kGdscCollapse) || (g & kGdscHwControl)) && !f.codecGdscStuck;
  }
  bool branchOn(uint32_t o)
  {
    const uint32_t v = R(kVideoCc + o);
    if (!(v & kCbcrEnable)) return false;
    switch (o) {
    case kCcMvs0cClk:
    case kCcMvs0cFreerun: return mvs0cOn() && rcgRunning() && !f.branchStuck;
    case kCcMvs0Clk: return mvs0On() && rcgRunning();
    case kCcMvs0Freerun: return mvs0On() && rcgRunning() && !(R(kVideoCc + kCcMvs0FreerunMem) & kMemEnable);
    default: return true;
    }
  }
  uint32_t ccRd(uint32_t o)
  {
    uint32_t v = R(kVideoCc + o);
    switch (o) {
    case kCcPll0: return (v & ~(kPllLockDet | kPllPcalNotDone)) | (pllLocked() ? kPllLockDet : 0) | (pcalDone ? 0 : kPllPcalNotDone);
    case kCcMvs0Rcg: {
      v &= ~(kRcgUpdate | kRcgRootOff);
      if (rcgPending) {
        v |= kRcgUpdate;
        const uint32_t cfg = R(kVideoCc + kCcMvs0Rcg + 4);
        const bool sourceOk = ((cfg >> 8) & 7) == 1 ? pllOut() : true;
        if (!f.rcgStuck && sourceOk && (R(kVideoCc + kCcMvs0Rcg) & kRcgRootEn)) {
          rcgPending = false;
          rcgActive = cfg;
          event(((cfg >> 8) & 7) == 1 ? "rcg pll0" : "rcg xo");
        }
      }
      const bool child = branchOn(kCcMvs0cClk) || branchOn(kCcMvs0cFreerun) || branchOn(kCcMvs0Clk) || branchOn(kCcMvs0Freerun);
      if (!(R(kVideoCc + kCcMvs0Rcg) & kRcgRootEn) && !child) v |= kRcgRootOff;
      return v;
    }
    case kCcMvs0cGdsc: return (v & ~kGdscPwrOn) | (mvs0cOn() ? kGdscPwrOn : 0);
    case kCcMvs0Gdsc: return (v & ~kGdscPwrOn) | (mvs0On() ? kGdscPwrOn : 0);
    case kCcMvs0cGdsc + kGdscCfg: return mvs0cOn() ? kGdscPowerUpComplete : kGdscPowerDownComplete;
    case kCcMvs0Gdsc + kGdscCfg: return mvs0On() ? kGdscPowerUpComplete : kGdscPowerDownComplete;
    case kCcMvs0cClk:
    case kCcMvs0cFreerun:
    case kCcMvs0Clk:
    case kCcMvs0Freerun:
    case kCcAhbClk:
    case kCcXoClk:
    case kCcSleepClk: return branchOn(o) ? v & ~kCbcrOff : v | kCbcrOff;
    case kCcMvs0FreerunMem: return (v & ~kMemAck) | ((v & kMemEnable) ? 0 : kMemAck);
    case kCcMvs0Div: return f.wrongDivider ? 0 : kMvs0DivFor240;
    case kCcMvs0cDiv2: return 1;
    }
    return v;
  }
  void ccWr(uint32_t o, uint32_t v)
  {
    const bool wasC = mvs0cOn(), wasD = mvs0On();
    if (o == kCcMvs0Div || o == kCcMvs0cDiv2) violation("a write to a read-only divider");
    if (o == kCcMvs0Rcg && (v & kRcgUpdate)) rcgPending = true;
    if ((o == kCcMvs0cFreerun || o == kCcMvs0Freerun) && (v & kCbcrAres) && !(R(kVideoCc + o) & kCbcrAres)) {
      event(o == kCcMvs0cFreerun ? "reset core" : "reset vcodec0");
      if (mvs0cOn()) violation("a VIDEO_CC ARES with the controller GDSC on");
    }
    if ((o == kCcMvs0cClk || o == kCcMvs0Clk) && (v & kCbcrEnable) && !(R(kVideoCc + o) & kCbcrEnable)) {
      if (!mvs0cOn()) violation("a core clock enabled with its GDSC off");
      if (mxc < kMxcSvs) violation("the core clock on below MXC svs");
    }
    R(kVideoCc + o) = v & (o == kCcMvs0Rcg ? ~kRcgUpdate : ~0u);
    if (o == kCcPll0 + kPllOpmode && v == kPllRun && pllLocked()) pcalDone = true;
    const bool isC = mvs0cOn(), isD = mvs0On();
    if (!wasC && isC) event("gdsc mvs0c on");
    if (wasC && !isC) {
      event("gdsc mvs0c off");
      coreReset();
    }
    if (!wasD && isD) {
      event("gdsc mvs0 on");
      if (!isC) violation("MVS0 on without MVS0C");
    }
    if (wasD && !isD) event("gdsc mvs0 off");
    for (uint32_t b : {kCcMvs0cClk, kCcMvs0cFreerun, kCcMvs0Clk, kCcMvs0Freerun})
      if (o == b) event(std::string("branch ") + std::to_string(b) + ((v & 1) ? " on" : " off"));
  }

  // ------------------------------------------------------------------------------------------------------------ the core
  bool ctrlOn() { return mvs0cOn() && gccEn(kGccVideoAxi1) && branchOn(kCcMvs0cClk) && branchOn(kCcMvs0cFreerun); }
  bool codecOn() { return ctrlOn() && mvs0On() && gccEn(kGccVideoAxi0) && branchOn(kCcMvs0Clk) && branchOn(kCcMvs0Freerun); }
  void coreReset()
  {
    for (auto it = r.lower_bound(kIris); it != r.end() && it->first < kIris + kIrisBytes;) it = r.erase(it);
    fw.hfiUp = false;
    fw.pcReady = false;
    fw.ucError = 0;
  }
  uint32_t irisRd(uint32_t o)
  {
    if (o >= kVcodecIdleStatus && o < kVcodecIdleStatus + 4 * kVppPipes) return f.notIdle ? 0 : kVcodecIdle;
    switch (o) {
    case kWrapperHwVersion: return 0x30050000;
    case kCorePowerStatus: return codecOn() ? kCorePowered : 0;
    case kAonNocLpiStatus: return (R(kIris + kAonNocLpiCtl) & 1) ? (f.lpiDeny ? kLpiDeny : kLpiDone) : 0;
    case kCpuNocLpiStatus: return (R(kIris + kCpuNocLpiCtl) & 1) && !f.cpuLpiStuck ? kLpiDone : 0;
    case kAonVideoCtlNocLpiStatus: return (R(kIris + kAonVideoCtlNocLpiCtl) & 1) ? (f.lpiDeny ? kLpiDeny : kLpiDone) : 0;
    case kDebugBridgeLpiStatus: return 0;
    case kCtrlStatus: return ctrlStatus();
    case kTzCpuStatus: return fw.wfi ? 1 : 0;
    }
    return R(kIris + o);
  }
  void irisWr(uint32_t o, uint32_t v)
  {
    if (o == kCpuCsA2hSoftIntClr) violation("A2HSOFTINTCLR written");
    R(kIris + o) = v;
    if (o == kCtrlInit && (v & 1)) fwBoot();
    if (o == kCpuIcSoftInt) fwService();
    if (o == kAonNocLpiCtl && (v & 1)) event("lpi mvp");
    if (o == kCpuNocLpiCtl && (v & 1)) event("lpi cpu");
  }

  // ----------------------------------------------------------------------------------------------------------- the SMMU
  void smmuInit()
  {
    R(kSmmu + vsmmu::kIdr0) = 1u << 30 | 1u << 14 | 100;
    R(kSmmu + vsmmu::kIdr1) = 0x60000053;
    R(kSmmu + vsmmu::kIdr2) = 1u << 12 | 2u << 4;
    for (unsigned g = 0; g < 100; ++g) R(kSmmu + vsmmu::s2crOffset(g)) = 0x0300006e; // observed invalid free baseline
    R(kSmmu + vsmmu::smrOffset(0)) = 0x80000800;
    R(kSmmu + vsmmu::s2crOffset(0)) = 2;
    R(kSmmu + vsmmu::smrOffset(1)) = 0x80000060;
    R(kSmmu + vsmmu::s2crOffset(1)) = 1u << 16;
    R(kSmmu + vsmmu::smrOffset(3)) = 0x80011400;
    R(kSmmu + vsmmu::s2crOffset(3)) = 2;
    R(kSmmu + (128 + 2) * 4096) = 1;    // bank 2 translates (someone's)
    R(kSmmu + (128 + 81) * 4096) = 1;   // bank 81 translates, no group yet (another kext's)
    R(kSmmu + vsmmu::cbarOffset(80)) = 0; // observed unsupported TYPE0, must never be restored
    R(kSmmu + (128 + 80) * 4096 + vsmmu::kFsr) = 0x400; // Format=2, no valid fault
    for (unsigned b : {2u, 81u}) {
      R(kSmmu + vsmmu::cbarOffset(b)) = vsmmu::kCbarStage1;
      R(kSmmu + (128 + b) * 4096 + vsmmu::kTtbr0Hi) = b << 16;
      R(kSmmu + (128 + b) * 4096 + vsmmu::kTtbr1Hi) = b << 16;
    }
    R(kSmmu + vsmmu::cba2rOffset(80)) = 0;
    groupsFound = snapshotSmmu();
  }
  std::map<uint64_t, uint32_t> groupsFound;
  std::map<uint64_t, uint32_t> snapshotSmmu()
  {
    std::map<uint64_t, uint32_t> s;
    for (unsigned g = 0; g < 100; ++g) {
      s[vsmmu::smrOffset(g)] = R(kSmmu + vsmmu::smrOffset(g));
      s[vsmmu::s2crOffset(g)] = R(kSmmu + vsmmu::s2crOffset(g));
    }
    for (unsigned b = 0; b < 83; ++b) {
      s[vsmmu::cbarOffset(b)] = R(kSmmu + vsmmu::cbarOffset(b));
      s[vsmmu::cba2rOffset(b)] = R(kSmmu + vsmmu::cba2rOffset(b));
      s[(128 + b) * 4096] = R(kSmmu + (128 + b) * 4096) & 1;
    }
    return s;
  }
  uint32_t smmuRd(uint32_t o)
  {
    if ((o & 0xfff) == vsmmu::kTlbStatus && o >= 128 * 4096) return f.tlbStuck ? 1 : 0;
    if (o == vsmmu::kTlbGstatus) return 0;
    return R(kSmmu + o);
  }
  void smmuWr(uint32_t o, uint32_t v)
  {
    if (!ctrlOn() || !codecOn() || mm1 != kBcmMm1On) violation("SMMU write/TLB without video power and NoC");
    if (o >= 0x1000 && o < 0x1000 + 4 * 83 && ((v >> 16) & 3) != 1) violation("unsupported CBAR type restored");
    if (o >= 0xc00 && o < 0xc00 + 4 * 100) {
      if (!vsmmu::s2crRestorable(v, 83)) violation("illegal S2CR bank restored");
      if (f.faultIgnored && v == vsmmu::kS2crFault) return; // trapped FAULT write leaves legal translate
    }
    if (o >= 0x800 && o < 0x800 + 4 * 100 && !(v & 0x80000000u)) {
      event("smmu route-off");
      if (f.releaseSmrStuck) return;
    }
    if (o == vsmmu::kTlbGsync) event("smmu global-sync");
    if (o >= 128 * 4096 && (o & 0xfff) == vsmmu::kSctlr) {
      if (v & 1) event("smmu bank-on");
      v &= ~(1u << 12); // hardware keeps ASIDPNE at zero
      if (f.s1BadSctlr) v &= ~4u;
    }
    if (o >= 128 * 4096 && (o & 0xfff) == vsmmu::kTcr2) v |= 0x60; // observed TBI0/1 readback
    if (o >= 128 * 4096 && (o & 0xfff) == vsmmu::kTcr && f.s1BadTcr) v ^= 1;

    if (!v && (o == vsmmu::kTlbiAllNsnh || o == vsmmu::kTlbGsync || (o >= 128 * 4096 && ((o & 0xfff) == vsmmu::kTlbiAll ||
                                                                                         (o & 0xfff) == vsmmu::kTlbSync))))
      violation("an SMMU TLB register written with 0 (the trap path needs a non-zero value)");
    if (o >= 0x800 && o < 0x800 + 4 * 100 && (v & 0x80000000u)) {
      const unsigned g = (o - 0x800) / 4;
      const uint32_t s2cr = R(kSmmu + vsmmu::s2crOffset(g));
      const unsigned b = s2cr & 0xff;
      if (((s2cr >> 16) & 3) == 0 && !(R(kSmmu + (128 + b) * 4096) & 1)) violation("an SMR made valid before its bank translates");
      event("smr valid " + std::to_string(v & 0x7fff));
    }
    if ((o & 0xfff) == vsmmu::kFsr && o >= 128 * 4096) {
      R(kSmmu + o) &= ~v;   // write one to clear
      return;
    }
    R(kSmmu + o) = v;
  }
  // The SMMU's translation of a video stream's access, as the hardware walks our bank's table.
  bool translate(uint16_t sid, uint32_t iova, uint64_t *pa)
  {
    for (unsigned g = 0; g < 100; ++g) {
      const uint32_t smr = R(kSmmu + vsmmu::smrOffset(g));
      if (!(smr & 0x80000000u) || !vsmmu::covers(smr, sid)) continue;
      const uint32_t s2cr = R(kSmmu + vsmmu::s2crOffset(g)), type = (s2cr >> 16) & 3, b = s2cr & 0xff;
      if (type == 1) {
        *pa = iova;
        return true;
      }
      if (type != 0) return false;
      const uint32_t cb = (128 + b) * 4096;
      if (!(R(kSmmu + cb) & 1)) {
        *pa = iova;
        return true;
      }
      auto faultAt = [&](uint32_t bit) {
        R(kSmmu + cb + vsmmu::kFsr) |= bit;
        R(kSmmu + cb + vsmmu::kFarLo) = iova;
        return false;
      };
      if (f.sepFaults && ((R(kSmmu + cb + vsmmu::kTcr2) >> 15) & 7) != 7 && (iova >> 31)) return faultAt(1u << 1);
      if ((R(kSmmu + cb + vsmmu::kTcr) & 0x3f) != 32) return faultAt(1u << 1);
      const uint64_t ttbr = (uint64_t(R(kSmmu + cb + vsmmu::kTtbr0Hi) & 0xffff) << 32) | R(kSmmu + cb + vsmmu::kTtbr0Lo);
      uint64_t d = phys64(ttbr + 8 * (iova >> 30));
      if ((d & 3) != 3) return faultAt(1u << 1);
      d = phys64((d & vsmmu::kOutMask) + 8 * ((iova >> 21) & 511));
      if ((d & 3) != 3) return faultAt(1u << 1);
      d = phys64((d & vsmmu::kOutMask) + 8 * ((iova >> 12) & 511));
      if ((d & 3) != 3) return faultAt(1u << 1);
      if (!(d & vsmmu::kAf)) return faultAt(1u << 2);
      if (((d >> 2) & 7) != 0 || ((d >> 6) & 3) != 1) return faultAt(1u << 3);   // attribute 0, read-write
      *pa = (d & vsmmu::kOutMask) | (iova & 0xfff);
      return true;
    }
    return false;
  }

  // ------------------------------------------------------------------------------------------ physical memory, DMA, the core's view
  std::map<uint64_t, uint8_t *> phys;   // 4 KiB page -> host bytes
  std::vector<uint8_t> uc, scratchPage, sessionArena;
  std::vector<uint64_t> tableMem, sessionPa;
  unsigned dmaAllocs = 0, dmaFrees = 0;
  uint8_t *physPtr(uint64_t pa)
  {
    auto it = phys.find(pa & ~0xfffull);
    return it == phys.end() ? nullptr : it->second + (pa & 0xfff);
  }
  uint64_t phys64(uint64_t pa)
  {
    uint8_t *p = physPtr(pa);
    uint64_t v = 0;
    if (p) std::memcpy(&v, p, 8);
    return v;
  }
  void hashActive(const volatile uint8_t *base, char text[65])
  {
    CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx);
    session::activeRows(base, [&](const uint8_t *p, unsigned n) { CC_SHA256_Update(&ctx,p,n); });
    uint8_t digest[32]; CC_SHA256_Final(digest,&ctx); session::hexDigest(digest,text);
  }
  bool dmaAlloc(Dma &d)
  {
    if (f.dmaFail) return false;
    ++dmaAllocs;
    uc.assign(kUcBytes, 0);
    scratchPage.assign(4096, 0);
    const unsigned tablePages = d.session ? vsmmu::kSessionTablePages : vsmmu::kTablePages;
    tableMem.assign(tablePages * 512, 0x5a5a5a5a5a5a5a5aull);
    for (unsigned i = 0; i < vsmmu::kUcPages; ++i) {
      d.pages[i] = 0x8d0000000ull + uint64_t((i * 389u) % 773u) * 0x1000;   // scattered, unique
      phys[d.pages[i]] = uc.data() + 4096 * i;
    }
    d.scratch = 0x8e0000000ull;
    phys[d.scratch] = scratchPage.data();
    d.tablesPa = 0x8e1000000ull;
    for (unsigned i = 0; i < tablePages; ++i) phys[d.tablesPa + 4096 * i] = reinterpret_cast<uint8_t *>(tableMem.data()) + 4096 * i;
    d.tables = tableMem.data();
    if (d.session) {
      sessionPa.assign(session::kArenaPages,0);
      d.arenaPages = sessionPa.data();
      sessionArena.assign(session::kArenaBytes,0);
      d.arena = sessionArena.data();
      for (unsigned i=0; i<session::kArenaPages; ++i) {
        d.arenaPages[i]=0x900000000ull+uint64_t((i*7907)%session::kArenaPages)*4096;
        phys[d.arenaPages[i]]=sessionArena.data()+4096*i;
      }
    }
    d.uc = reinterpret_cast<volatile uint32_t *>(uc.data());
    return true;
  }
  void dmaFree(Dma &d)
  {
    if (fw.authed) violation("DMA freed before PAS quiesce");
    if (f.tlbStuck) violation("DMA freed after unconfirmed TLB sync");
    for (unsigned g = 0; g < 100; ++g) {
      const uint32_t smr = R(kSmmu + vsmmu::smrOffset(g));
      if ((smr & 0x80000000u) && (vsmmu::covers(smr, kStreamFirst) || vsmmu::covers(smr, kStreamSecond)))
        violation("DMA freed with a video stream still routed");
    }
    event("dma free");
    ++dmaFrees;
    phys.clear();
    d.uc = nullptr;
    d.arena = nullptr;
    d.arenaPages = nullptr;
    d.tables = nullptr;
  }
  bool dmaRd(uint16_t sid, uint32_t iova, uint32_t *v)
  {
    uint64_t pa = 0;
    if (!translate(sid, iova, &pa)) return false;
    uint8_t *p = physPtr(pa);
    if (!p) {
      violation("the core read %#x -> pa %#llx: not memory of ours", iova, (unsigned long long)pa);
      return false;
    }
    std::memcpy(v, p, 4);
    return true;
  }
  bool dmaWr(uint16_t sid, uint32_t iova, uint32_t v)
  {
    uint64_t pa = 0;
    if (!translate(sid, iova, &pa)) return false;
    uint8_t *p = physPtr(pa);
    if (!p) {
      violation("the core wrote %#x -> pa %#llx: not memory of ours", iova, (unsigned long long)pa);
      return false;
    }
    std::memcpy(p, &v, 4);
    return true;
  }

  // ------------------------------------------------------------------------------------------------------------ the firmware
  struct Fw {
    bool authed = false, suspended = false, hfiUp = false, pcReady = false, idle = false, wfi = false;
    uint32_t ucError = 0, session = 0;
    unsigned boots = 0, inits = 0, versions = 0, ifpcs = 0, pcPreps = 0, services = 0;
    uint32_t ubwc[7] = {};
    std::vector<uint32_t> headerIds, packetIds;
    uint32_t replyId = 0;
  } fw;
  uint32_t ctrlStatus()
  {
    if (!fw.hfiUp) return fw.ucError;
    return kCtrlStatusInitDone | (fw.idle ? kCtrlStatusIdle : 0) | (fw.pcReady ? kCtrlStatusPcReady : 0);
  }
  uint32_t qtbl() { return R(kIris + kQtblAddr); }
  bool ucRight()
  {
    const uint32_t t = qtbl();
    if (R(kIris + kUcRegionAddr) != t || (t & (kUcRegionAlign - 1)) || R(kIris + kUcRegionSize) != kUcRegionBytes ||
        R(kIris + kQtblInfo) != 1 || R(kIris + kWrapperSpare0) != 1 || R(kIris + kSfrAddr) != t + kSfrOffset + 1)
      return false;
    uint32_t v = 0;
    const struct {
      uint32_t off, want;
    } words[] = {{hfi::kTblSize, 456}, {hfi::kTblQhdr0Offset, 288}, {hfi::kTblQhdrSize, 56}, {hfi::kTblNumQ, 3}, {hfi::kTblNumActiveQ, 3},
                 {kSfrOffset, kSfrBytes}};
    for (const auto &w : words)
      if (!dmaRd(kStreamFirst, t + w.off, &v) || v != w.want) return false;
    for (unsigned q = 0; q < 3; ++q) {
      const uint32_t h = t + hfi::qhdr(q);
      const struct {
        uint32_t off, want;
      } qw[] = {{hfi::kQStatus, 1}, {hfi::kQStart, t + hfi::qbase(q)}, {hfi::kQType, q | 0x0101u << 16}, {hfi::kQSize, kHfiQueueBytes / 4},
                {hfi::kQPktSize, 0}};
      for (const auto &w : qw)
        if (!dmaRd(kStreamFirst, h + w.off, &v) || v != w.want) return false;
    }
    return true;
  }
  void fwBoot()
  {
    ++fw.boots;
    event("ctrl init");
    if (!fw.authed || fw.suspended || f.fwNoBoot) {
      fw.ucError = 0;
      return;
    }
    if (f.fwBadUc || !ucRight()) {
      fw.ucError = kCtrlStatusErrorUcRegion;
      return;
    }
    fw.hfiUp = true;
    fw.idle = fw.wfi = true;
    fw.pcReady = false;
  }
  // The firmware's side of a queue (variable-size packets in words, wrapping), through the SMMU.
  bool qRead(unsigned q, std::vector<uint32_t> &out)
  {
    const uint32_t h = qtbl() + hfi::qhdr(q), base = qtbl() + hfi::qbase(q);
    uint32_t w = 0, rd = 0, size = 0;
    if (!dmaRd(kStreamFirst, h + hfi::kQWrite, &w) || !dmaRd(kStreamFirst, h + hfi::kQRead, &rd) || w == rd) return false;
    if (!dmaRd(kStreamFirst, base + 4 * rd, &size) || !size || size > 4096) return false;
    out.assign(size / 4, 0);
    for (uint32_t i = 0; i < size / 4; ++i) dmaRd(kStreamFirst, base + 4 * ((rd + i) % (kHfiQueueBytes / 4)), &out[i]);
    dmaWr(kStreamSecond, h + hfi::kQRead, (rd + size / 4) % (kHfiQueueBytes / 4));
    return true;
  }
  void qWrite(unsigned q, const std::vector<uint32_t> &msg)
  {
    const uint32_t h = qtbl() + hfi::qhdr(q), base = qtbl() + hfi::qbase(q);
    uint32_t w = 0;
    if (!dmaRd(kStreamFirst, h + hfi::kQWrite, &w)) return;
    for (size_t i = 0; i < msg.size(); ++i) dmaWr(kStreamSecond, base + 4 * ((w + i) % (kHfiQueueBytes / 4)), msg[i]);
    dmaWr(kStreamSecond, h + hfi::kQWrite, uint32_t((w + msg.size()) % (kHfiQueueBytes / 4)));
  }
  void reply(uint32_t type, uint32_t flags, const std::vector<uint32_t> &payload)
  {
    std::vector<uint32_t> m = {uint32_t(64 + 4 * payload.size()), 0, fw.replyId++, 0, 0, 0, 0, 1,
                               uint32_t(32 + 4 * payload.size()), type, flags, 0, 0, 0, 0, 0};
    m.insert(m.end(), payload.begin(), payload.end());
    qWrite(hfi::kMsgq, m);
  }
  void debugLine(const char *text)
  {
    std::vector<uint8_t> b(16 + 1 + std::strlen(text) + 1, 0);
    std::memcpy(b.data() + 17, text, std::strlen(text));
    while (b.size() % 4) b.push_back(0);
    std::vector<uint32_t> m(b.size() / 4);
    std::memcpy(m.data(), b.data(), b.size());
    m[0] = uint32_t(b.size());
    qWrite(hfi::kDbgq, m);
  }
  void fwService()
  {
    if (!fw.hfiUp || f.fwNoReply) return;
    ++fw.services;
    std::vector<uint32_t> msg;
    while (qRead(hfi::kCmdq, msg)) {
      if (msg[1] && msg[1] != fw.session) violation("a host message with a session before any session");
      fw.headerIds.push_back(msg[2]);
      uint32_t at = 8;
      for (uint32_t i = 0; i < msg[7] && at < msg.size(); ++i) {
        const uint32_t *p = msg.data() + at, size = p[0], type = p[1], flags = p[2];
        fw.packetIds.push_back(p[5]);
        if (type == session::Open) fw.session = p[8];
        else if (type == kHfiCmdInit) {
          ++fw.inits;
          if (p[8] != kHfiArchLx || flags != (kHfiHostResponse | kHfiHostIntr | kHfiHostNonDiscardable) || size != 36)
            violation("a malformed INIT packet");
          debugLine("VIDEO: system init");
          if (f.fwSysError) reply(0x05000001, kHfiFwSystemError, {});
          else reply(kHfiCmdInit, f.fwInitFail ? 0 : kHfiFwSuccess, {});
        } else if (type >= kHfiPropUbwcMaxChannels && type <= kHfiPropUbwcBankSpreading) {
          fw.ubwc[type - kHfiPropUbwcMaxChannels] = p[8];
        } else if (type == kHfiPropImageVersion) {
          ++fw.versions;
          if (!(flags & kHfiHostGetProperty) || size != 32) violation("a malformed IMAGE_VERSION get-property");
          if (!f.fwNoVersion) {
            char text[kHfiVersionChars] = "video-firmware.vpu35.model";
            text[30] = 0;   // NULs inside become spaces
            text[31] = 'x';
            std::vector<uint32_t> payload(kHfiVersionChars / 4);
            std::memcpy(payload.data(), text, kHfiVersionChars);
            reply(kHfiPropImageVersion, kHfiFwSuccess, payload);
          }
        } else if (type == kHfiPropIntraFramePc) {
          ++fw.ifpcs;
        } else if (type == kHfiCmdPowerCollapse) {
          ++fw.pcPreps;
          fw.pcReady = true;
        }
        at += size / 4;
      }
    }
    fw.idle = fw.wfi = !f.fwNotIdle;
  }

  // --------------------------------------------------------------------------------------------- the firmware file, the secure world
  std::vector<uint8_t> image, carve, meta;
  unsigned releases = 0, metaFrees = 0;
  bool carveMapped = false;
  int depth = 0, maxDepth = 0;
  std::vector<std::pair<std::string, int>> calls;
  int fwRead(const uint8_t **bytes, uint64_t *size)
  {
    if (f.noFile) return 2;
    *bytes = image.data();
    *size = image.size();
    return 0;
  }
  void fwRelease() { ++releases; }
  void sha256(const uint8_t *p, uint64_t n, uint8_t out[32]) { ::sha256(p, n, out); }
  uint8_t *fwMetadata(uint64_t bytes, uint64_t *pa)
  {
    meta.assign(bytes, 0xee);
    *pa = 0x8f0000000ull;
    return meta.data();
  }
  void fwMetadataFree() { ++metaFrees; }
  volatile uint8_t *carveoutMap(uint64_t pa, uint64_t bytes)
  {
    if (pa != kCarveout || bytes > kCarveoutBytes) violation("a carveout mapping outside video@9c300000");
    carveMapped = true;
    return carve.data();
  }
  void carveoutUnmap() { carveMapped = false; }
  void scmHold(bool take)
  {
    depth += take ? 1 : -1;
    if (depth < 0) violation("the secure-call lock given back without a hold");
    if (depth > maxDepth) maxDepth = depth;
    event(take ? "scm take" : "scm give");
  }
  int call(const char *name)
  {
    calls.push_back({name, depth});
    event(std::string("scm ") + name);
    return f.scmFail == name ? 1 : 0;
  }
  bool metaRight() const
  {
    const ImageSpec s;
    const std::vector<uint8_t> img = makeImage(s);
    if (meta.size() != 212 + 0x100) return false;
    return std::memcmp(meta.data(), img.data(), 212) == 0 && std::memcmp(meta.data() + 212, img.data() + 0x2000, 0x100) == 0;
  }
  bool carveRight() const
  {
    for (uint32_t i = 0; i < 0x800; ++i)
      if (carve[i] != image[0x1000 + i]) return false;
    for (uint32_t i = 0; i < 0x400; ++i)
      if (carve[0x1000 + i] != image[0x1800 + i]) return false;
    for (uint32_t i = 0x1400; i < 0x3000; ++i)
      if (carve[i]) return false;
    for (uint32_t i = 0x100000; i < 0x101000; ++i)
      if (carve[i]) return false;
    return carve[0x800] == 0xaa;   // not part of any segment: untouched
  }
  int scmPasInit(uint64_t pa)
  {
    if (pa != 0x8f0000000ull || !metaRight()) violation("init_image with the wrong metadata");
    if (!codecOn()) violation("PAS with the core unpowered");
    return call("init");
  }
  int scmPasMemSetup(uint64_t pa, uint64_t bytes)
  {
    if (pa != kCarveout || bytes != 0x101000) violation("mem_setup with %#llx + %#llx", (unsigned long long)pa, (unsigned long long)bytes);
    return call("memsetup");
  }
  int scmPasAuth()
  {
    if (!carveRight()) violation("auth_and_reset with the carveout not as the image says");
    if (carveMapped) violation("auth_and_reset with the carveout still mapped");
    const int rc = call("auth");
    if (!rc) fw.authed = true;
    return rc;
  }
  int scmMpVideoVar(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
  {
    if (a != 0 || b != 0x25800000 || c != 0x01000000 || d != 0x24800000) violation("MP_VIDEO_VAR with other ranges");
    if (!fw.authed) violation("MP_VIDEO_VAR before auth_and_reset");
    return call("mpvideovar");
  }
  int scmPasShutdown()
  {
    if (!codecOn()) violation("PAS shutdown with the core unpowered");
    const int rc = call("shutdown");
    if (!rc) { fw.authed = false; fw.hfiUp = false; }
    return rc;
  }
  int scmRemoteState(uint32_t s)
  {
    if (s == 0) {
      if (!fw.pcReady) violation("SET_REMOTE_STATE(0) before PC_READY");
      fw.suspended = true;
    } else {
      fw.suspended = false;
    }
    return call(s ? "remote1" : "remote0");
  }

  // ------------------------------------------------------------------------------------------------------------- dispatch
  uint32_t rd(uint64_t pa)
  {
    ++reads;
    if (in(pa, kGcc, 0x200000)) return gccRd(uint32_t(pa - kGcc));
    if (in(pa, kVideoCc, kVideoCcBytes)) {
      if (!vccOk()) violation("VIDEO_CC +%#x read with its rails or GCC's video AHB off", uint32_t(pa - kVideoCc));
      return ccRd(uint32_t(pa - kVideoCc));
    }
    if (in(pa, kIris, kIrisBytes)) {
      const uint32_t o = uint32_t(pa - kIris);
      if (o == kCpuIcSoftInt || o == kCpuCsA2hSoftIntClr) violation("an acting register read");
      if (o < 0xa0000 ? !codecOn() : !ctrlOn()) violation("core +%#x read with its domain off", o);
      return irisRd(o);
    }
    if (in(pa, kSmmu, kSmmuBytes)) return smmuRd(uint32_t(pa - kSmmu));
    if (in(pa, kRscDrv2, kRscDrv2Bytes)) {
      if (!drv2Held) violation("DRV-2 read without the DRV-2 lock");
      return rscRd(uint32_t(pa - kRscDrv2));
    }
    violation("a read of %#llx", (unsigned long long)pa);
    return 0;
  }
  void wr(uint64_t pa, uint32_t v)
  {
    ++writes;
    if (in(pa, kGcc, 0x200000)) return gccWr(uint32_t(pa - kGcc), v);
    if (in(pa, kVideoCc, kVideoCcBytes)) {
      if (!vccOk()) violation("VIDEO_CC +%#x written with its rails or GCC's video AHB off", uint32_t(pa - kVideoCc));
      return ccWr(uint32_t(pa - kVideoCc), v);
    }
    if (in(pa, kIris, kIrisBytes)) {
      const uint32_t o = uint32_t(pa - kIris);
      if (o < 0xa0000 ? !codecOn() : !ctrlOn()) violation("core +%#x written with its domain off", o);
      return irisWr(o, v);
    }
    if (in(pa, kSmmu, kSmmuBytes)) return smmuWr(uint32_t(pa - kSmmu), v);
    if (in(pa, kRscDrv2, kRscDrv2Bytes)) {
      if (!drv2Held) violation("DRV-2 written without the DRV-2 lock");
      return rscWr(uint32_t(pa - kRscDrv2), v);
    }
    violation("a write to %#llx", (unsigned long long)pa);
  }

  // Everything back off: what every failure must leave.
  bool allOff()
  {
    const bool clocks = !(R(kVideoCc + kCcMvs0cClk) & 1) && !(R(kVideoCc + kCcMvs0cFreerun) & 1) && !(R(kVideoCc + kCcMvs0Clk) & 1) &&
                        !(R(kVideoCc + kCcMvs0Freerun) & 1) && !gccEn(kGccVideoAxi0) && !gccEn(kGccVideoAxi1);
    const bool pll = R(kVideoCc + kCcPll0 + kPllOpmode) == kPllStandby && !(R(kVideoCc + kCcPll0) & kPllOutctrl);
    return !mvs0cOn() && !mvs0On() && clocks && pll && mxc == 0 && mm1 != kBcmMm1On && !drv2Held && depth == 0;
  }
};

// The pin of the synthetic image (Firmware::pinBytes/pinSha), set for every run.
struct Pin {
  uint8_t sha[32];
  uint64_t bytes;
};
static Pin pinOf(const std::vector<uint8_t> &img)
{
  Pin p;
  sha256(img.data(), img.size(), p.sha);
  p.bytes = img.size();
  return p;
}

struct Run {
  std::unique_ptr<Model> m = std::make_unique<Model>();
  std::unique_ptr<Bringup<Model>> b;
  Pin pin;
  Options o;
  Run(unsigned level)
  {
    b = std::make_unique<Bringup<Model>>(*m);
    o.level = level;
    o.holdMs = 5;
    o.idleMs = 1500;
    pin = pinOf(m->image);
    b->firmware.pinBytes = pin.bytes;
    b->firmware.pinSha = pin.sha;
  }
  int go() { return b->run(o); }
};

static void quiet(const Model &m, const char *what)
{
  if (!m.violations.empty()) {
    std::printf("  %s: %zu violation(s), first: %s\n", what, m.violations.size(), m.violations[0].c_str());
    ++failures;
  }
}

// ======================================================================================================================== V1
static void testInert()
{
  for (unsigned level : {0u, 1u, 7u}) {
    Run x(level);
    CHECK(x.go() == kErrArgument);
    CHECK(x.m->reads == 0 && x.m->writes == 0 && x.b->log.writes == 0);
  }
}

static void testV1Cycles()
{
  Run one(2);
  CHECK(one.go() == kOk);
  quiet(*one.m, "V1 one cycle");
  Run x(2);
  x.o.cycles = 20;
  const int rc = x.go();
  CHECK(rc == kOk);
  quiet(*x.m, "V1 20 cycles");
  Model &m = *x.m;
  CHECK(x.b->rep.cyclesOk == 20 && x.b->rep.cycles == 20);
  CHECK(m.allOff());
  // The votes: MXC up and down every cycle, MM1 on and off, MMCX sent once at the display's floor and never lowered.
  CHECK(m.count("vote mxc 4") == 20 && m.count("vote mxc 0") == 20);
  CHECK(m.count("mm1 on") == 20 && m.count("mm1 off") == 20);
  CHECK(m.count("vote mmcx 8") == 1 && m.mmcx == kMmcxNomL1);
  CHECK(one.b->votes.requests == 4 && x.b->votes.requests == 80);   // rails up, MM1 on, MM1 off, rails down: four a cycle
  // The order, cycle 1: rails before VIDEO_CC, MVS0C before its clocks, PLL0 locked before the RCG switch, MVS0C before MVS0, the
  // codec off before the controller, the resets after the controller's GDSC, MM1 off before the rails go down.
  CHECK(m.first("vote mxc 4") < m.first("gdsc mvs0c on"));
  CHECK(m.first("gdsc mvs0c on") < m.first("rcg pll0") && m.first("rcg pll0") < m.first("branch 32860 on"));
  CHECK(m.first("gdsc mvs0c on") < m.first("gdsc mvs0 on"));
  CHECK(m.first("gdsc mvs0 off") < m.first("gdsc mvs0c off"));
  CHECK(m.first("gdsc mvs0c off") < m.first("reset axi0") && m.first("reset vcodec0") > m.first("gdsc mvs0c off"));
  CHECK(m.first("lpi mvp") < m.first("gdsc mvs0 off") && m.first("lpi cpu") < m.first("gdsc mvs0c off"));
  CHECK(m.first("mm1 off") < m.first("vote mxc 0"));
  CHECK(m.first("rcg xo") > m.first("gdsc mvs0 off") && m.first("rcg xo") < m.first("gdsc mvs0c off"));
  CHECK(m.count("reset axi0") == 20 && m.count("reset axi1") == 20 && m.count("reset core") == 20 && m.count("reset vcodec0") == 20);
  CHECK(m.R(kVideoCc + kCcMvs0Rcg + 4) == kRcgCfgXo);
  // Every write logged once: twenty cycles log what two cycles log (the second cycle adds exactly the two GDSC words with RETAIN_FF
  // kept from the first, as Linux keeps it); the log has one line per logged pair.
  Run two(2);
  two.o.cycles = 2;
  CHECK(two.go() == kOk);
  std::set<std::string> firstCycle;
  for (const auto &l : one.m->lines)
    if (l.rfind("write ", 0) == 0) firstCycle.insert(l);
  std::vector<std::string> added;
  for (const auto &l : two.m->lines)
    if (l.rfind("write ", 0) == 0 && !firstCycle.count(l)) added.push_back(l);
  CHECK(added.size() == 2 && added[0] == "write videocc+0x8034 = 00226800 (mvs0c GDSC: out of collapse)" &&
        added[1] == "write videocc+0x8068 = 00226800 (mvs0 GDSC: out of collapse)");
  CHECK(x.b->log.logged == two.b->log.logged && x.b->log.writes > 15 * one.b->log.writes);
  unsigned writeLines = 0;
  for (const auto &l : m.lines) writeLines += l.rfind("write ", 0) == 0;
  CHECK(writeLines == x.b->log.logged && x.b->log.overflow == 0);
  CHECK(m.logged("V1: cycle 20/20 ok"));
  CHECK(m.logged("branch gcc_video_ahb_clk: on raw before 80000002"));
  CHECK(m.logged("branch gcc_video_ahb_clk: on raw after 80000003 (halt check skipped)"));
  CHECK(m.logged("branch gcc_video_xo_clk: on raw after 00000001 (halt check skipped)"));
  CHECK(m.logged("GDSC mvs0c GDSC: out of collapse raw before"));
  CHECK(m.logged("GDSC mvs0c GDSC: out of collapse raw after"));
  CHECK(m.logged("branch video_cc_mvs0c_freerun_clk: on raw after") && m.logged("(running)"));
  CHECK(std::strstr(x.b->rep.summary, "V1 ok: 20/20 cycles"));
  // The calibration ran once (PCAL_NOT_DONE), PLL0's configuration was written once (the probe's set-up is once per boot).
  CHECK(m.pcalDone);
}

static void testV1AhbOff()
{
  Run x(2);
  x.m->coldAhbOff();
  CHECK(x.go() == kOk);
  quiet(*x.m, "V1 with GCC's video AHB off at first");
  CHECK(x.b->pst.gccAhbOn && x.m->gccEn(kGccVideoAhb));   // turned on and left on, as Linux does
}

static void testV1Faults()
{
  struct Case {
    const char *name;
    void (*set)(Faults &);
    int error, step;
  } cases[] = {
      {"rails not completed", [](Faults &f) { f.rpmhNoComplete = true; }, kErrRpmh, kStepRails},
      {"DRV-2 lock busy", [](Faults &f) { f.drv2Busy = true; }, kErrRpmh, kStepRails},
      {"an RSC write not read back", [](Faults &f) { f.rscReadback = true; }, kErrRpmh, kStepRails},
      {"controller GDSC stuck", [](Faults &f) { f.gdscStuck = true; }, kErrGdsc, kStepCtrlGdsc},
      {"PLL0 no lock", [](Faults &f) { f.pllNoLock = true; }, kErrPll, kStepCtrlClocks},
      {"RCG update stuck", [](Faults &f) { f.rcgStuck = true; }, kErrRcg, kStepCtrlClocks},
      {"mvs0 divider", [](Faults &f) { f.wrongDivider = true; }, kErrDivider, kStepCtrlClocks},
      {"branch stuck", [](Faults &f) { f.branchStuck = true; }, kErrBranch, kStepCtrlClocks},
      {"codec GDSC stuck", [](Faults &f) { f.codecGdscStuck = true; }, kErrGdsc, kStepCodecGdsc},
  };
  for (const Case &c : cases) {
    Run x(2);
    c.set(x.m->f);
    if (x.m->f.gccStuck) x.m->coldAhbOff();
    const int rc = x.go();
    if (rc != c.error || x.b->rep.failedStep != c.step)
      std::printf("  case %s: rc %d (%s) step %d\n", c.name, rc, errorName(rc), x.b->rep.failedStep);
    CHECK(rc == c.error && x.b->rep.failedStep == c.step);
    CHECK(x.m->allOff());
    if (x.m->f.rpmhNoComplete) CHECK(x.b->votes.stuck[0] + x.b->votes.stuck[1] + x.b->votes.stuck[2] == 1 && x.m->mxc == 0);
    quiet(*x.m, c.name);
    CHECK(std::strstr(x.b->rep.summary, "V1 FAILED"));
  }
}

// MVS0 found hardware-controlled with SW_COLLAPSE clear (the survey's "off, hardware-controlled"): VIDEO_CC's set-up takes it out of
// hardware control, as Linux's gdsc_init does, which leaves it requested on; a failure before the codec's own step still collapses it.
static void testV1FoundRequestedOn()
{
  for (bool fail : {false, true}) {
    Run x(2);
    x.m->R(kVideoCc + kCcMvs0Gdsc) = kGdscHwControl | 0x00222000;
    x.m->f.pllNoLock = fail;
    CHECK(x.go() == (fail ? kErrPll : kOk));
    CHECK(x.m->logged("the codec GDSC was found requested on") && !x.m->mvs0On() && x.m->allOff());
    quiet(*x.m, "V1 MVS0 found requested on");
  }
}

// PLL0 found running at another rate: its configuration is kept (as Linux does), so the power-on refuses it at step 15.
static void testV1PllFoundRunning()
{
  Run x(2);
  const uint64_t p = kVideoCc + kCcPll0;
  x.m->R(p) = kPllOutctrl | kPllResetN;
  x.m->R(p + kPllOpmode) = kPllRun;
  x.m->R(p + kPllUserCtl) = kPllOutMask;
  x.m->R(p + kPllL) = 0x30;
  x.m->R(p + kPllAlpha) = 0;
  x.m->pcalDone = true;   // a PLL that runs has calibrated
  CHECK(x.go() == kErrPll && x.b->rep.failedStep == kStepCtrlClocks);
  CHECK(x.m->logged("PLL0 already runs (L 0x30)") && x.m->logged("not Linux's 720 MHz configuration: refused"));
  CHECK(!x.m->mvs0cOn() && !x.m->mvs0On() && x.m->mxc == 0 && x.m->mm1 != kBcmMm1On);
}

static void testV1Stops()
{
  for (int s : {int(kStepRails), int(kStepBus), int(kStepGcc), int(kStepCcInit), int(kStepCtrlGdsc), int(kStepCtrlClocks),
                int(kStepCodecGdsc), int(kStepCodecClocks), int(kStepPreset), int(kStepReadout)}) {
    Run x(2);
    x.o.stop = s;
    const int rc = x.go();
    CHECK(rc == kErrStopped && x.b->rep.failedStep == s);
    CHECK(x.m->allOff());
    quiet(*x.m, "V1 stop");
  }
}

static void testV1OffWarnings()
{
  for (int which = 0; which < 3; ++which) {
    Run x(2);
    if (which == 0) x.m->f.lpiDeny = true;
    if (which == 1) x.m->f.notIdle = true;
    if (which == 2) x.m->f.cpuLpiStuck = true;
    CHECK(x.go() == kOk);   // Linux goes on past these; so do we: the power-off completes
    CHECK(x.b->pst.offWarnings >= 1 && x.m->allOff());
    quiet(*x.m, "V1 off-side warnings");
  }
}

static void testMmcxFloor()
{
  Run x(2);
  x.o.mmcxFloor = 0;
  x.m->allowMmcxDrop = true;
  CHECK(x.go() == kOk);
  CHECK(x.m->count("vote mmcx 8") == 1 && x.m->count("vote mmcx 4") == 0 && x.m->count("vote mmcx 0") == 0);
  CHECK(x.b->votes.floorVlvl == kMmcxDisplayFloor);
  CHECK(x.m->logged("boot-display guard"));
  quiet(*x.m, "V1 floor 0");
}

// The shared votes (package/include/PianoMmVotes.h) with the camera as the other client: the camera already holds MXC at low_svs
// (hlvl 3), MMCX at the floor (hlvl 8) and MM1 on. A V1 cycle raises MXC to svs and, at its power-off, gives MXC back to the
// camera's level instead of 0 and leaves MM1 on: the video core's release never drops the camera's need. Then the camera's own
// release (as PianoCamera makes it) takes MXC to 0 and MM1 off.
static void testSharedVotes()
{
  Run x(2);
  Model &m = *x.m;
  volatile uint32_t *w = m.mmWords();
  w[piano_mm_need_index(PIANO_MM_MXC, PIANO_MM_CAMERA)] = 0x40;
  w[piano_mm_need_index(PIANO_MM_MMCX, PIANO_MM_CAMERA)] = 0x40;
  w[piano_mm_need_index(PIANO_MM_MM1, PIANO_MM_CAMERA)] = PIANO_MM_BCM_ON;
  w[piano_mm_floor_index(PIANO_MM_MMCX)] = kVlvlNomL1;
  w[piano_mm_sent_index(PIANO_MM_MXC)] = 3;
  w[piano_mm_sent_index(PIANO_MM_MMCX)] = 8;
  w[piano_mm_sent_index(PIANO_MM_MM1)] = PIANO_MM_BCM_ON;
  w[PIANO_MM_SENT_VALID] = PIANO_MM_ALL;
  m.mxc = 3;
  m.mm1 = kBcmMm1On;
  CHECK(x.go() == kOk);
  quiet(m, "V1 with the camera's votes");
  CHECK(m.count("vote mxc 4") == 1 && m.count("vote mxc 3") == 1 && m.count("vote mxc 0") == 0);
  CHECK(m.count("mm1 on") == 0 && m.count("mm1 off") == 0 && m.mm1 == kBcmMm1On && m.mxc == 3);
  CHECK(m.count("vote mmcx 8") == 0 && m.mmcx == kMmcxNomL1);
  CHECK(x.b->votes.requests == 2);   // MXC up, MXC back to the camera's level; MM1 needed by the camera throughout
  CHECK(piano_mm_need(&x.b->votes.mm, PIANO_MM_MXC, PIANO_MM_VIDEO) == 0 && w[piano_mm_sent_index(PIANO_MM_MXC)] == 3);
  // The camera's release through the same words (its kext's half, simulated with the video model's DRV-2 as the transport).
  struct Cam {
    Model *m;
    static uint32_t addr(void *, unsigned r) { return r == PIANO_MM_MXC ? kAddrMxc : r == PIANO_MM_MMCX ? kAddrMmcx : kAddrMm1; }
    static int level(void *, unsigned, uint32_t vlvl, uint32_t *h)
    {
      *h = vlvl == 0 ? 0 : vlvl <= 0x40 ? 3 : 8;
      return 0;
    }
    static int send(void *c, const PianoMmCmd *cmds, unsigned n)
    {
      for (unsigned i = 0; i < n; ++i) static_cast<Cam *>(c)->m->vote(cmds[i].addr, cmds[i].data);
      return 0;
    }
    static void sleep(void *, uint32_t) {}
  } cam{&m};
  const PianoMmVotesOps ops = {&cam, Cam::addr, Cam::level, Cam::send, Cam::sleep, nullptr};
  PianoMmVotes c{};
  c.words = w;
  c.lock = m.mmLock();
  c.tag = PianoMmTagCamera;
  c.client = PIANO_MM_CAMERA;
  c.floor[PIANO_MM_MMCX] = kVlvlNomL1;
  c.ops = &ops;
  CHECK(piano_mm_votes_bus(&c, 0) == 0 && m.mm1 == kBcmMm1Off);
  CHECK(piano_mm_votes_rails(&c, 0, 0) == 0 && m.mxc == 0 && m.mmcx == kMmcxNomL1);
  CHECK(m.allOff() && *m.mmLock() == 0);
  quiet(m, "the camera's release");
}

static void testNoCmdDb()
{
  Run x(2);
  x.m->f.noCmdDb = true;
  CHECK(x.go() == kErrRpmh && x.m->writes == 0);
  Run y(2);
  y.m->cmddb = makeCmdDb({{"mxc.lvl", kAddrMxc, levelsAux(kMxcLevels, 9, 7)}}, {{"MM1", kAddrMm1, bcmAux(1000, 32, 1)}});
  CHECK(y.go() == kErrRpmh && y.m->writes == 0 && y.b->res.missing && std::strcmp(y.b->res.missing, "mmcx.lvl") == 0);
}

// ======================================================================================================================== V2
static void testV2()
{
  CHECK(kPasId == 9 && kPasId != 1 && kPasId != 13 && kPasId != 36);
  CHECK(kCarveout == 0x9c300000 && kCarveoutBytes == 0x800000);
  Run x(3);
  x.o.cycles = 3;
  const int rc = x.go();
  CHECK(rc == kOk);
  quiet(*x.m, "V2");
  Model &m = *x.m;
  CHECK(x.b->rep.cyclesOk == 3 && m.allOff() && !m.fw.authed && x.b->votes.requests == 12);
  // Per cycle: init, mem_setup, auth, MP_VIDEO_VAR under one hold of the secure-call lock; the shutdown a call of its own.
  CHECK(m.calls.size() == 15);
  for (unsigned c = 0; c < 3; ++c) {
    const auto *k = &m.calls[5 * c];
    CHECK(k[0].first == "init" && k[1].first == "memsetup" && k[2].first == "auth" && k[3].first == "mpvideovar" && k[4].first == "shutdown");
    CHECK(k[0].second == 1 && k[1].second == 1 && k[2].second == 1 && k[3].second == 1 && k[4].second == 0);
  }
  CHECK(m.maxDepth == 1 && m.count("scm take") == 3 && m.count("scm give") == 3);
  CHECK(m.last("scm shutdown") < m.last("gdsc mvs0 off"));   // PAS shutdown while the core is powered
  CHECK(m.releases == 3 && m.metaFrees == 3);
  CHECK(m.logged("the pinned vpu35_4v.mbn"));
  CHECK(m.logged("3 loadable, hash at 4, relocatable, span 0x101000 at 0xf500000"));
  CHECK(std::strstr(x.b->rep.summary, "V2 ok: 3/3 cycles"));
  CHECK(!m.logged("V3 ") && m.dmaAllocs == 0 && m.fw.boots == 0);
}

static void testV2CpuLpiTimeoutCleanup()
{
  Run x(3);
  x.o.cycles = 3;
  x.m->f.cpuLpiStuck = true;
  CHECK(x.go() == kOk);
  quiet(*x.m, "V2 CPU NoC timeout cleanup");
  CHECK(x.b->rep.cyclesOk == 3);
  CHECK(x.m->count("lpi cpu") == 3);
  CHECK((x.m->R(kIris + kCpuNocLpiCtl) & kLpiReq) == 0);
  CHECK(x.m->logged("CPU NoC LPI: request withdrawn after timeout (cycle-safe)"));
  unsigned readbacks = 0;
  for (const auto &line : x.m->lines)
    readbacks += line == "power-off: CPU NoC request readback 00000000 after timeout (cycle-safe)";
  CHECK(readbacks == 3);
}

static void testV2Refusals()
{
  {   // the pin: another file is refused before any secure call
    Run x(3);
    x.b->firmware.pinBytes = kFirmwareBytes;
    x.b->firmware.pinSha = kFirmwareSha256;
    CHECK(x.go() == kErrFwPin && x.b->rep.failedStep == kStepFwPin);
    CHECK(x.m->calls.empty() && x.m->allOff() && x.m->releases == 1);
    CHECK(x.m->logged("NOT the pinned file"));
    quiet(*x.m, "V2 pin");
  }
  {   // one byte changed: refused
    Run x(3);
    x.m->image[0x1100] ^= 1;
    CHECK(x.go() == kErrFwPin && x.m->calls.empty());
  }
  for (int kind = 0; kind < 3; ++kind) {   // images that do not parse or do not fit
    Run x(3);
    ImageSpec s;
    s.noHash = kind == 0;
    s.outside = kind == 1;
    s.notElf = kind == 2;
    x.m->image = makeImage(s);
    x.pin = pinOf(x.m->image);
    x.b->firmware.pinBytes = x.pin.bytes;
    x.b->firmware.pinSha = x.pin.sha;
    CHECK(x.go() == kErrFwImage && x.b->rep.failedStep == kStepFwParse && x.m->calls.empty() && x.m->allOff());
  }
  {
    Run x(3);
    x.m->f.noFile = true;
    CHECK(x.go() == kErrFwFile && x.b->rep.failedStep == kStepFwFile && x.m->calls.empty() && x.m->allOff());
    CHECK(x.m->logged("PianoVideo/vpu35_4v.mbn not loaded") && x.m->logged("stage the owner's copy"));
  }
}

static void testV2ScmFailures()
{
  struct Case {
    const char *call;
    int step;
    bool shutdown;
  } cases[] = {{"init", kStepPasInit, false},   {"memsetup", kStepPasMemSetup, true}, {"auth", kStepPasAuth, true},
               {"mpvideovar", kStepMpVideoVar, true}, {"shutdown", kStepPasShutdown, false}};
  for (const Case &c : cases) {
    Run x(3);
    x.m->f.scmFail = c.call;
    const int rc = x.go();
    CHECK(rc == kErrScm && x.b->rep.failedStep == c.step);
    CHECK(x.m->allOff() && x.m->depth == 0);
    const unsigned shutdowns = x.m->count("scm shutdown");
    CHECK(std::strcmp(c.call, "shutdown") == 0 ? shutdowns == 1 : shutdowns == (c.shutdown ? 1u : 0u));
    if (c.shutdown) CHECK(x.m->calls.back().first == "shutdown" && x.m->calls.back().second == 1);   // inside the same hold
    quiet(*x.m, c.call);
  }
}

// ======================================================================================================================== V3
static void released(Model &m, const vsmmu::Plan &p)
{
  auto want = m.groupsFound;
  for (unsigned g : p.group) {
    CHECK(!(m.R(kSmmu + vsmmu::smrOffset(g)) & 0x80000000u));
    const uint32_t found = want[vsmmu::s2crOffset(g)];
    if (m.R(kSmmu + vsmmu::s2crOffset(g)) != found)
      want[vsmmu::s2crOffset(g)] = vsmmu::s2crRestorable(found, p.banks) ? found : m.f.faultIgnored ? p.bank : vsmmu::kS2crFault;
  }
  if (!vsmmu::cbarRestorable(p.cbarFound, p.idr0) || p.cba2rFound != 1) {
    want[vsmmu::cbarOffset(p.bank)] = vsmmu::kCbarStage1;
    want[vsmmu::cba2rOffset(p.bank)] = 1;
  }
  CHECK(m.snapshotSmmu() == want);
}

static void testV3()
{
  Run x(4);
  x.o.resumes = 10;
  x.o.unload = true;
  const int rc = x.go();
  if (rc) std::printf("  V3: %s at %d\n", errorName(rc), x.b->rep.failedStep);
  CHECK(rc == kOk);
  quiet(*x.m, "V3");
  Model &m = *x.m;
  // The plan: the two highest free groups, the first bank below the top that nobody uses and that does not translate.
  CHECK(x.b->plan.group[0] == 99 && x.b->plan.group[1] == 98 && x.b->plan.bank == 80 && x.b->plan.pasize == 2);
  // The handshake: INIT with the seven UBWC properties of SM8750, IMAGE_VERSION, the version string (NULs as spaces).
  CHECK(m.fw.inits == 1 && m.fw.versions == 11 && m.fw.ifpcs == 12 && m.fw.pcPreps == 11);
  const uint32_t ubwc[7] = {8, 32, 16, 0, 1, 1, 1};
  CHECK(std::memcmp(m.fw.ubwc, ubwc, sizeof ubwc) == 0);
  CHECK(x.b->rep.version && std::strcmp(x.b->rep.versionText, "video-firmware.vpu35.model     x") == 0);   // 5 NULs, trailing ones cut
  CHECK(m.logged("hfi: firmware version: video-firmware.vpu35.model     x"));
  for (size_t i = 0; i < m.fw.headerIds.size(); ++i) CHECK(m.fw.headerIds[i] == i);
  for (size_t i = 0; i < m.fw.packetIds.size(); ++i) CHECK(m.fw.packetIds[i] == i);
  CHECK(m.fw.boots == 12 && x.b->rep.resumesOk == 10 && x.b->rep.collapses == 11);
  CHECK(m.count("scm remote0") == 11 && m.count("scm remote1") == 11 && m.count("scm init") == 1 && m.count("scm shutdown") == 1);
  CHECK(x.b->rep.fsr == 0 && x.b->rep.debugLines >= 1 && m.logged("fw: VIDEO: system init"));
  // Unloaded: PAS shut down, everything off, the SMMU exactly as found, the memory freed.
  CHECK(m.allOff() && !m.fw.authed && m.dmaFrees == 1 && m.dmaAllocs == 1);
  released(m, x.b->plan);
  CHECK(m.count("smr valid 6464") == 1 && m.count("smr valid 6471") == 1);
  CHECK(std::strstr(x.b->rep.summary, "V3 ok: firmware video-firmware.vpu35.model     x;"));
  CHECK(std::strstr(x.b->rep.summary, "collapse/resume 10/10"));
  CHECK(m.logged("V3 memory: walk 1024/1024 ok"));
  CHECK(m.logged("V3 first-HFI PASS: INIT success version received cmd-consumed 113 doorbells 3"));
  CHECK(m.logged("V3 TX 1 packet 0 size 36 type 01000001 flags 00000007"));
  CHECK(m.logged("V3 RX 1 packet 0 size 32 type 01000001 flags 00000001"));
  CHECK(m.logged("V3 evidence complete: first-HFI PASS resumes 10/10 collapses 11 teardown 0 watchdog 0 fsr 00000000"));
  CHECK(x.b->txEvents == x.b->doorbells && x.b->teardownRc == kOk);
  CHECK(m.first("gdsc mvs0 on") < m.first("smmu bank-on"));
  CHECK(m.last("scm shutdown") < m.first("smmu route-off"));
  CHECK(m.last("smmu global-sync") < m.last("mm1 off") && m.last("mm1 off") < m.first("dma free"));
  CHECK(m.logged("verify SCTLR want 00001027 got 00000027 mask 00000027 mismatch 00000000 PASS"));
  CHECK(m.logged("TBI 60 tolerated 60") && m.logged("FSR raw 00000400 fault-bits 00000000 format 2 valid 0"));
}

static void testV3EvidenceFailures()
{
  // Do not turn INIT-only into "CPU never started", or discard the partial evidence on a version timeout.
  {
    Run x(4);
    x.m->f.fwNoVersion = true;
    CHECK(x.go() == kErrHfiTimeout);
    CHECK(x.m->logged("V3 INIT success elapsed") && x.m->logged("init 1 init-failed 0 version 0"));
    CHECK(x.m->logged("V3 version error -20 stage 48 received 0"));
    CHECK(x.m->logged("V3 teardown result 0 PAS 0 power 0 smmu 1 dma-held 0"));
    CHECK(!x.b->firstHfi);
  }
  // A failed final shutdown or TLB sync must not produce an unloaded/ok verdict.
  for (unsigned kind = 0; kind < 3; ++kind) {
    Run x(4);
    uint32_t dbBytes = 0;
    const uint8_t *db = x.m->cmdDb(&dbBytes);
    x.b->res = rpmh::resources(db, dbBytes);
    CHECK(x.b->smmuSetup() == kOk && x.b->on() == kOk && x.b->smmuApply() == kOk && x.b->firmware.load() == kOk);
    if (kind == 0) x.m->f.scmFail = "shutdown";
    if (kind == 1) x.m->f.tlbStuck = true;
    if (kind == 2) x.m->R(kIris + kWrapperIntrStatus) = 8;
    CHECK(x.b->teardown() == (kind == 0 ? kErrScm : kind == 1 ? kErrSmmu : kErrWatchdog));
    CHECK(x.m->dmaFrees == (kind == 2 ? 1u : 0u));
    CHECK(x.b->dmaHeld == (kind != 2));
    CHECK(x.m->allOff() == (kind == 2));
    quiet(*x.m, "V3 teardown evidence");
  }
  // An intervening group or bank owner is refused before apply's first MMIO write.
  for (unsigned kind = 0; kind < 4; ++kind) {
    Run x(4);
    const vsmmu::Plan p = vsmmu::plan([&](uint32_t off) { return x.m->rd(kSmmu + off); });
    CHECK(!p.error);
    if (kind == 0) x.m->R(kSmmu + vsmmu::smrOffset(p.group[0])) = 0x80000abc;
    if (kind == 1) x.m->R(kSmmu + p.cb() + vsmmu::kSctlr) = 1;
    if (kind >= 2) {
      x.m->R(kSmmu + vsmmu::smrOffset(50)) = kind == 2 ? 0x80000abc : 0x80001940;
      x.m->R(kSmmu + vsmmu::s2crOffset(50)) = p.bank;
    }
    const auto before = x.m->snapshotSmmu();
    vsmmu::Routed done;
    CHECK(vsmmu::apply(p, 0x880000000ull, x.b->ops, done) != nullptr);
    CHECK(!done.bank && !done.group[0] && !done.group[1] && x.m->writes == 0);
    CHECK(x.m->snapshotSmmu() == before);
  }
}

static void testV3PlatformLifecycle()
{
  // An ignored FAULT write is acceptable only with both SMRs invalid, M clear, and completed TLB maintenance.
  { Run x(4); x.o.unload = true; x.m->f.faultIgnored = true;
    CHECK(x.go() == kOk); released(*x.m, x.b->plan); quiet(*x.m, "FAULT emulation"); }
  // Real S1 mismatches still reject before PAS, but finish a legal partial release.
  for (unsigned i = 0; i < 2; ++i) {
    Run x(4); x.m->f.s1BadTcr = i == 0; x.m->f.s1BadSctlr = i == 1;
    CHECK(x.go() == kErrSmmu && x.b->rep.failedStep == kStepSmmuRoute);
    CHECK(x.m->count("scm init") == 0 && x.m->dmaFrees == 1 && x.m->allOff());
    released(*x.m, x.b->plan); quiet(*x.m, "required S1 fields");
  }
  // Independently used ASID in TTBR0 OR TTBR1 refuses the transaction without touching that owner.
  for (unsigned off : {vsmmu::kTtbr0Hi, vsmmu::kTtbr1Hi}) {
    Run x(4); x.m->R(kSmmu + (128 + 2) * 4096 + off) = 80u << 16;
    CHECK(x.go() == kErrSmmu && !x.b->routed.bank && x.m->count("scm init") == 0);
    CHECK(x.m->R(kSmmu + (128 + 2) * 4096 + off) == 80u << 16 && x.m->dmaFrees == 1);
    quiet(*x.m, "ASID collision");
  }
  // Failure to invalidate a match must retain tables, UC memory, bank ownership and power for safety.
  { Run x(4); x.o.unload = true; x.m->f.releaseSmrStuck = true;
    CHECK(x.go() == kErrSmmu && x.b->teardownRc == kErrSmmu);
    CHECK(x.b->dmaHeld && x.b->routed.bank && x.m->dmaFrees == 0 && !x.m->allOff());
    quiet(*x.m, "detach failure"); }
  // Failure while already collapsed still powers/resumes before PAS shutdown, then releases while buses remain on.
  { Run x(4); CHECK(x.go() == kOk && !x.b->pst.on);
    CHECK(x.b->teardown() == kOk && !x.b->dmaHeld && x.m->allOff());
    quiet(*x.m, "collapsed release"); }
}

static void testV3SteadyState()
{
  Run x(4);
  const uint64_t t0 = x.m->now;
  CHECK(x.go() == kOk);
  quiet(*x.m, "V3 steady state");
  Model &m = *x.m;
  // Collapsed with the firmware kept: GDSCs off, the votes dropped, the SMMU routes and the memory kept, PAS 9 still holding it.
  CHECK(m.allOff() && m.fw.authed && m.fw.suspended && m.dmaFrees == 0);
  CHECK((m.R(kSmmu + vsmmu::smrOffset(99)) & 0x80000000u) && (m.R(kSmmu + (128 + 80) * 4096) & 1));
  // The collapse came after the idle delay (1.5 s after the handshake), not before.
  CHECK(m.now - t0 >= 1500 * 1000);
  CHECK(m.logged("V3: steady state"));
}

static void testV3Failures()
{
  struct Case {
    const char *name;
    void (*set)(Faults &);
    int error, step;
  } cases[] = {
      {"existing entry", [](Faults &f) { f.smmuExisting = true; }, kErrSmmu, kStepSmmuPlan},
      {"no bank", [](Faults &f) { f.smmuNoBank = true; }, kErrSmmu, kStepSmmuPlan},
      {"no memory", [](Faults &f) { f.dmaFail = true; }, kErrNoMemory, kStepSmmuTables},
      {"TLB sync stuck", [](Faults &f) { f.tlbStuck = true; }, kErrSmmu, kStepSmmuRoute},
      {"no boot", [](Faults &f) { f.fwNoBoot = true; }, kErrBoot, kStepCtrlStatus},
      {"UC region refused", [](Faults &f) { f.fwBadUc = true; }, kErrBoot, kStepCtrlStatus},
      {"no reply", [](Faults &f) { f.fwNoReply = true; }, kErrHfiTimeout, kStepHfiWait},
      {"INIT failed", [](Faults &f) { f.fwInitFail = true; }, kErrHfi, kStepHfiWait},
      {"system error", [](Faults &f) { f.fwSysError = true; }, kErrSysError, kStepHfiWait},
      {"no version", [](Faults &f) { f.fwNoVersion = true; }, kErrHfiTimeout, kStepHfiWait},
      {"not idle", [](Faults &f) { f.fwNotIdle = true; }, kErrPc, kStepPcPrep},
      {"remote state", [](Faults &f) { f.scmFail = "remote0"; }, kErrScm, kStepPcPrep},
      {"auth", [](Faults &f) { f.scmFail = "auth"; }, kErrScm, kStepPasAuth},
  };
  for (const Case &c : cases) {
    Run x(4);
    c.set(x.m->f);
    if (x.m->f.smmuExisting) {
      x.m->R(kSmmu + vsmmu::smrOffset(5)) = 0x80001940;
      x.m->groupsFound = x.m->snapshotSmmu();
    }
    if (x.m->f.smmuNoBank) {
      for (unsigned b = 0; b < 83; ++b) x.m->R(kSmmu + (128 + b) * 4096) = 1;
      x.m->groupsFound = x.m->snapshotSmmu();
    }
    const unsigned writesBefore = x.m->writes;
    const int rc = x.go();
    if (rc != c.error || x.b->rep.failedStep != c.step)
      std::printf("  case %s: rc %d (%s) step %d\n", c.name, rc, errorName(rc), x.b->rep.failedStep);
    CHECK(rc == c.error && x.b->rep.failedStep == c.step);
    Model &m = *x.m;
    CHECK(m.allOff() == !m.f.tlbStuck && !m.fw.authed);
    CHECK(m.dmaFrees == (m.f.tlbStuck ? 0u : m.dmaAllocs));
    CHECK(x.b->dmaHeld == m.f.tlbStuck);
    if (!m.f.tlbStuck && x.b->plan.bank != ~0u && m.dmaAllocs) released(m, x.b->plan);
    else if (!m.f.tlbStuck) CHECK(m.snapshotSmmu() == m.groupsFound);
    if (c.step == kStepSmmuPlan) CHECK(m.writes == writesBefore);   // refused before any write
    if (c.step >= kStepCtrlStatus) CHECK(m.count("scm shutdown") == 1);
    if (c.step < kStepCtrlStatus) CHECK(m.count("scm init") == 0 || c.step == kStepPasAuth);
    if (!m.f.tlbStuck) quiet(m, c.name);
    CHECK(std::strstr(x.b->rep.summary, "V3 FAILED"));
  }
}

static void testV4Lifetime()
{
  // The session peer is tested separately; here an unresponsive peer reaches real V3 teardown with the expanded scatter arena.
  for (unsigned kind=0; kind<3; ++kind) {
    Run x(5);
    uint32_t bytes=0;
    const uint8_t *db=x.m->cmdDb(&bytes);
    x.b->opt=x.o; x.b->res=rpmh::resources(db,bytes);
    CHECK(x.b->smmuSetup()==0 && x.b->on()==0 && x.b->smmuApply()==0 && x.b->firmware.load()==0 && x.b->bootCore()==0);
    CHECK(x.b->tables.pageCount==132 && x.b->dma.arena);
    for (unsigned i=0;i<session::kArenaPages;++i) if (x.b->layout.mapped(i)) {
      uint64_t pa=0;
      CHECK(x.m->translate(kStreamFirst,session::kArenaBase+4096*i,&pa) && pa==x.b->dma.arenaPages[i]);
      CHECK(x.m->translate(kStreamSecond,session::kArenaBase+4096*i,&pa) && pa==x.b->dma.arenaPages[i]);
    }
    session::SelfTest<Bringup<Model>> test(*x.b,x.b->layout);
    CHECK(test.run()==kErrHfiTimeout && test.r.stage==session::Finished && test.r.close!=0 && !test.r.closed);
    CHECK(x.b->dmaHeld && x.m->dmaFrees==0 && x.m->fw.authed);
    if (kind==1) x.m->f.scmFail="shutdown";
    if (kind==2) x.m->f.tlbStuck=true;
    const int rc=x.b->teardown();
    CHECK(rc==(kind==1?kErrScm:kind==2?kErrSmmu:0));
    CHECK(x.b->dmaHeld==(kind!=0) && x.m->dmaFrees==(kind==0?1u:0u));
    if (!kind) CHECK(x.m->allOff() && !x.m->fw.authed && x.m->last("scm shutdown")<x.m->first("smmu route-off"));
    quiet(*x.m,"V4 unresponsive session teardown");
  }
}

static void testV5DemandLifetime()
{
  for (unsigned kind = 0; kind < 4; ++kind) {
    Run x(6);
    CHECK(x.go() == kOk && x.b->firstHfi && x.b->rep.version);
    CHECK(x.b->dmaHeld && x.b->dma.session && x.b->pst.on && x.m->fw.authed);
    CHECK(x.b->tables.pageCount == vsmmu::kSessionTablePages);
    CHECK(x.m->fw.session == 0 && x.b->v4.etb == 0 && x.b->rep.collapses == 0); // no autonomous session
    if (kind == 1) x.m->f.scmFail = "shutdown";
    if (kind == 2) x.m->f.tlbStuck = true;
    if (kind == 3) x.m->f.releaseSmrStuck = true;
    const int rc = x.b->teardown();
    CHECK(rc == (kind == 1 ? kErrScm : kind ? kErrSmmu : kOk));
    CHECK(x.b->dmaHeld == (kind != 0) && x.m->dmaFrees == (kind ? 0u : 1u));
    if (!kind) CHECK(x.m->allOff() && !x.m->fw.authed);
    quiet(*x.m, "V5 demand owner teardown");
  }
}

static void testNativeQuarantine()
{
  // Real Bringup against the existing register/PAS/DMA model: quarantine must
  // precede every cleanup MMIO, PAS shutdown, domain detach and physical off.
  for (unsigned kind=0;kind<4;++kind) {
    Run x(6);
    CHECK(x.go()==kOk && x.b->dmaHeld && x.b->pst.on && x.m->fw.authed);
    if(kind==0)x.b->quarantineReason=kErrHfiTimeout;
    if(kind==1)x.b->watchdogSeen=8;
    if(kind==2)x.b->rep.fsr=2;
    if(kind==3)x.m->R(kIris+kWrapperIntrStatus)=8;
    const unsigned reads=x.b->ops.reads,writes=x.b->log.writes,frees=x.m->dmaFrees;
    const auto calls=x.m->calls;
    const int want=kind==0?kErrHfiTimeout:(kind==1||kind==3)?kErrWatchdog:kErrFault;
    CHECK(x.b->teardown()==want && x.b->quarantineReason==want);
    CHECK(x.b->ops.reads==reads+(kind==3?1u:0u) && x.b->log.writes==writes && x.m->calls==calls);
    CHECK(x.b->dmaHeld && x.b->pst.on && x.m->fw.authed && x.m->dmaFrees==frees);
    CHECK(x.b->teardown()==want && x.b->ops.reads==reads+(kind==3?1u:0u) && x.b->log.writes==writes);
    CHECK(x.m->logged("no-cleanup-MMIO 1 reboot-required 1"));
    quiet(*x.m,"native failure quarantine");
  }
}

static void testV3SepFault()
{
  // With SEP 0 in TCR2 (what a bank left as found could hold), the model's hypervisor family faults every IOVA with bit 31 set: the
  // firmware would never see the queue table. Ours has SEP 7 (testV3 runs through it); here a bank with SEP 0 faults, FSR says so.
  CHECK(vsmmu::tcr2For(2) == (2u | 7u << 15));
  Model m;
  uint64_t pa = 0;
  m.R(kSmmu + vsmmu::smrOffset(10)) = 0x80001940;
  m.R(kSmmu + vsmmu::s2crOffset(10)) = 80;
  const uint32_t cb = (128 + 80) * 4096;
  m.R(kSmmu + cb) = 1;
  m.R(kSmmu + cb + vsmmu::kTcr) = 32;
  m.R(kSmmu + cb + vsmmu::kTcr2) = 2;   // SEP 0
  CHECK(!m.translate(kStreamFirst, kIovaBase, &pa) && (m.R(kSmmu + cb + vsmmu::kFsr) & 2));
}

// ============================================================================================================ the pieces alone
static void testQueues()
{
  std::vector<uint32_t> buf(kUcBytes / 4, 0);
  hfi::Mem m;
  m.uc = buf.data();
  hfi::initQueues(m);
  CHECK(m.ld(hfi::kTblSize) == 456 && m.ld(hfi::kTblQhdr0Offset) == 288 && m.ld(hfi::kTblQhdrSize) == 56 && m.ld(hfi::kTblNumQ) == 3);
  CHECK(m.ld(hfi::kTblName) == ('i' | 'r' << 8 | 'i' << 16 | 's' << 24));
  CHECK(m.ld(hfi::qhdr(0) + hfi::kQStart) == kIovaBase + 456 && m.ld(hfi::qhdr(2) + hfi::kQStart) == kIovaBase + 456 + 2 * kHfiQueueBytes);
  CHECK(m.ld(hfi::qhdr(1) + hfi::kQRxReq) == 1 && m.ld(hfi::qhdr(2) + hfi::kQRxReq) == 0 && m.ld(kSfrOffset) == 4096);
  auto bar = [] {};
  // A packet across the end of the queue: written in two parts, read back whole.
  const uint32_t end = kHfiQueueBytes / 4;
  m.st(hfi::qhdr(1) + hfi::kQWrite, end - 3);
  m.st(hfi::qhdr(1) + hfi::kQRead, end - 3);
  uint32_t pkt[16];
  for (unsigned i = 0; i < 16; ++i) pkt[i] = 0x1000 + i;
  pkt[0] = 64;
  CHECK(hfi::write(m, 1, pkt, 64, bar) == 0 && m.ld(hfi::qhdr(1) + hfi::kQWrite) == 13);
  uint32_t out[1024], bytes = 0;
  CHECK(hfi::read(m, 1, out, sizeof out, &bytes, bar) == 0 && bytes == 64 && std::memcmp(out, pkt, 64) == 0);
  CHECK(m.ld(hfi::qhdr(1) + hfi::kQRead) == 13 && hfi::read(m, 1, out, sizeof out, &bytes, bar) == 1);
  // A full queue refuses; an oversized packet is dropped; a zero size is an error; indices outside the queue are refused.
  m.st(hfi::qhdr(0) + hfi::kQWrite, 0);
  m.st(hfi::qhdr(0) + hfi::kQRead, 16);
  CHECK(hfi::write(m, 0, pkt, 64, bar) == -1);
  m.st(hfi::qbase(2), 8192);
  m.st(hfi::qhdr(2) + hfi::kQWrite, 100);
  CHECK(hfi::read(m, 2, out, sizeof out, &bytes, bar) == -2 && m.ld(hfi::qhdr(2) + hfi::kQRead) == 100);
  m.st(hfi::qbase(2) + 400, 0);
  m.st(hfi::qhdr(2) + hfi::kQWrite, 120);
  CHECK(hfi::read(m, 2, out, sizeof out, &bytes, bar) == -1);
  m.st(hfi::qhdr(2) + hfi::kQWrite, end);
  CHECK(hfi::read(m, 2, out, sizeof out, &bytes, bar) == -3);
}

static void testPackets()
{
  uint32_t b[1024];
  hfi::Ids ids;
  CHECK(hfi::sysInit(b, ids) == 320 && b[7] == 8 && b[2] == 0 && ids.header == 1 && ids.packet == 8);
  CHECK(b[8] == 36 && b[9] == kHfiCmdInit && b[10] == 7 && b[11] == kHfiPayloadU32 && b[16] == kHfiArchLx);
  CHECK(b[17] == 36 && b[18] == kHfiPropUbwcMaxChannels && b[25] == 8 && b[18 + 9 * 6] == kHfiPropUbwcBankSpreading);
  CHECK(hfi::imageVersion(b, ids) == 64 && b[9] == kHfiPropImageVersion && b[10] == (2 | 1 | 8) && b[13] == 8 && b[2] == 1);
  CHECK(hfi::interframePowerCollapse(b, ids) == 68 && b[9] == kHfiPropIntraFramePc && b[16] == 1);
  CHECK(hfi::pcPrep(b, ids) == 64 && b[9] == kHfiCmdPowerCollapse && b[10] == 0 && ids.header == 4 && ids.packet == 11);
  // Answers.
  hfi::Reply r;
  uint32_t a[48] = {64, 0, 5, 0, 0, 0, 0, 1, 32, kHfiCmdInit, kHfiFwSuccess, 0, 0, 0, 0, 0};
  CHECK(hfi::parse(a, 64, r) && r.initDone && !r.initFailed);
  a[10] = 0;
  hfi::Reply f;
  CHECK(hfi::parse(a, 64, f) && f.initFailed);
  hfi::Reply e;
  a[9] = 0x05000001;
  a[10] = kHfiFwSystemError;
  CHECK(hfi::parse(a, 64, e) && e.sysError && e.errorType == 0x05000001);
  hfi::Reply e2;
  a[9] = kHfiCmdInit;
  a[10] = kHfiFwSuccess | kHfiFwSystemError;
  CHECK(hfi::parse(a, 64, e2) && e2.sysError);
  hfi::Reply bad;
  a[8] = 200;   // a packet past the message's end
  CHECK(!hfi::parse(a, 64, bad) && bad.malformed == 1);
  a[8] = 32;
  a[0] = 60;
  CHECK(!hfi::parse(a, 64, bad));
  a[0] = 64;
  a[1] = 3;     // a session's message: not ours to answer yet, counted
  hfi::Reply s;
  CHECK(hfi::parse(a, 64, s) && s.sessionHeaders == 1 && !s.initDone);
  // The version string: 128 bytes, NULs as spaces, short ones refused.
  uint32_t v[8 + 8 + 32] = {64 + 128, 0, 1, 0, 0, 0, 0, 1, 32 + 128, kHfiPropImageVersion, 1, 0, 0, 0, 0, 0};
  std::memcpy(v + 16, "abc\0def", 8);
  hfi::Reply vr;
  CHECK(hfi::parse(v, 192, vr) && vr.version && std::strncmp(vr.text, "abc def", 7) == 0 && std::strlen(vr.text) == 127);
  v[0] = 64 + 8;
  v[8] = 40;
  hfi::Reply vs;
  CHECK(hfi::parse(v, 72, vs) && !vs.version && vs.malformed == 1);
  // A debug line.
  uint32_t d[8] = {24, 1, 0, 0, 0, 0, 0, 0};
  std::memcpy(reinterpret_cast<uint8_t *>(d) + 17, "hi\n!", 4);
  char line[16];
  CHECK(hfi::debugText(d, 32, line, sizeof line) && std::strcmp(line, "hi!") == 0);
}

static void testRpmhPieces()
{
  const std::vector<uint8_t> db = pianoCmdDb();
  rpmh::Arc a;
  CHECK(rpmh::arc(db.data(), uint32_t(db.size()), "mxc.lvl", &a) == rpmh::kFound && a.count == 9 && a.addr == kAddrMxc);
  uint32_t h = 0;
  CHECK(rpmh::level(a, 0x80, &h) && h == 4 && rpmh::level(a, 0x81, &h) && h == 5 && !rpmh::level(a, 0x200, &h));
  CHECK(rpmh::arc(db.data(), uint32_t(db.size()), "gfx.lvl", &a) == rpmh::kNotFound);
  uint32_t addr = 0;
  CHECK(rpmh::bcm(db.data(), uint32_t(db.size()), "MM1", &addr) == rpmh::kFound && addr == kAddrMm1);
  CHECK(rpmh::bcm(db.data(), uint32_t(db.size()), "mxc.lvl", &addr) == rpmh::kCorrupt);    // an ARC asked as a BCM (A830's rule)
  CHECK(rpmh::bcm(db.data(), uint32_t(db.size()), "MM9", &addr) == rpmh::kNotFound);
  std::vector<uint8_t> dup = makeCmdDb({{"mxc.lvl", kAddrMxc, levelsAux(kMxcLevels, 9, 7)}, {"mxc.lvl", kAddrMmcx, levelsAux(kMxcLevels, 9, 7)}}, {});
  CHECK(rpmh::arc(dup.data(), uint32_t(dup.size()), "mxc.lvl", &a) == rpmh::kCorrupt);
  std::vector<uint8_t> dummy = makeCmdDb({{"mxc.lvl", kAddrMxc, levelsAux(kMxcLevels, 1, 15)}}, {});
  CHECK(rpmh::arc(dummy.data(), uint32_t(dummy.size()), "mxc.lvl", &a) == rpmh::kCorrupt);
  std::vector<uint8_t> nodb(kCmdDbBytes, 0);
  CHECK(rpmh::arc(nodb.data(), uint32_t(nodb.size()), "mxc.lvl", &a) == rpmh::kNotCmdDb);
  CHECK(rpmh::commandOk({kAddrMxc, 4}) && !rpmh::commandOk({kAddrMxc, 0x100}) && rpmh::commandOk({kAddrMm1, kBcmMm1On}) &&
        !rpmh::commandOk({kAddrMm1, 0x80000000u}) && !rpmh::commandOk({0x40000, 1}) && !rpmh::commandOk({kAddrMxc + 1, 1}));
  CHECK(rpmh::layoutFor(0x00020000).msgid == 0x30 && rpmh::layoutFor(0x00030000).msgid == 0x34);
  // A busy TCS is skipped; a stuck one never used again; three stuck: no vote at all.
  Model m;
  WriteLog log;
  Ops<Model> ops(m, log);
  bool stuck[3] = {true, false, false};
  m.tcsBusy[1] = true;
  rpmh::TcsResult r;
  const rpmh::Cmd c = {kAddrMxc, 4};
  CHECK(rpmh::send(ops, &c, 1, 1, stuck, &r) == kOk && r.tcs == 2 && r.busy == 3 && m.mxc == 4 && !m.drv2Held);
  m.f.rpmhNoComplete = true;
  CHECK(rpmh::send(ops, &c, 1, 1, stuck, &r) == kErrRpmh && stuck[2] && r.step == rpmh::kTcsPoll && !m.drv2Held);
  CHECK(rpmh::send(ops, &c, 1, 1, stuck, &r) == kErrRpmh && r.step == rpmh::kTcsSelect);
  CHECK(rpmh::send(ops, &c, 1, 2, stuck, &r) == kErrArgument);
  quiet(m, "rpmh pieces");
}

static void testSmmuPieces()
{
  Model m;
  auto rd = [&m](uint32_t off) { return m.R(kSmmu + off); };
  vsmmu::Plan p = vsmmu::plan(rd);
  CHECK(!p.error && p.group[0] == 99 && p.group[1] == 98 && p.bank == 80 && p.freeGroups == 97 && p.pasize == 2);
  CHECK(p.s2crFound[0] == 0x0300006e && p.cbarFound == 0);
  m.R(kSmmu + vsmmu::kScr0) = vsmmu::kScr0ExidEnable;
  CHECK(vsmmu::plan(rd).error);
  m.R(kSmmu + vsmmu::kScr0) = 0;
  m.R(kSmmu + vsmmu::kIdr2) = 1u << 12;   // 32-bit output
  CHECK(vsmmu::plan(rd).error);
  m.R(kSmmu + vsmmu::kIdr2) = 1u << 12 | 2u << 4;
  m.R(kSmmu + vsmmu::kIdr0) = 100;        // no stage 1
  CHECK(vsmmu::plan(rd).error);
  m.R(kSmmu + vsmmu::kIdr0) = 1u << 30 | 3;
  m.R(kSmmu + vsmmu::smrOffset(2)) = 0x80000001;
  CHECK(vsmmu::plan(rd).error && std::strstr(vsmmu::plan(rd).error, "fewer than two"));
  m.R(kSmmu + vsmmu::kIdr0) = 1u << 30 | 100;
  m.R(kSmmu + vsmmu::smrOffset(7)) = 0x800f1940;   // a masked group covering 0x1940..0x194f
  CHECK(vsmmu::plan(rd).error && std::strstr(vsmmu::plan(rd).error, "already has"));
  // The table: mapped pages, scratch for the rest, faults outside; a page outside XNU's RAM is refused.
  std::vector<uint64_t> mem(vsmmu::kTablePages * 512);
  vsmmu::Tables t;
  t.cpu = mem.data();
  t.pa = 0x8e1000000ull;
  std::vector<uint64_t> pages(vsmmu::kUcPages);
  for (unsigned i = 0; i < vsmmu::kUcPages; ++i) pages[i] = 0x900000000ull + 0x2000ull * i;
  CHECK(vsmmu::build(t, pages.data(), vsmmu::kUcPages, 0x8e0000000ull));
  CHECK(vsmmu::walk(t, kIovaBase + 0x123) == pages[0] + 0x123 && vsmmu::walk(t, kIovaBase + kUcBytes - 4) == pages.back() + 0xffc);
  CHECK(vsmmu::walk(t, kIovaBase + kUcBytes) == 0x8e0000000ull && vsmmu::walk(t, kIovaBase + kUcRegionBytes) == 0);
  CHECK(vsmmu::walk(t, kIovaBase - 4) == 0 && vsmmu::walk(t, 0x10000000) == 0);
  pages[3] = 0x80000000ull;
  CHECK(!vsmmu::build(t, pages.data(), vsmmu::kUcPages, 0x8e0000000ull));
}

static void testPoweredVerdict()
{
  Clocks c;
  c.controller = c.codec = true;
  c.pllMode = kPllLockDet;
  c.pllL = kPllConfigL;
  c.pllAlpha = kPllConfigAlpha;
  c.mvs0RcgCfg = kMvs0Cfg720;
  c.mvs0Div = kMvs0DivFor240;
  Core r;
  r.controllerRead = r.codecRead = true;
  r.powerStatus = kCorePowered;
  r.hwVersion = 0;   // only inferred for VPU 3.5: recorded, not refused
  CHECK(poweredVerdict(c, r) == nullptr);
  r.hwVersion = 0xffffffffu;
  CHECK(poweredVerdict(c, r) && std::strstr(poweredVerdict(c, r), "all ones"));
  r.hwVersion = 0x30050000;
  r.ctrlStatus = 1;
  CHECK(poweredVerdict(c, r) && std::strstr(poweredVerdict(c, r), "CTRL_STATUS"));
  r.ctrlStatus = 0;
  r.powerStatus = 0;
  CHECK(poweredVerdict(c, r) && std::strstr(poweredVerdict(c, r), "CORE_POWER_STATUS"));
  r.powerStatus = kCorePowered;
  c.mvs0Div = 0;   // 720 MHz: not the 240 MHz OPP
  CHECK(poweredVerdict(c, r) && std::strstr(poweredVerdict(c, r), "240 MHz"));
  c.mvs0Div = kMvs0DivFor240;
  c.codec = false;
  CHECK(poweredVerdict(c, r) && std::strstr(poweredVerdict(c, r), "codec's gate"));
}

static void testIdleClock()
{
  IdleClock c;
  c.delayMs = 1500;
  c.touch(1000);
  CHECK(c.leftUs(1000) == 1500000 && c.leftUs(1000 + 1499999) == 1 && c.leftUs(1000 + 1500000) == 0 && c.leftUs(9000000) == 0);
  c.delayMs = 0;
  CHECK(c.leftUs(1000) == 0);
}

// The owner's own file when it is on this Mac: the real pin, the real headers.
static void testRealFirmware()
{
  const char *path = std::getenv("VIDEO_HOST_FW");
  if (!path || !*path) {
    std::printf("PianoVideo bring-up host tests: the owner's vpu35_4v.mbn not given (VIDEO_HOST_FW): its check skipped\n");
    return;
  }
  FILE *f = std::fopen(path, "rb");
  if (!f) {
    std::printf("PianoVideo bring-up host tests: %s not readable: its check skipped\n", path);
    return;
  }
  std::vector<uint8_t> bytes;
  uint8_t chunk[65536];
  for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
  std::fclose(f);
  Model m;
  m.image = bytes;
  WriteLog log;
  Ops<Model> ops(m, log);
  FwState st;
  Firmware<Ops<Model>> fw(ops, st);   // the real pin
  CHECK(fw.check(bytes.data(), bytes.size()) == kOk && st.pinOk == 1);
  CHECK(st.mdt.relocatable && st.span <= kCarveoutBytes && st.loadable > 0 && st.metaBytes > 0 && st.metaBytes < 64 * 1024);
  uint64_t stored = 0;
  for (unsigned i = 0; i < st.mdt.count; ++i)
    if (st.mdt.segment[i].load) stored += st.mdt.segment[i].file_size;
  std::printf("PianoVideo bring-up host tests: the owner's vpu35_4v.mbn: pin ok; %s; %llu bytes to store\n",
              m.lines.empty() ? "" : m.lines.back().c_str() + 10, (unsigned long long)stored);
}

int main()
{
  testInert();
  testV1Cycles();
  testV1AhbOff();
  testV1Faults();
  testV1FoundRequestedOn();
  testV1PllFoundRunning();
  testV1Stops();
  testV1OffWarnings();
  testMmcxFloor();
  testSharedVotes();
  testNoCmdDb();
  testV2();
  testV2CpuLpiTimeoutCleanup();
  testV2Refusals();
  testV2ScmFailures();
  testV3();
  testV3EvidenceFailures();
  testV3PlatformLifecycle();
  testV3SteadyState();
  testV3Failures();
  testV3SepFault();
  testV4Lifetime();
  testV5DemandLifetime();
  testNativeQuarantine();
  testQueues();
  testPackets();
  testRpmhPieces();
  testSmmuPieces();
  testPoweredVerdict();
  testIdleClock();
  testRealFirmware();
  if (failures) {
    std::printf("PianoVideo bring-up host tests: %d failed\n", failures);
    return 1;
  }
  std::printf("PianoVideo bring-up host tests: all passed\n");
  return 0;
}
