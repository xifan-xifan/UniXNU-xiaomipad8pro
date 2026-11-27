// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register, FIFO and power models shared by the PianoBluetooth bring-up and UART host tests.
#pragma once
#include "BtBringup.h"
#include "PianoWcnRails.h"
#include "QcaSim.h"
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pianobt::sim {

// ------------------------------------------------------------------------------------------------------------ the rails
struct Rails {
  std::map<std::string, bool> enabled;
  std::map<std::string, uint16_t> mv;
  std::map<uint16_t, std::string> byPpid;
  bool refuse = false;
  unsigned txs = 0;
  Rails()
  {
    for (const PianoWcnRail &t : piano_wcn_rail_table) {
      enabled[t.cmd_db] = !t.order;   // the PHY LDOs on, the six PMU rails off
      mv[t.cmd_db] = t.min_mv;
      byPpid[uint16_t(t.sid << 8 | (t.smps ? piano_wcn_smps_page(t.smps) : piano_wcn_ldo_page(t.ldo)))] = t.cmd_db;
    }
  }
  bool allOn() const
  {
    for (const PianoWcnRail &t : piano_wcn_rail_table)
      if (t.order && !enabled.at(t.cmd_db)) return false;
    return true;
  }
};

// ------------------------------------------------------------------------------------------------------------ the board
struct Board {
  uint64_t us = 0;
  // GCC
  std::map<uint32_t, uint32_t> gcc;
  bool stuckBranch = false, rcgStuck = false, rcgPending = false;
  unsigned rcgWrites = 0;
  // TLMM
  std::map<uint32_t, uint32_t> tlmm;
  // QUP2 / SE6
  std::map<uint32_t, uint32_t> se;
  uint32_t fwRevision = 0x0204, qupVersion = 0x30090000, hwParam = 0x20100000;   // 16 words of 32 bits each way
  bool mActive = false, sActive = false;
  uint32_t txRemaining = 0, mIrq = 0, sIrq = 0;
  std::deque<uint32_t> txFifo;
  std::deque<uint8_t> rxFifo, chipTx;
  static constexpr size_t kRxBytes = 64;
  // the controller and the wire
  Sim chip;
  bool powered = false, rtsStuck = false, loopCorrupt = false;
  uint64_t poweredAt = 0;
  unsigned toChip = 0, garbage = 0, lostBeforeReady = 0, lostEvents = 0, unclocked = 0, loopBytes = 0;
  // BT_EN
  uint8_t bten[0x50] = {};
  BtEnInfo btenInfo;
  bool pmicDenied = false;
  std::vector<std::pair<uint8_t, uint8_t>> pmicWrites;
  unsigned pmicBadRegs = 0;
  // the rails and the two radios
  Rails rails;
  volatile uint32_t railState = 0, pmuOwner = 0;
  PianoWcnRailsOps railOps{};
  PianoWcnRails bt{}, wifi{};
  // the rest
  uint64_t cancelAtToChip = 0;
  std::vector<int> steps;
  std::set<std::string> kinds;

  Board()
  {
    stock(chip);
    // BT_EN at hand-over: the stock state, low (level 1's expectation).
    bten[0x04] = kPgTypeGpio;
    bten[0x05] = 0x11;
    bten[0x40] = kPgModeOutput;
    bten[0x41] = kPgVin1;
    bten[0x42] = kPgPullNone;
    bten[0x45] = 0x01;
    bten[0x46] = kPgEnabled;
    btenInfo = {77, 0, kPgTypeGpio, 0x11};
    // The RCG as the boot chain might leave it: 19.2 MHz from TCXO, DFS off.
    gcc[kGccS6Rcg + kRcgCfg] = 1;
    railOps.context = this;
    railOps.vrm = [](void *c, const char *res, const PianoWcnVrmCmd *cmds, unsigned n) {
      Board *b = static_cast<Board *>(c);
      ++b->rails.txs;
      if (b->rails.refuse || !b->rails.enabled.count(res)) return -5;
      for (unsigned i = 0; i < n; ++i) {
        if (cmds[i].reg == PianoWcnVrmEnable) b->rails.enabled[res] = cmds[i].value != 0;
        if (cmds[i].reg == PianoWcnVrmVoltage) b->rails.mv[res] = uint16_t(cmds[i].value);
      }
      b->tick();
      return 0;
    };
    railOps.pmic = [](void *c, uint16_t ppid, uint8_t *st, uint8_t *en, uint8_t *mode, uint16_t *mv) {
      Board *b = static_cast<Board *>(c);
      auto it = b->rails.byPpid.find(ppid);
      if (it == b->rails.byPpid.end()) return -5;
      *st = *en = b->rails.enabled[it->second] ? 0x80 : 0;
      *mode = 7;
      *mv = b->rails.mv[it->second];
      return 0;
    };
    railOps.pmic_type = [](void *c, uint16_t ppid, uint8_t *type) {
      Board *b = static_cast<Board *>(c);
      auto it = b->rails.byPpid.find(ppid);
      if (it == b->rails.byPpid.end()) return -19;
      *type = it->second.rfind("ldo", 0) == 0 ? 0x04 : 0x21;
      return 0;
    };
    railOps.sleep_ms = [](void *c, uint32_t ms) { static_cast<Board *>(c)->us += uint64_t(ms) * 1000; };
    railOps.mutex_lock = [](void *) {};
    railOps.mutex_unlock = [](void *) {};
    for (PianoWcnRails *r : {&bt, &wifi}) {
      r->state = &railState;
      r->lock = &pmuOwner;
      r->ops = &railOps;
    }
    bt.tag = PianoWcnTagBt;
    wifi.tag = PianoWcnTagWifi;
  }

  // ---- derived state
  bool voted(uint32_t bit) const { auto it = gcc.find(kGccQup2Vote); return it != gcc.end() && (it->second & bit); }
  bool clocked() const
  {
    for (const Branch &b : kBranches)
      if (!voted(b.vote) || stuckBranch) return false;
    return true;
  }
  bool pinIsEngine(unsigned gpio) const
  {
    auto it = tlmm.find(gpio * kTlmmStride);
    return it != tlmm.end() && ((it->second >> 2) & 0xfu) == 1;
  }
  bool loopback() const { auto it = se.find(kUartLoopback); return it != se.end() && (it->second & kLoopRxTx); }
  uint32_t reg(uint32_t off) const { auto it = se.find(off); return it == se.end() ? 0 : it->second; }
  uint32_t g(uint32_t off) const { auto it = gcc.find(off); return it == gcc.end() ? 0 : it->second; }
  uint32_t sourceHz() const
  {
    const uint32_t r = kGccS6Rcg;
    if (g(r + kRcgDfsCmd) & 1) {
      const unsigned l = reg(kClkSel) & 7u;
      return rcgRate(g(r + kRcgDfsPerf + 4 * l), g(r + kRcgDfsM + 4 * l), g(r + kRcgDfsN + 4 * l));
    }
    return rcgRate(g(r + kRcgCfg), g(r + kRcgM), g(r + kRcgN));
  }
  uint32_t hostBaud() const
  {
    const SerClk k = decodeSerClk(reg(kSerMClkCfg));
    return k.enabled && k.divider ? sourceHz() / k.divider / 16 : 0;
  }
  bool chipReady() const { return powered && us >= poweredAt + 50000; }
  bool rfrReady() const
  {
    const uint32_t m = reg(kUartManualRfr);
    if (m & kRfrManual) return !(m & kRfrNotReady);
    return rxFifo.size() + 8 <= kRxBytes;
  }
  bool ctsOk() const
  {
    if (reg(kUartTxTransCfg) & kUartCtsMask) return true;
    if (loopback()) return rfrReady();
    return chipReady() && !rtsStuck && pinIsEngine(24);
  }

  // ---- the controller's power: rails up and BT_EN an enabled output driven high
  void power()
  {
    const bool on = rails.allOn() && (bten[0x46] & kPgEnabled) && (bten[0x40] & 3u) == kPgModeOutput && (bten[0x44] & kPgOutLevel);
    if (on && !powered) poweredAt = us;
    if (!on && powered) {   // off: the controller forgets everything
      Sim fresh;
      fresh.files = chip.files;
      fresh.expect[0] = chip.expect[0];
      fresh.expect[1] = chip.expect[1];
      std::memcpy(fresh.address, chip.nvmDefault, 6);
      std::memcpy(fresh.nvmDefault, chip.nvmDefault, 6);
      fresh.deafAfterBaud = chip.deafAfterBaud;
      chip = fresh;
      chipTx.clear();
    }
    powered = on;
  }
  // ---- the line moves on every access
  void wireOut(uint8_t b)
  {
    if (loopback()) {
      ++loopBytes;
      rxFifo.push_back(loopCorrupt && loopBytes % 7 == 0 ? uint8_t(b ^ 0x10) : b);
      return;
    }
    if (!pinIsEngine(26)) return;   // the pad is not the engine's: nothing reaches the controller
    if (!chipReady()) {
      ++lostBeforeReady;
      return;
    }
    if (hostBaud() != chip.chipBaud) {
      ++garbage;
      return;
    }
    chip.flow = !(reg(kUartTxTransCfg) & kUartCtsMask);
    ++toChip;
    chip.rxByte(b);
  }
  void tick()
  {
    power();
    while (mActive && !txFifo.empty() && ctsOk()) {
      const uint32_t w = txFifo.front();
      txFifo.pop_front();
      const unsigned n = txRemaining < 4 ? txRemaining : 4;
      for (unsigned i = 0; i < n; ++i) wireOut(uint8_t(w >> (8 * i)));
      txRemaining -= n;
      if (!txRemaining) {
        mActive = false;
        mIrq |= kMCmdDone;
      }
    }
    if (!sActive || !pinIsEngine(27) || loopback()) return;
    for (;;) {
      if (chipTx.empty()) {
        if (chip.events.empty()) break;
        auto e = chip.events.front();
        chip.events.pop_front();
        if (e.first != hostBaud()) {
          ++lostEvents;
          continue;
        }
        chipTx.insert(chipTx.end(), e.second.begin(), e.second.end());
        chipTx.push_front(kH4Event);
      }
      if (!rfrReady()) break;
      rxFifo.push_back(chipTx.front());
      chipTx.pop_front();
    }
  }

  // ---- the windows
  uint32_t read(unsigned win, uint32_t off)
  {
    tick();
    if (win == kWinGcc) {
      for (const Branch &b : kBranches)
        if (off == b.cbcr) return clocked() || (voted(b.vote) && !stuckBranch) ? (b.hwcg ? kCbcrClkOff | kCbcrHwCtl : 0u) : kCbcrClkOff;
      if (off == kGccS6Rcg) {
        const uint32_t v = g(off) | (rcgPending ? kRcgCmdUpdate : 0u);
        if (!rcgStuck) rcgPending = false;
        return v;
      }
      return g(off);
    }
    if (win == kWinTlmm) {
      if (off == kPins[kPinSwCtrl].gpio * kTlmmStride + 4) return powered ? 1u : 0u;
      auto it = tlmm.find(off);
      return it == tlmm.end() ? 0 : it->second;
    }
    if (!clocked()) {
      ++unclocked;
      return 0xffffffffu;
    }
    if (win == kWinQup) return off == kQupHwVersion ? qupVersion : 0;
    switch (off) {
    case kFwRevision: return fwRevision;
    case kHwParam0:
    case kHwParam1: return hwParam;
    case kGeniStatus: return (mActive ? kMCmdActive : 0u) | (sActive ? kSCmdActive : 0u);
    case kMIrqStatus: return mIrq;
    case kSIrqStatus: return sIrq | (rxFifo.empty() ? 0u : kSRxFifoLast);
    case kTxFifoStatus: return uint32_t(txFifo.size());
    case kRxFifoStatus: {
      const size_t n = rxFifo.size();
      return uint32_t((n + 3) / 4) | (n % 4 ? kRxLast | uint32_t(n % 4) << kRxLastValidShift : 0u);
    }
    case kRxFifo: {
      uint32_t w = 0;
      for (unsigned i = 0; i < 4 && !rxFifo.empty(); ++i) {
        w |= uint32_t(rxFifo.front()) << (8 * i);
        rxFifo.pop_front();
      }
      return w;
    }
    case kGeniIos: return (powered && pinIsEngine(27) ? kIosRx : 0u) | (ctsOk() ? 0u : kIosCts);
    case kMCmdCtrl:
    case kSCmdCtrl: return 0;
    default: return reg(off);
    }
  }
  void write(unsigned win, uint32_t off, uint32_t v, const char *what)
  {
    kinds.insert(what);
    if (win == kWinGcc) {
      if (off == kGccS6Rcg) {
        if (v & kRcgCmdUpdate) rcgPending = true;
        gcc[off] = v & ~kRcgCmdUpdate;
      } else {
        if (off >= kGccS6Rcg && off < kGccS6Rcg + 0x14) ++rcgWrites;
        gcc[off] = v;
      }
      tick();
      return;
    }
    if (win == kWinTlmm) {
      tlmm[off] = v;
      tick();
      return;
    }
    if (!clocked()) {
      ++unclocked;
      return;
    }
    if (win == kWinQup) return;
    switch (off) {
    case kMCmd0:
      if ((v >> kOpcodeShift) == kUartStartTx) {
        mActive = true;
        txRemaining = reg(kUartTxTransLen);
      }
      break;
    case kTxFifo:
      if (mActive) txFifo.push_back(v);
      break;
    case kSCmd0:
      if ((v >> kOpcodeShift) == kUartStartRead) sActive = true;
      break;
    case kMCmdCtrl:
      if (v & (kCmdCancel | kCmdAbort)) {
        mActive = false;
        txFifo.clear();
        mIrq |= (v & kCmdCancel) ? kIrqCmdCancel : kIrqCmdAbort;
      }
      break;
    case kSCmdCtrl:
      if (v & (kCmdCancel | kCmdAbort)) {
        sActive = false;
        sIrq |= (v & kCmdCancel) ? kIrqCmdCancel : kIrqCmdAbort;
      }
      break;
    case kMIrqClear: mIrq &= ~v; break;
    case kSIrqClear: sIrq &= ~v; break;
    default: se[off] = v; break;
    }
    tick();
  }
};

// ------------------------------------------------------------------------------------------------------------ the Hw
inline Hw hwFor(Board &b)
{
  Hw hw;
  hw.ctx = &b;
  hw.read = [](void *c, unsigned w, uint32_t off) { return static_cast<Board *>(c)->read(w, off); };
  hw.write = [](void *c, unsigned w, uint32_t off, uint32_t v, const char *what) { static_cast<Board *>(c)->write(w, off, v, what); };
  hw.udelay = [](void *c, unsigned us) {
    static_cast<Board *>(c)->us += us;
    static_cast<Board *>(c)->tick();
  };
  hw.msleep = [](void *c, unsigned ms) {
    static_cast<Board *>(c)->us += uint64_t(ms) * 1000;
    static_cast<Board *>(c)->tick();
  };
  hw.nowUs = [](void *c) { return static_cast<Board *>(c)->us; };
  hw.voteLock = [](void *, int) { return 0; };
  hw.pmuLock = [](void *c, int take) { return piano_wcn_pmu_lock(&static_cast<Board *>(c)->bt, take); };
  hw.railsGet = [](void *c) { return piano_wcn_rails_get(&static_cast<Board *>(c)->bt, PIANO_WCN_BT); };
  hw.railsPut = [](void *c, bool release) {
    Board *b = static_cast<Board *>(c);
    const unsigned left = piano_wcn_rails_put(&b->bt, PIANO_WCN_BT | (release ? unsigned(PIANO_WCN_RELEASE) : 0u));
    b->tick();
    return left;
  };
  hw.btEnInfo = [](void *c, BtEnInfo *out) {
    *out = static_cast<Board *>(c)->btenInfo;
    return out->apid < 0 ? -19 : 0;
  };
  hw.pmicRead = [](void *c, uint8_t reg, uint8_t *v, unsigned n) {
    Board *b = static_cast<Board *>(c);
    if (unsigned(reg) + n > sizeof b->bten) return -5;
    std::memcpy(v, b->bten + reg, n);
    return 0;
  };
  hw.pmicWrite = [](void *c, uint8_t reg, uint8_t v) {
    Board *b = static_cast<Board *>(c);
    if (reg != kPgMode && reg != kPgVin && reg != kPgPull && reg != kPgOutSrc && reg != kPgEn) ++b->pmicBadRegs;
    b->pmicWrites.push_back({reg, v});
    if (b->pmicDenied || b->btenInfo.owner) return -13;
    b->bten[reg] = v;
    b->tick();
    return 0;
  };
  hw.cancelled = [](void *c) {
    Board *b = static_cast<Board *>(c);
    return b->cancelAtToChip && b->toChip >= b->cancelAtToChip;
  };
  hw.step = [](void *c, int step, const char *) { static_cast<Board *>(c)->steps.push_back(step); };
  return hw;
}

inline FwIo fwFor(Board &b)
{
  FwIo f;
  f.ctx = &b;
  f.size = [](void *c, const char *name, uint32_t *bytes) {
    auto &files = static_cast<Board *>(c)->chip.files;
    auto it = files.find(name);
    if (it == files.end()) return -4;
    *bytes = uint32_t(it->second.size());
    return 0;
  };
  f.read = [](void *c, const char *name, uint32_t off, uint8_t *to, uint32_t n) {
    auto &files = static_cast<Board *>(c)->chip.files;
    auto it = files.find(name);
    if (it == files.end() || size_t(off) + n > it->second.size()) return -5;
    std::memcpy(to, it->second.data() + off, n);
    return 0;
  };
  return f;
}

}  // namespace pianobt::sim
