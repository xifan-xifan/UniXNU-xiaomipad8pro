// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// T-CP fork B: the integrated code (fork A's SC8541 driver ChargePumpImpl.h on Gauge.h's software I2C, fork C's session
// ChargePumpSession.c, fork D's MiPPS engine ChargeMipps.c) driven against the plant of cp_sim.h, the way PianoPower's worker drives
// them: the driver on the line-level bus (LineBus), the session's PianoCpSys filled from the plant (optionally STALE by up to one
// codec cycle, fork C's risk 1), the PPS words and QC pulses through a write queue with a step latency (risk 2), the pack current
// sign optionally inverted (risk 3). The worker glue here is a stand-in for PianoPower.cpp's (not built by the host tests).
#pragma once
#include "cp_sim.h"
#include "ChargePumpImpl.h"
#include "ChargePumpSession.h"
#include "ChargeMipps.h"

namespace cpsim {

struct RealCfg {
  uint32_t sys_stale_ms = 0;      // the session's system view refreshed only this often (0: every call)
  uint32_t step_latency_ms = 60;  // a PPS word / QC pulse taken this long after it is planned
  bool invert_sign = false;       // the packs' current sign inverted (+ = discharging) in the system view
  bool mipps = true;
};

struct RealController final : Controller {
  Plant &p;
  LineBus lb;
  pianopower::I2cBus bus;
  pianopower::Sc8541 drv;
  RealCfg rc;
  PianoCpConfig cfg{};
  PianoCpState st{};
  PianoCpPlan plan{};
  PianoMipps m{};
  PianoMippsKeys keys{};
  bool keys_loaded = false, mipps_will_run = false, mipps_running = false, probed = false, sleep_ok = true;
  uint32_t mipps_attach = 0;
  struct Pending {uint64_t at; uint32_t prop; uint32_t value;};
  std::vector<Pending> q;
  uint32_t pps_taken = 0;
  int32_t qc_taken = 0;
  PianoCpSys snap{};
  uint64_t snap_at = 0;
  bool have_snap = false;
  unsigned refused_writes = 0, decide_calls = 0;
  std::mt19937 rnd{99};
  bool trace = std::getenv("CPSIM_TRACE") != nullptr;
  uint8_t last_state = 0;

  static uint64_t clock_now(void *c) {return static_cast<RealController *>(c)->p.now;}
  static void hook(void *c, pianopower::I2cBus &b) {static_cast<RealController *>(c)->drv.beforeBusTransaction(b);}
  explicit RealController(Plant &plant) : p(plant), lb(plant), bus(lb.io()), drv(pianopower::Sc8541::Clock{this, clock_now}) {
    piano_cp_config_default(&cfg);
    cfg.qc3 = 1;
    piano_cp_init(&st);
    drv.arm.bootArg = drv.arm.sysctl = true;
    bus.setHook(hook, this);
  }
  const char *name() const override {return "integrated (A+C+D)";}
  uint32_t period_ms() const override {return 100;}
  bool sleep_ready() override {return sleep_ok;}

  void load_keys(const Config &c) {
    keys = {};
    if (c.host_has_key) {keys.count = 1; std::memcpy(keys.k[0].seed, c.host_seed, 16); std::memcpy(keys.k[0].key, c.host_key, 32);}
    keys_loaded = true;
  }
  void mipps_pass(const Sensors &s, Firmware &fw) {
    if (!mipps_running) return;
    for (int i = 0; i < 2000; ++i) {   // the worker's passes: again at once while the engine asks for no wait (its 10 ms wake otherwise)
      PianoMippsRequest req{};
      piano_mipps_next(&m, s.now_ms, s.attached && s.attach_seq == mipps_attach, 1, &req);
      if (req.kind == PIANO_MIPPS_REQ_NONE && req.wait_until_ms && req.wait_until_ms <= s.now_ms && m.state != PIANO_MIPPS_DONE) continue;
      if (req.kind == PIANO_MIPPS_REQ_WRITE) {const int r = fw.write(req.prop, req.data, req.len); piano_mipps_reply(&m, s.now_ms, r, nullptr, 0);}
      else if (req.kind == PIANO_MIPPS_REQ_READ) {
        uint8_t buf[16] = {};
        const int r = fw.read(req.prop, buf, req.len);
        piano_mipps_reply(&m, s.now_ms, r, buf, req.len);
      } else break;
      if (m.state == PIANO_MIPPS_DONE) break;
    }
    if (m.state == PIANO_MIPPS_DONE) mipps_running = false;
  }
  void fill_sys(const Sensors &s, PianoCpSys &y) {
    y = {};
    y.attach_seq = s.attach_seq; y.attached = s.attached; y.awake = !s.sleep_request; y.sleep_pending = s.sleep_request;
    y.health_hold = s.health_hold;
    y.soc_entry_max = p.cfg.health_policy ? 77 : 0;   // piano_charge_cp_sys(): the health limit's stop - 3
    y.src_kind = (s.real_type == prop::T_PPS || s.real_type == prop::T_PD_VERIFIED) && s.apdo_max_ma ? PIANO_CPS_SRC_PPS :
                 s.real_type == prop::T_QC3B ? PIANO_CPS_SRC_QC3B : s.real_type == prop::T_QC35 ? PIANO_CPS_SRC_QC35 : PIANO_CPS_SRC_NONE;
    y.apdo_min_mv = s.apdo_min_mv; y.apdo_max_mv = s.apdo_max_mv; y.apdo_ma = s.apdo_max_ma;
    y.contract_valid = p.powered();
    y.contract_pps = p.powered() && p.mode == Plant::Mode::pps;
    y.contract_mv = p.powered() ? p.set_mv : 0; y.contract_ma = p.powered() ? p.lim_ma : 0;
    y.bus_valid = 1; y.vbus_mv = int32_t(std::max(0.0, p.vbus_mv)); y.ibus_ma = int32_t(p.ibus_ma);
    y.gauges_valid = s.g[0].valid && s.g[1].valid;
    y.soc_pct = std::max(s.g[0].soc, s.g[1].soc);
    y.vbat_max_mv = std::max(s.g[0].mv, s.g[1].mv); y.vbat_min_mv = std::min(s.g[0].mv, s.g[1].mv);
    const int sign = rc.invert_sign ? -1 : 1;
    y.ibat_a_ma = sign * s.g[0].ma; y.ibat_b_ma = sign * s.g[1].ma;
    y.cells_valid = y.gauges_valid;
    y.t_cool_dc = std::min(s.g[0].temp_c10, s.g[1].temp_c10); y.t_warm_dc = std::max(s.g[0].temp_c10, s.g[1].temp_c10);
    y.conn_valid = 1; y.t_conn_dc = std::max(s.conn_c10, s.conn2_c10);
  }
  void tick(const Sensors &s, Bus &, Firmware &fw) override {
    const uint64_t now = s.now_ms;
    if (!keys_loaded) load_keys(p.cfg);
    // the write queue: words and pulses taken after the step latency
    bool volt_pending = false;
    for (auto it = q.begin(); it != q.end();) {
      if (it->at > now) {if (it->prop == prop::PPS_WORD || it->prop == prop::QC_PULSE) volt_pending = true; ++it; continue;}
      const int r = fw.set(it->prop, it->value);
      if (!r && it->prop == prop::PPS_WORD) pps_taken = it->value;
      if (!r && it->prop == prop::QC_PULSE) qc_taken += int32_t(it->value) > 0 ? 1 : -1;
      it = q.erase(it);
    }
    if (!probed) {probed = drv.probe(bus);}
    if (drv.kickDue(now)) drv.kick(bus);
    if (drv.guardDue(now)) drv.guard(bus);
    // MiPPS: one authentication per eligible attach
    // a new attach starts a new authentication only once the previous one has finished (its 0x21002 = 0 cleanup included)
    if (rc.mipps && s.attached && !s.sleep_request && s.attach_seq != mipps_attach && !mipps_running && !piano_mipps_cleanup_pending(&m)) {
      mipps_attach = s.attach_seq;
      const uint32_t pdo = s.pdo_above_5v ? 9000u : 5000u;
      mipps_will_run = piano_mipps_eligible(1, &keys, s.real_type, &pdo, 1, 1, 1, 0) != 0;
      if (mipps_will_run) {
        uint8_t ch[16];
        for (auto &x : ch) x = uint8_t(rnd());
        mipps_running = piano_mipps_start(&m, &keys, 0, ch, now) == 0;
        if (!mipps_running) mipps_will_run = false;
      }
    }
    mipps_pass(s, fw);
    // the driver's view
    struct piano_cp_in in{};
    in.now_ms = now;
    drv.fill(&in);
    in.faults = drv.takeFaults();
    drv.readAdc(bus, &in.adc);
    in.faults |= drv.takeFaults();
    piano_mipps_fill_cp_in(&m, mipps_will_run ? 1 : 0, &in);
    // the system view (fresh, or one codec cycle stale)
    PianoCpSys fresh{};
    fill_sys(s, fresh);
    if (!have_snap || !rc.sys_stale_ms || now >= snap_at + rc.sys_stale_ms) {snap = fresh; snap_at = now; have_snap = true;}
    PianoCpSys sys = snap;   // stale: the policy's view (health hold, attach, contract, source, temperatures)
    // fresh: the worker's own pack reads (fork C: serviceCp reads the packs each pass)
    sys.gauges_valid = fresh.gauges_valid; sys.cells_valid = fresh.cells_valid; sys.soc_pct = fresh.soc_pct;
    sys.vbat_max_mv = fresh.vbat_max_mv; sys.vbat_min_mv = fresh.vbat_min_mv; sys.ibat_a_ma = fresh.ibat_a_ma; sys.ibat_b_ma = fresh.ibat_b_ma;
    // fresh as in the worker (fork C, cp-int): the connector thermistors and the pack temperatures, the health hold (the packs'
    // SOC) and the attachment (the PMIC port) read every pass, not from the codec's snapshot
    sys.conn_valid = fresh.conn_valid; sys.t_conn_dc = fresh.t_conn_dc;
    sys.t_cool_dc = fresh.t_cool_dc; sys.t_warm_dc = fresh.t_warm_dc;
    sys.health_hold = fresh.health_hold;
    sys.attached = fresh.attached; sys.attach_seq = fresh.attach_seq;
    sys.now_ms = now;
    sys.mipps_cleanup_pending = uint8_t(piano_mipps_cleanup_pending(&m));
    sys.buck_icl_set_ma = p.icl_ma; sys.buck_fcc_set_ma = p.fcc_ma; sys.buck_pending = 0;
    sys.pps_taken_word = pps_taken; sys.volt_pending = volt_pending; sys.qc_pulses_taken = qc_taken;
    cfg.mipps = mipps_will_run ? 1 : 0;
    piano_cp_decide(&cfg, &sys, &in, &st, &plan);
    ++decide_calls;
    if (trace && plan.state != last_state) {std::printf("    [%llu] session %s -> %s exit %s target %u\n", (unsigned long long)now, piano_cp_state_name(last_state), piano_cp_state_name(plan.state), piano_cp_exit_name(plan.exit_reason), plan.ibus_target_ma); last_state = plan.state;}
    // execute
    switch (plan.out.action) {
      case PIANO_CP_ACT_START: drv.start(bus, plan.out.busocp_ma); break;
      case PIANO_CP_ACT_STOP: drv.stop(bus, plan.out.stop_reason); break;
      case PIANO_CP_ACT_SET_BUSOCP: drv.setBusOcp(bus, plan.out.busocp_ma); break;
      default: break;
    }
    if (plan.out.req_valid & PIANO_CP_REQ_PPS) {
      if (!piano_cp_write_allowed(&cfg, prop::PPS_WORD, plan.pps_word, 4)) ++refused_writes;
      else q.push_back({now + rc.step_latency_ms, prop::PPS_WORD, plan.pps_word});
    }
    if (plan.out.req_valid & PIANO_CPS_REQ_QC_PULSE)
      q.push_back({now + rc.step_latency_ms, prop::QC_PULSE, plan.qc_pulse == PIANO_CHG_QC_UP ? 1u : uint32_t(-1)});
    if (plan.out.req_valid & PIANO_CP_REQ_BUCK) {fw.set(prop::ICL, plan.out.buck_icl_ma); fw.set(prop::FCC, plan.out.buck_fcc_ma);}
    if (plan.handback) {   // the buck session takes the source back to vSafe5V (or 5 V QC) through its barrier
      fw.set(prop::ICL, 500); fw.set(prop::FCC, 500);
      q.push_back({now + rc.step_latency_ms, sys.src_kind == PIANO_CPS_SRC_PPS ? prop::FIXED_MV : prop::QC_MV, 5000});
    }
    sleep_ok = plan.sleep_ready && !mipps_running && !piano_mipps_cleanup_pending(&m);   // no sleep in an authentication
  }
};

}  // namespace cpsim
