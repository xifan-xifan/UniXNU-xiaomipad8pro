// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// charge/pd1: the Ubuntu port's buck fast-charge set in the strict session (piano-power-hv-order, -hv-caps, -qc, -fv-hot; the
// firmware source list piano-power-pdo-mca underneath). The message sequence driven end to end against synthetic chargers: binary
// GLINK/UCSI replies of this tablet's UCSI 1.1 PPM (bmOptionalFeatures 0), the charger firmware's decoded list (0x21011 triplets
// per MCA cycle), its answers to every planned write (taken, or refused), and the bus voltage it then reports. The model answers;
// it never assigns contract-valid bits. Every planned write must pass both allowlists (the policy's and the codec's), every raise
// must follow the reduced-current barrier, and the daily options (all off) must plan exactly what they planned before.
#include "ChargeContract.h"
#include "ChargeLink.h"
#include "GaugeAlerts.h"
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>
using namespace pianopower;

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)

static uint32_t fixedRdo(unsigned object, unsigned ma) {return object << 28 | ma / 10 << 10 | ma / 10;}
static uint32_t ppsRdo(unsigned object, unsigned mv, unsigned ma) {return object << 28 | (mv / 20) << 9 | ma / 50;}
static uint64_t key(uint32_t property, uint32_t value) {return uint64_t(property) << 32 | value;}

struct Options {unsigned hv = 1; bool order = false, caps = false, qc = false, fvHot = false;};

// One charger: its firmware list (what 0x21011 holds at SNK_Ready), whether it is a PD source, the firmware type it gets, and how
// it answers voltage requests (the bus it then shows; 0: it ignores the request).
struct Charger {
  uint32_t fw[7][3] = {};
  unsigned n = 0;
  bool pd = true;
  int32_t type = PIANO_CHG_T_PD;
  unsigned fixed9Object = 0, ppsObject = 0, fixed9RdoMa = 0;
  uint32_t nineVoltBus = 9000;   // the bus after a 9 V request (a broken source: 10.2 V, or 7.0 V)
};

static Charger xiaomi67() {   // an original-adapter-like list: PD 5/9/12/15/20 V and a 3.3-11 V APDO (6.1 A, read as 5 A)
  Charger c;
  const uint32_t l[6][3] = {{5000, 5000, 3000}, {9000, 9000, 3000}, {12000, 12000, 2250}, {15000, 15000, 1800},
                            {20000, 20000, 1350}, {3300, 11000, 5000}};
  for (unsigned i = 0; i < 6; ++i) for (unsigned k = 0; k < 3; ++k) c.fw[i][k] = l[i][k];
  c.n = 6; c.fixed9Object = 2; c.ppsObject = 6; c.fixed9RdoMa = 3000; c.type = PIANO_CHG_T_PPS;
  return c;
}
static Charger ppsBank() {    // a power bank with vSafe5V and one 5-11 V APDO only (no fixed 9 V)
  Charger c;
  const uint32_t l[2][3] = {{5000, 5000, 3000}, {5000, 11000, 3000}};
  for (unsigned i = 0; i < 2; ++i) for (unsigned k = 0; k < 3; ++k) c.fw[i][k] = l[i][k];
  c.n = 2; c.ppsObject = 2; c.type = PIANO_CHG_T_PPS;
  return c;
}
static Charger fixedOnly() {  // a plain PD 18 W charger: 5 V 3 A, 9 V 2 A
  Charger c;
  const uint32_t l[2][3] = {{5000, 5000, 3000}, {9000, 9000, 2000}};
  for (unsigned i = 0; i < 2; ++i) for (unsigned k = 0; k < 3; ++k) c.fw[i][k] = l[i][k];
  c.n = 2; c.fixed9Object = 2; c.fixed9RdoMa = 2000;
  return c;
}
static Charger qc(int32_t type) {Charger c; c.pd = false; c.type = type; return c;}

struct Bench {
  PianoChargeContract contract{};
  PianoMca m{};
  ChargeLink link;
  GaugeReading packs[2]{};
  piano_board_temps board{};
  GaugeAlerts alerts;
  Charger ch;
  Options opt;
  uint64_t now = 1000, control = 0;
  uint32_t rdo = fixedRdo(1, 3000), bus = 5000, icl = 100, fcc = 500, enable = 1, fv = 4400;
  bool connected = true, fwReady = true;
  unsigned raises = 0, barrierFails = 0;
  std::set<uint64_t> refuse;            // writes the firmware refuses (retcode -1)
  std::vector<PianoChargeWrite> writes;

  Bench(const Charger &c, const Options &o) : ch(c), opt(o) {
    piano_contract_init(&contract);
    piano_contract_allow_firmware_pdos(&contract, true);
    piano_contract_link(&contract, 1);
    piano_mca_init(&m, 1, o.hv != 0); m.links = 1; m.board_allowed = 1; m.board_done = 7;
    LinkOptions lo; lo.strictContract = true; lo.health = true; lo.hv = o.hv; lo.kick = false;
    lo.hvOrder = o.order; lo.hvCaps = o.caps; lo.qc = o.qc; lo.fvHot = o.fvHot;
    link.configure(m, lo);
    alerts.sampled(0x1e);
    board.version = PIANO_BOARD_TEMPS_VERSION; board.valid = 15; board.state = PIANO_BOARD_TEMPS_RUNNING;
    for (unsigned i = 0; i < 4; ++i) board.milli_c[i] = 30000;
    for (auto &g : packs) {g.ok = true; g.socPct = 40; g.tempDeciC = 300; g.voltageMv = 3900; g.currentMa = 300;}
  }
  void mcaCycle() {piano_contract_offer_pdos(&contract, ch.fw, fwReady && ch.pd ? ch.n : 0, fwReady && ch.pd);}
  void fill() {
    auto put = [&](int field, int32_t value) {m.valid |= 1u << field; m.value[field] = value;};
    put(McaBusVoltage, int32_t(bus * 1000)); put(McaBusCurrent, int32_t(icl * 1000));
    put(McaType, ch.type); put(McaIcl, int32_t(icl)); put(McaChargeEnable, int32_t(enable));
    put(McaTermVolt, int32_t(2 * fv - 3600)); put(McaVerify, 0); put(McaTbat, packs[0].tempDeciC / 10);
    put(McaPdState, fwReady && ch.pd ? 2 : 1); put(McaLpd, 0);
    LinkView v; v.nowMs = now; v.chargerPdUp = true; v.chargerPdUpMs = 1;
    v.contract = &contract; v.safetyFresh = true; v.cpOff = true; v.dtptValid = alerts.complete(); v.dtpt = alerts.active();
    v.packs = packs; v.packsFresh = true; v.packsMs = now; v.board = &board;
    ChargeLink::fill(link.in, m, v);
  }
  bool raise(const PianoChargeWrite &w) const {
    return (w.property == PIANO_CHG_PD_FIXED && w.value == 9000) || w.property == PIANO_CHG_PPS ||
           (w.property == PIANO_CHG_QC_VOLT && w.value == 9000);
  }
  void decide() {
    fill(); piano_charge_decide(&link.cfg, &link.in, &link.st, &link.plan);
    for (unsigned i = 0; i < link.plan.writes; ++i) {
      const auto w = link.plan.write[i];
      CHECK(piano_charge_write_allowed(&link.cfg, w.property, w.value, w.bytes));
      CHECK(piano_mca_write_allowed_for(&m, w.property, w.value, w.bytes));
      if (raise(w)) {
        // the barrier: reduced currents acknowledged and the input limit read back before any raise, from 5 V
        const bool ok = link.st.icl_set && link.st.icl_set <= 500 && link.st.fcc_set && link.st.fcc_set <= 500 &&
                        link.in.icl_ma >= 0 && link.in.icl_ma <= 500 && bus < 6000;
        CHECK(ok);
        if (!ok) {++barrierFails; if (getenv("FC_DEBUG")) std::printf("  raise %#x=%u icl_set=%u fcc_set=%u in.icl=%d bus=%u hv=%u\n", w.property, w.value, link.st.icl_set, link.st.fcc_set, link.in.icl_ma, bus, link.st.hv);}
        if (w.property != PIANO_CHG_QC_VOLT) CHECK(contract.valid && contract.mv == 5000);
        else CHECK(!contract.valid && !contract.pd && piano_charge_is_qc(ch.type));
        ++raises;
      }
      writes.push_back(w);
      if (refuse.count(key(w.property, w.value))) {piano_charge_ack(&link.st, w.property, w.value, -1); continue;}
      switch (w.property) {
      case PIANO_CHG_ICL: icl = w.value; break;
      case PIANO_CHG_FCC: fcc = w.value; break;
      case PIANO_CHG_FV: fv = w.value; break;
      case PIANO_CHG_ENABLE: enable = w.value; for (auto &g : packs) g.currentMa = enable ? 300 : 0; break;
      case PIANO_CHG_PD_FIXED:
        if (w.value == 9000 && ch.fixed9Object) {bus = ch.nineVoltBus; rdo = fixedRdo(ch.fixed9Object, ch.fixed9RdoMa);}
        else if (w.value == 5000) {bus = 5000; rdo = fixedRdo(1, 3000);}
        break;
      case PIANO_CHG_PPS:
        if (ch.ppsObject) {bus = (w.value >> 16) * 20 == 9000 ? ch.nineVoltBus : 5000;
                           rdo = ppsRdo(ch.ppsObject, (w.value >> 16) * 20, (w.value & 0xffff) * 50);}
        break;
      case PIANO_CHG_QC_VOLT: if (!ch.pd) bus = w.value == 9000 ? ch.nineVoltBus : 5000; break;
      default: CHECK(false);
      }
      piano_charge_ack(&link.st, w.property, w.value, 0);
    }
    // the ceilings: 5 V never above the stage A rows (no Rp here); 9 V the stage A rows, or with hv_caps the stock 9 V rows
    if (bus < 6000) CHECK(icl <= 1600 && fcc <= 1500);
    else if (!opt.caps) CHECK(icl <= 1600 && fcc <= 1500);
    else {CHECK(icl <= 2500 && fcc <= 3500 && fcc <= (2 * icl > 500 ? 2 * icl : 500u)); if (getenv("FC_DEBUG") && fcc > (2 * icl > 500 ? 2 * icl : 500u)) std::printf("  caps icl=%u fcc=%u bus=%u hv=%u plan icl=%u fcc=%u\n", icl, fcc, bus, link.st.hv, link.plan.icl_ma, link.plan.fcc_ma);}
    if (!opt.fvHot) CHECK(fv == 4400);
  }
  int wire() {
    uint8_t request[PIANO_CONTRACT_REQUEST_MAX];
    const auto n = piano_contract_next(&contract, now, request);
    if (!n) {now += 200; return 0;}
    const auto op = piano_contract_u32(request + 8);
    uint8_t response[544] = {};
    piano_contract_put32(response, PIANO_CONTRACT_OWNER); piano_contract_put32(response + 4, 1); piano_contract_put32(response + 8, op);
    uint32_t length = 16;
    if (op == PIANO_CONTRACT_WRITE) {
      control = piano_contract_u32(request + 20) | (uint64_t(piano_contract_u32(request + 24)) << 32);
    } else {
      length = 64; response[12] = 0x10; response[13] = 1;   // UCSI 1.1, bmOptionalFeatures 0
      uint32_t cci = 0; uint8_t *msg = response + 28;
      if (contract.stage != PianoContractProbe) {
        const unsigned command = control & 255;
        cci = command == 1 ? 1u << 27 : command == 4 ? 1u << 29 : 1u << 31;
        if (command == 6) {cci |= 16u << 8; piano_contract_put32(msg, 0x4046); msg[4] = 1; msg[5] = 0;}
        else if (command == 0x12) {
          cci |= 9u << 8;
          piano_contract_put32(msg, (connected ? 1u << 19 : 0) | (ch.pd && connected ? 3u : 1u) << 16);
          piano_contract_put32(msg + 4, ch.pd && connected ? rdo : 0);
        }
      }
      piano_contract_put32(response + 16, cci);
    }
    now += 20;
    return piano_contract_receive(&contract, now, response, length);
  }
  void observe() {
    const auto seq = contract.seq;
    piano_contract_refresh(&contract);
    for (unsigned i = 0; i < 200 && contract.seq == seq && !contract.quarantined; ++i)
      if (wire() < 0) break;
  }
  void tick() {now += 5000; mcaCycle(); observe(); decide();}
  void ticks(unsigned n) {for (unsigned i = 0; i < n; ++i) tick();}
  bool untilOn(unsigned n = 16) {for (unsigned i = 0; i < n && link.st.hv != PIANO_CHG_HV_ON; ++i) tick(); return link.st.hv == PIANO_CHG_HV_ON;}
  bool wrote(uint32_t property, uint32_t value, size_t after = 0) const {
    for (size_t i = after; i < writes.size(); ++i) if (writes[i].property == property && writes[i].value == value) return true;
    return false;
  }
  size_t count(uint32_t property, size_t after = 0) const {
    size_t k = 0;
    for (size_t i = after; i < writes.size(); ++i) if (writes[i].property == property) ++k;
    return k;
  }
  int path() const {return piano_charge_path(&link.st, &link.plan, &link.in);}
};

// The daily options (all new ones off): exactly the stage A behaviour of power/pd-hv1, and mode 0 never leaves 5 V.
static void dailyUnchanged() {
  {
    Options o; o.hv = 0;
    for (const Charger &c : {xiaomi67(), fixedOnly(), ppsBank(), qc(PIANO_CHG_T_HVDCP3)}) {
      Bench b(c, o); b.ticks(20);
      CHECK(!b.raises && b.bus == 5000 && !b.count(PIANO_CHG_FV) && !b.count(PIANO_CHG_QC_VOLT) && b.fcc <= 1500);
      CHECK(b.path() == (c.pd ? PIANO_CHG_PATH_PD_5V : PIANO_CHG_PATH_USB_5V));
    }
  }
  Options o;   // mode 1, nothing new
  Bench b(xiaomi67(), o);
  CHECK(b.untilOn() && b.bus == 9000 && b.raises == 1 && b.wrote(PIANO_CHG_PD_FIXED, 9000));
  b.ticks(2);
  CHECK(b.icl == 1600 && b.fcc == 1500 && b.path() == PIANO_CHG_PATH_PD_9V);
  // mode 1 on a PPS-only bank and mode 2 on a fixed-only charger: no 9 V object of the mode's own kind, no request (as before)
  Bench p(ppsBank(), o); p.ticks(16);
  CHECK(!p.raises && p.bus == 5000 && p.link.plan.fallback == PIANO_CHG_FALLBACK_SOURCE);
  Options two; two.hv = 2;
  Bench f(fixedOnly(), two); f.ticks(16);
  CHECK(!f.raises && f.bus == 5000);
  // a QC charger without piano-power-qc: no 0x2011 at all
  Bench q(qc(PIANO_CHG_T_HVDCP3), o); q.ticks(16);
  CHECK(!q.count(PIANO_CHG_QC_VOLT) && q.bus == 5000 && q.icl <= 1600 && q.fcc <= 1500);
}

// piano-power-hv-order: the other object when the mode's own is missing; the mode's own when both exist.
static void order() {
  Options o; o.order = true;
  Bench p(ppsBank(), o);
  CHECK(p.untilOn() && p.raises == 1 && p.wrote(PIANO_CHG_PPS, piano_charge_pps_word(9000, 3000)));
  CHECK(p.contract.protocol == PIANO_CONTRACT_PPS && p.contract.mv == 9000 && p.path() == PIANO_CHG_PATH_PPS_9V);
  Options two; two.hv = 2; two.order = true;
  Bench f(fixedOnly(), two);
  CHECK(f.untilOn() && f.wrote(PIANO_CHG_PD_FIXED, 9000) && !f.count(PIANO_CHG_PPS) && f.path() == PIANO_CHG_PATH_PD_9V);
  Bench x1(xiaomi67(), o);
  CHECK(x1.untilOn() && x1.wrote(PIANO_CHG_PD_FIXED, 9000) && !x1.count(PIANO_CHG_PPS));
  Bench x2(xiaomi67(), two);
  CHECK(x2.untilOn() && x2.wrote(PIANO_CHG_PPS, piano_charge_pps_word(9000, 3000)) && !x2.wrote(PIANO_CHG_PD_FIXED, 9000));
  // the offer changes under the session (the APDO goes): back to 5 V through vSafe5V, never to the other object at 9 V directly
  const auto start = p.writes.size();
  p.ch.n = 1; p.ticks(6);
  CHECK(p.wrote(PIANO_CHG_PD_FIXED, 5000, start) && p.bus == 5000 && p.icl <= 1600);
}

// piano-power-hv-caps: the stock rows at 9 V, within the RDO and the object; 5 V, transitions and holds keep stage A.
static void caps() {
  Options o; o.caps = true;
  Bench b(xiaomi67(), o);
  CHECK(b.untilOn()); b.ticks(2);
  // T-14 9 V: the RDO 3000 within the 9 V 3 A object, at most in_pps 2500 (a cool connector reads), the charge current chg_pps 3500
  CHECK(b.bus == 9000 && b.icl == 2500 && b.fcc == 3500 && b.link.plan.icl_why == PIANO_CHG_ICL_9V);
  // the object bounds the input: 9 V 2 A charger -> 2000 in, the charge current min(2 x 2000, 3500)
  Bench f(fixedOnly(), o);
  CHECK(f.untilOn()); f.ticks(2);
  CHECK(f.icl == 2000 && f.fcc == 3500);
  // without a connector reading, or with one at 50 C or more: the in_pd row 1600, chg_pd 3000 (y10's values)
  Bench n(xiaomi67(), o);
  n.board.valid &= ~((1u << PIANO_BOARD_TEMP_CONNECTOR) | (1u << PIANO_BOARD_TEMP_CONNECTOR2));
  CHECK(n.untilOn()); n.ticks(2);
  CHECK(n.bus == 9000 && n.icl == 1600 && n.fcc == 3000);
  Bench w(xiaomi67(), o);
  CHECK(w.untilOn()); w.ticks(2); CHECK(w.icl == 2500);
  w.board.milli_c[PIANO_BOARD_TEMP_CONNECTOR] = 51000; w.ticks(2);
  CHECK(w.bus == 9000 && w.icl == 1600 && w.fcc <= 3200);
  w.board.milli_c[PIANO_BOARD_TEMP_CONNECTOR] = 30000; w.ticks(2);
  CHECK(w.icl == 2500 && w.fcc == 3500);
  Options pps = o; pps.hv = 2;
  Bench p(xiaomi67(), pps);
  CHECK(p.untilOn()); p.ticks(2);
  CHECK(p.contract.protocol == PIANO_CONTRACT_PPS && p.icl == 2500 && p.fcc == 3500);   // in_pps 2500, chg_pps 3500
  Bench q(ppsBank(), pps); q.ch.fw[1][2] = 2000;   // a 2 A APDO: the request, the RDO and the input follow it
  CHECK(q.untilOn()); q.ticks(2);
  CHECK(q.wrote(PIANO_CHG_PPS, piano_charge_pps_word(9000, 2000)) && q.icl == 2000 && q.fcc == 3500);
  // the charger thermistor at 85 C: input 500 at most, the charge current at most twice that
  p.board.milli_c[PIANO_BOARD_TEMP_CHARGER] = 86000; p.ticks(2);
  CHECK(p.icl <= 500 && p.fcc <= 1000);
  p.board.milli_c[PIANO_BOARD_TEMP_CHARGER] = 30000; p.ticks(3);
  CHECK(p.icl == 2500 && p.fcc == 3500);
  // a pack at 46 C: 5 V again (the 45 C rule), the currents back to stage A
  const auto start = p.writes.size();
  for (auto &g : p.packs) g.tempDeciC = 460;
  p.ticks(6);
  CHECK(p.wrote(PIANO_CHG_PD_FIXED, 5000, start) && p.bus == 5000 && p.fcc <= 1500 && p.icl <= 1600);
  for (auto &g : p.packs) g.tempDeciC = 300;
  // the 80 % stop from 9 V with the caps: 5 V, charging off; 75 % resumes, 9 V again through the barrier
  Bench h(xiaomi67(), pps);
  CHECK(h.untilOn()); h.ticks(1);
  for (auto &g : h.packs) g.socPct = 80;
  h.ticks(6);
  CHECK(!h.enable && h.bus == 5000 && h.link.st.hold && h.fcc <= 1500);
  CHECK(h.link.cfg.limit && h.link.cfg.stop_pct == 80 && h.link.cfg.resume_pct == 75);
  for (auto &g : h.packs) g.socPct = 76; h.ticks(4);
  CHECK(!h.enable);   // the hysteresis: still held above 75 %
  for (auto &g : h.packs) g.socPct = 75;
  const auto raises = h.raises;
  CHECK(h.untilOn(20) && h.enable && h.raises == raises + 1);
  // the firmware refuses the raised charge current: never above stage A unless taken, the input stays as it is
  Bench r(xiaomi67(), pps); r.refuse.insert(key(PIANO_CHG_FCC, 3500));
  CHECK(r.untilOn()); r.ticks(6);
  CHECK(r.fcc <= 1500 && r.icl == 2500);
}

// piano-power-qc: QC2/QC3 through 0x2011 behind the barrier, 5 V on any loss.
static void qcPath() {
  Options o; o.qc = true;
  Bench b(qc(PIANO_CHG_T_HVDCP3), o);
  b.ticks(1);
  CHECK((b.link.in.valid & PIANO_CHG_IN_NONPD) && !(b.link.in.valid & PIANO_CHG_IN_CONTRACT));
  CHECK(b.untilOn() && b.raises == 1 && b.wrote(PIANO_CHG_QC_VOLT, 9000) && b.bus == 9000);
  b.ticks(2);
  CHECK(b.icl == 1600 && b.fcc == 1500 && b.path() == PIANO_CHG_PATH_QC_9V);
  Options oc = o; oc.caps = true;
  Bench c(qc(PIANO_CHG_T_HVDCP3_B), oc);
  CHECK(c.untilOn()); c.ticks(2);
  CHECK(c.icl == 1600 && c.fcc == 3200);   // QC3 3500, at most 2 x 1600
  Bench c2(qc(PIANO_CHG_T_HVDCP), oc);
  CHECK(c2.untilOn()); c2.ticks(2);
  CHECK(c2.icl == 1200 && c2.fcc == 2400);  // QC2 1200 / 2600, at most 2 x 1200
  // the source drops to 5 V by itself: 0x2011 = 5000, blocked until a detach
  auto start = b.writes.size();
  b.bus = 5000; b.ticks(4);
  CHECK(b.wrote(PIANO_CHG_QC_VOLT, 5000, start) && b.link.st.hv == PIANO_CHG_HV_BLOCKED && b.icl <= 1600 && b.fcc <= 1500);
  const auto raises = b.raises; b.ticks(10);
  CHECK(b.raises == raises);
  b.bus = 0; b.ticks(2); b.bus = 5000;   // detach and plug again
  CHECK(b.untilOn(20) && b.raises == raises + 1);
  // a bounded inhibit (the 80 % stop's vote): 5 V and charging off; lifted: asked again after the pause
  start = b.writes.size();
  CHECK(!piano_charge_config_transaction(&b.link.cfg, 1, 80, 75, 1, 1));
  b.ticks(4);
  CHECK(!b.enable && b.bus == 5000 && b.wrote(PIANO_CHG_QC_VOLT, 5000, start));
  CHECK(!piano_charge_config_transaction(&b.link.cfg, 1, 80, 75, 1, 0));
  CHECK(b.untilOn(20) && b.enable);
  // mode 0: 5 V and no request any more
  start = b.writes.size();
  CHECK(!piano_charge_config_transaction(&b.link.cfg, 1, 80, 75, 0, 0));
  b.ticks(10);
  CHECK(b.bus == 5000 && b.wrote(PIANO_CHG_QC_VOLT, 5000, start) && !b.wrote(PIANO_CHG_QC_VOLT, 9000, start));
  // the firmware refuses 9 V: the session waits at the barrier's currents, then 5 V and blocked
  Bench r(qc(PIANO_CHG_T_HVDCP3), o); r.refuse.insert(key(PIANO_CHG_QC_VOLT, 9000));
  r.ticks(16);
  CHECK(r.bus == 5000 && r.link.st.hv != PIANO_CHG_HV_ON && r.icl <= 1600 && r.fcc <= 1500);
  // a source that answers with 10.2 V: never taken as 9 V, back to 5 V
  Charger hi = qc(PIANO_CHG_T_HVDCP3); hi.nineVoltBus = 10200;
  Bench v(hi, o); v.ticks(14);
  CHECK(v.link.st.hv == PIANO_CHG_HV_BLOCKED && v.wrote(PIANO_CHG_QC_VOLT, 5000) && v.bus == 5000);
  // a hot pack: no request
  Bench t(qc(PIANO_CHG_T_HVDCP3), o);
  for (auto &g : t.packs) g.tempDeciC = 460;
  t.ticks(16);
  CHECK(!t.raises && t.bus == 5000);
  // a PD charger the firmware calls HVDCP3: the UCSI status says PD, so no QC request (the PD session decides)
  Charger pdqc = fixedOnly(); pdqc.type = PIANO_CHG_T_HVDCP3;
  Bench d(pdqc, o); d.ticks(16);
  CHECK(!d.count(PIANO_CHG_QC_VOLT));
}

// The other safety fallbacks of the PD session with the options on: a bus out of window after the request, a lost contract.
static void pdFallbacks() {
  Options o; o.order = o.caps = true;
  Charger hi = xiaomi67(); hi.nineVoltBus = 10200;
  Bench v(hi, o); v.ticks(14);
  CHECK(v.link.st.hv != PIANO_CHG_HV_ON && v.wrote(PIANO_CHG_PD_FIXED, 5000) && v.icl <= 1600 && v.fcc <= 1500);
  Charger lo = xiaomi67(); lo.nineVoltBus = 7000;
  Bench w(lo, o); w.ticks(14);
  CHECK(w.link.st.hv != PIANO_CHG_HV_ON && w.wrote(PIANO_CHG_PD_FIXED, 5000));
  Bench b(xiaomi67(), o);
  CHECK(b.untilOn()); b.ticks(1);
  const auto start = b.writes.size();
  b.fwReady = false; b.ticks(5);   // the firmware leaves SNK_Ready: the list is withdrawn, the contract goes
  CHECK(b.wrote(PIANO_CHG_PD_FIXED, 5000, start) && b.bus == 5000 && b.icl <= 500);
}

// piano-power-fv-hot: 4060 mV from 48 C, the warm hold at that float, 4400 again below 46 C; the 80/75 limit unchanged.
static void warmFloat() {
  Options o; o.hv = 0; o.fvHot = true;
  Bench b(fixedOnly(), o);
  b.ticks(4);
  CHECK(!b.count(PIANO_CHG_FV) && b.fv == 4400);
  for (auto &g : b.packs) {g.tempDeciC = 485; g.voltageMv = 4000;}
  b.ticks(3);
  CHECK(b.fv == 4060 && b.link.st.term_hot && !b.link.st.fv_hold && b.enable);
  for (auto &g : b.packs) g.voltageMv = 4080;   // above the warm float: held
  b.ticks(2);
  CHECK(b.link.st.fv_hold && !b.enable && b.link.plan.why_off == PIANO_CHG_OFF_WARM_FV);
  for (auto &g : b.packs) g.tempDeciC = 470;   // 47 C: still the warm row (hysteresis to 46)
  b.ticks(2);
  CHECK(b.fv == 4060 && !b.enable);
  for (auto &g : b.packs) g.tempDeciC = 455;
  b.ticks(3);
  CHECK(b.fv == 4400 && !b.link.st.term_hot && !b.link.st.fv_hold && b.enable);
  CHECK(b.link.cfg.limit && b.link.cfg.stop_pct == 80 && b.link.cfg.resume_pct == 75);
  // the 80 % stop wins over a warm release: still held at 80 %
  for (auto &g : b.packs) {g.socPct = 80; g.tempDeciC = 300;}
  b.ticks(3);
  CHECK(!b.enable && b.link.plan.why_off == PIANO_CHG_OFF_LIMIT);
  // without the option the float is never written, whatever the temperature
  Options n; n.hv = 0;
  Bench d(fixedOnly(), n);
  for (auto &g : d.packs) {g.tempDeciC = 500; g.voltageMv = 4200;}
  d.ticks(6);
  CHECK(!d.count(PIANO_CHG_FV) && d.fv == 4400);
  // with 9 V on and the pack warming through 46 to 48 C: 5 V first (45 C), then the warm float
  Options h; h.caps = h.fvHot = true;
  Bench w(xiaomi67(), h);
  CHECK(w.untilOn());
  for (auto &g : w.packs) g.tempDeciC = 490;
  w.ticks(6);
  CHECK(w.bus == 5000 && w.fv == 4060 && w.fcc <= 1500);
}

// The codec side: the fast rows and the warm float only with their options.
static void codec() {
  PianoMca m{};
  piano_mca_init(&m, 1, 1); piano_mca_use_external_policy(&m, 1, 1, 0);
  CHECK(!piano_mca_write_allowed_for(&m, PIANO_MCA_FCC, 3000, 4) && piano_mca_write_allowed_for(&m, PIANO_MCA_FCC, 1500, 4));
  CHECK(!piano_mca_write_allowed_for(&m, PIANO_MCA_TERM_VOLT, 4060, 4) && !piano_mca_write_allowed_for(&m, PIANO_MCA_TERM_VOLT, 4400, 4));
  CHECK(!piano_mca_write_allowed_for(&m, PIANO_MCA_QC_VOLT, 9000, 4));
  piano_mca_allow_fast(&m, 1, 1); piano_mca_use_external_policy(&m, 1, 1, 1);
  CHECK(piano_mca_write_allowed_for(&m, PIANO_MCA_FCC, 3500, 4) && !piano_mca_write_allowed_for(&m, PIANO_MCA_FCC, 3501, 4));
  CHECK(piano_mca_write_allowed_for(&m, PIANO_MCA_TERM_VOLT, 4060, 4) && piano_mca_write_allowed_for(&m, PIANO_MCA_TERM_VOLT, 4400, 4));
  CHECK(!piano_mca_write_allowed_for(&m, PIANO_MCA_TERM_VOLT, 4480, 4) && !piano_mca_write_allowed_for(&m, PIANO_MCA_TERM_VOLT, 4060, 1));
  CHECK(piano_mca_write_allowed_for(&m, PIANO_MCA_QC_VOLT, 9000, 4) && !piano_mca_write_allowed_for(&m, PIANO_MCA_QC_VOLT, 12000, 4));
  PianoMca off{};
  piano_mca_init(&off, 0, 1); piano_mca_use_external_policy(&off, 1, 1, 1); piano_mca_allow_fast(&off, 1, 1);
  CHECK(!off.fcc_fast && !off.fv_hot);   // reads only: nothing admitted
  // the policy's own allowlist follows the options
  PianoChargeConfig c; piano_charge_config_default(&c); c.strict_contract = 1; c.hv = 1;
  CHECK(!piano_charge_write_allowed(&c, PIANO_CHG_FCC, 1600, 4) && !piano_charge_write_allowed(&c, PIANO_CHG_ICL, 2500, 4));
  CHECK(!piano_charge_write_allowed(&c, PIANO_CHG_PPS, piano_charge_pps_word(9000, 3000), 4));
  CHECK(!piano_charge_write_allowed(&c, PIANO_CHG_QC_VOLT, 9000, 4) && !piano_charge_write_allowed(&c, PIANO_CHG_FV, 4060, 4));
  c.hv_caps = c.hv_order = c.qc = c.fv_hot = 1;
  CHECK(piano_charge_write_allowed(&c, PIANO_CHG_FCC, 3500, 4) && !piano_charge_write_allowed(&c, PIANO_CHG_FCC, 3600, 4));
  CHECK(piano_charge_write_allowed(&c, PIANO_CHG_ICL, 2500, 4) && !piano_charge_write_allowed(&c, PIANO_CHG_ICL, 2600, 4));
  CHECK(piano_charge_write_allowed(&c, PIANO_CHG_PPS, piano_charge_pps_word(9000, 3000), 4));
  CHECK(!piano_charge_write_allowed(&c, PIANO_CHG_PPS, piano_charge_pps_word(11000, 3000), 4));
  CHECK(piano_charge_write_allowed(&c, PIANO_CHG_QC_VOLT, 9000, 4) && piano_charge_write_allowed(&c, PIANO_CHG_FV, 4060, 4));
  CHECK(!piano_charge_write_allowed(&c, PIANO_CHG_FV, 4480, 4));
  CHECK(piano_charge_qc_icl_9v(PIANO_CHG_T_HVDCP) == 1200 && piano_charge_qc_icl_9v(PIANO_CHG_T_HVDCP3P5) == 1600 &&
        !piano_charge_qc_icl_9v(PIANO_CHG_T_PD) && piano_charge_qc_fcc_9v(PIANO_CHG_T_HVDCP) == 2600);
}

int main() {
  dailyUnchanged();
  order();
  caps();
  qcPath();
  pdFallbacks();
  warmFloat();
  codec();
  std::printf("fast charge (charge/pd1): %u checks / %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
