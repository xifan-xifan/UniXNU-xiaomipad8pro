// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: c++ -std=c++20 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all -D_Static_assert=static_assert -I.. -I../../include -x c++ cp_safety_invariants_host.cpp ../ChargePolicy.c ../ChargePumpSession.c ../ChargeMipps.c -o /tmp/cp_safety_invariants_host && /tmp/cp_safety_invariants_host
//
// B4 offline check. Only the observations, firmware acknowledgements and SC8541 bus are modeled.
// The owner-attended gate remains on hold. The host worker orders a CP pass before its buck pass.
#include "ChargeLink.h"
#include "ChargeMipps.h"
#include "ChargePumpImpl.h"
#include "ChargePumpSession.h"
#include "cp_sim.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

using namespace pianopower;

static const char *scenario;
static constexpr uint32_t kFixed5Rdo = (1u << 28) | (300u << 10) | 300u;
#define CHECK(expr) do { \
  if (!(expr)) { std::fprintf(stderr, "%s (line %d): %s\n", scenario, __LINE__, #expr); std::exit(1); } \
} while (0)

struct Bench {
  cpsim::Plant plant;
  cpsim::LineBus wire{plant};
  I2cBus bus{wire.io()};
  Sc8541 driver{{&plant, [](void *c) { return static_cast<cpsim::Plant *>(c)->now; }}};
  PianoChargeContract contract{};
  PianoMca mca{};
  PianoChargeConfig cfg{};
  PianoChargeState buck{};
  PianoChargeInput in{};
  PianoChargePlan charge{};
  PianoCpConfig cpCfg{};
  PianoCpState cp{};
  PianoCpPlan plan{};
  PianoMipps mipps{};
  GaugeReading packs[2]{};
  piano_board_temps board{};
  uint32_t pdos[4][3] = {{5000, 5000, 3000}, {9000, 9000, 3000}, {12000, 12000, 2250}, {3300, 11000, 5000}};
  uint32_t rdo = kFixed5Rdo;
  uint32_t ppsTaken = 0, icl = 100, fcc = 500, enable = 1, fv = 4400;
  unsigned starts = 0, stops = 0, fixed5Writes = 0, ppsWrites = 0;
  bool boardReadable = true;
  uint64_t gaugeAge = 0;

  Bench() {
    plant.now = 1000;
    plant.src.apdo_max_ma = 5000;
    plant.attach();
    piano_contract_init(&contract);
    piano_contract_link(&contract, 1);
    piano_contract_allow_firmware_pdos(&contract, 1);
    piano_charge_config_default(&cfg);
    cfg.strict_contract = 1;
    cfg.kick = 0;
    CHECK(piano_charge_config_limit(&cfg, 1, cfg.stop_pct, cfg.resume_pct) == 0);
    piano_charge_init(&buck);
    piano_cp_config_default(&cpCfg);
    piano_cp_init(&cp);
    mca.links = 1;
    mca.board_allowed = 1;
    mca.board_done = 7;
    for (auto &p : packs) {
      p.ok = true;
      p.socPct = 40;
      p.tempDeciC = 300;
      p.voltageMv = 3900;
      p.currentMa = 300;
    }
    board.version = PIANO_BOARD_TEMPS_VERSION;
    board.valid = 15;
    board.state = PIANO_BOARD_TEMPS_RUNNING;
    for (auto &t : board.milli_c) t = 30000;
    CHECK(driver.probe(bus));
  }

  bool fetEnabled() const {
    return (plant.r[PIANO_CP_REG_CTRL2] & (PIANO_CP_CHG_EN | PIANO_CP_EN_BYPASS)) != 0;
  }

  void observe() {
    // Start at the publication boundary of an agreeing UCSI status pair.
    piano_contract_offer_pdos(&contract, pdos, mca.pdos, plant.attached);
    contract.stage_pdos = 0;  // the PPM lacks GET_PDOS
    contract.stage_connected = contract.stage_sink = contract.stage_pd = plant.attached;
    contract.stage_rdo = plant.attached ? rdo : 0;
    piano_contract_publish(&contract, plant.now);
    const auto put = [&](unsigned field, int32_t value) {
      mca.valid |= 1u << field;
      mca.value[field] = value;
    };
    put(McaBusVoltage, int32_t(plant.set_mv) * 1000);
    put(McaType, PIANO_CHG_T_PPS);
    put(McaIcl, int32_t(icl));
    put(McaChargeEnable, int32_t(enable));
    put(McaTermVolt, piano_charge_fv_raw(fv));
    put(McaVerify, 0);
    put(McaLpd, 0);
    mca.valid &= ~(1u << McaPdos);
    if (mca.pdos) {
      put(McaPdos, mca.pdos);
      for (unsigned i = 0; i < mca.pdos; ++i)
        for (unsigned j = 0; j < 3; ++j) mca.pdo[i][j] = pdos[i][j];
    }
    auto freshBoard = board;
    // The live CP worker's freshness boundary for board thermistors.
    for (unsigned i = 0; i < 4; ++i)
      if (board.age_ms[i] > 1500) freshBoard.valid &= ~(1u << i);
    LinkView view;
    view.nowMs = plant.now;
    view.contract = &contract;
    view.cpOff = !fetEnabled() && !plan.owns_volt && !plan.handback;
    view.safetyFresh = view.dtptValid = true;
    view.packs = packs;
    view.packsFresh = gaugeAge <= 45000;
    view.packsMs = plant.now > gaugeAge ? plant.now - gaugeAge : 0;
    view.board = boardReadable ? &freshBoard : nullptr;
    view.mippsBusy = piano_mipps_cleanup_pending(&mipps);
    ChargeLink::fill(in, mca, view);
  }

  void pumpStep() {
    driver.beforeBusTransaction(bus);
    observe();
    PianoCpSys sys{};
    piano_charge_cp_sys(&cfg, &in, &buck, &sys);
    sys.attach_seq = plant.attach_seq;
    sys.attached = plant.attached;
    sys.cells_valid = packs[0].ok && packs[1].ok && gaugeAge <= 1500;
    sys.vbat_min_mv = std::min(packs[0].voltageMv, packs[1].voltageMv);
    sys.ibat_a_ma = packs[0].currentMa;
    sys.ibat_b_ma = packs[1].currentMa;
    sys.pps_taken_word = ppsTaken;
    sys.mipps_cleanup_pending = piano_mipps_cleanup_pending(&mipps);
    piano_cp_in input{};
    driver.fill(&input);
    if (input.switching) driver.readAdc(bus, &input.adc);
    input.faults = driver.takeFaults();
    piano_mipps_fill_cp_in(&mipps, cpCfg.mipps, &input);
    piano_cp_decide(&cpCfg, &sys, &input, &cp, &plan);
    switch (plan.out.action) {
      case PIANO_CP_ACT_NONE: break;
      case PIANO_CP_ACT_START:
        ++starts;
        CHECK(driver.start(bus, plan.out.busocp_ma));
        break;
      case PIANO_CP_ACT_STOP:
        ++stops;
        driver.stop(bus, plan.out.stop_reason);
        break;
      case PIANO_CP_ACT_SET_BUSOCP:
        CHECK(driver.setBusOcp(bus, plan.out.busocp_ma));
        break;
    }
    if (plan.out.req_valid & PIANO_CP_REQ_PPS) {
      CHECK(piano_cp_write_allowed(&cpCfg, PIANO_CHG_PPS, plan.pps_word, 4));
      acceptPps(plan.pps_word);
      ppsTaken = plan.pps_word;
    }
    if (plan.handback) {
      CHECK(!fetEnabled() && !plan.owns_volt);
      piano_charge_cp_handback(&buck, plan.handback_prop, plan.handback_value, plant.now);
      ppsTaken = 0;
    }
    CHECK(plant.write_violations.empty());
  }

  void acceptPps(uint32_t word) {
    ++ppsWrites;
    plant.mode = cpsim::Plant::Mode::pps;
    plant.set_mv = (word >> 16) * 20;
    plant.lim_ma = (word & 0xffffu) * 50;
    const unsigned obj = piano_charge_listed_object(&in, 1, plant.set_mv, plant.lim_ma);
    CHECK(obj);
    rdo = (obj << 28) | (plant.set_mv / 20 << 9) | (plant.lim_ma / 50);
  }

  void buckStep() {
    observe();
    piano_charge_decide(&cfg, &in, &buck, &charge);
    for (unsigned i = 0; i < charge.writes; ++i) {
      const auto &w = charge.write[i];
      CHECK(piano_charge_write_allowed(&cfg, w.property, w.value, w.bytes));
      switch (w.property) {
        case PIANO_CHG_ICL: icl = w.value; break;
        case PIANO_CHG_FCC: fcc = w.value; break;
        case PIANO_CHG_ENABLE:
          if (!w.value) CHECK(!fetEnabled());
          enable = w.value;
          break;
        case PIANO_CHG_FV: fv = w.value; break;
        case PIANO_CHG_PD_FIXED:
          CHECK(w.value == 5000 && !fetEnabled() && !plan.owns_volt && icl <= 500 && fcc <= 500);
          if (plant.attached) {
            ++fixed5Writes;
            plant.mode = cpsim::Plant::Mode::fixed5;
            plant.set_mv = 5000;
            plant.lim_ma = 3000;
            rdo = kFixed5Rdo;
          }
          break;
        case PIANO_CHG_PPS:
          CHECK(!fetEnabled() && !plan.owns_volt);
          acceptPps(w.value);
          break;
        default: CHECK(false);
      }
      piano_charge_ack(&buck, w.property, w.value, 0);
    }
  }

  void ticks(unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
      for (unsigned j = 0; j < 10; ++j) plant.step();  // 100 ms, no wall-clock sleep
      pumpStep();
      buckStep();
    }
  }

  void arm() {
    mca.pdos = 4;
    driver.arm.bootArg = driver.arm.sysctl = true;
  }

  void openPump(unsigned maxTicks = 200) {
    arm();
    // Enter through the session's acknowledged reduced-current barrier.
    cfg.icl_cap_ma = cfg.fcc_cap_ma = 500;
    const unsigned previousStarts = starts;
    for (unsigned i = 0; i < maxTicks && !(cp.state == PIANO_CPS_OPENING && plant.active); ++i) ticks(1);
    CHECK(cp.state == PIANO_CPS_OPENING && fetEnabled() && plant.active && starts == previousStarts + 1);
    CHECK(contract.valid && contract.protocol == PIANO_CONTRACT_PPS && plant.set_mv > 5000);
  }

  void checkStop(unsigned reason) const {
    CHECK(plan.state == PIANO_CPS_STOPPING && plan.out.action == PIANO_CP_ACT_STOP);
    CHECK(plan.exit_reason == reason);
    CHECK(!plan.handback && plan.owns_volt);
    CHECK(!(plan.out.req_valid & PIANO_CP_REQ_PPS));
    CHECK(!fetEnabled() && !plant.active);
  }

  void checkFallback() {
    const unsigned priorFixed5Writes = fixed5Writes;
    const unsigned priorStops = stops;
    ticks(20);  // guard/readback and buck barrier must finish within two simulated seconds
    CHECK(!fetEnabled() && !plant.active && !plan.owns_volt && !plan.handback);
    CHECK(plant.set_mv == 5000 && fixed5Writes == priorFixed5Writes + 1 && icl <= 1500 && fcc <= 1500);
    CHECK(stops <= priorStops + 1);
  }
};

static void defaults() {
  const LinkOptions options;
  CHECK(options.hv == 0 && !options.health && options.stopPct == 80 && options.resumePct == 75);
  // The worker's boot-argument defaults live in the kext translation unit.
  std::ifstream source("../PianoPower.cpp");
  CHECK(source.is_open());
  const std::string code{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
  for (const char *setting : {
         "gArgs.hv = bootArg(\"piano-power-hv\", 0);",
         "gArgs.b4pd = bootArg(\"piano-power-b4pd\", 0) == 1;",
         "gArgs.cp = bootArg(\"piano-power-cp\", 0) == 1;",
         "gArgs.mipps = bootArg(\"piano-power-mipps\", 0) == 1;",
         "gArgs.health = bootArg(\"piano-power-health\", 0);",
         "gArgs.healthStop = bootArg(\"piano-power-health-stop\", 80);",
         "gArgs.healthResume = bootArg(\"piano-power-health-resume\", 75);",
         "int gCpArm = 0;",
       }) CHECK(code.find(setting) != std::string::npos);

  Bench b;
  b.mca.pdos = 4;
  CHECK(b.cfg.hv == 0 && b.cfg.stop_pct == 80 && b.cfg.resume_pct == 75);
  CHECK(!b.driver.arm.bootArg && !b.driver.arm.sysctl);
  for (unsigned gate = 0; gate < 3; ++gate) {
    b.driver.arm.bootArg = gate == 1;
    b.driver.arm.sysctl = gate == 2;
    CHECK(!b.driver.start(b.bus, 2000));
    b.ticks(160);
    CHECK(b.contract.valid && b.buck.ladder == PIANO_CHG_LAD_5V_READY);
    CHECK(!b.fetEnabled() && !b.starts && !b.ppsWrites && !b.plant.cp_writes);
  }
}

static void unmappedVendorRdo() {
  for (const unsigned expectedIcl : {1500u, 900u, 500u, 100u}) {
    Bench b;
    b.cfg.hv = 2;
    b.arm();
    b.ticks(5);
    CHECK(b.contract.valid && b.contract.object == 1 && b.contract.mv == 5000);
    // cp-first2: object 7 appears after the initial 5 V snapshot, with the
    // firmware's four-entry list out of source order. The tablet's exact list was lost.
    std::swap(b.pdos[0], b.pdos[3]);
    if (expectedIcl == 900) {
      b.pdos[3][2] = 900;
    } else if (expectedIcl == 500) {
      b.pdos[3][0] = b.pdos[3][1] = 15000;  // no fixed 5 V row
    } else if (expectedIcl == 100) {
      b.mca.pdos = 0;  // no current list read, so fallback is unavailable
    }
    b.rdo = 0x7301f441u;
    for (unsigned i = 0; i < 200; ++i) {
      b.ticks(1);
      CHECK(b.contract.status_valid && !b.contract.valid && b.contract.last_error == PIANO_CONTRACT_RDO);
      CHECK(!(b.in.valid & PIANO_CHG_IN_CONTRACT));
      if (expectedIcl > 100) {
        CHECK(b.in.pdos == 4 && (b.in.valid & PIANO_CHG_IN_PD_UNMAPPED) && b.in.rdo_object == 7);
      } else {
        CHECK(!(b.in.valid & PIANO_CHG_IN_PD_UNMAPPED));
      }
      CHECK(b.cp.state == PIANO_CPS_IDLE && !b.starts && !b.fetEnabled() && !b.plant.cp_writes);
      CHECK(b.plant.set_mv == 5000 && !b.ppsWrites && !b.fixed5Writes);
    }
    CHECK(b.icl == expectedIcl && b.charge.icl_ma == expectedIcl && b.fcc <= 1500);
    CHECK(b.buck.ladder == PIANO_CHG_LAD_5V);
  }
}

static void healthHold() {
  Bench b;
  b.openPump();
  b.packs[1].socPct = 79;
  b.ticks(1);
  CHECK(b.fetEnabled() && b.enable == 1 && !b.buck.hold);
  b.packs[1].socPct = 80;  // one pack suffices
  b.ticks(1);
  b.checkStop(PIANO_CPS_EXIT_HEALTH);
  CHECK(b.buck.hold && !b.enable && b.charge.why_off == PIANO_CHG_OFF_LIMIT);
  b.checkFallback();
  for (int soc = 79; soc >= 76; --soc) {
    b.packs[1].socPct = soc;
    b.ticks(100);
    CHECK(b.buck.hold && !b.enable && !b.fetEnabled() && b.starts == 1);
  }

  // A stale 75% observation cannot clear the hold. Then clear the SOC vote
  // with a fresh reading while entry is still blocked by the pack or connector.
  b.packs[1].socPct = 75;
  b.gaugeAge = 46000;
  b.ticks(10);
  CHECK(b.buck.hold && !b.enable && !b.fetEnabled());
  b.gaugeAge = 0;
  b.packs[1].tempDeciC = 371;
  b.ticks(200);
  CHECK(!b.buck.hold && b.enable && !b.fetEnabled() && b.starts == 1);
  b.packs[1].tempDeciC = 300;
  b.board.milli_c[PIANO_BOARD_TEMP_CONNECTOR2] = 50000;
  b.ticks(200);
  CHECK(!b.fetEnabled() && b.starts == 1);
  b.board.milli_c[PIANO_BOARD_TEMP_CONNECTOR2] = 30000;
  b.ticks(40);
  CHECK(b.fetEnabled() && b.starts == 2 && b.enable);
}

static void thermalAndSensorStops() {
  {
    scenario = "37 C entry boundary";
    Bench b;
    b.arm();
    b.packs[1].tempDeciC = 371;
    b.ticks(200);
    CHECK(b.cp.sessions == 0 && b.starts == 0 && !b.fetEnabled() && b.plant.set_mv == 5000);
    // The live entry rule is inclusive at 37.0 C.
    b.packs[1].tempDeciC = 370;
    b.openPump();
  }

  struct Cutoff {
    const char *name;
    unsigned reason;
    void (*inject)(Bench &);
  };
  const Cutoff cases[] = {
    {"45 C pack", PIANO_CPS_EXIT_TBAT, [](Bench &b) { b.packs[1].tempDeciC = 450; }},
    {"48 C pack / 4060 mV float", PIANO_CPS_EXIT_TBAT, [](Bench &b) {
       b.packs[1].tempDeciC = 480;
       b.packs[1].voltageMv = 4055;
     }},
    {"stale connector samples", PIANO_CPS_EXIT_CONN, [](Bench &b) {
       b.board.age_ms[PIANO_BOARD_TEMP_CONNECTOR] = b.board.age_ms[PIANO_BOARD_TEMP_CONNECTOR2] = 1501;
     }},
    {"failed connector read", PIANO_CPS_EXIT_CONN, [](Bench &b) { b.boardReadable = false; }},
    {"stale pack samples", PIANO_CPS_EXIT_GAUGE, [](Bench &b) { b.gaugeAge = 1501; }},
    {"failed pack B gauge read", PIANO_CPS_EXIT_GAUGE, [](Bench &b) { b.packs[1].ok = false; }},
  };
  for (const auto &test : cases) {
    scenario = test.name;
    Bench b;
    b.cfg.fv_hot = 1;
    b.openPump();
    test.inject(b);
    b.ticks(1);
    b.checkStop(test.reason);
    b.checkFallback();
    b.ticks(700);
    CHECK(b.starts == 1 && !b.fetEnabled() && b.fixed5Writes == 1);
    if (b.packs[1].tempDeciC == 480) {
      CHECK(b.fv == 4060 && !b.enable && b.charge.why_off == PIANO_CHG_OFF_WARM_FV);
      b.packs[1].tempDeciC = 470;
      b.ticks(2);
      CHECK(b.fv == 4060 && !b.enable);
    }
  }
}

static void electricalFaults() {
  for (const unsigned reason : {PIANO_CPS_EXIT_BUS_HIGH, PIANO_CPS_EXIT_CELL_OCP, PIANO_CPS_EXIT_PUMP_FAULT}) {
    scenario = piano_cp_exit_name(reason);
    Bench b;
    CHECK(b.cpCfg.latch_faults == 3);
    b.cpCfg.latch_faults = 1;  // explicit first-fault latch; the live default remains three
    b.openPump();
    if (reason == PIANO_CPS_EXIT_BUS_HIGH) {
      // Inject one over-voltage ADC sample while CHG_EN is set.
      constexpr unsigned vbus = PIANO_CP_REG_ADC_BASE + 2 * PIANO_CP_ADC_VBUS;
      constexpr unsigned raw = 10000 * 1000 / 3750;
      b.plant.r[vbus] = uint8_t(raw >> 8);
      b.plant.r[vbus + 1] = uint8_t(raw);
    } else if (reason == PIANO_CPS_EXIT_CELL_OCP) {
      b.packs[1].currentMa = 7000;
    } else {
      // A transient address NACK on the real bit-banged ADC read. The bus
      // is usable for the emergency STOP on the same session pass.
      b.plant.faults.push_back({cpsim::BusFault::nak, b.plant.now, b.plant.now + 1});
      piano_cp_adc adc{};
      CHECK(!b.driver.readAdc(b.bus, &adc) && b.plant.cp_err > 0);
      b.plant.faults.clear();
    }
    b.pumpStep();
    b.checkStop(reason);
    if (reason == PIANO_CPS_EXIT_PUMP_FAULT) CHECK(b.plan.exit_faults & PIANO_CP_FAULT_BUS);
    b.packs[1].currentMa = 300;
    b.checkFallback();
    b.ticks(700);
    CHECK(b.plan.latched && b.cp.faults_this_attach == 1 && !b.fetEnabled() && b.starts == 1);
    b.plant.detach();
    b.ticks(10);
    CHECK(b.plan.latched && !b.fetEnabled());
    b.rdo = kFixed5Rdo;
    b.plant.attach();
    b.ticks(10);
    CHECK(!b.plan.latched && b.cp.faults_this_attach == 0 && !b.fetEnabled());
    b.ticks(180);
    CHECK(b.starts == 2 && b.fetEnabled());
  }

  scenario = "detach during PPS opening";
  Bench b;
  b.openPump();
  b.plant.detach();
  b.ticks(1);
  b.checkStop(PIANO_CPS_EXIT_DETACH);
  b.ticks(700);
  CHECK(!b.fetEnabled() && !b.plan.owns_volt && b.starts == 1 && b.plant.set_mv == 0);
  b.rdo = kFixed5Rdo;
  b.plant.attach();
  b.ticks(180);
  CHECK(b.starts == 2 && b.fetEnabled());
}

static void defaultFaultBudget() {
  scenario = "default latch after three faults";
  Bench b;
  CHECK(b.cpCfg.latch_faults == 3);
  for (unsigned fault = 1; fault <= 3; ++fault) {
    b.openPump(fault == 1 ? 200 : 650);
    b.packs[1].currentMa = 7000;
    b.pumpStep();
    b.checkStop(PIANO_CPS_EXIT_CELL_OCP);
    b.packs[1].currentMa = 300;
    b.checkFallback();
    CHECK(b.cp.faults_this_attach == fault && bool(b.plan.latched) == (fault == 3));
  }
  b.ticks(700);
  CHECK(b.plan.latched && b.starts == 3 && !b.fetEnabled());
}

static void sustainedNack() {
  scenario = "sustained NACK and modeled hardware watchdog";
  Bench b;
  b.openPump();
  b.plant.faults.push_back({cpsim::BusFault::nak, b.plant.now, b.plant.now + b.plant.wd_ms + 1000});
  b.pumpStep();
  CHECK(b.plan.out.action == PIANO_CP_ACT_STOP && (b.plan.exit_faults & PIANO_CP_FAULT_BUS));
  // The NACK can also block the STOP write. The driver stops kicking and the
  // plant's watchdog clears the register during the simulated hardware deadline.
  CHECK(!b.driver.switching() && b.fetEnabled());
  const auto lastKick = b.plant.wd_kick;
  while (b.plant.now <= lastKick + b.plant.wd_ms) b.plant.step();
  CHECK(!b.fetEnabled() && !b.plant.active && (b.plant.flags & cpsim::reg::F_WD));
}

static void ppsRefreshTimeout() {
  scenario = "buck PPS refresh timeout";
  Bench b;
  b.mca.pdos = 4;
  b.cfg.hv = 2;
  for (unsigned i = 0; i < 200 && b.buck.hv != PIANO_CHG_HV_ON; ++i) b.ticks(1);
  CHECK(b.buck.hv == PIANO_CHG_HV_ON && b.plant.set_mv == 9000 && b.ppsWrites == 1);
  const auto refreshedAt = b.buck.hv_asked_ms;
  b.plant.now = refreshedAt + 7999;
  CHECK(piano_charge_pps_due(&b.buck, b.plant.now) && !piano_charge_pps_expired(&b.buck, b.plant.now));
  ++b.plant.now;
  b.buckStep();
  CHECK(b.charge.fallback == PIANO_CHG_FALLBACK_TIMEOUT && b.buck.hv == PIANO_CHG_HV_BLOCKED);
  b.checkFallback();
  b.ticks(200);
  CHECK(!b.starts && b.ppsWrites == 1 && b.fixed5Writes == 1);
}

static void mippsTimeout() {
  scenario = "MiPPS unanswered authentication command";
  Bench b;
  b.mca.pdos = 4;
  b.ticks(160);
  CHECK(b.buck.ladder == PIANO_CHG_LAD_5V_READY);
  b.arm();
  b.cpCfg.mipps = 1;
  b.cfg.hv = 2;
  b.plant.src.mipps = cpsim::Mipps::timeout_cmd;
  b.plant.src.mipps_fault_cmd = 1;
  PianoMippsKeys keys{};
  keys.count = 1;
  uint8_t challenge[16]{};
  CHECK(piano_mipps_start(&b.mipps, &keys, 0, challenge, b.plant.now) == 0);
  unsigned sends = 0;
  const auto deadline = b.plant.now + 1200;
  while (b.plant.now < deadline && piano_mipps_cleanup_pending(&b.mipps)) {
    PianoMippsRequest req{};
    piano_mipps_next(&b.mipps, b.plant.now, 1, 1, &req);
    if (req.kind == PIANO_MIPPS_REQ_WRITE) {
      if (req.prop == PIANO_MIPPS_PROP_VDM_BASE) ++sends;
      const int status = b.plant.write(req.prop, req.data, req.len);
      CHECK(status == 0);
      piano_mipps_reply(&b.mipps, b.plant.now, status, nullptr, 0);
    } else if (req.kind == PIANO_MIPPS_REQ_READ) {
      uint8_t data[16]{};
      const int status = b.plant.read(req.prop, data, req.len);
      CHECK(status == 0);
      piano_mipps_reply(&b.mipps, b.plant.now, status, data, req.len);
    } else {
      b.plant.step();
    }
    b.pumpStep();
    b.buckStep();
    CHECK(!b.fetEnabled() && !b.starts && !b.ppsWrites && b.plant.set_mv == 5000);
  }
  CHECK(sends == 2 && b.mipps.state == PIANO_MIPPS_DONE && b.mipps.error == PIANO_MIPPS_E_TIMEOUT);
  CHECK(b.mipps.cleared && !b.plant.verify && !b.mipps.verified);
  CHECK(!b.cp.sessions && !b.plant.cp_writes);
  piano_cp_in drv{};
  piano_mipps_fill_cp_in(&b.mipps, 1, &drv);
  CHECK(drv.mipps_done && !drv.mipps_verified && drv.source_budget_ma == 5000);
}

int main() {
  scenario = "default-off and owner arm";
  defaults();
  scenario = "cp-first2 unmapped vendor object";
  unmappedVendorRdo();
  scenario = "80/75 health hold during PPS";
  healthHold();
  thermalAndSensorStops();
  electricalFaults();
  defaultFaultBudget();
  sustainedNack();
  ppsRefreshTimeout();
  mippsTimeout();
  std::puts("cp_safety_invariants_host: all checks passed");
}
