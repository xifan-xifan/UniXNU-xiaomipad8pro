// SPDX-License-Identifier: BSD-2-Clause-Patent
// charge/cp-sess: the SC8541 pump session (ChargePumpSession.c) against a model of the worker, the pump and the source. Every gate,
// every transition, every exit (the pump off first, the source handed back only once CHG_EN reads clear), the latch, the derate,
// the QC3 path, the allowlist, the sleep handover, and the buck side's hand-back through the strict session's barrier.
#include "ChargePumpSession.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)

// The worker + pump + source model. The pump carries ~7 mA per mV above 2 x VBAT + 280 mV on the bus (about 140 mA per 20 mV).
struct Bench {
  PianoCpConfig c{};
  PianoCpSys y{};
  piano_cp_in d{};
  PianoCpState s{};
  PianoCpPlan p{};
  uint64_t now = 1000000;
  bool autoAck = true, autoBuck = true, pumpObeys = true, contractLive = true;
  bool busLag = false;   // charge/mipps2: the firmware takes the word but the bus does not follow (the step stays unconfirmed)
  int32_t pumpOffsetMv = 280;
  std::vector<uint32_t> ppsWords;   // every PPS word asked
  std::vector<int> actions;         // every non-NONE action
  unsigned startsWhileUnarmed = 0, writesWhilePending = 0, raisesAboveCeiling = 0;
  uint32_t lastReqMv = 0;

  Bench() {piano_cp_config_default(&c); piano_cp_init(&s); healthy();}
  void healthy() {
    std::memset(&y, 0, sizeof y);
    y.attach_seq = 1; y.attached = 1; y.awake = 1;
    y.src_kind = PIANO_CPS_SRC_PPS; y.apdo_min_mv = 3300; y.apdo_max_mv = 11000; y.apdo_ma = 5000;
    y.contract_valid = 1; y.contract_pps = 0; y.contract_mv = 9000; y.contract_ma = 1600;
    y.bus_valid = 1; y.vbus_mv = 9000; y.ibus_ma = 1500;
    y.buck_icl_set_ma = 1600; y.buck_fcc_set_ma = 3000;
    y.gauges_valid = 1; y.cells_valid = 1; y.soc_pct = 40;
    y.vbat_max_mv = 3900; y.vbat_min_mv = 3890; y.ibat_a_ma = 600; y.ibat_b_ma = 600;
    y.t_cool_dc = 250; y.t_warm_dc = 260; y.conn_valid = 1; y.t_conn_dc = 300;
    std::memset(&d, 0, sizeof d);
    d.armed = 1; d.present = 1; d.mipps_done = 1; d.source_budget_ma = 5000;
  }
  void model() {
    if (d.switching && pumpObeys) {
      const int32_t bus = lastReqMv ? (int32_t)lastReqMv - 60 : y.vbus_mv;
      int32_t ibus = 7 * (bus - (2 * y.vbat_max_mv + pumpOffsetMv));
      if (ibus < 0) ibus = 0;
      d.adc.valid = (1u << PIANO_CP_ADC_IBUS) | (1u << PIANO_CP_ADC_VBUS) | (1u << PIANO_CP_ADC_TDIE) | (1u << PIANO_CP_ADC_VBAT);
      d.adc.ibus_ma = ibus; d.adc.vbus_mv = bus; d.adc.vbat_mv = y.vbat_max_mv;
      if (!d.adc.tdie_dc) d.adc.tdie_dc = 600;
      y.ibat_a_ma = y.ibat_b_ma = ibus * 95 / 100;   // 2:1, two packs
    }
  }
  void run() {
    y.now_ms = now;
    model();
    piano_cp_decide(&c, &y, &d, &s, &p);
    d.faults = 0;
    exec();
  }
  void exec() {
    if (p.out.action != PIANO_CP_ACT_NONE) actions.push_back(p.out.action);
    if (p.out.action == PIANO_CP_ACT_START) {
      if (!d.armed) ++startsWhileUnarmed;
      d.switching = 1; d.busocp_ma = p.out.busocp_ma;
    }
    if (p.out.action == PIANO_CP_ACT_SET_BUSOCP) d.busocp_ma = p.out.busocp_ma;
    if (p.out.action == PIANO_CP_ACT_STOP && pumpObeys) d.switching = 0;
    if (p.out.req_valid & PIANO_CP_REQ_PPS) {
      if (y.volt_pending || y.mipps_cleanup_pending) ++writesWhilePending;
      const uint32_t ceiling = std::min<uint32_t>({y.apdo_max_mv, 2u * (uint32_t)y.vbat_max_mv + 2000u, 11000u});
      if (p.out.pps_mv > ceiling) ++raisesAboveCeiling;
      ppsWords.push_back(p.pps_word);
      CHECK(piano_cp_write_allowed(&c, PIANO_CHG_PPS, p.pps_word, 4));
      if (autoAck) {
        y.pps_taken_word = p.pps_word; lastReqMv = p.out.pps_mv;
        if (contractLive) {y.contract_valid = 1; y.contract_pps = 1; y.contract_mv = p.out.pps_mv; y.contract_ma = p.out.pps_ma;}
        y.vbus_mv = (int32_t)p.out.pps_mv - (busLag ? 1500 : 60);
      } else {
        y.volt_pending = 1;
      }
    }
    if (p.out.req_valid & PIANO_CPS_REQ_QC_PULSE) {
      CHECK(piano_cp_write_allowed(&c, PIANO_CHG_QC_PULSE, p.qc_pulse, 1));
      const int32_t step = y.src_kind == PIANO_CPS_SRC_QC3B ? 200 : 20;
      y.qc_pulses_taken += p.qc_pulse == PIANO_CHG_QC_UP ? 1 : -1;
      y.vbus_mv += p.qc_pulse == PIANO_CHG_QC_UP ? step : -step;
      lastReqMv = (uint32_t)y.vbus_mv + 60;
    }
    if (autoBuck && (p.out.req_valid & PIANO_CP_REQ_BUCK)) {
      y.buck_icl_set_ma = std::min<uint32_t>(y.buck_icl_set_ma, p.out.buck_icl_ma);
      y.buck_fcc_set_ma = std::min<uint32_t>(y.buck_fcc_set_ma, p.out.buck_fcc_ma);
    }
  }
  void tick(uint64_t ms) {now += ms; run();}
  void ticks(unsigned n, uint64_t ms = 100) {for (unsigned i = 0; i < n; i++) tick(ms);}
  // drive to a state (100 ms calls), at most ms of time
  bool until(uint8_t state, uint64_t ms = 30000) {
    for (uint64_t t = 0; t < ms; t += 100) {tick(100); if (s.state == state) return true;}
    return false;
  }
  bool toCC() {
    run();
    return until(PIANO_CPS_CC, 40000);
  }
};

static void gates() {
  // the healthy bench starts (after the 10 s wake settle)
  {Bench b; b.run(); b.ticks(5); CHECK(b.s.state == PIANO_CPS_IDLE); CHECK(b.until(PIANO_CPS_BUCK_HOLD, 12000));
   CHECK(b.p.events & PIANO_CPS_EV_START);}
  // each gate alone keeps it idle for 30 s
  struct G {const char *what; void (*f)(Bench &);};
  const G g[] = {
    {"unarmed", [](Bench &b) {b.d.armed = 0;}},
    {"absent", [](Bench &b) {b.d.present = 0;}},
    {"lost", [](Bench &b) {b.d.lost = 1;}},
    {"detached", [](Bench &b) {b.y.attached = 0;}},
    {"asleep", [](Bench &b) {b.y.awake = 0;}},
    {"sleep pending", [](Bench &b) {b.y.sleep_pending = 1;}},
    {"health hold", [](Bench &b) {b.y.health_hold = 1;}},
    {"soc 91", [](Bench &b) {b.y.soc_pct = 91;}},
    {"soc at the entry limit", [](Bench &b) {b.y.soc_entry_max = 77; b.y.soc_pct = 77;}},
    {"gauges", [](Bench &b) {b.y.gauges_valid = 0;}},
    {"cells", [](Bench &b) {b.y.cells_valid = 0;}},
    {"vbat min 3449", [](Bench &b) {b.y.vbat_min_mv = 3449;}},
    {"vbat max near CV", [](Bench &b) {b.y.vbat_max_mv = 4350;}},
    {"cool 14.9", [](Bench &b) {b.y.t_cool_dc = 149;}},
    {"warm 37.1", [](Bench &b) {b.y.t_warm_dc = 371;}},
    {"connector unknown", [](Bench &b) {b.y.conn_valid = 0;}},
    {"connector 50", [](Bench &b) {b.y.t_conn_dc = 495;}},
    {"tdie 115", [](Bench &b) {b.d.adc.valid = 1u << PIANO_CP_ADC_TDIE; b.d.adc.tdie_dc = 1145;}},
    {"no contract", [](Bench &b) {b.y.contract_valid = 0;}},
    {"no source", [](Bench &b) {b.y.src_kind = PIANO_CPS_SRC_NONE;}},
    {"APDO too low", [](Bench &b) {b.y.apdo_max_mv = 8500;}},
    {"APDO below 2 A", [](Bench &b) {b.y.apdo_ma = 1950;}},
    {"APDO starts above the start", [](Bench &b) {b.y.apdo_min_mv = 8500;}},
    {"QC without cfg.qc3", [](Bench &b) {b.y.src_kind = PIANO_CPS_SRC_QC3B;}},
    {"MiPPS not done", [](Bench &b) {b.c.mipps = 1; b.d.mipps_done = 0;}},
    {"MiPPS cleanup", [](Bench &b) {b.y.mipps_cleanup_pending = 1;}},
    {"voltage pending", [](Bench &b) {b.y.volt_pending = 1;}},
    {"pump already on", [](Bench &b) {b.d.switching = 1; b.pumpObeys = false;}},
    {"cell current", [](Bench &b) {b.y.ibat_a_ma = 7000;}},
  };
  for (const G &one : g) {
    Bench b; one.f(b); b.run();
    bool started = false;
    for (int i = 0; i < 300; i++) {b.tick(100); if (b.s.state != PIANO_CPS_IDLE && b.s.state != PIANO_CPS_LATCHED) started = true;}
    if (started) std::printf("  gate not held: %s\n", one.what);
    CHECK(!started);
    CHECK(b.ppsWords.empty());
  }
  // the wake settle: awake again 9 s ago is not enough, 10 s is
  {Bench b; b.y.awake = 0; b.run(); b.tick(100); b.y.awake = 1; b.tick(100); b.ticks(89); CHECK(b.s.state == PIANO_CPS_IDLE);
   b.ticks(15); CHECK(b.s.state != PIANO_CPS_IDLE);}
}

static void happyPath() {
  Bench b;
  b.run();
  CHECK(b.until(PIANO_CPS_BUCK_HOLD, 12000));
  CHECK(b.p.owns_volt);
  CHECK(b.p.out.req_valid & PIANO_CP_REQ_BUCK);
  CHECK(b.p.out.buck_icl_ma == 500 && b.p.out.buck_fcc_ma == 500);
  CHECK(!b.p.sleep_ready);
  CHECK(b.ppsWords.empty());   // nothing asked before the buck's barrier is taken
  b.tick(100);                 // the buck took 500 / 500 (autoBuck)
  CHECK(b.s.state == PIANO_CPS_SET_START);
  b.tick(100);
  CHECK(b.ppsWords.size() == 1);
  // 2 x 3900 + 300 = 8100 mV at min(APDO 5000, the standard budget 5000)
  CHECK(b.ppsWords[0] == ((8100u / 20) << 16 | 5000u / 50));
  b.tick(100);
  CHECK(b.s.state == PIANO_CPS_ENABLING);
  b.tick(100);
  CHECK(b.actions.size() && b.actions.back() == PIANO_CP_ACT_START);
  CHECK(b.d.busocp_ma == PIANO_CP_BUSOCP_START_MA);
  b.tick(100);
  CHECK(b.s.state == PIANO_CPS_OPENING);
  CHECK(b.p.events & PIANO_CPS_EV_ENABLED);
  // opening: 140 mA at the start, +280 mA per 40 mV step: two steps, 200 ms apart, the first 300 ms after the enable
  const size_t before = b.ppsWords.size();
  CHECK(b.until(PIANO_CPS_CC, 3000));
  CHECK(b.ppsWords.size() - before >= 1 && b.ppsWords.size() - before <= 10);
  CHECK(b.p.events & PIANO_CPS_EV_OPENED);
  // CC: regulate towards the target (normal table 8000 mA, the stage limit 3800 on the bus)
  b.ticks(200);
  CHECK(b.s.state == PIANO_CPS_CC);
  CHECK(b.s.ibus_target_ma == PIANO_CPS_STAGE_IBUS_MA);
  CHECK(b.d.adc.ibus_ma >= 3800 - 200 - 140 && b.d.adc.ibus_ma <= 3800 + 100 + 140);
  CHECK(b.d.busocp_ma == 4750);   // 3800 + 1000, on the 250 mA register step
  CHECK(b.raisesAboveCeiling == 0);
  // CV and the taper: the higher pack rises to the limit
  b.y.vbat_max_mv = 4350; b.y.vbat_min_mv = 4340;
  CHECK(b.until(PIANO_CPS_CV, 2000));
  b.y.vbat_max_mv = 4380;
  bool stopped = false;
  for (int i = 0; i < 600 && !stopped; i++) {b.tick(100); if (b.s.state == PIANO_CPS_STOPPING || b.s.state == PIANO_CPS_IDLE) stopped = true;}
  CHECK(stopped);
  CHECK(b.s.exit_reason == PIANO_CPS_EXIT_TAPER);
  // the pump is off before the source is handed back; the hand-back carries the last word taken
  bool handback = b.p.handback;
  for (int i = 0; i < 10 && !handback; i++) {b.tick(100); handback = b.p.handback;}
  CHECK(handback);
  CHECK(b.p.handback_prop == PIANO_CHG_PPS && b.p.handback_value == b.y.pps_taken_word);
  CHECK(!b.d.switching);
  CHECK(b.s.state == PIANO_CPS_IDLE);
  b.tick(100);
  CHECK(!b.p.owns_volt && b.p.sleep_ready);
  // no new session for 300 s after a taper
  for (int i = 0; i < 200; i++) b.tick(1000);
  CHECK(b.s.state == PIANO_CPS_IDLE);
  CHECK(b.writesWhilePending == 0 && b.startsWhileUnarmed == 0);
}

// a session in CC, then one condition: the reason, STOP first, the hand-back only once the pump reads off
static void oneExit(const char *what, uint8_t want, bool fault, void (*f)(Bench &)) {
  Bench b;
  CHECK(b.toCC());
  f(b);
  b.contractLive = false;
  b.tick(100);
  b.pumpObeys = true;   // the frozen readings did their job; the pump obeys the STOP from here
  const bool stoppedFirst = b.s.state == PIANO_CPS_STOPPING || (b.s.state != PIANO_CPS_CC && !b.d.switching);
  if (b.s.exit_reason != want) std::printf("  %s: exit %s, want %s\n", what, piano_cp_exit_name(b.s.exit_reason), piano_cp_exit_name(want));
  CHECK(b.s.exit_reason == want);
  CHECK(stoppedFirst);
  CHECK(std::find(b.actions.begin(), b.actions.end(), PIANO_CP_ACT_STOP) != b.actions.end());
  bool handback = false;
  for (int i = 0; i < 30 && !handback; i++) {if (b.p.handback) handback = true; else b.tick(100);}
  if (!handback) std::printf("  %s: no hand-back (state %s)\n", what, piano_cp_state_name(b.s.state));
  CHECK(handback);
  CHECK(!b.d.switching);
  CHECK(fault ? b.s.faults_this_attach == 1 : b.s.faults_this_attach == 0);
}

static void exits() {
  oneExit("detach", PIANO_CPS_EXIT_DETACH, false, [](Bench &b) {b.y.attached = 0;});
  oneExit("sleep", PIANO_CPS_EXIT_SLEEP, false, [](Bench &b) {b.y.sleep_pending = 1;});
  oneExit("disarm", PIANO_CPS_EXIT_DISARM, false, [](Bench &b) {b.d.armed = 0;});
  oneExit("health", PIANO_CPS_EXIT_HEALTH, false, [](Bench &b) {b.y.health_hold = 1;});
  oneExit("gauge", PIANO_CPS_EXIT_GAUGE, false, [](Bench &b) {b.y.gauges_valid = 0;});
  oneExit("cells", PIANO_CPS_EXIT_GAUGE, false, [](Bench &b) {b.y.cells_valid = 0;});
  oneExit("warm 45", PIANO_CPS_EXIT_TBAT, false, [](Bench &b) {b.y.t_warm_dc = 445;});
  oneExit("cool 14", PIANO_CPS_EXIT_TBAT, false, [](Bench &b) {b.y.t_cool_dc = 149;});
  oneExit("connector 50", PIANO_CPS_EXIT_CONN, false, [](Bench &b) {b.y.t_conn_dc = 495;});
  oneExit("connector lost", PIANO_CPS_EXIT_CONN, false, [](Bench &b) {b.y.conn_valid = 0;});
  oneExit("MiPPS cleanup", PIANO_CPS_EXIT_CONTRACT, false, [](Bench &b) {b.y.mipps_cleanup_pending = 1;});
  oneExit("not PPS", PIANO_CPS_EXIT_CONTRACT, false, [](Bench &b) {b.y.contract_pps = 0;});
  oneExit("pump fault", PIANO_CPS_EXIT_PUMP_FAULT, true, [](Bench &b) {b.d.faults = PIANO_CP_FAULT_HW;});
  oneExit("bus fault", PIANO_CPS_EXIT_PUMP_FAULT, true, [](Bench &b) {b.d.faults = PIANO_CP_FAULT_BUS;});
  oneExit("late kick", PIANO_CPS_EXIT_WATCHDOG, true, [](Bench &b) {b.d.faults = PIANO_CP_FAULT_KICK_LATE;});
  oneExit("tdie 125", PIANO_CPS_EXIT_TDIE, true, [](Bench &b) {b.d.adc.tdie_dc = 1245; b.pumpObeys = true;});
  oneExit("cell 7 A", PIANO_CPS_EXIT_CELL_OCP, true, [](Bench &b) {b.pumpObeys = false; b.y.ibat_b_ma = 7000;});
  oneExit("bus high", PIANO_CPS_EXIT_BUS_HIGH, true, [](Bench &b) {b.pumpObeys = false; b.d.adc.vbus_mv = 2 * 3900 + 2001;});
  oneExit("pump stopped itself", PIANO_CPS_EXIT_PUMP_FAULT, true, [](Bench &b) {b.d.switching = 0;});
  oneExit("pump readings lost", PIANO_CPS_EXIT_GAUGE, false, [](Bench &b) {b.pumpObeys = false; b.d.adc.valid = 0;});
  // the input lost, seen only on the pump's own VBUS (the stale system view still attached): a detach, STOP at once
  oneExit("input lost on the pump ADC", PIANO_CPS_EXIT_DETACH, false, [](Bench &b) {b.pumpObeys = false; b.d.adc.vbus_mv = 300;});
  // the contract lost for 3 s
  {Bench b; CHECK(b.toCC()); b.contractLive = false; b.y.contract_valid = 0; b.ticks(20); CHECK(b.s.state == PIANO_CPS_CC); b.ticks(15);
   CHECK(b.s.exit_reason == PIANO_CPS_EXIT_CONTRACT);}
  // soft cell OCP: one pack 2 A above its share, two polls
  {Bench b; CHECK(b.toCC()); b.pumpObeys = false; b.y.ibat_a_ma = 3800 + 2001; b.y.ibat_b_ma = 1000;
   b.ticks(8); CHECK(b.s.state == PIANO_CPS_CC || b.s.state == PIANO_CPS_STOPPING || b.s.state == PIANO_CPS_IDLE);
   b.ticks(10); CHECK(b.s.exit_reason == PIANO_CPS_EXIT_CELL_OCP);}
  // the pump lost by the driver: latched at once, not unlatched by a new attach while it stays lost
  {Bench b; CHECK(b.toCC()); b.d.lost = 1; b.ticks(10); CHECK(b.s.state == PIANO_CPS_LATCHED); CHECK(b.s.exit_reason == PIANO_CPS_EXIT_LOST);
   b.y.attach_seq = 2; b.ticks(3); CHECK(b.s.state == PIANO_CPS_LATCHED);}
  // errors: a request never answered, 11 times
  {Bench b; CHECK(b.toCC()); b.autoAck = false; b.ticks(400);
   CHECK(b.s.exit_reason == PIANO_CPS_EXIT_ERRORS || b.s.exit_reason == PIANO_CPS_EXIT_CONTRACT);
   CHECK(b.s.state != PIANO_CPS_CC);}
}

static void startFailures() {
  // the start word never taken: two tries, then START_FAIL (a fault), the source handed back (the word was asked)
  {Bench b; b.autoAck = false; b.run(); CHECK(b.until(PIANO_CPS_SET_START, 12000)); b.ticks(80);
   CHECK(b.s.exit_reason == PIANO_CPS_EXIT_START_FAIL); CHECK(b.s.faults_this_attach == 1);
   CHECK(b.actions.empty());}   // never a START
  // CHG_EN never reads set: ENABLE_FAIL, STOP asked
  {Bench b; b.pumpObeys = false; b.run(); CHECK(b.until(PIANO_CPS_ENABLING, 15000)); b.tick(100); b.d.switching = 0; b.ticks(15);
   CHECK(b.s.exit_reason == PIANO_CPS_EXIT_ENABLE_FAIL);}
  // the opening never reaches 600 mA: 10 tries
  {Bench b; b.pumpOffsetMv = 2000; b.run(); CHECK(b.until(PIANO_CPS_OPENING, 15000)); b.ticks(40);
   CHECK(b.s.exit_reason == PIANO_CPS_EXIT_OPEN_FAIL);}
  // the buck never takes its barrier: BUCK (no fault), nothing asked
  {Bench b; b.autoBuck = false; b.run(); CHECK(b.until(PIANO_CPS_BUCK_HOLD, 12000)); b.ticks(210);
   CHECK(b.s.exit_reason == PIANO_CPS_EXIT_BUCK); CHECK(b.ppsWords.empty()); CHECK(b.s.faults_this_attach == 0);}
}

static void latch() {
  Bench b;
  for (int n = 0; n < 3; n++) {
    CHECK(b.toCC() || b.s.state == PIANO_CPS_LATCHED);
    b.d.faults = PIANO_CP_FAULT_HW;
    b.ticks(10);
    if (n < 2) {CHECK(b.s.state == PIANO_CPS_IDLE); b.now += 61000;}
  }
  CHECK(b.s.state == PIANO_CPS_LATCHED);
  CHECK(b.p.latched);
  b.ticks(400);
  CHECK(b.s.state == PIANO_CPS_LATCHED);
  b.y.attach_seq = 2;   // a new attach
  b.tick(100);
  CHECK(b.s.state != PIANO_CPS_LATCHED);
  CHECK(b.p.events & PIANO_CPS_EV_UNLATCHED);
}

static void stuck() {
  Bench b;
  CHECK(b.toCC());
  b.pumpObeys = false;   // CHG_EN keeps reading set
  b.y.attached = 1; b.y.health_hold = 1;
  b.tick(100);
  CHECK(b.s.state == PIANO_CPS_STOPPING);
  CHECK(!b.p.handback);
  CHECK(!b.p.sleep_ready);
  b.ticks(10);
  CHECK(b.s.state == PIANO_CPS_STOPPING);
  CHECK(!b.p.handback);   // the source is held while the pump may still switch
  b.ticks(12);
  CHECK(b.s.state == PIANO_CPS_LATCHED);
  CHECK(b.s.exit_reason == PIANO_CPS_EXIT_STUCK);
  unsigned stops = 0;
  for (int a : b.actions) stops += a == PIANO_CP_ACT_STOP;
  CHECK(stops >= 3);   // asked again every 500 ms
}

static void guardStuck() {
  Bench b;
  CHECK(b.toCC());
  b.y.health_hold = 1;
  b.tick(100);
  CHECK(b.s.state == PIANO_CPS_STOPPING);
  b.ticks(3);
  b.d.faults = PIANO_CP_FAULT_STOP_STUCK;   // the guard found CHG_EN still set (and stopped it again)
  b.tick(100);
  CHECK(b.s.state == PIANO_CPS_LATCHED);
  CHECK(b.s.exit_reason == PIANO_CPS_EXIT_STUCK);
}

static void derateTdie() {
  Bench b;
  CHECK(b.toCC());
  b.ticks(100);
  const uint32_t full = b.s.ibat_target_ma;
  b.d.adc.tdie_dc = 1205;
  b.ticks(7);    // one CC step (700 ms)
  CHECK(b.s.ibat_target_ma <= full - 200);
  CHECK(b.p.events & PIANO_CPS_EV_DERATE || b.s.derate_ma < PIANO_CPS_IBAT_MAX_MA);
  // held hot, the target keeps coming down 200 mA a poll; below the 2500 mA exit current the buck takes over (no fault)
  uint32_t last = b.s.ibat_target_ma;
  bool monotone = true;
  for (int i = 0; i < 700 && b.s.state == PIANO_CPS_CC; i++) {b.tick(100); if (b.s.ibat_target_ma > last) monotone = false; last = b.s.ibat_target_ma;}
  CHECK(monotone);
  CHECK(b.s.exit_reason == PIANO_CPS_EXIT_TAPER);
  CHECK(b.s.faults_this_attach == 0);
  Bench r;
  CHECK(r.toCC());
  r.d.adc.tdie_dc = 1205; r.ticks(14);
  const uint32_t low = r.s.derate_ma;
  r.d.adc.tdie_dc = 1140; r.ticks(7);
  CHECK(r.s.derate_ma == low + 50);   // a quarter of the 200 mA step
}

static void budget() {
  Bench b;
  b.y.apdo_ma = 3000;
  CHECK(piano_cp_ibus_budget(&b.y, &b.d, 9000) == 3000);
  b.y.apdo_ma = 6100;
  CHECK(piano_cp_ibus_budget(&b.y, &b.d, 9000) == PIANO_CPS_STAGE_IBUS_MA);   // the stage limit binds below both budgets
  b.d.mipps_verified = 0; b.d.source_budget_ma = 6000;   // not verified: 5000 whatever the field says
  b.y.apdo_ma = 6000;
  piano_cp_in d2 = b.d; d2.mipps_verified = 1;
  CHECK(piano_cp_ibus_budget(&b.y, &b.d, 9000) == piano_cp_ibus_budget(&b.y, &d2, 9000));   // both above the stage limit
  b.y.src_kind = PIANO_CPS_SRC_QC35;
  CHECK(piano_cp_ibus_budget(&b.y, &b.d, 9000) == 2100);   // 4200 / 2
  b.y.src_kind = PIANO_CPS_SRC_QC3B;
  CHECK(piano_cp_ibus_budget(&b.y, &b.d, 9000) == 2500);
  b.y.src_kind = PIANO_CPS_SRC_NONE;
  CHECK(piano_cp_ibus_budget(&b.y, &b.d, 9000) == 0);
  // the request's current: the standard budget without MiPPS, the APDO's when below it
  Bench v; v.y.apdo_ma = 6000; v.run(); CHECK(v.until(PIANO_CPS_ENABLING, 15000));
  CHECK(!v.ppsWords.empty() && (v.ppsWords[0] & 0xffffu) * 50u == 5000u);
  Bench w; w.y.apdo_ma = 6000; w.d.mipps_verified = 1; w.d.source_budget_ma = 6000; w.run(); CHECK(w.until(PIANO_CPS_ENABLING, 15000));
  CHECK(!w.ppsWords.empty() && (w.ppsWords[0] & 0xffffu) * 50u == 6000u);
}

static void qc3() {
  Bench b;
  b.c.qc3 = 1;
  b.y.src_kind = PIANO_CPS_SRC_QC3B;
  b.y.contract_pps = 0; b.y.vbus_mv = 5000; b.y.apdo_ma = 0; b.y.apdo_min_mv = b.y.apdo_max_mv = 0;
  b.run();
  CHECK(b.until(PIANO_CPS_SET_START, 12000));
  CHECK(b.until(PIANO_CPS_ENABLING, 8000));
  // 8100 mV from 5000 in 200 mV pulses: 16
  CHECK(b.y.qc_pulses_taken == 16);
  CHECK(b.ppsWords.empty());
  CHECK(b.until(PIANO_CPS_CC, 5000));
  b.ticks(100);
  CHECK(b.s.ibat_target_ma <= PIANO_CPS_QC3B_IBAT_MA);
  b.y.attached = 0;
  b.ticks(3);
  CHECK(b.s.exit_reason == PIANO_CPS_EXIT_DETACH);
  bool hb = false;
  for (int i = 0; i < 12; i++) {if (b.p.handback) {hb = true; CHECK(b.p.handback_prop == PIANO_CHG_QC_VOLT);} b.tick(100);}
  CHECK(hb || b.s.state == PIANO_CPS_IDLE);
}

static void allowlist() {
  PianoCpConfig c; piano_cp_config_default(&c);
  CHECK(piano_cp_write_allowed(&c, PIANO_CHG_PPS, (8100u / 20) << 16 | 5000u / 50, 4));
  CHECK(piano_cp_write_allowed(&c, PIANO_CHG_PPS, (11000u / 20) << 16 | 6000u / 50, 4));
  CHECK(!piano_cp_write_allowed(&c, PIANO_CHG_PPS, (11020u / 20) << 16 | 5000u / 50, 4));
  CHECK(!piano_cp_write_allowed(&c, PIANO_CHG_PPS, (4980u / 20) << 16 | 5000u / 50, 4));
  CHECK(!piano_cp_write_allowed(&c, PIANO_CHG_PPS, (9000u / 20) << 16 | 6050u / 50, 4));
  CHECK(!piano_cp_write_allowed(&c, PIANO_CHG_PPS, (9000u / 20) << 16 | 950u / 50, 4));
  CHECK(!piano_cp_write_allowed(&c, PIANO_CHG_PPS, (9000u / 20) << 16 | 5000u / 50, 2));
  CHECK(!piano_cp_write_allowed(&c, PIANO_CHG_QC_PULSE, PIANO_CHG_QC_UP, 1));   // QC only with cfg.qc3
  c.qc3 = 1;
  CHECK(piano_cp_write_allowed(&c, PIANO_CHG_QC_PULSE, PIANO_CHG_QC_UP, 1));
  CHECK(piano_cp_write_allowed(&c, PIANO_CHG_QC_PULSE, PIANO_CHG_QC_DOWN, 1));
  CHECK(!piano_cp_write_allowed(&c, PIANO_CHG_QC_PULSE, 0x30, 1));
  CHECK(!piano_cp_write_allowed(&c, PIANO_CHG_ICL, 3000, 4));
  CHECK(!piano_cp_write_allowed(&c, PIANO_CHG_PD_FIXED, 9000, 4));
}

static void sleepHandover() {
  Bench b;
  CHECK(b.toCC());
  b.pumpObeys = false;
  b.y.sleep_pending = 1; b.y.awake = 0;
  b.tick(100);
  CHECK(b.s.state == PIANO_CPS_STOPPING);
  CHECK(!b.p.sleep_ready);
  CHECK(b.p.out.action == PIANO_CP_ACT_STOP);
  b.d.switching = 0;   // CHG_EN reads clear
  b.tick(100);
  CHECK(!b.p.handback);   // the driver's guard (500 ms) re-reads CHG_EN first
  CHECK(!b.p.sleep_ready);
  bool hb = false;
  for (int i = 0; i < 10 && !hb; i++) {b.tick(100); hb = b.p.handback; if (hb) CHECK(!b.p.sleep_ready);}
  CHECK(hb);
  b.tick(100);
  CHECK(b.p.sleep_ready);
  // awake again: no session before the settle
  b.y.sleep_pending = 0; b.y.awake = 1; b.pumpObeys = true;
  b.ticks(50);
  CHECK(b.s.state == PIANO_CPS_IDLE);
  CHECK(b.until(PIANO_CPS_BUCK_HOLD, 8000));
}

static void requestsBounded() {
  // a long session with the pack voltage creeping up: every request within the APDO, 2 x VBAT + 2000 and 11 V
  Bench b;
  b.y.apdo_max_mv = 9500;
  CHECK(b.toCC());
  for (int i = 0; i < 3000 && b.s.state == PIANO_CPS_CC; i++) {
    if (i % 50 == 0 && b.y.vbat_max_mv < 4300) {b.y.vbat_max_mv += 10; b.y.vbat_min_mv += 10;}
    b.tick(100);
  }
  CHECK(b.raisesAboveCeiling == 0);
  for (uint32_t w : b.ppsWords) CHECK((w >> 16) * 20u <= 9500u);
}

// the buck side: piano_charge_cp_handback puts the strict session on its barrier, then vSafe5V, never a raise first
static void buckHandback() {
  PianoChargeConfig c;
  piano_charge_config_default(&c);
  c.strict_contract = 1; c.hv = 2; c.hv_order = 1; c.hv_caps = 1;
  PianoChargeState s;
  piano_charge_init(&s);
  s.started = 1; s.attached = 1; s.type = PIANO_CHG_T_PPS; s.icl_set = 500; s.fcc_set = 500;
  s.enable_set = 1; s.fv_set = 4400;
  const uint32_t word = (8600u / 20) << 16 | 5000u / 50;
  piano_charge_cp_handback(&s, PIANO_CHG_PPS, word, 50000);
  CHECK(s.volt_set_prop == PIANO_CHG_PPS && s.volt_set_value == word);
  CHECK(s.volt_want_prop == PIANO_CHG_PD_FIXED && s.volt_want_value == 5000);
  CHECK(s.contract_transition && s.transition_ma == 500);
  CHECK(s.contract_mode_change);
  CHECK(s.hv == PIANO_CHG_HV_OFF);
  CHECK(s.hv_retry_ms == 50000 + 15000);
  PianoChargeState q;
  piano_charge_init(&q);
  q.hv = PIANO_CHG_HV_BLOCKED;
  piano_charge_cp_handback(&q, PIANO_CHG_QC_VOLT, 8100, 1000);
  CHECK(q.volt_want_prop == PIANO_CHG_QC_VOLT && q.volt_want_value == 5000);
  CHECK(q.hv == PIANO_CHG_HV_BLOCKED && q.hv_qc);
  CHECK(!q.contract_mode_change);
}

// the glue: the system side from the buck's input and state
static void glue() {
  PianoChargeConfig c;
  piano_charge_config_default(&c);
  c.strict_contract = 1; c.hv = 2; c.limit = 1; c.stop_pct = 80; c.resume_pct = 75;
  PianoChargeInput in;
  std::memset(&in, 0, sizeof in);
  in.now_ms = 100000;
  in.valid = PIANO_CHG_IN_CONTRACT | PIANO_CHG_IN_PDOS | PIANO_CHG_IN_BUS | PIANO_CHG_IN_GAUGES | PIANO_CHG_IN_CONN1 |
             PIANO_CHG_IN_CONN2 | PIANO_CHG_IN_IBUS;
  in.generation = 3; in.contract_generation = 7; in.contract_link_generation = 3; in.contract_protocol = 1;
  in.contract_mv = 9000; in.contract_ma = 1600;
  in.pdos = 4;
  const uint32_t pdo[4][3] = {{5000, 5000, 3000}, {9000, 9000, 3000}, {3300, 11000, 6100}, {3300, 16000, 3000}};
  std::memcpy(in.pdo, pdo, sizeof pdo);
  in.bus_mv = 8900; in.ibus_ma = 1500;
  in.soc_max_pct = 60; in.soc_min_pct = 58; in.vbat_max_mv = 3950; in.t_warm_dc = 300; in.t_cool_dc = 280; in.gauge_ms = 100000;
  in.t_conn1_dc = 310; in.t_conn2_dc = 330;
  PianoChargeState s;
  piano_charge_init(&s);
  s.attached = 1; s.icl_set = 1600; s.fcc_set = 3000;
  PianoCpSys y;
  piano_charge_cp_sys(&c, &in, &s, &y);
  CHECK(y.contract_valid && !y.contract_pps && y.contract_mv == 9000);
  CHECK(y.src_kind == PIANO_CPS_SRC_PPS);
  CHECK(y.apdo_max_mv == 16000 || y.apdo_max_mv == 11000);   // the one reaching furthest (2 A or more)
  CHECK(y.apdo_ma >= 2000);
  CHECK(y.soc_entry_max == 77 && !y.health_hold);
  CHECK(y.conn_valid && y.t_conn_dc == 330);
  CHECK(!y.cells_valid);   // the worker's
  CHECK(y.buck_icl_set_ma == 1600);
  in.soc_max_pct = 80;
  piano_charge_cp_sys(&c, &in, &s, &y);
  CHECK(y.health_hold);
  s.hold = 1; in.soc_max_pct = 76;
  piano_charge_cp_sys(&c, &in, &s, &y);
  CHECK(y.health_hold);
  in.contract_notify_pending = 1;   // a notification in flight hides the contract
  piano_charge_cp_sys(&c, &in, &s, &y);
  CHECK(!y.contract_valid && y.src_kind == PIANO_CPS_SRC_NONE);
}

// charge/mipps2 ladder (UBUNTU-COMPARE.md): the pump never starts while the buck's ladder holds it (not at a confirmed 5 V after
// MiPPS or a confirmed 9 V), and a PPS step up goes out only after the previous word is taken with the bus following it.
static void ladder() {
  {Bench b; b.y.ladder_hold = 1; b.ticks(300);
   CHECK(b.s.state == PIANO_CPS_IDLE && b.ppsWords.empty());
   CHECK(!piano_cp_may_start(&b.c, &b.y, &b.d, &b.s));
   b.y.ladder_hold = 0; CHECK(piano_cp_may_start(&b.c, &b.y, &b.d, &b.s));
   CHECK(b.until(PIANO_CPS_CC, 40000));}
  {Bench b; CHECK(b.toCC());
   uint32_t top = 0;
   for (uint32_t w : b.ppsWords) top = std::max(top, (w >> 16) * 20);
   b.busLag = true; b.y.vbus_mv = (int32_t)b.s.req_mv - 1500;   // the last step's bus never arrives
   const size_t n = b.ppsWords.size();
   bool held = false;
   for (unsigned i = 0; i < 60; ++i) {b.tick(100); if (b.p.events & PIANO_CPS_EV_UNCONFIRMED) held = true;}
   for (size_t i = n; i < b.ppsWords.size(); ++i) CHECK((b.ppsWords[i] >> 16) * 20 <= top);   // never higher than before
   CHECK(held);
   CHECK(!piano_cp_may_start(&b.c, &b.y, &b.d, &b.s));}   // not idle: no second start
}

int main() {
  ladder();
  gates();
  happyPath();
  exits();
  startFailures();
  latch();
  stuck();
  guardStuck();
  derateTdie();
  budget();
  qc3();
  allowlist();
  sleepHandover();
  requestsBounded();
  buckHandback();
  glue();
  std::printf("cp_session_host: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
