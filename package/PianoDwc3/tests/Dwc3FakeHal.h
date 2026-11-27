// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared host fixture for the DWC3 register, event-ring, and DMA interface.
#pragma once
#include "Dwc3Gadget.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace pianousb::test {
using namespace dwc3;

struct FakeHal {
  std::map<uint32_t, uint32_t> regs;
  std::vector<uint8_t> mem = std::vector<uint8_t>(Layout::kBytes);
  const uint64_t base = 0x8c1230000ull;
  struct Cmd { unsigned phys; uint32_t command, p0, p1, p2; };
  std::vector<Cmd> cmds;
  struct EpModel { bool active, stalled; uint64_t trb; };
  EpModel eps[32] = {};
  uint32_t evWrite = 0;
  std::string tx[kPorts], rx[kPorts];
  unsigned rxRoom = 1u << 20;
  uint16_t lines[kPorts] = {};
  bool quiet = true;

  FakeHal() { regs[GCTL] = 0x00102004; }   // the core's reset value on piano (C7, after USB30_PRIM_BCR): the device role
  uint32_t rd(uint32_t off) { return regs[off]; }
  void wr(uint32_t off, uint32_t v)
  {
    if (off >= 0xc800 && off < 0xca00 && (off & 0xf) == 0xc) { command((off - 0xc800) / 16, v); return; }
    if (off == GEVNTCOUNT) { assert(v <= regs[off]); regs[off] -= v; return; }
    if (off == DCTL) {
      if (v & DCTL_CSFTRST) {   // the core soft reset: every transfer, command and held SETUP gone
        ++softResets;
        stuck = owes = false;
        held.clear();
        for (EpModel &e : eps) e = EpModel{};
        regs[GEVNTCOUNT] = 0;
        evWrite = 0;
      }
      v &= ~DCTL_CSFTRST;
      if (v & DCTL_RUN_STOP) regs[DSTS] &= ~DSTS_DEVCTRLHLT; else regs[DSTS] |= DSTS_DEVCTRLHLT;
    }
    regs[off] = v;
  }
  void command(unsigned phys, uint32_t v)
  {
    const uint32_t b = depcmdBase(phys);
    Cmd c{phys, v & 0xf, regs[b + 8], regs[b + 4], regs[b + 0]};
    cmds.push_back(c);
    if (fsm && (stuck || (c.command == CMD_ENDTRANSFER && !held.empty()))) {   // not taken: CMDACT stays set
      stuck = true;
      ++stuckCmds;
      regs[b + 12] = v | DEPCMD_CMDACT;
      return;
    }
    uint32_t result = v & ~DEPCMD_CMDACT;
    switch (c.command) {
    case CMD_STARTTRANSFER:
      assert(!eps[phys].active);
      eps[phys].active = true;
      eps[phys].trb = uint64_t(c.p0) << 32 | c.p1;
      result = (result & 0xff80ffffu) | (phys + 1) << 16;
      break;
    case CMD_ENDTRANSFER: eps[phys].active = false; break;
    case CMD_SETSTALL:
      eps[phys].stalled = true;
      if (phys == 0) owes = false;   // EP0 stalled: the core's control state machine is back in its SETUP phase
      break;
    case CMD_CLEARSTALL: eps[phys].stalled = false; break;
    default: break;
    }
    regs[b + 12] = result;
    if (fsm && phys == 0 && (c.command == CMD_STARTTRANSFER || c.command == CMD_SETSTALL)) deliverHeld();
  }
  // The controller's own control-transfer state machine (USB-WEDGE.md), on with fsm: after a SETUP is handed out the core owes that
  // transfer's stages until its status TRB completes or EP0 is stalled; a SETUP from the host meanwhile is held, not put into an armed
  // SETUP TRB (chg-fix1, confirmed); END TRANSFER while a SETUP is held is not taken (the reference's rule, dwc3_stop_active_transfer)
  // and once a command is stuck every later one is (inferred: energy-e4 after the replug); a bus reset drops a held SETUP but not a
  // stuck command; DCTL.CSFTRST clears it all.
  bool fsm = false, owes = false, stuck = false;
  std::string held;
  unsigned softResets = 0, stuckCmds = 0;
  void deliverHeld()
  {
    if (held.empty() || owes || !eps[0].active || trbType(0) != TRBCTL_CONTROL_SETUP) return;
    const std::string s = held;
    held.clear();
    complete(0, s);
  }
  // The host sends a SETUP: into the armed SETUP TRB, or held by the core (it owes a stage, or no SETUP TRB is armed). True: handed out.
  bool hostSetup(const std::string &s)
  {
    if (!owes && eps[0].active && trbType(0) == TRBCTL_CONTROL_SETUP && held.empty()) { complete(0, s); return true; }
    held = s;
    return false;
  }
  void hostReset()
  {
    held.clear();
    owes = false;
    devEvent(DEVT_RESET);
  }
  uint64_t now = 0;
  void barrier() {}
  void udelay(unsigned us) { now += us; }
  uint64_t nowUs() { return now; }
  uint8_t *dma() { return mem.data(); }
  uint64_t dmaPhys() { return base; }
  unsigned txFill(unsigned port, uint8_t *dst, unsigned cap)
  {
    const unsigned n = tx[port].size() < cap ? unsigned(tx[port].size()) : cap;
    std::memcpy(dst, tx[port].data(), n);
    tx[port].erase(0, n);
    return n;
  }
  unsigned rxSpace(unsigned) { return rxRoom; }
  void rxData(unsigned port, const uint8_t *src, unsigned len) { rx[port].append(reinterpret_cast<const char *>(src), len); }
  void lineState(unsigned port, uint16_t state) { lines[port] = state; }
  // the network function's side
  std::deque<std::string> netTx;   // frames to the host
  std::vector<std::string> netRx;  // frames from the host
  unsigned netRoom = 1u << 20, netDone = 0, linkEvents = 0;
  bool linkUp = false;
  uint32_t linkBps = 0;
  unsigned netTxNext(uint8_t *dst, unsigned room)
  {
    if (netTx.empty() || netTx.front().size() > room) return 0;
    const unsigned n = unsigned(netTx.front().size());
    std::memcpy(dst, netTx.front().data(), n);
    netTx.pop_front();
    return n;
  }
  unsigned netRxSpace() { return netRoom; }
  bool netRxFrame(const uint8_t *src, unsigned len)
  {
    if (netRoom < len + 2) return false;
    netRx.emplace_back(reinterpret_cast<const char *>(src), len);
    return true;
  }
  void netRxDone() { ++netDone; }
  void netLink(bool up, uint32_t bps) { linkUp = up; linkBps = bps; ++linkEvents; }
  void note(const char *fmt, ...)
  {
    if (quiet) return;
    va_list ap;
    va_start(ap, fmt);
    std::printf("  note: ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
  }

  // device side of the model
  uint8_t *at(uint64_t pa) { assert(pa >= base && pa + 1 <= base + Layout::kBytes); return &mem[pa - base]; }
  uint32_t *trb(unsigned phys, unsigned n = 0) { return reinterpret_cast<uint32_t *>(at(eps[phys].trb + 16 * n)); }
  void event(uint32_t e)
  {
    std::memcpy(&mem[Layout::kEvents + evWrite], &e, 4);
    evWrite = (evWrite + 4) % Layout::kEventBytes;
    regs[GEVNTCOUNT] += 4;
  }
  void devEvent(uint32_t type, uint32_t info = 0) { event(1u | type << 8 | info << 16); }
  void epEvent(unsigned phys, uint32_t type, uint32_t status = 0) { event(phys << 1 | type << 6 | status << 12); }
  unsigned trbType(unsigned phys, unsigned n = 0) { return (trb(phys, n)[3] >> 4) & 0x3f; }
  // The host's side of one transfer: consumes the TRB (and a chained zero-length one), returns IN data or takes OUT data.
  std::string complete(unsigned phys, const std::string &out = std::string(), bool *zlp = nullptr)
  {
    assert(eps[phys].active);
    uint32_t *t = trb(phys);
    assert(t[3] & TRB_HWO);
    if (phys < 2) {
      const unsigned type = trbType(phys);
      if (type == TRBCTL_CONTROL_SETUP) owes = true;
      if (type == TRBCTL_CONTROL_STATUS2 || type == TRBCTL_CONTROL_STATUS3) owes = false;
    }
    const uint32_t len = trbResidual(t[2]);
    std::string data;
    if (phys & 1) data.assign(reinterpret_cast<const char *>(at(uint64_t(t[1]) << 32 | t[0])), len);
    else {
      assert(out.size() <= len);
      std::memcpy(at(uint64_t(t[1]) << 32 | t[0]), out.data(), out.size());
    }
    const bool chained = t[3] & TRB_CHN;
    if (zlp) *zlp = chained;
    if (chained) {
      uint32_t *z = trb(phys, 1);
      assert((z[3] & TRB_HWO) && (z[3] & TRB_LST) && trbResidual(z[2]) == 0);
      z[3] &= ~TRB_HWO;
    }
    t[2] = (phys & 1) ? 0 : uint32_t(len - out.size());
    t[3] &= ~TRB_HWO;
    eps[phys].active = false;
    epEvent(phys, EPT_XFER_COMPLETE);
    return data;
  }
  unsigned count(uint32_t command, unsigned phys) const
  {
    unsigned n = 0;
    for (const Cmd &c : cmds) n += c.command == command && c.phys == phys;
    return n;
  }
  const Cmd *last(uint32_t command, unsigned phys) const
  {
    for (auto i = cmds.rbegin(); i != cmds.rend(); ++i) if (i->command == command && i->phys == phys) return &*i;
    return nullptr;
  }
};

inline std::string setupBytes(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length)
{
  const char s[8] = {char(type), char(request), char(value), char(value >> 8), char(index), char(index >> 8), char(length), char(length >> 8)};
  return std::string(s, 8);
}

}  // namespace pianousb::test
