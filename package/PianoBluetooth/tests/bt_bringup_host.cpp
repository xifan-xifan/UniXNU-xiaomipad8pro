// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoBluetooth levels 2..4 (BtBringup.h) and the RSC transaction (BtRpmh.h), the whole ladder against models:
//   - the board: GCC (the QUP2 vote word, five branches that run only when voted, the SE6 RCG with its update bit and DFS registers),
//     TLMM (the four UART pins, SW_CTRL), the QUP2 wrapper and SE6 as a GENI UART (TX command and FIFO, RX sequencer and FIFO with its
//     status, cancel/abort, internal loopback, manual RFR, CTS masking, the serial clock from the source and the divider), BT_EN's
//     PMIC GPIO over SPMI (owner, kind, five registers), the PMU's rails through the real shared helper (PianoWcnRails.h) on a PMIC
//     model, and a clock;
//   - the wire: the engine's TX pad reaches the simulated controller (tests/QcaSim.h) only when gpio26 is the engine's, at the speed
//     the registers give, and only once the controller is powered (rails up, BT_EN high) for 50 ms; the controller's events come back
//     through gpio27 when the engine listens and RFR is ready, at the controller's speed; CTS is the controller's readiness;
//   - success at 3.2 Mbit/s with DFS off (the RCG programmed) and on (a level chosen), at 115200 only, levels 2 and 3 alone, a stop
//     request, a suspend (safeOff) and a resume, Wi-Fi owning the rails first;
//   - faults at every level: BT_EN owned by another EE, not an LV/MV GPIO, a denied PMIC write, a refused rail vote, a branch that
//     never runs, SE6 without the UART protocol, an RCG that never updates, a broken loopback, a controller whose RTS never asserts,
//     one deaf at the new speed, a missing rampatch, a pending sleep in the middle of the download;
//   - and the invariants: no SE6/QUP2 access while unclocked, no byte to the controller before the pins are the engine's, only BT_EN's
//     five registers written, only this driver's vote bits withdrawn, the rails put on every failure.
//   c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -I../../PianoKeyboard -I../../include bt_bringup_host.cpp -o /tmp/bt_bringup_host && /tmp/bt_bringup_host
#include "BtBoardSim.h"
#include "Address.h"
#include "BtRpmh.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pianobt;
using namespace pianobt::sim;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static const uint8_t kMadeUpAddress[6] = {0x02, 0x00, 0x00, 0x5e, 0x10, 0x01};   // a locally administered address, display order
static uint8_t gNvm[kNvmMaxBytes];
static Bringup gB;   // 3 KiB of port buffers, as the kext keeps it: not on the stack

static Options options(unsigned level)
{
  Options o;
  o.level = level;
  o.setup.address = kMadeUpAddress;
  return o;
}

// After a failure or a suspend: the engine stopped, the pins inputs with pull-downs, BT_EN low, the votes withdrawn, the rails put.
static void checkSafe(Board &b, uint32_t votesBefore = 0)
{
  CHECK(!(b.bten[kPgOutSrc] & kPgOutLevel) && !b.powered);
  CHECK(b.g(kGccQup2Vote) == votesBefore);
  for (unsigned i = kPinCts; i <= kPinRx; ++i)
    if (b.tlmm.count(kPins[i].gpio * kTlmmStride)) CHECK(b.tlmm[kPins[i].gpio * kTlmmStride] == pinConfig(-1, 0, kUartDriveMa, false));
  CHECK(!(b.railState & PIANO_WCN_BT) && b.pmuOwner == 0);
  CHECK(!gB.up);
}

// ------------------------------------------------------------------------------------------------------------ the tests
static void testSuccess(bool dfs)
{
  Board b;
  if (dfs) {   // the boot chain enabled DFS: level 0 7.3728 MHz, level 3 51.2 MHz, the rest TCXO
    const uint32_t r = kGccS6Rcg;
    b.gcc[r + kRcgDfsCmd] = 1;
    b.gcc[r + kRcgDfsPerf + 0] = (6u << 8) | (2u << 12) | 1u;
    b.gcc[r + kRcgDfsM + 0] = 384;
    b.gcc[r + kRcgDfsN + 0] = ~(15625u - 384u);
    b.gcc[r + kRcgDfsPerf + 12] = (6u << 8) | (2u << 12) | 1u;
    b.gcc[r + kRcgDfsM + 12] = 64;
    b.gcc[r + kRcgDfsN + 12] = ~(375u - 64u);
  }
  const Hw hw = hwFor(b);
  const FwIo fw = fwFor(b);
  Options o = options(4);
  CHECK(bringUp(gB, hw, o, fw, gNvm, sizeof gNvm) == kBtOk && gB.up && gB.error == 0 && gB.step == kStepLocalVersion);
  // Level 2: the rails voted by the helper, BT_EN high (only its level written: the stock configuration was there).
  CHECK(b.rails.allOn() && b.railState == (PIANO_WCN_BT | PIANO_WCN_UP | (0x3fu << PIANO_WCN_ADDED_SHIFT)) && b.pmuOwner == 0);
  CHECK(b.pmicWrites.size() == 1 && b.pmicWrites[0].first == kPgOutSrc && b.pmicWrites[0].second == kPgOutLevel && !b.pmicBadRegs);
  CHECK(gB.power.configured && !gB.power.wasHigh && gB.power.high && (gB.power.swCtrlIo & 1));
  // Level 3: the five votes, the engine at 115200 first, the loopback, the pins after it.
  CHECK(b.g(kGccQup2Vote) == kVoteBits && gB.uart.added == kVoteBits && b.unclocked == 0);
  CHECK(gB.uart.loopEchoed == 48 && gB.uart.loopMismatch == 0 && b.loopBytes == 48);
  CHECK(gB.uart.clock.sourceHz == 7372800 && gB.uart.clock.divider == 4 && gB.uart.dfs == dfs);
  CHECK(dfs ? b.rcgWrites == 0 : gB.uart.rcgProgrammed);
  CHECK(!b.chip.junk && b.garbage == 0 && b.lostBeforeReady == 0);   // nothing reached the controller unframed or too early
  for (unsigned i = kPinCts; i <= kPinRx; ++i) CHECK(b.pinIsEngine(kPins[i].gpio));
  CHECK(b.tlmm[26 * kTlmmStride] == pinConfig(1, 1, 2, false) && b.tlmm[24 * kTlmmStride] == pinConfig(-1, 1, 2, false));
  // Level 4: at 3.2 Mbit/s, the firmware sent whole, the address the unit's own and kept over the second reset.
  const SetupResult &r = gB.setup;
  CHECK(r.error == kSetupOk && r.baud == 3200000 && b.hostBaud() == 3200000 && b.chip.chipBaud == 3200000);
  CHECK(b.sourceHz() == 51200000 && decodeSerClk(b.reg(kSerMClkCfg)).divider == 1);
  CHECK(b.reg(kClkSel) == (dfs ? 3u : 6u));
  CHECK(b.chip.received[Sim::kPatch] == b.chip.files["gngbtfw20.mbn"] && !strcmp(r.nvm, "gngbtnv20.b2e"));
  CHECK(r.nvmInfo.baudAfter == 0x11 && r.nvmInfo.hciAfter == 0x03 && !strcmp(r.build, "BTFW1"));
  CHECK(r.reset0 == kSetupOk && b.chip.resets == 3 && r.local.manufacturer == kManufacturerQualcomm);
  CHECK(r.addressIsNvmDefault && r.addressSet && r.addressKept && b.chip.address[0] == 0x01 && b.chip.address[5] == 0x02);
  CHECK(b.chip.periFrames == 1 && b.chip.wakeInds == 1 && b.chip.junk == 0);
  CHECK(gB.port.txTimeouts == 0 && gB.port.rxBytes > 0);
  // The steps in order.
  const std::vector<int> want = {201, 202, 203, 204, 205, 301, 302, 303, 304, 305, 306, 400, 401, 402, 403, 404, 405, 406,
                                 407, 408, 409, 410, 411, 412};
  CHECK(b.steps == want);

  // System sleep: safeOff; the controller off; then the wake: the same ladder again from the start.
  safeOff(gB, hw, o);
  checkSafe(b);
  CHECK(!b.rails.allOn() && b.railState == 0);   // nobody else on the rails: this driver's enables withdrawn
  b.steps.clear();
  CHECK(bringUp(gB, hw, o, fw, gNvm, sizeof gNvm) == kBtOk && gB.up && b.chip.received[Sim::kPatch].size() == kStockRampatchBytes);
  CHECK(b.unclocked == 0 && b.chip.junk == 0);
}

static void testLevels()
{
  {   // level 2 alone: the rails and BT_EN, no clock and no SE6 access
    Board b;
    const Hw hw = hwFor(b);
    CHECK(bringUp(gB, hw, options(2), fwFor(b), gNvm, sizeof gNvm) == kBtOk && gB.up && b.powered);
    CHECK(b.gcc.find(kGccQup2Vote) == b.gcc.end() && b.se.empty() && b.tlmm.empty());
    safeOff(gB, hw, options(2));
    checkSafe(b);
  }
  {   // level 3 alone: the UART up, not a byte to the controller
    Board b;
    const Hw hw = hwFor(b);
    CHECK(bringUp(gB, hw, options(3), fwFor(b), gNvm, sizeof gNvm) == kBtOk && gB.up && b.toChip == 0 && b.chip.log.empty());
    CHECK(gB.uart.step == kStepPins && b.sActive && !b.loopback());
  }
  {   // level 3 without the self test
    Board b;
    Options o = options(3);
    o.loopback = false;
    CHECK(bringUp(gB, hwFor(b), o, fwFor(b), gNvm, sizeof gNvm) == kBtOk && b.loopBytes == 0);
  }
  {   // 115200 only: the RCG stays at 7.3728 MHz, no peripheral frame
    Board b;
    Options o = options(4);
    o.setup.operBaud = 115200;
    CHECK(bringUp(gB, hwFor(b), o, fwFor(b), gNvm, sizeof gNvm) == kBtOk && b.hostBaud() == 115200 && b.chip.periFrames == 0);
    CHECK(b.sourceHz() == 7372800);
  }
  {   // a stop request after the speed change: the state of that step stays
    Board b;
    Options o = options(4);
    o.setup.stopAfter = kStepBaud;
    CHECK(bringUp(gB, hwFor(b), o, fwFor(b), gNvm, sizeof gNvm) == kBtStopped && b.powered && b.hostBaud() == 3200000);
    CHECK(b.chip.segments[Sim::kPatch] == 0);
  }
  {   // a stop at 204: BT_EN high, the PMU lock given back
    Board b;
    Options o = options(4);
    o.stopAfter = kStepBtEnHigh;
    CHECK(bringUp(gB, hwFor(b), o, fwFor(b), gNvm, sizeof gNvm) == kBtStopped && b.powered && b.pmuOwner == 0);
  }
  {   // BT_EN found high: low for 100 ms first
    Board b;
    b.bten[kPgOutSrc] = kPgOutLevel;
    for (auto &[n, on] : b.rails.enabled) on = true;
    CHECK(bringUp(gB, hwFor(b), options(2), fwFor(b), gNvm, sizeof gNvm) == kBtOk && gB.power.wasHigh);
    CHECK(b.pmicWrites.size() == 2 && b.pmicWrites[0].second == 0 && b.pmicWrites[1].second == kPgOutLevel);
  }
  {   // BT_EN not configured as the DT says (an input): the five registers in PGPIO's order, each read back
    Board b;
    b.bten[kPgMode] = 0;
    b.bten[kPgPull] = 4;
    CHECK(bringUp(gB, hwFor(b), options(2), fwFor(b), gNvm, sizeof gNvm) == kBtOk && !gB.power.configured);
    const std::vector<std::pair<uint8_t, uint8_t>> want = {{kPgOutSrc, 0},       {kPgVin, kPgVin1},  {kPgPull, kPgPullNone},
                                                           {kPgMode, kPgModeOutput}, {kPgEn, kPgEnabled}, {kPgOutSrc, kPgOutLevel}};
    CHECK(b.pmicWrites == want && b.pmicBadRegs == 0 && b.powered);
  }
}

static void testWifiFirst()
{
  Board b;
  CHECK(piano_wcn_rails_get(&b.wifi, PIANO_WCN_WLAN) == PianoWcnOk && b.rails.txs == 6);
  const Hw hw = hwFor(b);
  const Options o = options(4);
  CHECK(bringUp(gB, hw, o, fwFor(b), gNvm, sizeof gNvm) == kBtOk && b.rails.txs == 6);   // a read-back, no vote
  CHECK(b.railState == (PIANO_WCN_WLAN | PIANO_WCN_BT | PIANO_WCN_UP | (0x3fu << PIANO_WCN_ADDED_SHIFT)));
  safeOff(gB, hw, o);   // Bluetooth sleeps: Wi-Fi still owns the rails, nothing withdrawn
  CHECK(b.rails.allOn() && b.railState == (PIANO_WCN_WLAN | PIANO_WCN_UP | (0x3fu << PIANO_WCN_ADDED_SHIFT)) && !b.powered);
  safeOff(gB, hw, o);   // repeated teardown does not put a second time
  CHECK(b.bt.puts == 1 && b.bt.put_unbalanced == 0 && b.pmuOwner == 0 && b.rails.txs == 6);
  CHECK(bringUp(gB, hw, o, fwFor(b), gNvm, sizeof gNvm) == kBtOk && b.bt.verifies == 2 && b.rails.txs == 6);
  safeOff(gB, hw, o);
  CHECK(piano_wcn_rails_owners(&b.wifi) == (PIANO_WCN_WLAN | PIANO_WCN_UP) && b.bt.puts == 2 && b.rails.txs == 6);
  CHECK(piano_wcn_rails_put(&b.wifi, PIANO_WCN_WLAN | PIANO_WCN_RELEASE) == 0);
  CHECK(b.railState == 0 && b.pmuOwner == 0 && b.rails.txs == 12 && !b.rails.allOn());
  // Wi-Fi holding the PMU lock past the bound: level 2 stops before anything, nothing written.
  Board c;
  c.pmuOwner = PianoWcnTagWifi;
  c.bt.bound_ms = 20;
  CHECK(bringUp(gB, hwFor(c), options(4), fwFor(c), gNvm, sizeof gNvm) == kBtPmuLock && gB.power.step == -kStepRails);
  CHECK(c.rails.txs == 0 && c.pmicWrites.empty() && c.pmuOwner == PianoWcnTagWifi);
}

static void testFaults()
{
  struct Case {
    const char *what;
    void (*setup)(Board &);
    int error, step;
    bool pmicTouched;
  };
  static const Case cases[] = {
    {"BT_EN owned by EE 2", [](Board &b) { b.btenInfo.owner = 2; }, kBtBtEnOwner, -kStepBtEnCheck, false},
    {"BT_EN not found", [](Board &b) { b.btenInfo.apid = -1; }, kBtNoBtEn, -kStepBtEnCheck, false},
    {"BT_EN an older GPIO", [](Board &b) { b.btenInfo.subtype = 0x01; }, kBtBtEnKind, -kStepBtEnCheck, false},
    {"PMIC write denied", [](Board &b) { b.pmicDenied = true; }, kBtBtEnWrite, -kStepBtEnHigh, true},
    {"a rail vote refused", [](Board &b) { b.rails.refuse = true; }, kBtRails, -kStepRails, false},
    {"a branch never runs", [](Board &b) { b.stuckBranch = true; }, kBtBranch, -kStepClocks, true},
    {"SE6 without UART firmware", [](Board &b) { b.fwRevision = 0xff00; }, kBtProtocol, -kStepSeCheck, true},
    {"the RCG never updates", [](Board &b) { b.rcgStuck = true; }, kBtRcg, -kStepSource, true},
    {"the loopback corrupts", [](Board &b) { b.loopCorrupt = true; }, kBtLoopback, -kStepLoopback, true},
    {"FIFO interface off", [](Board &b) { b.se[kIfDisable] = kFifoIfDisable; }, kBtFifo, -kStepSeCheck, true},
  };
  for (const Case &c : cases) {
    Board b;
    c.setup(b);
    const Hw hw = hwFor(b);
    const int rc = bringUp(gB, hw, options(4), fwFor(b), gNvm, sizeof gNvm);
    const int step = gB.step;
    if (rc != c.error || step != c.step) std::printf("  case '%s': rc %d step %d\n", c.what, rc, step);
    CHECK(rc == c.error && step == c.step);
    CHECK(!b.pmicWrites.empty() == c.pmicTouched);
    CHECK(b.unclocked == 0 && b.toChip == 0 && b.pmicBadRegs == 0);
    checkSafe(b);
    if (!b.rails.refuse) CHECK(!b.rails.allOn());   // the helper's enables withdrawn (nobody else on the rails)
  }
  {   // the controller's RTS never asserts: the first command cannot leave (L4, step 400)
    Board b;
    b.rtsStuck = true;
    const int rc = bringUp(gB, hwFor(b), options(4), fwFor(b), gNvm, sizeof gNvm);
    CHECK(rc == kBtSetup && gB.setup.step == -kStepReset0 && gB.setup.error == kSetupTransport && gB.port.txTimeouts == 1);
    checkSafe(b);
  }
  {   // deaf at the new speed
    Board b;
    b.chip.deafAfterBaud = true;
    CHECK(bringUp(gB, hwFor(b), options(4), fwFor(b), gNvm, sizeof gNvm) == kBtSetup && gB.setup.error == kSetupBaud);
    CHECK(gB.setup.step == -kStepBaud);
    checkSafe(b);
  }
  {   // no rampatch on the partition
    Board b;
    b.chip.files.erase("gngbtfw20.mbn");
    CHECK(bringUp(gB, hwFor(b), options(4), fwFor(b), gNvm, sizeof gNvm) == kBtSetup && gB.setup.error == kSetupNoFile);
    checkSafe(b);
  }
  {   // a system sleep pending in the middle of the download: out at once, then safe
    Board b;
    b.cancelAtToChip = 20000;
    const Hw hw = hwFor(b);
    CHECK(bringUp(gB, hw, options(4), fwFor(b), gNvm, sizeof gNvm) == kBtSetup && gB.setup.step == -kStepRampatch);
    CHECK(b.toChip < 20000 + 300 && b.chip.segments[Sim::kPatch] < 957);
    checkSafe(b);
  }
  {   // no private address: the NVM's default stays and is said to be the default
    Board b;
    Options o = options(4);
    o.setup.address = nullptr;
    CHECK(bringUp(gB, hwFor(b), o, fwFor(b), gNvm, sizeof gNvm) == kBtOk && gB.setup.addressIsNvmDefault && !gB.setup.addressSet);
  }
  {   // votes another driver holds stay when this one withdraws its own
    Board b;
    b.gcc[kGccQup2Vote] = kVoteCore | kVoteSAhb | (1u << 17);
    b.stuckBranch = false;
    b.fwRevision = 0xff00;
    CHECK(bringUp(gB, hwFor(b), options(4), fwFor(b), gNvm, sizeof gNvm) == kBtProtocol);
    CHECK(b.g(kGccQup2Vote) == (kVoteCore | kVoteSAhb | (1u << 17)));
  }
}

// The persistent H4 owner can be backpressured after a partial tty write. A
// full capture must leave the UART FIFO untouched; space later receives every
// byte, including an ACL header/payload that the setup event queue would drop.
static void testRawBackpressure()
{
  Board b;
  const Hw hw = hwFor(b);
  CHECK(bringUp(gB, hw, options(3), fwFor(b), gNvm, sizeof gNvm) == kBtOk);
  uint8_t capture[64] = {};
  gB.port.raw = capture;
  gB.port.rawCap = sizeof capture;
  gB.port.rawHave = sizeof capture - 4;
  const uint8_t acl[] = {2, 1, 0x20, 3, 0, 0xaa, 0xbb, 0xcc};
  for (uint8_t byte : acl) b.rxFifo.push_back(byte);
  const uint64_t received = gB.port.rxBytes;
  CHECK(portPoll(gB.port) == 0 && b.rxFifo.size() == sizeof acl);
  CHECK(gB.port.rawHave == sizeof capture - 4 && gB.port.backpressurePolls == 1);
  gB.port.rawHave = sizeof capture - sizeof acl;
  CHECK(portPoll(gB.port) == sizeof acl && b.rxFifo.empty());
  CHECK(gB.port.rawHave == sizeof capture && !memcmp(capture + sizeof capture - sizeof acl, acl, sizeof acl));
  CHECK(gB.port.rxBytes == received + sizeof acl && gB.port.droppedPackets == 0);
  // Include a last partial word in the reservation calculation.
  b.rxFifo.push_back(0x04); b.rxFifo.push_back(0x10); b.rxFifo.push_back(0x01);
  CHECK(portPoll(gB.port) == 0 && b.rxFifo.size() == 3);
  gB.port.rawHave = sizeof capture - 3;
  CHECK(portPoll(gB.port) == 3 && b.rxFifo.empty());
  CHECK(capture[61] == 4 && capture[62] == 0x10 && capture[63] == 1);
  gB.port.raw = nullptr;
  gB.port.rawHave = 0;
  safeOff(gB, hw, options(3));
}

static void testAddress()
{
  const uint8_t seed[8] = {1, 2, 3, 4, 5, 6, 7, 8}, zeros[8] = {};
  uint8_t first[6], second[6];
  CHECK(deriveAddress(seed, sizeof seed, first) && deriveAddress(seed, sizeof seed, second));
  CHECK(!memcmp(first, second, 6) && addressValid(first) && (first[0] & 3) == 2);
  CHECK(!deriveAddress(zeros, sizeof zeros, second) && !deriveAddress(seed, 7, second));
  CHECK(!deriveAddress(nullptr, sizeof seed, second) && !addressValid(zeros));
  first[0] |= 1;
  CHECK(!addressValid(first));
}

// The RSC transaction against a TCS model: an idle TCS taken (2 first), the commands, the trigger, completion, the TCS back to idle;
// a TCS that never completes is left and skipped next time.
struct RscModel {
  std::map<uint32_t, uint32_t> regs;
  RscLayout l = pianokeyboard::rscLayout(0x00030000);
  bool complete = true;
  unsigned triggers = 0, sleeps = 0, delays = 0, delayedUs = 0;
  uint32_t lastAddr = 0, lastData = 0, lastWait = 0;
};
static void testRsc()
{
  RscModel m;
  for (unsigned t = 0; t < 3; ++t) m.regs[m.l.tcsStride * t + m.l.status] = 1;   // idle
  RscIo io;
  io.ctx = &m;
  io.rd = [](void *c, uint32_t off) {
    RscModel *x = static_cast<RscModel *>(c);
    auto it = x->regs.find(off);
    return it == x->regs.end() ? 0u : it->second;
  };
  io.wr = [](void *c, uint32_t off, uint32_t v) {
    RscModel *x = static_cast<RscModel *>(c);
    x->regs[off] = v;
    for (unsigned t = 0; t < 3; ++t) {
      const uint32_t at = x->l.tcsStride * t;
      if (off == at + x->l.control && v == (kAmcEnable | kAmcTrigger)) {
        ++x->triggers;
        x->lastAddr = x->regs[at + x->l.cmdAddr];
        x->lastData = x->regs[at + x->l.cmdData];
        x->lastWait = x->regs[at + x->l.waitForCmpl];
        if (x->complete) x->regs[x->l.irqStatus] |= 1u << t;
      }
      if (off == x->l.irqClear) x->regs[x->l.irqStatus] &= ~v;
    }
  };
  io.sleepMs = [](void *c, unsigned) { ++static_cast<RscModel *>(c)->sleeps; };
  Rsc rsc;
  rsc.layout = m.l;
  const PianoWcnVrmCmd cmds[3] = {{PianoWcnVrmMode, 7}, {PianoWcnVrmVoltage, 1800}, {PianoWcnVrmEnable, 1}};
  VrmResult res;
  CHECK(vrmSend(rsc, io, 0x30100, cmds, 3, res) == 0 && res.tcs == 2 && res.done && m.triggers == 1);
  const uint32_t at = m.l.tcsStride * 2;
  CHECK(m.regs[at + m.l.cmdAddr] == 0x30108 && m.regs[at + m.l.cmdStride * 2 + m.l.cmdAddr] == 0x30104);
  CHECK(m.regs[at + m.l.cmdStride + m.l.cmdData] == 1800 && m.lastWait == 7 && m.regs[at + m.l.control] == 0);
  // the reclaim leaves CMD_ENABLE, WAIT_FOR_CMPL and CONTROL at 0: what PianoA830's idle test reads (P17)
  CHECK(m.regs[at + m.l.cmdEnable] == 0 && m.regs[at + m.l.waitForCmpl] == 0 && !(m.regs[m.l.irqStatus] & 4u));
  m.complete = false;
  CHECK(vrmSend(rsc, io, 0x30100, cmds, 1, res) == -60 && res.tcs == 2 && rsc.stuck[2] && res.waited == kTcsPollMs);
  CHECK(m.sleeps == kTcsPollMs && res.waitedUs == kTcsPollMs * 1000);   // no delayUs: the 1 ms fallback
  {   // with delayUs (what every owner gives): a busy poll of 10 us steps, never a sleep under the DRV-2 lock
    RscModel b;
    for (unsigned t = 0; t < 3; ++t) b.regs[b.l.tcsStride * t + b.l.status] = 1;
    RscIo bio = io;
    bio.ctx = &b;
    bio.sleepMs = [](void *c, unsigned) { ++static_cast<RscModel *>(c)->sleeps; };
    bio.delayUs = [](void *c, unsigned us) {
      RscModel *x = static_cast<RscModel *>(c);
      ++x->delays;
      x->delayedUs += us;
    };
    Rsc r2;
    r2.layout = b.l;
    b.complete = false;
    CHECK(vrmSend(r2, bio, 0x30100, cmds, 1, res) == -60 && r2.stuck[2] && b.sleeps == 0);
    CHECK(b.delays == kTcsPollMs * 1000 / kTcsPollUs && b.delayedUs == kTcsPollMs * 1000 && res.waited == kTcsPollMs);
    b.complete = true;
    CHECK(vrmSend(r2, bio, 0x30100, cmds, 1, res) == 0 && res.tcs == 1 && b.sleeps == 0 && res.waitedUs == 0);
    RscIo none = bio;
    none.sleepMs = nullptr;
    none.delayUs = nullptr;
    CHECK(vrmSend(r2, none, 0x30100, cmds, 1, res) == -22);
  }
  m.complete = true;
  CHECK(vrmSend(rsc, io, 0x30100, cmds, 1, res) == 0 && res.tcs == 1);   // the stuck one is skipped
  m.regs[m.l.tcsStride * 1 + m.l.status] = 0;
  m.regs[m.l.tcsStride * 0 + m.l.status] = 0;
  CHECK(vrmSend(rsc, io, 0x30100, cmds, 1, res) == -16);
  CHECK(vrmSend(rsc, io, 0, cmds, 1, res) == -22 && vrmSend(rsc, io, 0x30100, cmds, 4, res) == -22);
}

int main()
{
  testSuccess(false);
  testSuccess(true);
  testLevels();
  testWifiFirst();
  testFaults();
  testRsc();
  testRawBackpressure();
  testAddress();
  if (failures) {
    std::printf("bt_bringup_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("bt_bringup_host: all tests passed\n");
  return 0;
}
