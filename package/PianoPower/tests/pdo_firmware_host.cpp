// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// piano-power-pdo-mca: this tablet's UCSI 1.1 PPM advertises no PDO details
// (bmOptionalFeatures 0, saved chg-buck4 and installed-v12 snapshots), so a PD
// sink status carries an RDO but no source list. Binary GLINK/UCSI replies with
// that capability, the charger firmware's decoded list (0x21011 triplets) offered
// per MCA cycle, ChargeLink and the production policy. The model answers queries;
// it never assigns contract-valid bits.
#include "ChargeContract.h"
#include "ChargeLink.h"
#include "GaugeAlerts.h"
#include "ChargePumpSession.h"
#include <cstdio>
#include <vector>
using namespace pianopower;

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)

// cp-first1's vendor-object RDOs (MIPPS-RDO.md): object 7 PPS-format 5000 mV / 3250 mA, object 5
static constexpr uint32_t kVendorRdo7 = 0x7301f441u, kVendorRdo5 = 0x53026e3cu;
static uint32_t fixedRdo(unsigned object, unsigned ma) {return object << 28 | ma / 10 << 10 | ma / 10;}
static uint32_t ppsRdo(unsigned object, unsigned ma = 3000) {return object << 28 | 450u << 9 | ma / 50;}

struct Bench {
  PianoChargeContract contract{};
  PianoMca m{};
  ChargeLink link;
  GaugeReading packs[2]{};
  piano_board_temps board{};
  GaugeAlerts alerts;
  uint64_t now = 1000, control = 0;
  uint32_t rdo = fixedRdo(1, 3000), bus = 5000, icl = 100, fcc = 500, enable = 1;
  // the firmware's decoded source list: 5 V 3 A, 9 V 2 A, PPS 3.3-11 V 3 A
  uint32_t fw[7][3] = {{5000, 5000, 3000}, {9000, 9000, 2000}, {3300, 11000, 3000}};
  unsigned fwN = 3;
  unsigned features = 0;          // the PPM's bmOptionalFeatures (16: GET_PDOS)
  bool pd = true, connected = true, fwReady = true;
  bool mcaPdos = false;           // ChargeLink also sees the cycle's 0x21011 list itself (position true), as on the tablet
  bool mcaOwn = false;            // ... a 0x21011 list other than the offered one (mca/mcaN)
  uint32_t mca[7][3] = {};
  unsigned mcaN = 0;
  bool mippsPending = false, cpFirst = false;   // charge/mipps2 ladder inputs (PianoPower's LinkView)
  int32_t type = PIANO_CHG_T_PD;  // the firmware's real type (0x2000c) of a PD source (cp-first2: 11, PD PPS, after MiPPS)
  int32_t verify = 0;             // 0x21002 (1 while MiPPS runs)
  std::vector<uint8_t> ladders;   // the ladder stages seen, in order (changes only)
  unsigned statusReads = 0, pdoQueries = 0, voltageRequests = 0;
  unsigned wrongObject = 0;       // the firmware confirms 9 V on this object instead of the one asked for
  bool wrongPps = false;          // ... as a PPS RDO
  int flipRdoAfterStatus = -1;    // change the RDO after this many status reads (Verify disagreement)
  std::vector<PianoChargeWrite> writes;

  explicit Bench(bool fallback, unsigned hv = 1, bool rp = false) {
    piano_contract_init(&contract);
    piano_contract_allow_firmware_pdos(&contract, fallback);
    piano_contract_link(&contract, 1);
    piano_mca_init(&m, 1, hv != 0); m.links = 1; m.board_allowed = 1; m.board_done = 7;
    LinkOptions o; o.strictContract = true; o.health = true; o.hv = hv; o.kick = false;
    o.rp = rp; o.ramp = rp; o.rpMax = rp ? 3000 : 0;
    link.configure(m, o);
    alerts.sampled(0x1e);
    board.version = PIANO_BOARD_TEMPS_VERSION; board.valid = 15; board.state = PIANO_BOARD_TEMPS_RUNNING;
    for (unsigned i = 0; i < 4; ++i) board.milli_c[i] = 30000;
    for (auto &g : packs) {g.ok = true; g.socPct = 40; g.tempDeciC = 300; g.voltageMv = 3900; g.currentMa = 300;}
  }
  // one finished MCA cycle: what PianoPower::offerFirmwarePdos hands over
  void mcaCycle() {piano_contract_offer_pdos(&contract, fw, fwReady && pd ? fwN : 0, fwReady && pd);}
  void fill() {
    auto put = [&](int field, int32_t value) {m.valid |= 1u << field; m.value[field] = value;};
    put(McaBusVoltage, int32_t(bus * 1000)); put(McaBusCurrent, int32_t(icl * 1000));
    put(McaType, pd ? type : PIANO_CHG_T_SDP); put(McaIcl, int32_t(icl)); put(McaChargeEnable, int32_t(enable));
    put(McaTermVolt, 5200); put(McaVerify, verify); put(McaTbat, 30); put(McaPdState, fwReady && pd ? 2 : 1); put(McaLpd, 0);
    if (mcaPdos && pd) {
      const unsigned n = mcaOwn ? mcaN : fwN;
      put(McaPdos, int32_t(n)); m.pdos = uint8_t(n);
      for (unsigned i = 0; i < 7; ++i) for (unsigned k = 0; k < 3; ++k) m.pdo[i][k] = i < n ? (mcaOwn ? mca[i][k] : fw[i][k]) : 0;
    }
    LinkView v; v.nowMs = now; v.chargerPdUp = true; v.chargerPdUpMs = 1;
    v.contract = &contract; v.safetyFresh = true; v.cpOff = true; v.dtptValid = alerts.complete(); v.dtpt = alerts.active();
    v.packs = packs; v.packsFresh = true; v.packsMs = now; v.board = &board;
    v.mippsPending = mippsPending; v.cpFirst = cpFirst;
    ChargeLink::fill(link.in, m, v);
  }
  void decide() {
    fill(); piano_charge_decide(&link.cfg, &link.in, &link.st, &link.plan);
    if (ladders.empty() || ladders.back() != link.plan.ladder) ladders.push_back(link.plan.ladder);
    CHECK(link.plan.ladder == link.st.ladder);
    for (unsigned i = 0; i < link.plan.writes; ++i) {
      const auto w = link.plan.write[i];
      CHECK(piano_charge_write_allowed(&link.cfg, w.property, w.value, w.bytes));
      CHECK(piano_mca_write_allowed_for(&m, w.property, w.value, w.bytes));
      if ((w.property == PIANO_CHG_PD_FIXED && w.value == 9000) || (w.property == PIANO_CHG_PPS && link.st.hv == PIANO_CHG_HV_ASKED)) {
        // the barrier: reduced currents acknowledged and the input limit read back before any raise
        CHECK(link.st.icl_set && link.st.icl_set <= link.st.transition_ma && link.st.icl_set <= 500);
        CHECK(link.st.fcc_set && link.st.fcc_set <= 500);
        CHECK(link.in.icl_ma >= 0 && uint32_t(link.in.icl_ma) <= link.st.transition_ma);
        CHECK(bus < 6000 && contract.valid && contract.mv == 5000);
        ++voltageRequests;
      }
      writes.push_back(w);
      switch (w.property) {
      case PIANO_CHG_ICL: icl = w.value; break;
      case PIANO_CHG_FCC: fcc = w.value; break;
      case PIANO_CHG_ENABLE: enable = w.value; for (auto &g : packs) g.currentMa = enable ? 300 : 0; break;
      case PIANO_CHG_PD_FIXED: {
        bus = w.value;
        unsigned obj = 1;
        for (unsigned i = 0; i < fwN; ++i) if (fw[i][0] == w.value && fw[i][1] == w.value) obj = i + 1;
        if (wrongObject && bus == 9000) obj = wrongObject;
        rdo = wrongPps && bus == 9000 ? ppsRdo(obj) : fixedRdo(obj, bus == 9000 ? 1600 : 3000);
        break;
      }
      case PIANO_CHG_PPS: bus = (w.value >> 16) * 20; rdo = ppsRdo(wrongObject ? wrongObject : 3, (w.value & 0xffff) * 50); break;
      default: CHECK(false);
      }
      piano_charge_ack(&link.st, w.property, w.value, 0);
    }
    CHECK(fcc <= 1500 && icl <= 3000);
    if (bus >= 6000) CHECK(icl <= 1600);
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
      CHECK(n == 64);
      control = piano_contract_u32(request + 20) | (uint64_t(piano_contract_u32(request + 24)) << 32);
      if ((control & 255) == 0x10) ++pdoQueries;
    } else {
      length = 64; response[12] = 0x10; response[13] = 1;   // UCSI 1.1
      uint32_t cci = 0; uint8_t *msg = response + 28;
      if (contract.stage != PianoContractProbe) {
        const unsigned command = control & 255;
        cci = command == 1 ? 1u << 27 : command == 4 ? 1u << 29 : 1u << 31;
        if (command == 6) {cci |= 16u << 8; piano_contract_put32(msg, 0x4046); msg[4] = 1; msg[5] = uint8_t(features);}
        else if (command == 0x12) {
          ++statusReads;
          if (flipRdoAfterStatus >= 0 && statusReads == unsigned(flipRdoAfterStatus) + 1) rdo = fixedRdo(1, 2000);
          cci |= 9u << 8;
          piano_contract_put32(msg, (connected ? 1u << 19 : 0) | (pd && connected ? 3u : 1u) << 16);
          piano_contract_put32(msg + 4, pd && connected ? rdo : 0);
        } else if (command == 0x10) {
          // only the UCSI-PDO PPM answers GET_PDOS: the same list as raw words
          cci |= 12u << 8;
          uint32_t raw[7] = {};
          piano_contract_firmware_pdos(fw, fwN, raw);
          for (unsigned i = 0; i < 3; ++i) piano_contract_put32(msg + 4 * i, raw[i]);
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
    for (unsigned i = 0; i < 200 && contract.seq == seq && !contract.quarantined; ++i) {
      const int r = wire();
      if (r < 0) break;
    }
  }
  void tick() {now += 5000; mcaCycle(); observe(); decide();}
  bool wrote(uint32_t property, uint32_t value, size_t after = 0) const {
    for (size_t i = after; i < writes.size(); ++i) if (writes[i].property == property && writes[i].value == value) return true;
    return false;
  }
  size_t count(uint32_t property) const {
    size_t k = 0;
    for (const auto &w : writes) if (w.property == property) ++k;
    return k;
  }
};

// Today's daily path on this PPM: a PD charger gets no contract, the strict policy holds the input at the firmware's 100 mA
// and never asks for 9 V even with hv=1 (the owner's 67 W try-out: AC attached, not charging, packs about -3 A).
static void baselineWithoutFallback() {
  for (unsigned hv : {0u, 1u, 2u}) {
    Bench b(false, hv, true);
    for (unsigned i = 0; i < 16; ++i) b.tick();
    CHECK(b.contract.status_valid && b.contract.pd && !b.contract.valid && !b.contract.pdos);
    CHECK(b.contract.last_error == PIANO_CONTRACT_RDO && !b.pdoQueries);
    CHECK(!b.voltageRequests && b.bus == 5000 && b.icl <= 100);
    CHECK(!(b.link.in.valid & PIANO_CHG_IN_CONTRACT) && !b.contract.offer_n && !b.contract.offers);
  }
}

static void fixedNineThenPps() {
  Bench b(true, 1);
  b.tick();
  CHECK(b.contract.valid && b.contract.pdo_origin == PIANO_CONTRACT_PDO_FIRMWARE && !b.pdoQueries);
  CHECK(b.contract.pdos == 3 && b.contract.mv == 5000 && b.contract.ma == 3000 && b.contract.protocol == PIANO_CONTRACT_FIXED);
  CHECK(b.link.in.fixed9_ma == 2000 && b.link.in.pps9_ma == 3000 && b.link.in.contract_ma == 3000);
  // a PD sink status pair: the second read must agree before publication (two 0x12 per refresh)
  const auto reads = b.statusReads; b.observe(); CHECK(b.statusReads == reads + 2);
  for (unsigned i = 0; i < 14 && b.link.st.hv != PIANO_CHG_HV_ON; ++i) b.tick();
  CHECK(b.voltageRequests == 1 && b.wrote(PIANO_CHG_PD_FIXED, 9000));
  CHECK(b.link.st.hv == PIANO_CHG_HV_ON && b.bus == 9000 && b.contract.mv == 9000 && b.contract.ma == 1600);
  CHECK(b.contract.object == 2 && b.contract.pdo_origin == PIANO_CONTRACT_PDO_FIRMWARE);
  b.tick();
  CHECK(b.icl == 1600 && b.fcc <= 1500 && b.link.plan.icl_why == PIANO_CHG_ICL_9V);
  // PPS through the same administrator mode transaction: 5 V withdrawal first
  const auto start = b.writes.size();
  CHECK(!piano_charge_config_transaction(&b.link.cfg, 1, 80, 75, 2, 0));
  b.link.syncPpsOption(b.m);
  for (unsigned i = 0; i < 20 && b.contract.protocol != PIANO_CONTRACT_PPS; ++i) b.tick();
  for (unsigned i = 0; i < 4; ++i) b.tick();
  CHECK(b.contract.protocol == PIANO_CONTRACT_PPS && b.contract.mv == 9000 && b.contract.object == 3);
  CHECK(b.link.st.hv == PIANO_CHG_HV_ON && b.icl <= 1600 && b.fcc <= 1500);
  bool five = false;
  for (size_t i = start; i < b.writes.size(); ++i) {
    if (b.writes[i].property == PIANO_CHG_PD_FIXED && b.writes[i].value == 5000) five = true;
    if (b.writes[i].property == PIANO_CHG_PPS) CHECK(five && b.writes[i].value == piano_charge_pps_word(9000, 1600));
  }
  CHECK(five);
  // a bounded administrator inhibit (a stop vote like the 80 % hold) ends 9 V and charging; 80/75 untouched
  CHECK(!piano_charge_config_transaction(&b.link.cfg, 1, 80, 75, 2, 1));
  for (unsigned i = 0; i < 6 && (b.enable || b.bus != 5000); ++i) b.tick();
  CHECK(!b.enable && b.bus == 5000);
  CHECK(!piano_charge_config_transaction(&b.link.cfg, 1, 80, 75, 1, 0));
  for (unsigned i = 0; i < 6 && !b.enable; ++i) b.tick();
  CHECK(b.enable && b.link.cfg.limit && b.link.cfg.stop_pct == 80 && b.link.cfg.resume_pct == 75);
  // the 80 % stop itself from 9 V: back to 5 V, charging off
  Bench h(true, 1);
  for (unsigned i = 0; i < 14 && h.link.st.hv != PIANO_CHG_HV_ON; ++i) h.tick();
  CHECK(h.link.st.hv == PIANO_CHG_HV_ON);
  for (auto &g : h.packs) g.socPct = 80;
  for (unsigned i = 0; i < 6 && (h.enable || h.bus != 5000); ++i) h.tick();
  CHECK(!h.enable && h.bus == 5000 && h.link.st.hold);
}

// hv=0 with the stepped Type-C current: a 5 V PD contract now authorizes its RDO current through the guarded steps.
static void fiveVoltContract() {
  Bench b(true, 0, true);
  for (unsigned i = 0; i < 16; ++i) b.tick();
  CHECK(b.contract.valid && b.link.plan.icl_why == PIANO_CHG_ICL_CONTRACT_5V);
  CHECK(b.icl >= 2000 && b.icl <= 3000 && !b.voltageRequests && b.bus == 5000);
  Bench off(false, 0, true);
  for (unsigned i = 0; i < 16; ++i) off.tick();
  CHECK(off.icl <= 100);
}

static void withdrawal() {
  Bench b(true, 1);
  for (unsigned i = 0; i < 14 && b.link.st.hv != PIANO_CHG_HV_ON; ++i) b.tick();
  CHECK(b.link.st.hv == PIANO_CHG_HV_ON);
  // one cycle without SNK_Ready (a renegotiation) keeps the offer and the session
  b.fwReady = false; b.tick();
  CHECK(b.contract.offer_n == 3 && b.contract.valid && b.link.st.hv == PIANO_CHG_HV_ON && b.bus == 9000);
  // two: withdrawn; the next status pair has no list and the session goes back to 5 V
  const auto start = b.writes.size();
  b.tick();
  CHECK(!b.contract.offer_n && b.contract.offer_withdrawals == 1);
  b.tick(); b.tick();
  CHECK(!b.contract.valid && b.wrote(PIANO_CHG_PD_FIXED, 5000, start) && b.bus == 5000 && b.icl <= 100);
  // ready again: a new contract, 9 V only through the full barrier again
  b.fwReady = true;
  const auto asks = b.voltageRequests;
  for (unsigned i = 0; i < 14 && b.link.st.hv != PIANO_CHG_HV_ON; ++i) b.tick();
  CHECK(b.contract.valid && b.voltageRequests == asks + 1);
}

static void badLists() {
  const uint32_t lists[][7][3] = {
      {{9000, 9000, 2000}, {5000, 5000, 3000}},                     // vSafe5V not first
      {{5000, 5000, 3000}, {12000, 12000, 1500}, {9000, 9000, 2000}},   // fixed not ascending
      {{5000, 5000, 3000}, {3300, 11000, 3000}, {9000, 9000, 2000}},    // a fixed object after an APDO
      {{5000, 5000, 3000}, {9000, 9000, 6000}},                     // above 5 A
      {{5000, 5000, 3000}, {9010, 9010, 2000}},                     // not in 50 mV units
      {{5000, 5000, 3000}, {9000, 9000, 50}},                       // below 100 mA
      {{5000, 5000, 3000}, {3300, 3350, 3000}},                     // an APDO range that rounds to nothing
  };
  const unsigned sizes[] = {2, 3, 3, 2, 2, 2, 2};
  for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; ++k) {
    Bench b(true, 1);
    for (unsigned i = 0; i < 7; ++i) for (unsigned j = 0; j < 3; ++j) b.fw[i][j] = lists[k][i][j];
    b.fwN = sizes[k]; b.rdo = fixedRdo(1, 3000);
    for (unsigned i = 0; i < 8; ++i) b.tick();
    CHECK(!b.contract.valid && !b.contract.offer_n && b.contract.offer_rejects >= 4);
    CHECK(!b.voltageRequests && b.bus == 5000 && b.icl <= 100);
  }
  uint32_t raw[7];
  const uint32_t eight[8][3] = {{5000, 5000, 3000}, {9000, 9000, 2000}};
  CHECK(!piano_contract_firmware_pdos(eight, 8, raw));
  CHECK(!piano_contract_firmware_pdos(eight, 0, raw));
  CHECK(piano_contract_firmware_pdo_word(5000, 5000, 3000) == (100u << 10 | 300u));
  CHECK(piano_contract_firmware_pdo_word(3300, 11000, 3000) == (3u << 30 | 110u << 17 | 33u << 8 | 60u));
  CHECK(piano_contract_firmware_pdo_word(3300, 21000, 5000) == (3u << 30 | 210u << 17 | 33u << 8 | 100u));
  CHECK(!piano_contract_firmware_pdo_word(3300, 21100, 3000) && !piano_contract_firmware_pdo_word(5000, 5000, 5001));
  // an RDO that does not fit the firmware's object: beyond the list, or more current than the object offers
  for (uint32_t r : {fixedRdo(4, 1000), fixedRdo(2, 2500), uint32_t(2u << 28 | 160u << 10 | 250u)}) {
    Bench b(true, 1); b.rdo = r;
    for (unsigned i = 0; i < 4; ++i) b.tick();
    CHECK(b.contract.status_valid && b.contract.pd && !b.contract.valid && b.contract.last_error == PIANO_CONTRACT_RDO);
    CHECK(!b.voltageRequests);
  }
}

static void sessionBoundaries() {
  // the second status read disagrees: no publication of either RDO
  Bench v(true, 1);
  v.mcaCycle(); v.flipRdoAfterStatus = int(v.statusReads) + 1; v.observe();
  CHECK(!v.contract.valid && v.contract.last_error == PIANO_CONTRACT_CHANGED);
  // a disconnect drops the offer: the next connection needs a list read after it
  Bench c(true, 1);
  c.tick(); CHECK(c.contract.valid && c.contract.offer_n == 3);
  c.connected = false; c.now += 5000; c.observe();
  CHECK(!c.contract.offer_n && !c.contract.valid && !c.contract.connected);
  c.connected = true; c.now += 5000; c.observe();
  CHECK(!c.contract.valid && c.contract.pd);   // connected again, no list offered since
  c.tick(); CHECK(c.contract.valid && c.contract.pdo_origin == PIANO_CONTRACT_PDO_FIRMWARE);
  // a relink keeps the option, drops the offer
  piano_contract_link(&c.contract, 0); piano_contract_link(&c.contract, 1);
  CHECK(c.contract.pdo_fallback && !c.contract.offer_n && !c.contract.valid);
  // a non-PD sink never takes a list, offered or not
  Bench s(true, 1); s.pd = false;
  for (unsigned i = 0; i < 6; ++i) s.tick();
  CHECK(s.contract.status_valid && !s.contract.pd && !s.contract.valid && !s.contract.offer_n);
  piano_contract_offer_pdos(&s.contract, s.fw, s.fwN, 1); s.observe();
  CHECK(!s.contract.valid && !s.voltageRequests);
  // a PPM with PDO details: its own list, never the firmware's
  Bench u(true, 1); u.features = 16;
  u.fw[1][2] = 2000; u.tick();
  CHECK(u.contract.valid && u.contract.pdo_origin == PIANO_CONTRACT_PDO_UCSI && u.pdoQueries);
  // the option off ignores offers entirely
  Bench o(false, 1);
  piano_contract_offer_pdos(&o.contract, o.fw, o.fwN, 1);
  CHECK(!o.contract.offer_n && !o.contract.offers);
}

// The codec's read of 0x21011 itself: one whole cycle against a firmware that answers every request.
static bool codecCycle(PianoMca &m, const uint32_t list[][3], unsigned n, int32_t pdState) {
  piano_mca_refresh(&m);
  for (unsigned guard = 0; guard < 400; ++guard) {
    uint8_t req[PIANO_MCA_REQUEST_BYTES] = {}, resp[PIANO_MCA_RESPONSE_BYTES] = {};
    if (!piano_mca_next(&m, req)) return false;
    for (unsigned i = 0; i < 12; ++i) resp[i] = req[i];
    const uint32_t property = piano_contract_u32(req + 12), seq = piano_contract_u32(req + 16);
    piano_contract_put32(resp + 12, property); piano_contract_put32(resp + 20, seq);
    if (piano_contract_u32(req + 8) == PIANO_MCA_OP_READ) {
      if (property == PIANO_MCA_PDOS)
        for (unsigned i = 0; i < n; ++i) for (unsigned k = 0; k < 3; ++k) piano_contract_put32(resp + 24 + 12 * i + 4 * k, list[i][k]);
      else if (property == PIANO_MCA_PD_STATE) piano_contract_put32(resp + 24, uint32_t(pdState));
      else if (property == PIANO_MCA_BUS_VOLTAGE) piano_contract_put32(resp + 24, 5000000);
      else if (property == PIANO_MCA_REAL_TYPE) piano_contract_put32(resp + 24, PIANO_MCA_TYPE_PD);
    }
    if (piano_mca_receive(&m, resp, sizeof resp) == McaCycleDone) return true;
  }
  return false;
}

static void codecList() {
  const uint32_t xiaomi[3][3] = {{5000, 5000, 3000}, {9000, 9000, 3000}, {3300, 11000, 6100}};
  const uint32_t garbage[2][3] = {{5000, 5000, 3000}, {3300, 11000, 7000}};
  for (int clamp = 0; clamp < 2; ++clamp) {
    PianoMca m{};
    piano_mca_init(&m, 1, 1); piano_mca_use_external_policy(&m, 1, 1, 0); piano_mca_allow_board(&m, 0);
    piano_mca_clamp_pdos(&m, clamp); piano_mca_link(&m, 1);
    CHECK(codecCycle(m, xiaomi, 3, 2));
    CHECK(piano_mca_has(&m, McaPdState) && m.value[McaPdState] == 2);
    // without the clamp one 6.1 A object no longer discards the whole list (charge/mipps2): its slot is left empty and marked
    if (!clamp) CHECK(piano_mca_has(&m, McaPdos) && m.pdos == 2 && m.value[McaPdos] == (2 | 4 << 8) && !m.pdo[2][0]);
    else {
      CHECK(piano_mca_has(&m, McaPdos) && m.pdos == 3 && m.pdo[2][0] == 3300 && m.pdo[2][1] == 11000 && m.pdo[2][2] == 5000);
      PianoChargeContract c{}; piano_contract_init(&c); piano_contract_allow_firmware_pdos(&c, 1);
      piano_contract_offer_pdos(&c, m.pdo, m.pdos, 1);
      CHECK(c.offer_n == 3 && c.offer_pdo[2][2] == 5000 && !c.offer_rejects);
    }
    // charge/mipps2: an object beyond any PD current field is left empty at its position, marked; the rest of the list stays
    CHECK(codecCycle(m, garbage, 2, 2));
    CHECK(piano_mca_has(&m, McaPdos) && m.pdos == 1 && m.value[McaPdos] == (1 | 2 << 8) && !m.pdo[1][0] && !m.pdo[1][2]);
    // ... but an unusable vSafe5V discards it
    const uint32_t badFirst[2][3] = {{5000, 5000, 20000}, {9000, 9000, 3000}};
    CHECK(codecCycle(m, badFirst, 2, 2));
    CHECK(!piano_mca_has(&m, McaPdos) && !m.pdos);
    // a verified MiPPS adapter's list with gaps: every object at its own position (object 7 = slot 6)
    const uint32_t sparse[7][3] = {{5000, 5000, 3000}, {9000, 9000, 3000}, {}, {}, {}, {}, {3300, 11000, 6200}};
    CHECK(codecCycle(m, sparse, 7, 2));
    CHECK(piano_mca_has(&m, McaPdos) && !m.pdo[2][0] && !m.pdo[5][1]);
    if (clamp) {
      CHECK(m.pdos == 7);
      CHECK(m.value[McaPdos] == 7 && m.pdo[6][0] == 3300 && m.pdo[6][1] == 11000 && m.pdo[6][2] == 5000);
      PianoChargeContract c{}; piano_contract_init(&c); piano_contract_allow_firmware_pdos(&c, 1);
      piano_contract_offer_pdos(&c, m.pdo, m.pdos, 1);
      CHECK(c.offer_n == 7 && !c.offer_rejects && c.offer_pdo[6][2] == 5000 && !c.offer_pdo[3][0]);
      uint32_t raw[7] = {};
      CHECK(piano_contract_firmware_pdos(sparse, 7, raw) == 0);   // 6.2 A unclamped is no PD word
      CHECK(piano_contract_firmware_pdos(m.pdo, 7, raw) == 7 && !raw[2] && raw[6] >> 30 == 3);
      PianoContractDecoded d{};
      CHECK(piano_contract_decode(raw, 7, kVendorRdo7, &d) && d.protocol == PIANO_CONTRACT_PPS && d.mv == 5000 && d.ma == 3250);
      CHECK(!piano_contract_decode(raw, 7, 0x3301f441u, &d));   // the same word at an empty position decodes as nothing
    } else {
      CHECK(m.pdos == 2 && m.value[McaPdos] == (2 | 1 << (8 + 6)));   // unclamped 6.2 A: slot 7 unusable, so object 7 is unmapped
    }
  }
  // a list whose count ends on an empty slot is no list
  uint32_t raw[7] = {};
  const uint32_t trailing[3][3] = {{5000, 5000, 3000}, {9000, 9000, 3000}, {}};
  CHECK(!piano_contract_firmware_pdos(trailing, 3, raw) && piano_contract_firmware_pdos(trailing, 2, raw) == 2);
}

// cp-first1 (2026-11-24 13:10, RAM y12d + piano-power-cp=1 piano-power-mipps=1, the stock Xiaomi charger; MIPPS-RDO.md): after the
// MiPPS exchange the firmware's own PD engine re-requests the verified adapter's vendor object at 5 V, RDO 0x7301f441 (object 7,
// PPS-format 5000 mV / 3250 mA) or 0x53026e3c (object 5), while the source list the policy holds has 2 objects. Before
// charge/mipps2: ContractError 6 (fresh PD status/invalid RDO) and the PD clamp held the input at 100 mA for the whole attach.
static void mippsVendorObject() {
  // (a) the list as seen (5 V 3 A, 9 V 3 A) and the object-7 RDO at a 5 V bus: the vendor object is unmapped, not invalid
  for (uint32_t r : {kVendorRdo7, kVendorRdo5}) {
    Bench b(true, 1, true);
    b.fw[1][2] = 3000; b.fw[2][0] = b.fw[2][1] = b.fw[2][2] = 0; b.fwN = 2; b.mcaPdos = true; b.rdo = r;
    for (unsigned i = 0; i < 16; ++i) b.tick();
    CHECK(b.contract.status_valid && b.contract.pd && !b.contract.valid && b.contract.last_error == PIANO_CONTRACT_RDO);
    CHECK(b.contract.rdo == r);
    CHECK(!(b.link.in.valid & PIANO_CHG_IN_CONTRACT));                       // never a contract: no 9 V, no PPS, no pump
    CHECK((b.link.in.valid & PIANO_CHG_IN_PD_UNMAPPED) && b.link.in.rdo_object == (r >> 28));
    CHECK(!b.voltageRequests && b.bus == 5000);
    CHECK(b.icl == 1500 && b.link.plan.icl_why == PIANO_CHG_ICL_ROW);       // the vSafe5V row (Ubuntu: min(vSafe5V, 3000)), not 100
    CHECK(b.fcc <= 1500);
    // the same RDO with the bus above the 5 V window: no budget of an unknown voltage
    b.bus = 6220;
    for (unsigned i = 0; i < 4; ++i) {b.tick(); b.bus = b.bus == 5000 ? 5000 : 6220; b.rdo = r;}
    CHECK(!b.wrote(PIANO_CHG_PD_FIXED, 9000) && !b.count(PIANO_CHG_PPS));
  }
  // (b) no list read at all: still the firmware's 100 mA (nothing known of the source)
  {
    Bench b(true, 1, true); b.fwN = 2; b.rdo = kVendorRdo7;
    for (unsigned i = 0; i < 12; ++i) b.tick();
    CHECK(!(b.link.in.valid & PIANO_CHG_IN_PD_UNMAPPED) && b.icl <= 100);
  }
  // (c) an RDO whose object is in the list but does not fit it (cp-first2: 0x21011 positions are not the source's objects): the
  //     same 5 V row as any undecodable RDO (Ubuntu's 5 V PD input never reads the RDO), still never a contract and no 9 V
  {
    Bench b(true, 1, true); b.mcaPdos = true; b.rdo = fixedRdo(2, 2500);
    for (unsigned i = 0; i < 12; ++i) b.tick();
    CHECK(!b.contract.valid && (b.link.in.valid & PIANO_CHG_IN_PD_UNMAPPED) && b.link.in.rdo_object == 2 && b.icl == 1500);
    CHECK(!(b.link.in.valid & PIANO_CHG_IN_CONTRACT) && !b.voltageRequests && b.bus == 5000);
  }
  // (d) the firmware lists the vendor object at its own position (gaps kept): the RDO decodes as a PPS contract at 5 V; the ladder
  //     then goes on as Ubuntu's does without a pump: 9 V fixed on the listed object 2, confirmed by the contract's object
  {
    Bench b(true, 1, true);
    const uint32_t list[7][3] = {{5000, 5000, 3000}, {9000, 9000, 3000}, {}, {}, {}, {}, {3300, 11000, 5000}};
    for (unsigned i = 0; i < 7; ++i) for (unsigned k = 0; k < 3; ++k) b.fw[i][k] = list[i][k];
    b.fwN = 7; b.mcaPdos = true; b.rdo = kVendorRdo7;
    b.tick();
    CHECK(b.contract.valid && b.contract.object == 7 && b.contract.protocol == PIANO_CONTRACT_PPS);
    CHECK(b.contract.mv == 5000 && b.contract.ma == 3250 && b.contract.pdos == 7 && !b.contract.offer_rejects);
    CHECK(b.link.in.contract_protocol == PIANO_CONTRACT_PPS && !(b.link.in.valid & PIANO_CHG_IN_PD_UNMAPPED));
    CHECK(b.icl <= 1500 && !b.voltageRequests && b.bus == 5000);
    // the pump's source view at this 5 V: a valid PPS contract and the 3.3-11 V APDO; held until the ladder is ready
    PianoCpSys y{};
    piano_charge_cp_sys(&b.link.cfg, &b.link.in, &b.link.st, &y);
    CHECK(y.contract_valid && y.contract_pps && y.src_kind == PIANO_CPS_SRC_PPS && y.apdo_max_mv == 11000 && y.apdo_ma == 5000);
    CHECK(y.ladder_hold && b.link.st.ladder == PIANO_CHG_LAD_5V_CONTRACT);
    for (unsigned i = 0; i < 16 && b.link.st.hv != PIANO_CHG_HV_ON; ++i) b.tick();
    CHECK(b.link.st.hv == PIANO_CHG_HV_ON && b.voltageRequests == 1 && b.wrote(PIANO_CHG_PD_FIXED, 9000));
    CHECK(b.link.st.ladder_obj == 2 && b.contract.object == 2 && b.link.st.ladder == PIANO_CHG_LAD_9V);
    piano_charge_cp_sys(&b.link.cfg, &b.link.in, &b.link.st, &y);
    CHECK(!y.ladder_hold);   // the pump may start from the confirmed 9 V (Ubuntu cp_try_start with hv ON)
  }
}

// cp-first2 (2026-11-24 22:06-22:2x, RAM y12f3, stock Xiaomi charger), replayed: a valid 5 V / 3 A contract on the 2-object list
// (RDO object 1), MiPPS at 5 V (Ladder 3, 0x21002 = 1), MiPPS done; then the firmware's own RDO 0x7301f441 (object 7, PPS layout),
// raw type 11 and a 4-entry 0x21011 list. The tablet logged `error=6 fresh PD status/invalid RDO ... obj=7 unmapped=0 mca_pdos=4`
// with ICL 100 / FCC 500 at a 5.0 V bus. By elimination of ChargeLink's conjuncts (fresh PD sink status, invalid RDO, a list read,
// object 7 > 4) the list's first slot was not a 5000/5000 object: 0x21011 is not the source's object list by position. The lists
// below are such lists (the tablet's exact contents were not captured: the console lost 327-632 s). Ubuntu's 5 V PD input
// (xiaomi_mca_5v_input / xiaomi_mca_pd_5v_ma) takes a 5000/5000 entry at any slot, else 500 mA, never gated on the RDO.
static void cpFirst2Replay() {
  struct Case {uint32_t list[4][3]; uint32_t icl;};
  const Case cases[] = {
      // control: vSafe5V at slot 1 (in source order): unmapped already on a8ff8e74, so this is not what the tablet held
      {{{5000, 5000, 3000}, {9000, 9000, 3000}, {3300, 11000, 5000}, {3300, 21000, 3250}}, 1500},
      {{{3300, 11000, 5000}, {5000, 5000, 3000}, {9000, 9000, 3000}, {3300, 21000, 3250}}, 1500},   // APDO first, vSafe5V at slot 2
      {{{9000, 9000, 3000}, {5000, 5000, 3000}, {12000, 12000, 2250}, {3300, 11000, 5000}}, 1500},  // not in source order
      {{{3300, 11000, 5000}, {3300, 21000, 3250}, {9000, 9000, 3000}, {12000, 12000, 2250}}, 500},  // no vSafe5V: Ubuntu's 500
  };
  for (const Case &k : cases) {
    Bench b(true, 1, true);
    b.fw[1][2] = 3000; b.fw[2][0] = b.fw[2][1] = b.fw[2][2] = 0; b.fwN = 2; b.mcaPdos = true; b.rdo = fixedRdo(1, 3000);
    // 1. the valid 5 V / 3 A contract, MiPPS pending (the ladder holds 9 V), then MiPPS running (0x21002 = 1)
    b.mippsPending = true;
    for (unsigned i = 0; i < 4; ++i) b.tick();
    CHECK(b.contract.valid && b.contract.object == 1 && b.contract.mv == 5000 && b.link.st.ladder == PIANO_CHG_LAD_MIPPS);
    b.verify = 1;
    for (unsigned i = 0; i < 2; ++i) b.tick();
    CHECK(!b.voltageRequests && b.bus == 5000);
    // 2. MiPPS done (MiPPSState 3, MiPPSActive 0)
    b.mippsPending = false; b.verify = 0;
    // 3. the firmware's vendor-object RDO, raw type 11 and the 4-entry list (the offer sees the same list)
    b.rdo = kVendorRdo7; b.type = PIANO_CHG_T_PPS;
    for (unsigned i = 0; i < 7; ++i) for (unsigned j = 0; j < 3; ++j) b.fw[i][j] = i < 4 ? k.list[i][j] : 0;
    b.fwN = 4;
    for (unsigned i = 0; i < 16; ++i) b.tick();
    // the tablet's line: error 6, fresh PD status, object 7, the codec holding 4 entries
    CHECK(b.contract.status_valid && b.contract.pd && !b.contract.valid && b.contract.last_error == PIANO_CONTRACT_RDO);
    CHECK(((b.contract.rdo >> 28) & 15u) == 7 && b.m.pdos == 4 && !(b.link.in.valid & PIANO_CHG_IN_CONTRACT));
    // the fix: the vendor object is unmapped (no 100 mA), never a contract: no 9 V, no PPS word, the bus stays at 5 V
    CHECK((b.link.in.valid & PIANO_CHG_IN_PD_UNMAPPED) && b.link.in.rdo_object == 7);
    CHECK(b.icl == k.icl && b.link.plan.icl_ma == k.icl);
    if (b.icl != k.icl) std::printf("  cp-first2 replay: icl=%u fcc=%u (want %u)\n", b.icl, b.fcc, k.icl);
    CHECK(b.fcc > 500 || k.icl == 500);
    CHECK(!b.voltageRequests && b.bus == 5000 && !b.wrote(PIANO_CHG_PD_FIXED, 9000) && !b.count(PIANO_CHG_PPS));
    CHECK(b.link.st.ladder == PIANO_CHG_LAD_5V);
  }
}

// charge/mipps2: the charging ladder in the Ubuntu port's order (UBUNTU-COMPARE.md section 4): 5 V, MiPPS at 5 V, then the pump from
// 5 V when it would start, else the buck's 9 V on an object the latest 0x21011 list advertises, confirmed by the contract's object.
static bool seen(const std::vector<uint8_t> &v, std::initializer_list<uint8_t> order) {
  size_t at = 0;
  for (uint8_t want : order) {
    while (at < v.size() && v[at] != want) ++at;
    if (at == v.size()) return false;
  }
  return true;
}
static void ladderOrder() {
  // 1. the order: MiPPS pending holds the 9 V request (no barrier, no write), then 9 V on the listed object 2
  {
    Bench b(true, 1, true); b.mcaPdos = true; b.mippsPending = true;
    for (unsigned i = 0; i < 10; ++i) b.tick();
    CHECK(!b.voltageRequests && b.bus == 5000 && b.link.st.ladder == PIANO_CHG_LAD_MIPPS);
    CHECK(b.link.plan.ladder_refusal == PIANO_CHG_LREF_ORDER && b.link.st.hv == PIANO_CHG_HV_OFF);
    b.mippsPending = false;
    for (unsigned i = 0; i < 12 && b.link.st.hv != PIANO_CHG_HV_ON; ++i) b.tick();
    CHECK(b.link.st.hv == PIANO_CHG_HV_ON && b.voltageRequests == 1 && b.link.st.ladder_obj == 2 && b.contract.object == 2);
    CHECK(seen(b.ladders, {PIANO_CHG_LAD_MIPPS, PIANO_CHG_LAD_5V_READY, PIANO_CHG_LAD_9V_ASKED, PIANO_CHG_LAD_9V}));
  }
  // 2. the pump first: an armed pump that would start from this 5 V holds the buck's 9 V (cp_try_start before apply_policy)
  {
    Bench b(true, 1, true); b.mcaPdos = true; b.cpFirst = true;
    for (unsigned i = 0; i < 12; ++i) b.tick();
    CHECK(!b.voltageRequests && b.link.st.ladder == PIANO_CHG_LAD_5V_READY);
    PianoCpSys y{};
    piano_charge_cp_sys(&b.link.cfg, &b.link.in, &b.link.st, &y);
    CHECK(!y.ladder_hold && y.src_kind == PIANO_CPS_SRC_PPS);   // the pump may start here, on the listed 3.3-11 V APDO
    b.cpFirst = false;   // it cannot (or ended): the buck's 9 V follows
    for (unsigned i = 0; i < 12 && b.link.st.hv != PIANO_CHG_HV_ON; ++i) b.tick();
    CHECK(b.link.st.hv == PIANO_CHG_HV_ON && b.voltageRequests == 1);
  }
  // 3. a skipped stage: no validated 5 V contract (vendor object unmapped, or no contract) -> never a 9 V request, pump held
  for (uint32_t r : {kVendorRdo7, 0u}) {
    Bench b(true, 1, true); b.mcaPdos = true; b.rdo = r ? r : fixedRdo(2, 2500);
    for (unsigned i = 0; i < 16; ++i) b.tick();
    CHECK(!b.voltageRequests && b.bus == 5000 && b.link.st.ladder == PIANO_CHG_LAD_5V);
    PianoCpSys y{};
    piano_charge_cp_sys(&b.link.cfg, &b.link.in, &b.link.st, &y);
    CHECK(y.ladder_hold && !y.contract_valid);
  }
  // 4. an object the latest 0x21011 list does not advertise is never requested: the offer had 9 V, this cycle's list does not
  {
    Bench b(true, 1, true); b.mcaPdos = true; b.mcaOwn = true;
    const uint32_t only5[1][3] = {{5000, 5000, 3000}};
    for (unsigned k = 0; k < 3; ++k) b.mca[0][k] = only5[0][k];
    b.mcaN = 1;
    for (unsigned i = 0; i < 16; ++i) b.tick();
    CHECK(!b.voltageRequests && !b.wrote(PIANO_CHG_PD_FIXED, 9000) && !b.count(PIANO_CHG_PPS));
    CHECK(b.link.st.ladder_refused >= 1);
  }
  // 5. a failed confirmation -> back to the previous stage, no second request on this attach:
  //    a. fixed 9 V asked (object 2), the firmware lands on the APDO (object 3, PPS): never confirmed, vSafe5V again
  {
    Bench b(true, 1, true); b.mcaPdos = true; b.wrongObject = 3; b.wrongPps = true;
    for (unsigned i = 0; i < 20; ++i) b.tick();
    CHECK(b.voltageRequests == 1 && b.wrote(PIANO_CHG_PD_FIXED, 5000) && b.bus == 5000);
    CHECK(b.link.st.hv == PIANO_CHG_HV_BLOCKED && b.link.st.ladder != PIANO_CHG_LAD_9V);
  }
  //    b. PPS 9 V asked on the listed APDO 3, the contract confirms another APDO (object 4): refused, back to 5 V
  {
    Bench b(true, 2, true); b.mcaPdos = true; b.wrongObject = 4;
    const uint32_t two[4][3] = {{5000, 5000, 3000}, {9000, 9000, 2000}, {3300, 11000, 3000}, {3300, 21000, 3000}};
    for (unsigned i = 0; i < 4; ++i) for (unsigned k = 0; k < 3; ++k) b.fw[i][k] = two[i][k];
    b.fwN = 4;
    for (unsigned i = 0; i < 20; ++i) b.tick();
    CHECK(b.voltageRequests == 1 && b.link.st.ladder_obj == 3);
    CHECK(b.link.st.hv == PIANO_CHG_HV_BLOCKED && b.link.st.ladder_refused >= 1 && b.wrote(PIANO_CHG_PD_FIXED, 5000));
    CHECK(b.bus == 5000);
  }
  // 6. a multi-protocol charger (5 V, 9 V fixed, a 3.3-11 V APDO): the buck takes the fixed 9 V object (Ubuntu pd_9v_ma prefers it),
  //    the PPS mode the APDO; each confirmed on its own position
  for (unsigned hv : {1u, 2u}) {
    Bench b(true, hv, true); b.mcaPdos = true;
    for (unsigned i = 0; i < 20 && b.link.st.hv != PIANO_CHG_HV_ON; ++i) b.tick();
    CHECK(b.link.st.hv == PIANO_CHG_HV_ON && b.link.st.ladder_obj == (hv == 1 ? 2 : 3));
    CHECK(b.contract.object == b.link.st.ladder_obj && b.voltageRequests == 1);
    CHECK(hv == 1 ? b.wrote(PIANO_CHG_PD_FIXED, 9000) : b.count(PIANO_CHG_PPS) >= 1);
  }
}

int main() {
  ladderOrder();
  mippsVendorObject();
  cpFirst2Replay();
  codecList();
  baselineWithoutFallback();
  fixedNineThenPps();
  fiveVoltContract();
  withdrawal();
  badLists();
  sessionBoundaries();
  std::printf("firmware PDO list: %u checks / %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
