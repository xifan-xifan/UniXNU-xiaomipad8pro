// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoVideo V3 (piano-video=4), the memory part: the video streams 0x1940 and 0x1947 through a stage-1 context bank of their own
// on the apps SMMU, with our table mapping the UC region (queue table, three queues, SFR) at IOVA 0x80000000. Portable (host tests
// in tests/bringup_host.cpp: the plan against SMMU models, the table walked as the SMMU would, the routes, the teardown, and a
// firmware model that reaches the queues only through this table).
//
// After two helpers of this repository (BSD-2-Clause-Patent; UPSTREAM has their sha256): package/PianoUsbHost/SmmuIdentity.h
// (the plan/apply split, the geometry checks, the bank search from one below the top skipping every bank a valid group translates
// through and every bank with translation on, the context bank's words in Linux arm_smmu_write_context_bank's order for stage 1,
// S2CR before SMR, the context TLBIALL + TLBSYNC and the global TLBIALLNSNH + sTLBGSYNC with non-zero values (Qualcomm's trapped
// SMMU MMIO), bounded waits, nothing panics) and package/PianoDpu/DpuSmmu.h (the VMSAv8-64 4 KiB-granule table with a 32-bit
// input (T0SZ 32, the walk from level 1), TCR2 SEP 7 because SEP 0 faults every IOVA with bit 31 set under this hypervisor
// family, non-cacheable walks over tables cleaned to the PoC, the fault registers). Re-expressed for this case:
//   - two streams, each an exact stream-match group (mask 0, as the DT gives them), both translating through one fresh bank; the
//     groups are taken from the highest free ones down (the other kexts take the lowest), and a video stream that already has an
//     entry (level 1 lists it) is refused, never replaced;
//   - the table maps the UC region's 4 KiB pages (any physical pages of XNU's RAM) read-write, Normal non-cacheable (MAIR0
//     attribute 0 = 0x44), outer shareable, AF set, executable (as Linux's iommu_map without IOMMU_NOEXEC); the rest of the 4 MiB
//     the firmware is told about maps one zeroed scratch page; everything else faults (terminated, CFCFG 0);
//   - SCTLR = M | TRE | AFE | CFRE | ASIDPNE (Linux's stage-1 bits without CFIE: no fault interrupt is attached, FSR is polled);
//   - WifiSmmuWindow.h's required SCTLR bits and bank-number ASID; CameraSmmu.h's capability-checked restoration. Release
//     invalidates SMRs before a legal S2CR, disables translation and synchronizes with the video buses still on. A failed release
//     keeps the tables and UC memory. Unsupported reset words are never written back (docs/drivers/camera.md section 4.3).
#pragma once
#include <stdint.h>
#include "VideoHw.h"

namespace pianovideo {
namespace vsmmu {

constexpr uint32_t kScr0 = 0x000, kIdr0 = 0x020, kIdr1 = 0x024, kIdr2 = 0x028, kTlbiAllNsnh = 0x068, kTlbGsync = 0x070,
                   kTlbGstatus = 0x074;
constexpr uint32_t kScr0ExidEnable = 1u << 3, kIdr0S1ts = 1u << 30, kIdr2Ptfs4k = 1u << 12;
constexpr uint32_t smrOffset(unsigned n) { return 0x800 + 4 * n; }
constexpr uint32_t s2crOffset(unsigned n) { return 0xc00 + 4 * n; }
constexpr uint32_t cbarOffset(unsigned n) { return 0x1000 + 4 * n; }
constexpr uint32_t cba2rOffset(unsigned n) { return 0x1800 + 4 * n; }
constexpr uint32_t kSctlr = 0x000, kResume = 0x008, kTcr2 = 0x010, kTtbr0Lo = 0x020, kTtbr0Hi = 0x024, kTtbr1Lo = 0x028,
                   kTtbr1Hi = 0x02c, kTcr = 0x030, kContextIdr = 0x034, kMair0 = 0x038, kMair1 = 0x03c, kFsr = 0x058, kFarLo = 0x060,
                   kFarHi = 0x064, kFsynr0 = 0x068, kTlbiAll = 0x618, kTlbSync = 0x7f0, kTlbStatus = 0x7f4;
constexpr uint32_t kSctlrM = 1u << 0, kSctlrVideo = 1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12;   // M TRE AFE CFRE ASIDPNE
constexpr uint32_t kSctlrRequired = kSctlrVideo & ~(1u << 12);
constexpr uint32_t kTcr2Fields = 0x0003801fu, kTcr2Tbi = 0x60u;
constexpr uint32_t kS2crFault = 2u << 16, kS2crFields = 0x300ffu, kS2crAttrs = 0x03000c00u;
constexpr uint32_t ttbrHi(uint64_t pa, unsigned asid) { return uint32_t(pa >> 32) | (asid & 0xffu) << 16; }
constexpr uint32_t kCbarStage1 = 1u << 16 | 0xfu << 12 | 3u << 8;   // stage 1, stage 2 bypass; MemAttr WB, BPSHCFG NSH (Linux)
constexpr uint32_t kCba2rVa64 = 1;
constexpr uint32_t kTcrVideo = 32u | 1u << 23;                      // T0SZ 32, IRGN0/ORGN0/SH0 0 (non-cacheable walks), TG0 4 KiB, EPD1
constexpr uint32_t tcr2For(unsigned pasize) { return uint32_t(pasize) | 7u << 15; }
constexpr uint32_t kMair0Video = 0x44;                              // attribute 0: Normal, inner and outer non-cacheable
constexpr uint32_t kFsrFaults = 0xc00001feu, kFsrSs = 1u << 30;
constexpr unsigned kPaBits[6] = {32, 36, 40, 42, 44, 48};
constexpr uint16_t kStreams[2] = {kStreamFirst, kStreamSecond};
constexpr unsigned kSyncPolls = 100000;                             // x 1 us

constexpr uint32_t kBankWords[9] = {kTcr2, kTcr, kTtbr0Lo, kTtbr0Hi, kTtbr1Lo, kTtbr1Hi, kContextIdr, kMair0, kMair1};

// ---------------------------------------------------------------------------------------------------------------- the plan
struct Plan {
  uint32_t scr0 = 0, idr0 = 0, idr1 = 0, idr2 = 0;
  unsigned groups = 0, banks = 0, pages = 0, freeGroups = 0, pasize = 0;
  unsigned group[2] = {~0u, ~0u}, bank = ~0u, asid = 0;
  uint32_t smrFound[2] = {}, s2crFound[2] = {}, sctlrFound = 0, cbarFound = 0, cba2rFound = 0;
  uint32_t bankFound[9] = {};
  const char *error = nullptr;
  uint32_t cb() const { return (pages + bank) * 4096; }
};

inline bool covers(uint32_t smr, uint16_t sid)
{
  const uint32_t mask = (smr >> 16) & 0x7fffu;
  return ((smr ^ sid) & 0x7fffu & ~mask) == 0;
}

template <class Read>
Plan plan(Read rd)
{
  Plan p;
  p.scr0 = rd(kScr0);
  p.idr0 = rd(kIdr0);
  p.idr1 = rd(kIdr1);
  p.idr2 = rd(kIdr2);
  if (!(p.idr0 & kIdr0S1ts) || !(p.idr2 & kIdr2Ptfs4k)) {
    p.error = "no stage 1 with 4 KiB AArch64 tables (IDR0/IDR2)";
    return p;
  }
  if (p.scr0 & kScr0ExidEnable) {
    p.error = "extended stream IDs are enabled (sCR0.EXIDENABLE): the stream-match layout differs";
    return p;
  }
  p.groups = p.idr0 & 0xffu;
  if (p.groups > 128) p.groups = 128;   // Qualcomm: groups above 128 do not behave architecturally
  p.banks = p.idr1 & 0xffu;
  p.pages = 1u << (((p.idr1 >> 28) & 7u) + 1);
  if ((p.idr1 >> 31) || p.groups < 2 || p.banks < 2 || p.banks > 128 || p.pages > 128 || uint64_t(p.pages + p.banks) * 4096 > kSmmuBytes) {
    p.error = "unsupported SMMU geometry (64 KiB pages or out of the 1 MiB window)";
    return p;
  }
  const unsigned oas = (p.idr2 >> 4) & 0xfu;
  p.pasize = oas > 5 ? 5 : oas;
  if ((kXnuDramEnd - 1) >> kPaBits[p.pasize]) {
    p.error = "the SMMU's output size does not reach XNU's RAM";
    return p;
  }
  uint32_t used[4] = {};
  for (unsigned i = 0; i < p.groups; ++i) {
    const uint32_t smr = rd(smrOffset(i));
    if (!(smr & 0x80000000u)) {
      ++p.freeGroups;
      continue;
    }
    if (covers(smr, kStreamFirst) || covers(smr, kStreamSecond)) {
      p.error = "a video stream already has a stream-match entry (piano-video=1 lists it): not replaced";
      return p;
    }
    const uint32_t s2cr = rd(s2crOffset(i));
    if (((s2cr >> 16) & 3u) == 0 && (s2cr & 0xffu) < p.banks) used[(s2cr & 0x7fu) / 32] |= 1u << ((s2cr & 0x7fu) % 32);
  }
  // Two free groups, the highest first.
  unsigned taken = 0;
  for (unsigned i = p.groups; i-- > 0 && taken < 2;) {
    if (rd(smrOffset(i)) & 0x80000000u) continue;
    p.group[taken++] = i;
  }
  if (taken < 2) {
    p.error = "fewer than two free stream-match groups";
    return p;
  }
  for (unsigned b = p.banks - 1; b-- > 0;) {   // banks - 2 down to 0
    if ((used[b / 32] >> (b % 32)) & 1u) continue;
    if (rd((p.pages + b) * 4096 + kSctlr) & kSctlrM) continue;   // translation on: someone's
    p.bank = b;
    break;
  }
  if (p.bank == ~0u) {
    p.error = "no free context bank";
    return p;
  }
  for (unsigned k = 0; k < 2; ++k) {
    p.smrFound[k] = rd(smrOffset(p.group[k]));
    p.s2crFound[k] = rd(s2crOffset(p.group[k]));
  }
  p.asid = p.bank;   // Same shared ASID namespace contract as PianoWifi.
  p.sctlrFound = rd(p.cb() + kSctlr);
  p.cbarFound = rd(cbarOffset(p.bank));
  p.cba2rFound = rd(cba2rOffset(p.bank));
  for (unsigned i = 0; i < 9; ++i) p.bankFound[i] = rd(p.cb() + kBankWords[i]);
  return p;
}

// --------------------------------------------------------------------------------------------------------------- the table
// Four 4 KiB pages, physically contiguous: L1, L2, then two L3 tables covering [kIovaBase, kIovaBase + 4 MiB).
constexpr unsigned kTablePages = 4, kWindowPages = kUcRegionBytes / 4096, kUcPages = kUcBytes / 4096;
constexpr unsigned kSessionTablePages = 132;
constexpr unsigned kMaxSessionTablePages = 388;
// The session aperture starts after the two UC L3 tables and must stay in
// their L2. Zero rejects unsupported sizes before any table is allocated.
constexpr unsigned sessionTablePages(uint32_t arenaBytes)
{
  return !arenaBytes || (arenaBytes & ((2 * 1024 * 1024) - 1)) ||
    arenaBytes > (kMaxSessionTablePages - kTablePages) * (2 * 1024 * 1024) ? 0 :
    kTablePages + arenaBytes / (2 * 1024 * 1024);
}
static_assert(kWindowPages == 2 * 512, "two level-3 tables");
constexpr uint64_t kOutMask = 0x0000fffffffff000ull;
constexpr uint64_t kDescTable = 3, kDescPage = 3, kAf = 1ull << 10, kShOuter = 2ull << 8, kApRw = 1ull << 6;
constexpr uint64_t tableDesc(uint64_t pa) { return (pa & kOutMask) | kDescTable; }
constexpr uint64_t pageDesc(uint64_t pa) { return (pa & kOutMask) | kAf | kShOuter | kApRw | kDescPage; }   // AttrIndx 0
struct Tables {
  uint64_t *cpu = nullptr;   // pageCount x 512 descriptors, cleaned before the SMMU walks
  uint64_t pa = 0;           // of page 0; 4 KiB aligned, all table pages contiguous
  unsigned pageCount = kTablePages;
};
// pages: the UC region's kUcPages physical 4 KiB pages; scratch: a zeroed 4 KiB page for the rest of the window. False when a page
// is not a 4 KiB-aligned page of XNU's RAM.
inline bool build(Tables &t, const uint64_t *pages, unsigned count, uint64_t scratch)
{
  if (!t.cpu || (t.pa & 0xfff) || count != kUcPages) return false;
  for (unsigned i = 0; i < count; ++i)
    if ((pages[i] & 0xfff) || pages[i] < kXnuDramBase || pages[i] >= kXnuDramEnd) return false;
  if ((scratch & 0xfff) || scratch < kXnuDramBase || scratch >= kXnuDramEnd) return false;
  if (t.pageCount < kTablePages || t.pageCount > kMaxSessionTablePages) return false;
  for (unsigned i = 0; i < t.pageCount * 512; ++i) t.cpu[i] = 0;
  uint64_t *l1 = t.cpu, *l2 = t.cpu + 512, *l3 = t.cpu + 1024;
  l1[kIovaBase >> 30] = tableDesc(t.pa + 4096);
  const unsigned l2i = (kIovaBase >> 21) & 511u;
  l2[l2i] = tableDesc(t.pa + 2 * 4096);
  l2[l2i + 1] = tableDesc(t.pa + 3 * 4096);
  for (unsigned i = 0; i < kWindowPages; ++i) l3[i] = pageDesc(i < count ? pages[i] : scratch);
  return true;
}
// The translation our table gives an IOVA (the SMMU's walk over the CPU's view): 0 when it faults.
inline uint64_t walk(const Tables &t, uint32_t iova)
{
  auto at = [&](uint64_t pa) -> uint64_t {
    if (pa < t.pa || pa >= t.pa + t.pageCount * 4096) return 0;
    return t.cpu[(pa - t.pa) / 8];
  };
  uint64_t d = at(t.pa + 8 * (iova >> 30));
  if ((d & 3) != kDescTable) return 0;
  d = at((d & kOutMask) + 8 * ((iova >> 21) & 511u));
  if ((d & 3) != kDescTable) return 0;
  d = at((d & kOutMask) + 8 * ((iova >> 12) & 511u));
  if ((d & 3) != kDescPage || !(d & kAf)) return 0;
  return (d & kOutMask) | (iova & 0xfff);
}

// ------------------------------------------------------------------------------------------------------- the bank and routes
// The bank programmed completely, translation on, then each stream's S2CR and SMR; TLBs maintained. nullptr on success, else what
// failed (the caller then tears down what was written). The Ops write offsets from kSmmu (logged once each).
template <class O>
bool tlbSync(O &ops, uint32_t cb)
{
  ops.wr(kSmmu + cb + kTlbiAll, 0xffffffffu, "smmu bank TLBIALL");
  ops.wr(kSmmu + cb + kTlbSync, 0xffffffffu, "smmu bank TLBSYNC");
  uint32_t last = 0;
  const bool ok = ops.poll(kSmmu + cb + kTlbStatus, 1, 0, kSyncPolls, 1, &last);
  ops.hw.note("smmu: bank sync cb %#x result %u status %08x", cb, unsigned(ok), last);
  return ok;
}
template <class O>
bool globalSync(O &ops)
{
  ops.wr(kSmmu + kTlbiAllNsnh, 0xffffffffu, "smmu TLBIALLNSNH");
  ops.wr(kSmmu + kTlbGsync, 0xffffffffu, "smmu sTLBGSYNC");
  uint32_t last = 0;
  const bool ok = ops.poll(kSmmu + kTlbGstatus, 1, 0, kSyncPolls, 1, &last);
  ops.hw.note("smmu: global sync result %u status %08x", unsigned(ok), last);
  return ok;
}

// Recheck immediately before the first write. This detects stale plans, not an atomic reservation across kexts.
template <class O>
const char *available(const Plan &p, O &ops)
{
  auto rd = [&](uint32_t off) { return ops.rd(kSmmu + off); };
  if (rd(kScr0) != p.scr0 || rd(kIdr0) != p.idr0 || rd(kIdr1) != p.idr1 || rd(kIdr2) != p.idr2)
    return "SMMU geometry changed since plan";
  for (unsigned k = 0; k < 2; ++k)
    if (rd(smrOffset(p.group[k])) != p.smrFound[k] || rd(s2crOffset(p.group[k])) != p.s2crFound[k])
      return "a selected stream group changed since plan";
  if (rd(p.cb() + kSctlr) != p.sctlrFound || rd(cbarOffset(p.bank)) != p.cbarFound || rd(cba2rOffset(p.bank)) != p.cba2rFound)
    return "the selected bank changed since plan";
  for (unsigned i = 0; i < 9; ++i)
    if (rd(p.cb() + kBankWords[i]) != p.bankFound[i]) return "the selected S1 bank words changed since plan";
  for (unsigned g = 0; g < p.groups; ++g) {
    const uint32_t smr = rd(smrOffset(g));
    if (!(smr & 0x80000000u)) continue;
    if (covers(smr, kStreamFirst) || covers(smr, kStreamSecond)) return "a video SID acquired a route since plan";
    const uint32_t s2cr = rd(s2crOffset(g));
    if ((s2cr & 0x300ffu) == p.bank) return "the selected bank acquired a stream since plan";
  }
  return nullptr;
}

struct Routed {
  bool bank = false;           // the bank's registers were written
  bool group[2] = {};          // the group's S2CR was written
};

// A stable fingerprint is a checkable no-competition precondition, not an atomic cross-kext lock. Check it again before
// publishing either SID. Every active S1 bank's TTBR0 and TTBR1 shares the ASID namespace when ASIDPNE reads zero.
template <class O>
const char *owners(const Plan &p, O &ops, uint64_t &stamp)
{
  stamp = 1469598103934665603ull;
  auto mix = [&](uint32_t v) { stamp = (stamp ^ v) * 1099511628211ull; };
  for (unsigned g = 0; g < p.groups; ++g) {
    if (g == p.group[0] || g == p.group[1]) continue;
    mix(ops.rd(kSmmu + smrOffset(g)));
    mix(ops.rd(kSmmu + s2crOffset(g)));
  }
  for (unsigned b = 0; b < p.banks; ++b) {
    if (b == p.bank) continue;
    const uint64_t cb = kSmmu + (p.pages + b) * 4096;
    const uint32_t sctlr = ops.rd(cb + kSctlr);
    mix(sctlr);
    if (!(sctlr & kSctlrM)) continue;
    const uint32_t cbar = ops.rd(kSmmu + cbarOffset(b));
    mix(cbar);
    if (((cbar >> 16) & 3u) == 0) continue;   // S2 has no S1 ASID.
    const uint32_t t0 = ops.rd(cb + kTtbr0Hi), t1 = ops.rd(cb + kTtbr1Hi);
    mix(t0); mix(t1);
    if (((t0 >> 16) & 0xffffu) == p.asid || ((t1 >> 16) & 0xffffu) == p.asid) {
      ops.hw.note("smmu: ASID conflict bank %u asid %u active-bank %u ttbr-hi %08x/%08x", p.bank, p.asid, b, t0, t1);
      return "our ASID is held by an active S1 bank";
    }
  }
  return nullptr;
}

template <class O>
bool verify(O &ops, const char *word, uint64_t pa, uint32_t want, uint32_t mask)
{
  const uint32_t got = ops.rd(pa), mismatch = (want ^ got) & mask;
  ops.hw.note("smmu: verify %s want %08x got %08x mask %08x mismatch %08x %s", word, want, got, mask, mismatch,
              mismatch ? "FAIL" : "PASS");
  return !mismatch;
}

template <class O>
const char *apply(const Plan &p, uint64_t tablePa, O &ops, Routed &done)
{
  if (p.error) return p.error;
  if (!tablePa || (tablePa & 0xfff) || (tablePa + kTablePages * 4096 - 1) >> kPaBits[p.pasize])
    return "the table is not aligned or exceeds the output size";
  const char *conflict = available(p, ops);
  uint64_t before = 0, after = 0;
  if (!conflict) conflict = owners(p, ops, before);
  ops.hw.note("smmu: V3 recheck %s bank %u ASID %u owners %016llx (no cross-kext lock)", conflict ? conflict : "clear", p.bank,
              p.asid, (unsigned long long)before);
  if (conflict) return conflict;
  const uint32_t cb = p.cb();
  done.bank = true;
  ops.wr(kSmmu + cb + kSctlr, 0, "smmu bank SCTLR: translation off while programmed");
  if (!verify(ops, "SCTLR-off", kSmmu + cb + kSctlr, 0, kSctlrM)) return "bank translation did not stop";
  ops.wr(kSmmu + cba2rOffset(p.bank), kCba2rVa64, "smmu CBA2R: VA64");
  ops.wr(kSmmu + cbarOffset(p.bank), kCbarStage1, "smmu CBAR: stage 1, stage 2 bypass");
  ops.wr(kSmmu + cb + kTcr2, tcr2For(p.pasize), "smmu bank TCR2: PASize, SEP upstream");
  ops.wr(kSmmu + cb + kTcr, kTcrVideo, "smmu bank TCR: T0SZ 32, 4 KiB, non-cacheable walks");
  ops.wr(kSmmu + cb + kTtbr0Lo, uint32_t(tablePa), "smmu bank TTBR0 low: our table");
  ops.wr(kSmmu + cb + kTtbr0Hi, ttbrHi(tablePa, p.asid), "smmu bank TTBR0 high: PA and own ASID");
  ops.wr(kSmmu + cb + kTtbr1Lo, 0, "smmu bank TTBR1 low: none");
  ops.wr(kSmmu + cb + kTtbr1Hi, ttbrHi(0, p.asid), "smmu bank TTBR1 high: own ASID, EPD1");
  ops.wr(kSmmu + cb + kContextIdr, 0, "smmu bank CONTEXTIDR");
  ops.wr(kSmmu + cb + kMair0, kMair0Video, "smmu bank MAIR0: attribute 0 Normal non-cacheable");
  ops.wr(kSmmu + cb + kMair1, 0, "smmu bank MAIR1");
  const uint32_t fsr = ops.rd(kSmmu + cb + kFsr);
  ops.hw.note("smmu: stale FSR raw %08x fault-bits %08x format %u valid %u", fsr, fsr & kFsrFaults, (fsr >> 9) & 3,
              unsigned((fsr & kFsrFaults) != 0));
  if (fsr & kFsrSs) ops.wr(kSmmu + cb + kResume, 1, "smmu bank RESUME: terminate a stalled transaction");
  if (fsr & kFsrFaults) ops.wr(kSmmu + cb + kFsr, fsr & kFsrFaults, "smmu bank FSR: stale faults cleared");
  if (!tlbSync(ops, cb)) return "the bank's TLB sync did not finish";
  ops.wr(kSmmu + cb + kSctlr, kSctlrVideo, "smmu bank SCTLR: M TRE AFE CFRE, optional ASIDPNE");
  bool ok = true;
  auto check = [&](const char *word, uint32_t off, uint32_t want, uint32_t mask = 0xffffffffu) {
    ok = verify(ops, word, kSmmu + off, want, mask) && ok;   // Do not short-circuit the evidence.
  };
  check("SCTLR", cb + kSctlr, kSctlrVideo, kSctlrRequired);
  check("CBA2R", cba2rOffset(p.bank), kCba2rVa64);
  check("CBAR", cbarOffset(p.bank), kCbarStage1);
  check("TCR", cb + kTcr, kTcrVideo);
  check("TCR2", cb + kTcr2, tcr2For(p.pasize), kTcr2Fields);
  const uint32_t tcr2 = ops.rd(kSmmu + cb + kTcr2);
  ops.hw.note("smmu: TCR2 fields PASize %u SEP %u AS %u TBI %02x tolerated %02x", tcr2 & 7, (tcr2 >> 15) & 7,
              (tcr2 >> 4) & 1, tcr2 & kTcr2Tbi, kTcr2Tbi);
  check("TTBR0-lo", cb + kTtbr0Lo, uint32_t(tablePa));
  check("TTBR0-hi", cb + kTtbr0Hi, ttbrHi(tablePa, p.asid));
  check("TTBR1-lo", cb + kTtbr1Lo, 0);
  check("TTBR1-hi", cb + kTtbr1Hi, ttbrHi(0, p.asid));
  check("MAIR0", cb + kMair0, kMair0Video);
  check("MAIR1", cb + kMair1, 0);
  if (!ok) return "the S1 bank fields do not read back (see verify FAIL)";
  conflict = owners(p, ops, after);
  ops.hw.note("smmu: owners before-routes %016llx->%016llx ASID %u result %u", (unsigned long long)before,
              (unsigned long long)after, p.asid, unsigned(!conflict && before == after));
  if (conflict || before != after) return conflict ? conflict : "other SMMU owners changed during bank apply";
  // Recheck the selected groups immediately before publishing. Program and verify BOTH S2CRs before either SMR.
  for (unsigned k = 0; k < 2; ++k) {
    const unsigned g = p.group[k];
    if (ops.rd(kSmmu + smrOffset(g)) != p.smrFound[k] || ops.rd(kSmmu + s2crOffset(g)) != p.s2crFound[k])
      return "a selected group changed during bank apply";
  }
  for (unsigned k = 0; k < 2; ++k) {
    const unsigned g = p.group[k];
    done.group[k] = true;
    ops.wr(kSmmu + s2crOffset(g), p.bank, "smmu S2CR: translate through our bank");
    if (!verify(ops, "S2CR", kSmmu + s2crOffset(g), p.bank, kS2crFields))
      return "a stream's S2CR type/bank do not read back";
  }
  for (unsigned k = 0; k < 2; ++k) {
    const unsigned g = p.group[k];
    ops.hw.note("smmu: S2CR group %u TYPE/bank verified attributes want 00000000 got %08x", g,
                ops.rd(kSmmu + s2crOffset(g)) & kS2crAttrs);
    ops.wr(kSmmu + smrOffset(g), 0x80000000u | kStreams[k], "smmu SMR: valid, the video stream, mask 0");
    if (!verify(ops, "SMR", kSmmu + smrOffset(g), 0x80000000u | kStreams[k], 0xffffffffu))
      return "a stream's SMR does not read back";
  }
  if (!globalSync(ops)) return "the global TLB sync did not finish";
  return nullptr;
}

// Capability rules from this project's CameraSmmu.h. Never write an unsupported as-found word through trapped MMIO.
inline bool s2crRestorable(uint32_t v, unsigned banks)
{
  const unsigned type = (v >> 16) & 3u;
  return type == 1 || type == 2 || (type == 0 && (v & 0xffu) < banks);
}
inline bool cbarRestorable(uint32_t v, uint32_t idr0)
{
  const unsigned type = (v >> 16) & 3u;
  if (type == 1) return (idr0 & kIdr0S1ts) != 0;
  if (type == 3) return (idr0 & (1u << 28)) && (idr0 & kIdr0S1ts) && (idr0 & (1u << 29));
  return (idr0 & (1u << 29)) != 0;
}

// Caller has confirmed PAS stopped (or never began), with c5 buses/NoC still held. On any unconfirmed detach keep ownership
// flags set: no caller may free DMA or remove those dependencies. An invalid SMR plus M=0 is a legal free state.
template <class O>
bool teardown(const Plan &p, O &ops, Routed &done)
{
  if (!done.bank && !done.group[0] && !done.group[1]) return true;
  for (unsigned k = 0; k < 2; ++k) {
    if (!done.group[k]) continue;
    ops.wr(kSmmu + smrOffset(p.group[k]), p.smrFound[k] & ~0x80000000u, "smmu release SMR: invalid");
    if (!verify(ops, "release-SMR", kSmmu + smrOffset(p.group[k]), p.smrFound[k] & ~0x80000000u, 0xffffffffu)) return false;
  }
  // Do not bypass/fault-probe a live route. Only after BOTH matches are confirmed invalid may the S2CR change.
  for (unsigned k = 0; k < 2; ++k) {
    if (!done.group[k]) continue;
    const unsigned g = p.group[k];
    const bool restore = s2crRestorable(p.s2crFound[k], p.banks);
    const uint32_t want = restore ? p.s2crFound[k] : kS2crFault;
    ops.wr(kSmmu + s2crOffset(g), want, restore ? "smmu release S2CR: legal as found" : "smmu release S2CR: fault, illegal baseline discarded");
    const uint32_t got = ops.rd(kSmmu + s2crOffset(g));
    // Known Qualcomm FAULT emulation may leave the already accepted translation. Never accept an arbitrary/illegal value.
    const bool fallback = !restore && (got & (kS2crFields)) == p.bank;
    ops.hw.note("smmu: release group %u smr %08x S2CR found %08x want %08x got %08x fallback %u result %u", g,
                ops.rd(kSmmu + smrOffset(g)), p.s2crFound[k], want, got, unsigned(fallback), unsigned(got == want || fallback));
    if (got != want && !fallback) return false;
  }
  // A newly acquired external reference is a conflict, never permission to disable its bank.
  for (unsigned g = 0; g < p.groups; ++g) {
    const uint32_t smr = ops.rd(kSmmu + smrOffset(g));
    if ((smr & 0x80000000u) && (ops.rd(kSmmu + s2crOffset(g)) & kS2crFields) == p.bank) {
      ops.hw.note("smmu: release conflict group %u still references bank %u: DMA held", g, p.bank);
      return false;
    }
  }
  if (done.bank) {
    const uint32_t cb = p.cb();
    ops.wr(kSmmu + cb + kSctlr, 0, "smmu release SCTLR: translation off");
    if (!verify(ops, "release-SCTLR", kSmmu + cb + kSctlr, 0, kSctlrM)) return false;
    if (!tlbSync(ops, cb)) return false;
    const bool restore = cbarRestorable(p.cbarFound, p.idr0) && p.cba2rFound == kCba2rVa64;
    if (restore) {
      ops.wr(kSmmu + cbarOffset(p.bank), p.cbarFound, "smmu release CBAR: supported as found");
      ops.wr(kSmmu + cba2rOffset(p.bank), p.cba2rFound, "smmu release CBA2R: legal VA64 as found");
    }
    // A partial apply must also have left a legal S1 pair before being called clean.
    const uint32_t cbar = ops.rd(kSmmu + cbarOffset(p.bank)), cba2r = ops.rd(kSmmu + cba2rOffset(p.bank));
    const bool legal = cbar == (restore ? p.cbarFound : kCbarStage1) && cba2r == (restore ? p.cba2rFound : kCba2rVa64);
    ops.hw.note("smmu: release bank %u M0 1 CBAR %08x CBA2R %08x found %08x kept-S1 %u result %u", p.bank, cbar, cba2r,
                p.cbarFound, unsigned(!restore), unsigned(legal));
    if (!legal) return false;
  }
  if (!globalSync(ops)) return false;
  for (unsigned k = 0; k < 2; ++k)
    if (done.group[k] && (ops.rd(kSmmu + smrOffset(p.group[k])) & 0x80000000u)) return false;
  if (done.bank && (ops.rd(kSmmu + p.cb() + kSctlr) & kSctlrM)) return false;
  done.bank = done.group[0] = done.group[1] = false;
  ops.hw.note("smmu: release complete result 1 routes invalid bank M0 TLB complete");
  return true;
}

struct Fault {
  uint32_t fsr = 0, farLo = 0, farHi = 0, fsynr0 = 0, cbfrsynra = 0;
};
template <class O>
Fault fault(const Plan &p, O &ops)
{
  Fault f;
  const uint32_t cb = p.cb();
  f.fsr = ops.rd(kSmmu + cb + kFsr);
  f.farLo = ops.rd(kSmmu + cb + kFarLo);
  f.farHi = ops.rd(kSmmu + cb + kFarHi);
  f.fsynr0 = ops.rd(kSmmu + cb + kFsynr0);
  f.cbfrsynra = ops.rd(kSmmu + 0x1400 + 4 * p.bank);
  return f;
}

}  // namespace vsmmu
}  // namespace pianovideo
