// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoVideo levels 2-4 (the ladder's V1-V3, docs/drivers/video.md sections 8 and 12) as one portable sequence over the owner's Hw:
//   piano-video=2  V1: N power cycles (piano-video-cycles, default 1; the test plan's 20): on (VideoPower.h), the level 1 readout
//                  with the gate open and its verdict, hold (piano-video-hold-ms), off; every cycle must pass
//   piano-video=3  V2: N cycles of on, the firmware (VideoFirmware.h: the pinned file, PAS 9, MP_VIDEO_VAR), hold, PAS
//                  shutdown, off
//   piano-video=4  V3: the SMMU bank (VideoSmmu.h) and the UC region, on, the firmware, the boot words, MVS0 to hardware mode, the
//                  handshake (SYS_INIT, IMAGE_VERSION, interframe power collapse; the answers within 1 s; the version string), no
//                  SMMU fault; then idle: the core collapses piano-video-idle-ms (1.5 s) after its last activity (prepare-PC,
//                  SET_REMOTE_STATE(0), off; the firmware stays in the carveout); piano-video-resumes=N more resume (on,
//                  SET_REMOTE_STATE(1), boot words, hardware mode, interframe power collapse, IMAGE_VERSION as a ping) / idle /
//                  collapse cycles; piano-video-unload=1 ends with Linux's deinit (resume, PAS shutdown, off, the bank released).
// V3 failures capture evidence, stop PAS, release the domain with power/NoC held, then off/free. An unconfirmed quiesce/detach/TLB
// completion keeps DMA and available dependencies, and reports the cleanup failure. V1/V2 retain their power/PAS sequence; piano-video-stop=STEP stops the same way after that step (the bisecting aid). Every register write is
// logged once (VideoOps.h); every wait is bounded.
//
// The Hw (a duck type: the kernel's in PianoVideo.cpp, the model in tests/bringup_host.cpp): VideoOps.h's rd/wr/delayUs/sleepMs/
// nowUs/note/stage, VideoRpmh.h's drv2Take/drv2Give, VideoFirmware.h's firmware, carveout and secure-call members, and:
//   const uint8_t *cmdDb(uint32_t *bytes)   the AOP command database (a snapshot, kCmdDbBytes; nullptr when not readable)
//   bool dmaAlloc(Dma &d) / void dmaFree(Dma &d)   V3's memory (below); zeroed, the tables cleaned by cleanToPoc
// The includer declares snprintf (libkern/libkern.h in the kernel, <stdio.h> on the host).
#pragma once
#include <stdint.h>
#include "VideoFirmware.h"
#include "VideoHfi.h"
#include "VideoPower.h"
#include "VideoSmmu.h"
#include "VideoArena.h"
#include "VideoSession.h"
#include "VideoEncode.h"

namespace pianovideo {

struct Options {
  unsigned level = 0;                     // piano-video
  unsigned cycles = 1;                    // piano-video-cycles (V1, V2)
  unsigned holdMs = 50;                   // piano-video-hold-ms (V1, V2)
  unsigned idleMs = kIdleCollapseMs;      // piano-video-idle-ms (V3)
  unsigned resumes = 0;                   // piano-video-resumes (V3)
  bool unload = false;                    // piano-video-unload (V3)
  int stop = 0;                           // piano-video-stop
  uint32_t mmcxFloor = kMmcxDisplayFloor; // piano-video-mmcx-floor
};

// V3's memory, the owner's: the UC region (kUcBytes) as 4 KiB physical pages and a write-combined CPU view; one zeroed scratch page;
// the table's four contiguous 4 KiB pages and their cacheable CPU view.
struct Dma {
  volatile uint32_t *uc = nullptr;
  uint64_t pages[vsmmu::kUcPages] = {};
  uint64_t scratch = 0;
  uint64_t *tables = nullptr;
  uint64_t tablesPa = 0;
  void *owner = nullptr;   // the Hw's bookkeeping
  bool session = false;
  uint32_t arenaBytes = session::kAvcArenaBytes;
  volatile uint8_t *arena = nullptr;
  uint64_t *arenaPages = nullptr; // CPU metadata owned with the prepared buffers, not KC static __DATA
};

// "Off 1.5 s after the last frame" (docs/drivers/video.md section 4.5): the collapse decision, from the last activity.
struct IdleClock {
  uint64_t lastUs = 0;
  uint32_t delayMs = kIdleCollapseMs;
  void touch(uint64_t now) { lastUs = now; }
  uint64_t leftUs(uint64_t now) const
  {
    const uint64_t due = lastUs + uint64_t(delayMs) * 1000;
    return now >= due ? 0 : due - now;
  }
};

struct Report {
  int level = 0, error = kOk, failedStep = 0;
  unsigned cycles = 0, cyclesOk = 0, resumesOk = 0, collapses = 0;
  uint64_t onUsMax = 0, offUsMax = 0, handshakeUs = 0;
  uint32_t ctrlStatus = 0, fsr = 0, debugLines = 0;
  bool version = false;
  char versionText[kHfiVersionChars] = {};
  char summary[240] = {};
};

template <class Hw>
class Bringup {
 public:
  explicit Bringup(Hw &hw) : hw(hw), ops(hw, log), power(ops, res, votes, pst), firmware(ops, fst) {}
  Hw &hw;
  WriteLog log;
  Ops<Hw> ops;
  rpmh::Resources res;
  rpmh::Votes votes;
  PowerState pst;
  Power<Ops<Hw>> power;
  FwState fst;
  Firmware<Ops<Hw>> firmware;
  vsmmu::Plan plan;
  vsmmu::Routed routed;
  vsmmu::Tables tables;
  Dma dma;
  bool dmaHeld = false;
  hfi::Mem mem;
  hfi::Ids ids;
  uint32_t tx[hfi::kBufferWords] = {}, rx[hfi::kBufferWords] = {};
  IdleClock idle;
  Report rep;
  Options opt;
  uint32_t avcWidth = 0, avcHeight = 0; // selected before DMA preparation; zero preserves the legacy qualification
  bool hevc = false;
  uint32_t bitdepth = 8;
  // E1 (piano-video-enc=1): an encoder session's buffers replace the decoder layout while encoder is set; cleared after it.
  bool encoder = false;
  enc::Geometry encGeometry;
  unsigned doorbells = 0, txEvents = 0, rxEvents = 0, dbgChecks = 0, dbgErrors = 0, msgDrops = 0, watchdogSamples = 0;
  uint32_t watchdogSeen = 0;
  int teardownRc = kOk;
  int quarantineReason = kOk; // native safety latch; dependencies live until boot reset
  bool firstHfi = false;
  session::Layout layout;
  session::Result v4;
  unsigned lifecycleEvent = 0;
  void phase(const char *name, int rc) { hw.note("V3 lifecycle %u %s result %d", ++lifecycleEvent, name, rc); }
  bool domainPowerReady() const { return pst.on && pst.railsUp && pst.busOn && pst.axi0 && power.codecReadable(); }
  void powerTrace()
  {
    hw.note("V3 power ready %u rails %u bus %u controller %u codec %u AXI %u/%u PLL %u RCG %u", unsigned(domainPowerReady()),
            unsigned(pst.railsUp), unsigned(pst.busOn), unsigned(power.controllerReadable()), unsigned(power.codecReadable()),
            unsigned(pst.axi0), unsigned(pst.axi1), unsigned(pst.pll), unsigned(pst.rcg));
    if (!domainPowerReady()) return;
    hw.note("V3 power raw GCC ahb %08x axi0 %08x axi1 %08x PLL %08x RCG %08x", ops.rd(kGcc + kGccVideoAhb),
            ops.rd(kGcc + kGccVideoAxi0), ops.rd(kGcc + kGccVideoAxi1), ops.rd(kVideoCc + kCcPll0),
            ops.rd(kVideoCc + kCcMvs0Rcg));
    hw.note("V3 power raw GDSC %08x/%08x branches ctrl %08x/%08x codec %08x/%08x (HWCG idle permitted)",
            ops.rd(kVideoCc + kCcMvs0cGdsc), ops.rd(kVideoCc + kCcMvs0Gdsc), ops.rd(kVideoCc + kCcMvs0cClk),
            ops.rd(kVideoCc + kCcMvs0cFreerun), ops.rd(kVideoCc + kCcMvs0Clk), ops.rd(kVideoCc + kCcMvs0Freerun));
  }

  // V3 evidence is independent of the deduplicated MMIO write log. No IRQ-clear or new power/PAS sequence here.
  void bankTrace(const char *when)
  {
    if (plan.error || plan.bank == ~0u) return;
    const uint64_t cb = kSmmu + plan.cb();
    for (unsigned k = 0; k < 2; ++k)
      hw.note("V3 bank %s group %u sid %#x smr %08x s2cr %08x", when, plan.group[k], vsmmu::kStreams[k],
              ops.rd(kSmmu + vsmmu::smrOffset(plan.group[k])), ops.rd(kSmmu + vsmmu::s2crOffset(plan.group[k])));
    hw.note("V3 bank %s bank %u sctlr %08x cbar %08x cba2r %08x tcr %08x tcr2 %08x", when,
            plan.bank, ops.rd(cb + vsmmu::kSctlr), ops.rd(kSmmu + vsmmu::cbarOffset(plan.bank)),
            ops.rd(kSmmu + vsmmu::cba2rOffset(plan.bank)), ops.rd(cb + vsmmu::kTcr), ops.rd(cb + vsmmu::kTcr2));
    hw.note("V3 bank %s TTBR0 %08x%08x TTBR1 %08x%08x ASID %u mair %08x/%08x", when,
            ops.rd(cb + vsmmu::kTtbr0Hi), ops.rd(cb + vsmmu::kTtbr0Lo), ops.rd(cb + vsmmu::kTtbr1Hi),
            ops.rd(cb + vsmmu::kTtbr1Lo), plan.asid, ops.rd(cb + vsmmu::kMair0), ops.rd(cb + vsmmu::kMair1));
    const vsmmu::Fault f = vsmmu::fault(plan, ops);
    hw.note("V3 bank %s FSR raw %08x fault-bits %08x format %u valid %u FAR %08x%08x FSYNR %08x (invalid is stale)", when,
            f.fsr, f.fsr & vsmmu::kFsrFaults, (f.fsr >> 9) & 3, unsigned((f.fsr & vsmmu::kFsrFaults) != 0), f.farHi, f.farLo, f.fsynr0);
  }
  void queueTrace(const char *when)
  {
    if (!mem.uc) return;
    hw.barrier();
    for (unsigned q = 0; q < hfi::kNumQ; ++q) {
      const uint32_t h = hfi::qhdr(q);
      hw.note("V3 queue %s q%u read %u write %u drop %u rxreq %u txreq %u", when, q, mem.ld(h + hfi::kQRead),
              mem.ld(h + hfi::kQWrite), mem.ld(h + hfi::kQDrop), mem.ld(h + hfi::kQRxReq), mem.ld(h + hfi::kQTxReq));
    }
  }
  void coreTrace(const char *when)
  {
    if (!power.controllerReadable()) {
      hw.note("V3 core %s: MMIO skipped (controller not readable)", when);
      return;
    }
    const uint32_t irq = ops.rd(kIris + kWrapperIntrStatus);
    watchdogSeen |= irq & (1u << 3);
    ++watchdogSamples;
    if (opt.level == 6 && watchdogSeen) {
      hw.note("V9c core %s watchdog observed irq %08x no-further-trace-MMIO 1",when,irq);
      return;
    }
    hw.note("V3 core %s us %llu ctrl %08x tz %08x irq %08x mask %08x watchdog %u cpu-noc %08x/%08x video-noc %08x/%08x", when,
            (unsigned long long)hw.nowUs(), ops.rd(kIris + kCtrlStatus), ops.rd(kIris + kTzCpuStatus), irq,
            ops.rd(kIris + kWrapperIntrMask), unsigned(watchdogSeen != 0), ops.rd(kIris + kCpuNocLpiCtl),
            ops.rd(kIris + kCpuNocLpiStatus), ops.rd(kIris + kAonVideoCtlNocLpiCtl), ops.rd(kIris + kAonVideoCtlNocLpiStatus));
  }
  void sfrTrace(const char *when)
  {
    if (!mem.uc) return;
    char sfr[160];
    hw.barrier();
    hfi::sfrText(mem, sfr, sizeof sfr);
    hw.note("V3 SFR %s size %u text [%s]", when, mem.ld(kSfrOffset), sfr);
  }
  // Bounds checked before indexing even for a malformed RX; called before the response parser.
  void packetTrace(const char *direction, unsigned seq, const uint32_t *buf, uint32_t bytes)
  {
    // Native sessions retain binary TX/RX history and parsed error evidence.
    // The ladder's duplicate packet dump must not serialize first-picture work.
    if (opt.level == 6 && hfi::validFraming(buf, bytes)) return;
    if (bytes < kHfiHeaderBytes) {
      hw.note("V3 %s %u short header bytes %u word0 %08x", direction, seq, bytes, bytes >= 4 ? buf[0] : 0);
      return;
    }
    hw.note("V3 %s %u header bytes %u size %u session %u id %u reserved %08x/%08x/%08x/%08x packets %u", direction, seq, bytes,
            buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
    uint32_t at = kHfiHeaderBytes;
    for (unsigned p = 0; p < buf[7] && at <= bytes && bytes - at >= kHfiPacketBytes; ++p) {
      const uint32_t *w = buf + at / 4, size = w[0];
      const bool valid = size >= kHfiPacketBytes && !(size & 3) && size <= bytes - at;
      hw.note("V3 %s %u packet %u size %u type %08x flags %08x payload %u port %u id %u reserved %08x/%08x valid %u payload0 %08x",
              direction, seq, p, size, w[1], w[2], w[3], w[4], w[5], w[6], w[7], unsigned(valid), valid && size > kHfiPacketBytes ? w[8] : 0);
      if (!valid) break;
      at += size;
    }
  }

  // ------------------------------------------------------------------------------------------------------------------- run
  int run(const Options &o)
  {
    opt = o;
    rep = Report{};
    rep.level = int(o.level);
    if (o.level < 2 || o.level > 6) return kErrArgument;   // inert below piano-video=2: nothing is read or written
    power.stopAt = o.stop;
    firmware.stopAt = o.stop;
    // The boot framebuffer owns the display rail until a native display voter exists.  Keep
    // the guard in the portable bring-up too, so a caller cannot bypass the boot-arg clamp.
    votes.floorVlvl = o.mmcxFloor < kMmcxDisplayFloor ? kMmcxDisplayFloor : o.mmcxFloor;
    if (votes.floorVlvl != o.mmcxFloor)
      hw.note("rpmh: MMCX floor %#x raised to display floor %#x (boot-display guard)", o.mmcxFloor, votes.floorVlvl);
    idle.delayMs = o.idleMs;
    uint32_t bytes = 0;
    const uint8_t *db = hw.cmdDb(&bytes);
    res = rpmh::resources(db, bytes);
    if (res.error) {
      hw.note("rpmh: cmd-db %s: %s (error %d): nothing powered", db ? "read" : "not readable", res.missing ? res.missing : "-", res.error);
      return finish(kStepRails, kErrRpmh);
    }
    hw.note("rpmh: mxc.lvl %#x (%u levels, top vlvl %#x), mmcx.lvl %#x (%u levels), MM1 %#x; MMCX floor %#x", res.mxc.addr,
            res.mxc.count, res.mxc.vlvl[res.mxc.count - 1], res.mmcx.addr, res.mmcx.count, res.mm1, votes.floorVlvl);
    const int rc = o.level == 2 ? v1() : o.level == 3 ? v2() : v3();
    return finish(rc ? failedStep() : kStepDone, rc);
  }

  int failedStep() const
  {
    return pst.error ? pst.failedStep : fst.error ? fst.failedStep : rep.failedStep;
  }
  int finish(int stepNo, int rc)
  {
    rep.error = rc;
    rep.failedStep = rc ? stepNo : 0;
    if (rc) hw.stage(-stepNo, errorName(rc));
    else hw.stage(kStepDone, "done");
    summarize();
    return rc;
  }
  void summarize()
  {
    char *s = rep.summary;
    const unsigned size = sizeof rep.summary;
    const char *v = rep.level == 2 ? "V1" : rep.level == 3 ? "V2" : rep.level == 4 ? "V3" : "V4";
    if (rep.error) {
      snprintf(s, size, "%s FAILED at step %d: %s; cycles %u/%u; writes %u (%u logged)", v, rep.failedStep, errorName(rep.error),
               rep.cyclesOk, rep.cycles, log.writes, log.logged);
      return;
    }
    if (rep.level == 6)
      snprintf(s, size, "V5 native core ready: INIT/version %u dma-held %u", unsigned(rep.version), unsigned(dmaHeld));
    else if (rep.level == 5)
      snprintf(s, size, "V4 ok: kernel AVC decoded %u checksum PASS close %d teardown %d dma-held %u", v4.decoded, v4.close,
               teardownRc, unsigned(dmaHeld));
    else if (rep.level == 4)
      snprintf(s, size, "V3 ok: firmware %.48s; handshake %llu us; collapse/resume %u/%u; fsr %08x; fw lines %u; writes %u (%u logged)",
               rep.versionText, (unsigned long long)rep.handshakeUs, rep.resumesOk, opt.resumes, rep.fsr, rep.debugLines, log.writes,
               log.logged);
    else
      snprintf(s, size, "%s ok: %u/%u cycles; on max %llu us, off max %llu us; rpmh requests %u; writes %u (%u logged)", v,
               rep.cyclesOk, rep.cycles, (unsigned long long)rep.onUsMax, (unsigned long long)rep.offUsMax, votes.requests,
               log.writes, log.logged);
  }

  // ------------------------------------------------------------------------------------------------------------ V1 pieces
  int on()
  {
    hw.note("V9c power-on reason level-%u lifecycle %u already-on %u",opt.level,lifecycleEvent,unsigned(pst.on));
    const uint64_t t0 = hw.nowUs();
    const int rc = power.on();
    const uint64_t us = hw.nowUs() - t0;
    if (us > rep.onUsMax) rep.onUsMax = us;
    return rc;
  }
  int off()
  {
    hw.note("V9c power-off reason level-%u lifecycle %u watchdog %u",opt.level,lifecycleEvent,unsigned(watchdogSeen != 0));
    const uint64_t t0 = hw.nowUs();
    const int rc = power.off();
    const uint64_t us = hw.nowUs() - t0;
    if (us > rep.offUsMax) rep.offUsMax = us;
    return rc;
  }

  // Keep a bounded, safe snapshot beside every V2/V3 failure.  The snapshot is taken before
  // teardown while the domains that are readable are still clocked; early file/SMMU failures
  // explicitly say that no core MMIO was read.
  void failureRaw(int stepNo, int rc)
  {
    hw.note("failure raw: stage %d rc %d", stepNo, rc);
    if (stepNo >= kStepSmmuPlan && stepNo <= kStepSmmuRoute)
      hw.note("failure raw: smmu idr0 %08x idr1 %08x idr2 %08x scr0 %08x", plan.idr0, plan.idr1, plan.idr2, plan.scr0);
    bool any = false;
    if (power.ccReadable()) {
      any = true;
      hw.note("failure raw: gcc ahb %08x axi0 %08x axi1 %08x; videocc mvs0c-gdsc %08x mvs0-gdsc %08x mvs0c %08x mvs0c-fr %08x mvs0 %08x mvs0-fr %08x",
              ops.rd(kGcc + kGccVideoAhb), ops.rd(kGcc + kGccVideoAxi0), ops.rd(kGcc + kGccVideoAxi1),
              ops.rd(kVideoCc + kCcMvs0cGdsc), ops.rd(kVideoCc + kCcMvs0Gdsc), ops.rd(kVideoCc + kCcMvs0cClk),
              ops.rd(kVideoCc + kCcMvs0cFreerun), ops.rd(kVideoCc + kCcMvs0Clk), ops.rd(kVideoCc + kCcMvs0Freerun));
    }
    if (power.controllerReadable()) {
      any = true;
      hw.note("failure raw: iris ctrl-init %08x ctrl-status %08x core-power %08x core-clock %08x cpu-lpi %08x video-lpi %08x",
              ops.rd(kIris + kCtrlInit), ops.rd(kIris + kCtrlStatus), ops.rd(kIris + kCorePowerStatus), ops.rd(kIris + kCoreClockConfig),
              ops.rd(kIris + kCpuNocLpiStatus), ops.rd(kIris + kAonVideoCtlNocLpiStatus));
    }
    if (power.codecReadable()) {
      any = true;
      hw.note("failure raw: iris codec-power %08x tz-cpu %08x idle0 %08x idle1 %08x idle2 %08x idle3 %08x",
              ops.rd(kIris + kCorePowerStatus), ops.rd(kIris + kTzCpuStatus), ops.rd(kIris + kVcodecIdleStatus),
              ops.rd(kIris + kVcodecIdleStatus + 4), ops.rd(kIris + kVcodecIdleStatus + 8), ops.rd(kIris + kVcodecIdleStatus + 12));
    }
    if (routed.bank) {
      any = true;
      const vsmmu::Fault f = vsmmu::fault(plan, ops);
      hw.note("failure raw: smmu fsr %08x far %08x%08x fsynr0 %08x", f.fsr, f.farHi, f.farLo, f.fsynr0);
    }
    if (!any && stepNo < kStepSmmuPlan) hw.note("failure raw: no powered core MMIO was read at this stage");
  }
  // The level 1 survey with the gate open; V1's verdict.
  const char *readout(bool firmwareExpected)
  {
    hw.stage(kStepReadout, "the readout with the gate open");
    unsigned loads = 0;
    auto ld = [this](uint64_t pa) { return ops.rd(pa); };
    const Clocks c = surveyClocks(ld, 1, &loads);
    const Core r = surveyCore(c, ld, &loads);
    rep.ctrlStatus = r.ctrlStatus;
    hw.note("readout: pll0 %s %llu MHz, mvs0 %llu MHz (OPP %d); gdsc mvs0c %08x mvs0 %08x; core version %08x ctrl status %08x power "
            "status %08x (codec %s) intr mask %08x tz cpu %08x",
            pllLocked(c.pllMode) ? "locked" : "NOT locked", (unsigned long long)(pllHz(c.pllL, c.pllAlpha) / 1000000),
            (unsigned long long)(mvs0Hz(c) / 1000000), oppIndex(mvs0Hz(c)), c.mvs0cGdsc, c.mvs0Gdsc, r.hwVersion, r.ctrlStatus,
            r.powerStatus, (r.powerStatus & kCorePowered) ? "powered" : "collapsed", r.intrMask, r.tzCpuStatus);
    if (firmwareExpected) return c.codec && r.codecRead ? nullptr : "the codec's gate is closed after the power-on";
    return poweredVerdict(c, r);
  }

  int v1()
  {
    hw.note("V1: %u power cycle(s) at 240 MHz, %u ms powered each", opt.cycles, opt.holdMs);
    for (unsigned c = 1; c <= opt.cycles; ++c) {
      ++rep.cycles;
      int rc = on();
      if (rc) {
        failureRaw(pst.failedStep, rc);
        return rc;
      }
      const char *bad = readout(false);
      if (opt.stop == kStepReadout) {
        off();
        return fail(kStepReadout, kErrStopped);
      }
      hw.sleepMs(opt.holdMs);
      const int offRc = off();
      if (bad) {
        hw.note("V1: cycle %u: %s", c, bad);
        return fail(kStepReadout, kErrReadout);
      }
      if (offRc) return fail(kStepOffCheck, offRc);
      ++rep.cyclesOk;
      if (c == 1 || c == opt.cycles || c % 5 == 0)
        hw.note("V1: cycle %u/%u ok (on max %llu us, off max %llu us; %u writes, %u logged)", c, opt.cycles,
                (unsigned long long)rep.onUsMax, (unsigned long long)rep.offUsMax, log.writes, log.logged);
    }
    return kOk;
  }
  int fail(int stepNo, int rc)
  {
    rep.failedStep = stepNo;
    pst.error = kOk;   // the step is ours, not the power sequence's
    fst.error = kOk;
    return rc;
  }

  // ----------------------------------------------------------------------------------------------------------------- V2
  int v2()
  {
    hw.note("V2: %u cycle(s): power on, PAS 9 with the pinned file (the boot's addon or the ESP), MP_VIDEO_VAR, %u ms, PAS shutdown, "
            "power off", opt.cycles, opt.holdMs);
    for (unsigned c = 1; c <= opt.cycles; ++c) {
      ++rep.cycles;
      int rc = on();
      if (rc) {
        failureRaw(pst.failedStep, rc);
        return rc;
      }
      rc = firmware.load();
      if (rc) {
        failureRaw(firmware.st.failedStep, rc);
        off();
        return rc;
      }
      const uint32_t status = ops.rd(kIris + kCtrlStatus), tz = ops.rd(kIris + kTzCpuStatus);
      hw.note("V2: cycle %u: the firmware is authenticated; CTRL_STATUS %08x (no HFI yet), tz cpu status %08x", c, status, tz);
      hw.sleepMs(opt.holdMs);
      const int un = firmware.unload();
      if (un) failureRaw(kStepPasShutdown, un);
      const int offRc = off();
      if (un) return fail(kStepPasShutdown, un);
      if (offRc) {
        failureRaw(kStepOffCheck, offRc);
        return fail(kStepOffCheck, offRc);
      }
      ++rep.cyclesOk;
    }
    return kOk;
  }

  // ----------------------------------------------------------------------------------------------------------------- V3
  // Domain lifetime: resume a collapsed firmware for PAS shutdown, stop DMA, detach with the c5 dependencies still held,
  // synchronize, then drop power and free memory. A failure retains DMA (and any available dependencies), never reports unloaded.
  int teardown()
  {
    // A native firmware/session failure is not an ordinary idle transition.
    // Do not touch failed hardware again (including diagnostic MMIO), collapse
    // a NoC with unconfirmed work, or free its DMA on this path.
    if (opt.level == 6 && (quarantineReason || watchdogSeen || rep.fsr)) {
      teardownRc = quarantineReason ? quarantineReason : watchdogSeen ? kErrWatchdog : kErrFault;
      quarantineReason = teardownRc;
      hw.note("V9c quarantine reason %d watchdog %u fault %08x dma-held %u power-held %u no-cleanup-MMIO 1 reboot-required 1",
              teardownRc,unsigned(watchdogSeen != 0),rep.fsr,unsigned(dmaHeld),unsigned(pst.on));
      phase("native-quarantine",teardownRc);
      return teardownRc;
    }
    phase("release-begin", kOk);
    int un = kOk, faultRc = kOk, offRc = kOk;
    const bool hadPas = fst.loaded || fst.initDone;
    if (hadPas && !pst.on) {
      // Do not repeat HFI/queue commands just to stop the processor. Power and secure resume suffice for PAS shutdown.
      un = on();
      if (!un) un = hw.scmRemoteState(kRemoteStateResume) ? kErrScm : kOk;
      phase("release-resume", un);
    }
    coreTrace("before-teardown");
    if (opt.level == 6 && watchdogSeen) {
      quarantineReason = kErrWatchdog;
      return teardown();
    }
    queueTrace("before-teardown");
    sfrTrace("before-teardown");
    if (routed.bank) faultRc = checkFault();
    if (opt.level == 6 && faultRc) {
      quarantineReason = faultRc;
      return teardown(); // the guard above logs without further hardware access
    }
    if (!un && hadPas) {
      if (domainPowerReady()) un = firmware.unload();
      else un = kErrScm;
    }
    hw.note("V3 teardown PAS %s result %d loaded %u init %u", hadPas ? "performed" : "skipped-never-started", un,
            unsigned(fst.loaded), unsigned(fst.initDone));
    phase("device-quiesced", un);
    const bool touched = routed.bank || routed.group[0] || routed.group[1];
    bool detached = !touched;
    if (!un && touched && domainPowerReady()) {
      hw.stage(kStepSmmuTeardown, "video routes invalid, legal S1 release while power/NoC held");
      detached = vsmmu::teardown(plan, ops, routed);
      bankTrace("released");
    }
    phase("domain-release", detached ? kOk : kErrSmmu);
    // No TLB operation after off(). Do not withdraw the buses after an unconfirmed detach or quiesce.
    if (!un && detached) {
      offRc = off();
      if (opt.level == 6 && !offRc && pst.offWarnings) offRc = kErrOffCheck;
      phase("power-off", offRc);
      hw.note("V3 teardown power result %d warnings %u rails %u bus %u controller %u codec %u", offRc, pst.offWarnings,
              unsigned(pst.railsUp), unsigned(pst.busOn), unsigned(pst.ctrlGdsc), unsigned(pst.codecGdsc));
      if (!offRc && dmaHeld) {
        hw.dmaFree(dma);
        dmaHeld = false;
        mem.uc = nullptr;
      }
    } else hw.note("V3 teardown dependencies held: PAS %d detached %u power-ready %u", un, unsigned(detached), unsigned(domainPowerReady()));
    phase(dmaHeld ? "DMA-held" : "DMA-freed", dmaHeld ? kErrSmmu : kOk);
    teardownRc = un ? un : !detached ? kErrSmmu : offRc ? offRc : faultRc ? faultRc : watchdogSeen ? kErrWatchdog : kOk;
    hw.note("V3 teardown result %d PAS %d power %d smmu %u dma-held %u watchdog %u samples %u", teardownRc, un, offRc,
            unsigned(detached), unsigned(dmaHeld), unsigned(watchdogSeen != 0), watchdogSamples);
    phase("release-end", teardownRc);
    return teardownRc;
  }
  int v3fail(int stepNo, int rc)
  {
    failureRaw(stepNo, rc);
    coreTrace("failure");
    queueTrace("failure");
    bankTrace("failure");
    sfrTrace("failure");
    if (routed.bank) checkFault();   // a fault on the video bank says why the firmware did not answer
    hw.note("V3: %s at step %d: powering everything down", errorName(rc), stepNo);
    hw.note("V3 evidence: first-HFI %s version %u teardown pending; error %d step %d", firstHfi ? "passed" : "not passed",
            unsigned(rep.version), rc, stepNo);
    hw.note("V3 version error %d stage %d received %u (see CTRL/RX/SFR evidence)", rc, stepNo, unsigned(rep.version));
    if (opt.level == 6) quarantineReason = rc;
    teardown();
    hw.note("V3 outcome first-HFI %s first-error %d stage %d teardown %d dma-held %u", firstHfi ? "PASS" : "FAIL", rc, stepNo,
            teardownRc, unsigned(dmaHeld));
    if (opt.level == 5)
      hw.note("V4 outcome first-error %d stage %d decoded 0 checksum FAIL close 0 teardown %d dma-held %u", rc, stepNo,
              teardownRc, unsigned(dmaHeld));
    return fail(stepNo, rc);
  }

  int smmuFail(int stepNo, int rc)
  {
    rep.failedStep = stepNo;
    failureRaw(stepNo, rc);
    return rc;
  }
  bool prepareLayout()
  {
    if (encoder) return enc::buildLayout(layout, encGeometry);
    return hevc ? layout.initHevc(avcWidth, avcHeight, bitdepth) :
           avcWidth ? layout.init(avcWidth, avcHeight) : layout.init();
  }
  bool mapArena()
  {
    if (!dma.arena || !session::buildArena(tables, layout, dma.arenaPages, dma.arenaBytes)) return false;
    const uint32_t span = layout.arenaBytes();
    unsigned mapped = 0;
    for (unsigned i = 0; i < span / 4096; ++i) mapped += layout.mapped(i);
    hw.note("V4 mapping arena [%08x,%08x) reserved %u pages mapped %u guards-invalid %u tables %u full-walk PASS",
            session::kArenaBase, session::kArenaBase + span, layout.bytes, mapped, span / 4096 - mapped, tables.pageCount);
    for (unsigned i = 0; i < layout.used; ++i) {
      const session::Buffer &b = layout.buf[i];
      hw.note("V4 mapping type %u index %u port %u capacity %u IOVA [%08x,%08x) PA-first %llx PA-last %llx",
              b.type,b.index,b.port,b.capacity,b.iova(),b.iova()+b.capacity,
              (unsigned long long)dma.arenaPages[b.off/4096],
              (unsigned long long)dma.arenaPages[(b.off+b.capacity-1)/4096]);
    }
    return true;
  }
  int prepareResidentSession()
  {
    if (opt.level != 6 || !firstHfi || !fst.loaded || !dmaHeld || !domainPowerReady()) return kErrHfi;
    watchdogSeen |= ops.rd(kIris + kWrapperIntrStatus) & (1u << 3);
    ++watchdogSamples;
    if (watchdogSeen) return kErrWatchdog;
    if (const int rc = checkFault()) return rc;
    if (!prepareLayout() || !layout.arenaBytes() || layout.arenaBytes() > dma.arenaBytes) return kErrNoMemory;
    // The previous CLOSE ack fences arena DMA. Keep the live UC/queues
    // intact and synchronize changed arena leaves before the next OPEN.
    if (!mapArena()) return kErrSmmu;
    hw.cleanToPoc(tables.cpu, tables.pageCount * 4096);
    if (!vsmmu::tlbSync(ops, plan.cb()) || !vsmmu::globalSync(ops)) return kErrSmmu;
    return checkFault();
  }
  int smmuSetup()
  {
    hw.stage(kStepSmmuPlan, "SMMU: two free groups and a free bank for 0x1940/0x1947");
    plan = vsmmu::plan([this](uint32_t off) { return ops.rd(kSmmu + off); });
    hw.note("smmu: idr0 %08x idr1 %08x idr2 %08x scr0 %08x: %u groups (%u free), %u banks; groups %d/%d bank %d PASize %u%s%s", plan.idr0,
            plan.idr1, plan.idr2, plan.scr0, plan.groups, plan.freeGroups, plan.banks, int(plan.group[0]), int(plan.group[1]),
            int(plan.bank), plan.pasize, plan.error ? ": " : "", plan.error ? plan.error : "");
    if (plan.error) return smmuFail(kStepSmmuPlan, kErrSmmu);
    bankTrace("planned");
    hw.stage(kStepSmmuTables, "the UC region's memory and our table");
    dma.session = opt.level >= 5;
    if (dma.session && !prepareLayout()) return smmuFail(kStepSmmuTables, kErrNoMemory);
    if (!dmaHeld) {
      dma.arenaBytes = layout.arenaBytes();
      if (!hw.dmaAlloc(dma)) return smmuFail(kStepSmmuTables, kErrNoMemory);
      dmaHeld = true;
    }
    tables.cpu = dma.tables;
    tables.pa = dma.tablesPa;
    tables.pageCount = dma.session ? vsmmu::sessionTablePages(dma.arenaBytes) : vsmmu::kTablePages;
    if (!vsmmu::build(tables, dma.pages, vsmmu::kUcPages, dma.scratch)) {
      hw.note("smmu: a page of the UC region or the table is not a 4 KiB page of XNU's RAM");
      return smmuFail(kStepSmmuTables, kErrSmmu);
    }
    if (dma.session && !mapArena()) return smmuFail(kStepSmmuTables, kErrSmmu);
    hw.cleanToPoc(tables.cpu, tables.pageCount * 4096);
    for (unsigned i = 0; i < vsmmu::kWindowPages; ++i) {   // the self-check: every page of the window as the SMMU will walk it
      const uint64_t want = i < vsmmu::kUcPages ? dma.pages[i] : dma.scratch;
      if (vsmmu::walk(tables, kIovaBase + 4096 * i + 0x10) != want + 0x10) {
        hw.note("smmu: the table's walk of IOVA %#x does not give its page", kIovaBase + 4096 * i);
        return smmuFail(kStepSmmuTables, kErrSmmu);
      }
    }
    hw.note("smmu: table at %#llx maps IOVA %#x + %#x (%u pages of the UC region, the rest on scratch %#llx)",
            (unsigned long long)tables.pa, kIovaBase, kUcRegionBytes, vsmmu::kUcPages, (unsigned long long)dma.scratch);
    hw.note("V3 memory: walk %u/%u ok UC physical first %#llx last %#llx table [%#llx,%#llx) scratch %#llx alias [%#x,%#x)",
            vsmmu::kWindowPages, vsmmu::kWindowPages, (unsigned long long)dma.pages[0],
            (unsigned long long)dma.pages[vsmmu::kUcPages - 1], (unsigned long long)tables.pa,
            (unsigned long long)(tables.pa + vsmmu::kTablePages * 4096), (unsigned long long)dma.scratch,
            kIovaBase + kUcBytes, kIovaBase + kUcRegionBytes);
    if (opt.stop == kStepSmmuTables) return smmuFail(kStepSmmuTables, kErrStopped);
    hw.stage(kStepQueues, "the queue table, three queues and the SFR");
    mem.uc = dma.uc;
    mem.iova = kIovaBase;
    hfi::initQueues(mem);
    hw.barrier();
    hw.note("V3 layout: version %u table %u qhdr0 %u qhdr-size %u queues %u active %u cookie %08x%08x name iris-hfi-queues",
            mem.ld(hfi::kTblVersion), mem.ld(hfi::kTblSize), mem.ld(hfi::kTblQhdr0Offset), mem.ld(hfi::kTblQhdrSize),
            mem.ld(hfi::kTblNumQ), mem.ld(hfi::kTblNumActiveQ), mem.ld(hfi::kTblDeviceAddr + 4), mem.ld(hfi::kTblDeviceAddr));
    for (unsigned q = 0; q < hfi::kNumQ; ++q) {
      const uint32_t h = hfi::qhdr(q);
      hw.note("V3 layout: q%u [%#x,%#x) status %u type %08x size-words %u packet %u drop %u wm %u/%u req %u/%u irq %u/%u read %u write %u",
              q, mem.ld(h + hfi::kQStart), mem.ld(h + hfi::kQStart) + kHfiQueueBytes, mem.ld(h + hfi::kQStatus), mem.ld(h + hfi::kQType),
              mem.ld(h + hfi::kQSize), mem.ld(h + hfi::kQPktSize), mem.ld(h + hfi::kQDrop), mem.ld(h + hfi::kQRxWm), mem.ld(h + hfi::kQTxWm),
              mem.ld(h + hfi::kQRxReq), mem.ld(h + hfi::kQTxReq), mem.ld(h + hfi::kQRxIrq), mem.ld(h + hfi::kQTxIrq),
              mem.ld(h + hfi::kQRead), mem.ld(h + hfi::kQWrite));
    }
    hw.note("V3 layout: SFR [%#x,%#x) size %u valid-pages %u padding-alias-pages %u", kIovaBase + kSfrOffset,
            kIovaBase + kSfrOffset + kSfrBytes, mem.ld(kSfrOffset), vsmmu::kUcPages, vsmmu::kWindowPages - vsmmu::kUcPages);
    return kOk;
  }

  int smmuApply()
  {
    if (!domainPowerReady()) return smmuFail(kStepSmmuRoute, kErrSmmu);
    hw.stage(kStepSmmuRoute, "SMMU: the bank, then the two streams through it");
    bankTrace("before-apply");
    if (const char *why = vsmmu::apply(plan, tables.pa, ops, routed)) {
      hw.note("smmu: %s", why);
      return smmuFail(kStepSmmuRoute, kErrSmmu);
    }
    bankTrace("applied");
    if (checkFault()) return smmuFail(kStepSmmuRoute, kErrFault);
    if (opt.stop == kStepSmmuRoute) return smmuFail(kStepSmmuRoute, kErrStopped);
    phase("routes-effective", kOk);
    return kOk;
  }

  void ring()
  {
    ops.wr(kIris + kCpuIcSoftInt, kSoftIntH2a, "IC_SOFTINT: doorbell");
    ++doorbells;
    if (opt.level != 6) hw.note("V3 doorbell %u tx %u cmd read %u write %u", doorbells, txEvents,
            mem.ld(hfi::qhdr(hfi::kCmdq) + hfi::kQRead), mem.ld(hfi::qhdr(hfi::kCmdq) + hfi::kQWrite));
  }
  int send(uint32_t bytes, const char *what)
  {
    const unsigned seq = ++txEvents;
    const uint32_t h = hfi::qhdr(hfi::kCmdq), beforeR = mem.ld(h + hfi::kQRead), beforeW = mem.ld(h + hfi::kQWrite);
    packetTrace("TX", seq, tx, bytes);
    const int rc = hfi::write(mem, hfi::kCmdq, tx, bytes, [this] { hw.barrier(); });
    if (opt.level != 6 || rc) {
      hw.note("hfi: %s: %u bytes, header %u, %u packet(s)%s", what, bytes, tx[2], tx[7], rc ? ": the command queue has no room" : "");
      hw.note("V3 TX %u %s result %d cmd read %u->%u write %u->%u", seq, what, rc, beforeR, mem.ld(h + hfi::kQRead), beforeW, mem.ld(h + hfi::kQWrite));
    }
    if (rc) return kErrHfi;
    ring();
    return kOk;
  }
  void drainDebug()
  {
    uint32_t bytes = 0;
    char line[120];
    for (unsigned n = 0; n < 64; ++n) {
      ++dbgChecks;
      const int rc = hfi::read(mem, hfi::kDbgq, rx, sizeof rx, &bytes, [this] { hw.barrier(); });
      if (rc == 1) return;
      hw.note("V3 DBG read result %d bytes %u size %u level %08x", rc, bytes, bytes >= 4 ? rx[0] : 0, bytes >= 8 ? rx[1] : 0);
      if (rc || !hfi::debugText(rx, bytes, line, sizeof line)) { ++dbgErrors; continue; }
      ++rep.debugLines;
      if (rep.debugLines <= 32 || rep.debugLines % 64 == 0) hw.note("fw: %s", line);
    }
  }
  // Keep partial INIT/version evidence even when the other answer never arrives.
  int waitFor(bool wantInit, bool wantVersion, hfi::Reply &r, uint32_t ms)
  {
    const uint64_t start = hw.nowUs();
    const uint32_t target = mem.ld(hfi::qhdr(hfi::kCmdq) + hfi::kQWrite);
    unsigned checks = 0;
    bool seenInit = false, seenVersion = false, seenReply = false;
    int result = kErrHfiTimeout;
    for (;;) {
      ++checks;
      uint32_t bytes = 0;
      for (unsigned n = 0; n < 64; ++n) {
        const uint32_t h = hfi::qhdr(hfi::kMsgq), beforeR = mem.ld(h + hfi::kQRead), beforeW = mem.ld(h + hfi::kQWrite);
        const int rc = hfi::read(mem, hfi::kMsgq, rx, sizeof rx, &bytes, [this] { hw.barrier(); });
        if (rc == 1) break;
        if (!seenReply) {
          seenReply = true;
          hw.note("V3 RX first elapsed %llu us", (unsigned long long)(hw.nowUs() - start));
        }
        hw.note("V3 RX %u result %d bytes %u msg read %u->%u write %u->%u", ++rxEvents, rc, bytes, beforeR,
                mem.ld(h + hfi::kQRead), beforeW, mem.ld(h + hfi::kQWrite));
        if (rc == -2) { ++msgDrops; continue; }
        if (!rc) packetTrace("RX", rxEvents, rx, bytes);
        if (rc || !hfi::parse(rx, bytes, r)) {
          hw.note("hfi: a malformed message (read %d, %u bytes, word 0 %08x)", rc, bytes, rx[0]);
          result = kErrHfi;
          break;
        }
        if (r.initDone && !seenInit) {
          seenInit = true;
          hw.note("V3 INIT success elapsed %llu us", (unsigned long long)(hw.nowUs() - start));
        }
        if (r.version && !seenVersion) {
          seenVersion = true;
          hw.note("V3 version received elapsed %llu us", (unsigned long long)(hw.nowUs() - start));
          noteVersion(r);
        }
      }
      drainDebug();
      const uint32_t irq = ops.rd(kIris + kWrapperIntrStatus);
      ++watchdogSamples;
      watchdogSeen |= irq & (1u << 3);
      if (watchdogSeen) { result = kErrWatchdog; break; }
      if (result == kErrHfi) break;
      if (r.sysError) {
        hw.note("hfi: system error %08x (flags %08x)", r.errorType, r.errorFlags);
        result = kErrSysError;
        break;
      }
      if (r.initFailed || r.malformed) { result = kErrHfi; break; }
      const bool consumed = mem.ld(hfi::qhdr(hfi::kCmdq) + hfi::kQRead) == target;
      if ((!wantInit || r.initDone) && (!wantVersion || r.version) && consumed) { result = kOk; break; }
      if (hw.nowUs() - start >= uint64_t(ms) * 1000) break;
      hw.sleepMs(1);
    }
    hw.note("V3 wait result %d elapsed %llu us checks %u init %u init-failed %u version %u headers %u packets %u session %u malformed %u drops %u",
            result, (unsigned long long)(hw.nowUs() - start), checks, unsigned(r.initDone), unsigned(r.initFailed), unsigned(r.version),
            r.headers, r.packets, r.sessionHeaders, r.malformed, msgDrops);
    hw.note("V3 DBG summary checks %u lines %u errors %u watchdog %u samples %u", dbgChecks, rep.debugLines, dbgErrors,
            unsigned(watchdogSeen != 0), watchdogSamples);
    queueTrace("after-wait");
    coreTrace("after-wait");
    sfrTrace("after-wait");
    return result;
  }
  void noteVersion(const hfi::Reply &r)
  {
    unsigned n = 0;
    while (n < sizeof rep.versionText - 1 && r.text[n]) {
      rep.versionText[n] = r.text[n];
      ++n;
    }
    while (n && rep.versionText[n - 1] == ' ') --n;
    rep.versionText[n] = 0;
    rep.version = true;
    hw.note("hfi: firmware version: %s", rep.versionText);
  }
  int checkFault()
  {
    const vsmmu::Fault f = vsmmu::fault(plan, ops);
    rep.fsr |= f.fsr & vsmmu::kFsrFaults;
    hw.note("V3 fault snapshot bank %u raw %08x fault-bits %08x format %u valid %u far %08x%08x fsynr0 %08x", plan.bank, f.fsr,
            f.fsr & vsmmu::kFsrFaults, (f.fsr >> 9) & 3, unsigned((f.fsr & vsmmu::kFsrFaults) != 0), f.farHi, f.farLo, f.fsynr0);
    hw.note("V3 fault syndrome %s CBFRSYNRA %08x SID %04x (FSYNR0 is not SID)", (f.fsr & vsmmu::kFsrFaults) ? "valid" : "stale",
            f.cbfrsynra, f.cbfrsynra & 0xffffu);
    if (!(f.fsr & vsmmu::kFsrFaults)) return kOk;
    hw.note("smmu: FAULT on the video bank: fsr %08x far %08x%08x fsynr0 %08x", f.fsr, f.farHi, f.farLo, f.fsynr0);
    return kErrFault;
  }
  // The boot words and hardware mode (iris_core_init / iris_hfi_pm_resume after the power-on).
  int bootCore()
  {
    hw.stage(kStepBoot, "the boot words: UC region, queue table, SFR, SPARE_0, CTRL_INIT");
    coreTrace("before-CTRL_INIT");
    uint32_t status = 0;
    const int rc = hfi::boot(ops, kIovaBase, &status);
    hw.stage(kStepCtrlStatus, "CTRL_STATUS");
    rep.ctrlStatus = status;
    hw.note("hfi: CTRL_STATUS %08x: %s", status,
            rc == kOk ? "init done"
            : (status & kCtrlStatusErrorMask) == kCtrlStatusErrorUcRegion ? "error 0x4: the UC region is refused"
            : status ? "nonzero status failed the init/error rule" : "no answer in the bounded 100 ms poll");
    coreTrace("after-CTRL_STATUS");
    if (rc) return rc;
    hw.stage(kStepHwMode, "MVS0 GDSC to hardware mode");
    power.hardwareMode();
    const uint32_t gdsc = ops.rd(kVideoCc + kCcMvs0Gdsc);
    hw.note("V3 hardware-mode result %u mvs0-gdsc %08x", unsigned((gdsc & kGdscHwControl) != 0), gdsc);
    if (!(gdsc & kGdscHwControl)) return kErrGdsc;
    return checkFault();
  }
  // iris_vpu_prepare_pc + SET_REMOTE_STATE(0) + the power-off: the firmware stays.
  int collapse()
  {
    hw.stage(kStepPcPrep, "prepare power collapse");
    bool ready = false;
    for (unsigned attempt = 0; attempt < 10 && !ready; ++attempt) {
      const uint32_t s = ops.rd(kIris + kCtrlStatus);
      const uint32_t wfi = ops.rd(kIris + kTzCpuStatus) & 1u;
      hw.note("V3 PC attempt %u ctrl %08x PC_READY %u WFI %u", attempt + 1, s, unsigned((s & kCtrlStatusPcReady) != 0), wfi);
      if (s & kCtrlStatusPcReady) {
        ready = true;
        break;
      }
      if (!wfi || !(s & kCtrlStatusIdle)) {   // Linux: "skip power collapse" (-EAGAIN), tried again later
        hw.note("hfi: not idle yet (ctrl %08x, wfi %u): collapse tried again in 100 ms", s, wfi);
        hw.sleepMs(100);
        continue;
      }
      if (send(hfi::pcPrep(tx, ids), "SYS_PC_PREP")) return kErrHfi;
      uint32_t ctrl = 0, tz = 0;
      const uint64_t start = hw.nowUs();
      const bool pc = ops.poll(kIris + kCtrlStatus, kCtrlStatusPcReady, kCtrlStatusPcReady, kPcPollUs, kPcStepUs, &ctrl);
      const bool asleep = pc && ops.poll(kIris + kTzCpuStatus, 1, 1, kPcPollUs, kPcStepUs, &tz);
      ready = pc && asleep;
      hw.note("V3 PC poll result %u PC_READY %u WFI %u ctrl %08x tz %08x elapsed %llu us", unsigned(ready), unsigned(pc),
              unsigned(asleep), ctrl, tz, (unsigned long long)(hw.nowUs() - start));
      if (!ready) break;
    }
    if (!ready) return kErrPc;
    coreTrace("PC-ready");
    queueTrace("PC-ready");
    if (const int rc = checkFault()) return rc;
    if (watchdogSeen) return kErrWatchdog;
    if (opt.stop == kStepPcPrep) return kErrStopped;
    hw.stage(kStepRemoteSuspend, "SET_REMOTE_STATE(0, 0)");
    if (hw.scmRemoteState(kRemoteStateSuspend)) return kErrScm;
    hw.stage(kStepCollapsed, "power off (the firmware stays in the carveout)");
    const int rc = off();
    if (rc) return rc;
    ++rep.collapses;
    hw.note("V3: collapsed after %u ms idle (collapse %u); the firmware kept", opt.idleMs, rep.collapses);
    return checkFault();
  }
  // iris_hfi_pm_resume: on, SET_REMOTE_STATE(1), the boot words, hardware mode, interframe power collapse; then a version ping.
  int resume(bool ping)
  {
    hw.note("V3 resume begin cycle %u ping %u", rep.resumesOk + 1, unsigned(ping));
    queueTrace("before-resume");
    hw.stage(kStepResume, "resume: power on");
    int rc = on();
    if (rc) return rc;
    hw.stage(kStepRemoteResume, "SET_REMOTE_STATE(1, 0)");
    if (hw.scmRemoteState(kRemoteStateResume)) return kErrScm;
    hw.stage(kStepReboot, "the boot words again (no PAS: the firmware stayed)");
    if ((rc = bootCore())) return rc;
    if ((rc = send(hfi::interframePowerCollapse(tx, ids), "INTRA_FRAME_POWER_COLLAPSE"))) return rc;
    if (!ping) return kOk;
    hw.stage(kStepPing, "IMAGE_VERSION as a ping");
    if ((rc = send(hfi::imageVersion(tx, ids), "IMAGE_VERSION"))) return rc;
    hfi::Reply r;
    if ((rc = waitFor(false, true, r, kHfiResponseMs))) return rc;
    if ((rc = checkFault())) return rc;
    hw.note("V3 resume ping PASS cycle %u version %u", rep.resumesOk + 1, unsigned(r.version));
    return kOk;
  }
  void waitIdle()
  {
    hw.stage(kStepIdle, "idle: the collapse delay after the last activity");
    idle.touch(hw.nowUs());
    for (uint64_t left; (left = idle.leftUs(hw.nowUs())) > 0;) {
      const uint64_t ms = (left + 999) / 1000;
      hw.sleepMs(uint32_t(ms > 100 ? 100 : ms));
    }
  }

  int v3()
  {
    if (opt.level == 5) hw.note("V4 image contract vid-v4a: same-bank one-AU bounded kernel self-test");
    hw.note("V3: prepare UC, power on, SMMU bank/routes, PAS 9, HFI boot and handshake; collapse after %u ms idle; %u resume(s)%s", opt.idleMs,
            opt.resumes, opt.unload ? "; then unload" : "");
    hw.note("V3 evidence contract vid-v3b: S1 lifecycle; c5 power/PAS and HFI unchanged");
    power.traceOff = true;
    ops.traceSmmu = true;
    ++rep.cycles;
    int rc = smmuSetup();
    phase("memory-prepared", rc);
    if (rc) return v3fail(rep.failedStep, rc);
    rc = on();
    phase("power-on", rc);
    powerTrace();
    if (rc) return v3fail(pst.failedStep, rc);
    coreTrace("after-power");
    rc = smmuApply();
    phase("S1-apply", rc);
    if (rc) return v3fail(rep.failedStep, rc);
    if ((rc = firmware.load())) return v3fail(fst.failedStep, rc);
    coreTrace("after-PAS");
    if ((rc = bootCore())) return v3fail(kStepCtrlStatus, rc);
    if (opt.stop == kStepHwMode) return v3fail(kStepHwMode, kErrStopped);
    hw.stage(kStepHfiSend, "SYS_INIT, IMAGE_VERSION, INTRA_FRAME_POWER_COLLAPSE");
    const uint64_t t0 = hw.nowUs();
    if ((rc = send(hfi::sysInit(tx, ids), "SYS_INIT (+7 UBWC properties)")) || (rc = send(hfi::imageVersion(tx, ids), "IMAGE_VERSION")) ||
        (rc = send(hfi::interframePowerCollapse(tx, ids), "INTRA_FRAME_POWER_COLLAPSE")))
      return v3fail(kStepHfiSend, rc);
    hw.stage(kStepHfiWait, "the answers (1 s)");
    hfi::Reply r;
    if ((rc = waitFor(true, true, r, kHfiResponseMs))) return v3fail(kStepHfiWait, rc);
    rep.handshakeUs = hw.nowUs() - t0;
    hw.note("hfi: INIT answered with success after %llu us (%u messages, %u packets)", (unsigned long long)rep.handshakeUs, r.headers,
            r.packets);
    if ((rc = checkFault())) return v3fail(kStepHfiWait, rc);
    if (opt.stop == kStepHfiWait) return v3fail(kStepHfiWait, kErrStopped);
    firstHfi = true;
    hw.note("V3 first-HFI PASS: INIT success version received cmd-consumed %u doorbells %u fsr %08x watchdog %u",
            mem.ld(hfi::qhdr(hfi::kCmdq) + hfi::kQRead), doorbells, rep.fsr, unsigned(watchdogSeen != 0));
    if (opt.level == 6) {
      hw.note("V5 native core ready INIT/version 1 exclusive 1 no-autonomous-ETB 1");
      return kOk;
    }
    if (opt.level == 5) {
      session::SelfTest<Bringup<Hw>> test(*this, layout);
      rc = test.run();
      int outerStep = 0;
      v4 = test.r;
      if (rc) {
        failureRaw(60 + int(v4.failedStage), rc);
        queueTrace("V4-error"); bankTrace("V4-error"); coreTrace("V4-error"); sfrTrace("V4-error");
      } else {
        waitIdle();
        rc = collapse();
        if (rc) outerStep = kStepPcPrep;
        else { rc = resume(false); if (rc) outerStep = kStepResume; }
      }
      const int un = teardown();
      if (!rc && un) { rc = un; outerStep = kStepUnload; }
      hw.note("V4 outcome first-error %d stage %u decoded %u checksum %s close %d teardown %d dma-held %u",
              rc, rc ? (v4.error ? v4.failedStage : unsigned(outerStep)) : 0u, v4.decoded, v4.checksum ? "PASS" : "FAIL", v4.close,
              teardownRc, unsigned(dmaHeld));
      hw.note("V4 complete ETB %u EBD %u PSC %u RELEASE_DONE %u drain-ack %u LAST %u STOP %u/%u CLOSE %u watchdog %u fault-bits %08x",
              v4.etb,v4.ebd,v4.psc,v4.releases,unsigned(v4.drainAck),unsigned(v4.last),unsigned(v4.stopRaw),unsigned(v4.stopInput),
              unsigned(v4.closed),unsigned(watchdogSeen != 0),rep.fsr);
      return rc ? fail(v4.error ? 60 + int(v4.failedStage) : outerStep, rc) : kOk;
    }
    waitIdle();
    if ((rc = collapse())) return v3fail(kStepPcPrep, rc);
    for (unsigned i = 1; i <= opt.resumes; ++i) {
      if ((rc = resume(true))) return v3fail(kStepResume, rc);
      waitIdle();
      if ((rc = collapse())) return v3fail(kStepPcPrep, rc);
      ++rep.resumesOk;
      hw.note("V3: collapse/resume %u/%u ok", i, opt.resumes);
    }
    if (opt.unload) {   // iris_core_deinit: resumed first, then PAS shutdown, then off
      hw.stage(kStepUnload, "unload: resume, PAS shutdown, power off, the bank released");
      if ((rc = resume(false))) return v3fail(kStepUnload, rc);
      if ((rc = teardown())) {
        hw.note("V3 outcome first-HFI PASS first-error %d stage %d teardown %d dma-held %u", rc, kStepUnload, teardownRc, unsigned(dmaHeld));
        return fail(kStepUnload, rc);
      }
      hw.note("V3: unloaded: PAS 9 shut down, the core off, the video streams released");
    } else {
      hw.note("V3: steady state: the core collapsed, the firmware in the carveout, the bank and the UC region kept");
    }
    hw.note("V3 outcome first-HFI PASS first-error 0 stage 0 teardown %d dma-held %u", teardownRc, unsigned(dmaHeld));
    hw.note("V3 evidence complete: first-HFI PASS resumes %u/%u collapses %u teardown %d watchdog %u fsr %08x",
            rep.resumesOk, opt.resumes, rep.collapses, teardownRc, unsigned(watchdogSeen != 0), rep.fsr);
    return watchdogSeen ? kErrWatchdog : kOk;
  }
};

}  // namespace pianovideo
