// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// T-CP fork B: the charge pump simulator's scenarios, mutants and fuzz (run by run-charge-host.sh). Every scenario runs the reference
// controller (cp_ref.h) against the plant (cp_sim.h) with an independent checker; each names its safe outcome. Every mutant (one
// guard off) must fail at least one scenario, which shows the scenario and the checker can see that guard missing. Then 10 000
// randomized episodes must never reach an unsafe state. The line-level I2C slave is checked against Gauge.h's I2cBus.
#include "cp_ref.h"
#include "cp_real.h"
#include <memory>
#include <cstdlib>
#include <functional>
#include <map>

using namespace cpsim;

static unsigned checks, failures;
#define CHECK(x, ...) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %s:%u: %s ", __FILE__, __LINE__, #x); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// the TEST key file: a fixed seed/key pair (never real key material)
static void test_key(uint8_t seed[16], uint8_t key[32]) {
  for (unsigned i = 0; i < 16; ++i) seed[i] = uint8_t(0xa0 + i);
  for (unsigned i = 0; i < 32; ++i) key[i] = uint8_t(0x11 * (i % 15) + 3);
}

struct Stats {
  uint64_t cp_on_ms = 0;
  double mean_ibus_on = 0, max_ibus = 0, max_ibat = 0, max_pack = 0, max_tdie = 0;
  unsigned verify_sets = 0, voltage_while_verify = 0, source_resets = 0;
  bool verify_left = false, verified = false, legit = false;
  uint32_t final_budget = 0;
  double soc_end = 0;
  unsigned violations = 0;
  std::vector<std::string> v;
  uint64_t first_on_ms = 0, last_on_ms = 0, sleep_ready_ms = 0;
  uint64_t on_unstepped_ms = 0;   // the pump on while a source powers the bus outside PPS/QC stepping (CHG_EN left set over a detach)
  unsigned unstepped_starts = 0;  // the pump started on such a source (BUSOCP may stop it within one step)
  bool slept = false;
};

struct Run {
  Plant p;
  Checker c;
  RefController ctl;
  std::unique_ptr<RealController> real;
  Controller &C() {return real ? static_cast<Controller &>(*real) : static_cast<Controller &>(ctl);}
  std::function<void(Run &, uint64_t)> events;
  bool sleep_req = false, stalled = false, asleep = false;
  uint64_t asleep_until = 0, stall_until = 0;
  double ibus_on_sum = 0;
  uint64_t ibus_on_n = 0;
  Stats st;
  void legacy_policy() {   // the existing buck policy's 80/75 (unchanged) and its rows while the pump is idle
    p.write(prop::CHG_STOP, reinterpret_cast<const uint8_t *>("\x01\x00\x00\x00"), 4);
    if (p.hold) {const uint8_t z[4] = {}; p.write(prop::CHG_STOP, z, 4);}
    // the buck's own 9 V request on a fixed PD charger (hv=1, charge/pd1), as the daily policy makes it
    if (p.src.kind == SrcKind::fixed_pd && p.powered() && p.mode == Plant::Mode::fixed5) p.set(prop::FIXED_MV, 9000);
  }
  Stats go(uint32_t ms) {
    ctl.pc = &p.cfg;
    uint64_t next_tick = 0, next_gauge = 0, next_policy = 0;
    p.gauge_worker();
    while (p.now < ms) {
      if (events) events(*this, p.now);
      p.step();
      if (p.now >= next_gauge) {p.gauge_worker(); next_gauge = p.now + 1000;}
      if (p.now >= next_policy) {legacy_policy(); next_policy = p.now + 1000;}
      stalled = stall_until && p.now < stall_until;
      if (asleep && p.now >= asleep_until) {asleep = false; sleep_req = false;}
      if (!stalled && !asleep && p.now >= next_tick) {
        p.bus_us = 0;
        Sensors s = p.sensors();
        s.sleep_request = sleep_req;
        C().tick(s, p, p);
        next_tick = p.now + C().period_ms() + p.bus_us / 1000;
        if (sleep_req && C().sleep_ready() && !asleep) {asleep = true; asleep_until = p.now + 4000; st.slept = true; st.sleep_ready_ms = p.now;}
      }
      c.check(p, sleep_req && !asleep, stalled || asleep, asleep);
      if (p.active) {ibus_on_sum += p.ibus_ma; ++ibus_on_n; if (!st.first_on_ms) st.first_on_ms = p.now; st.last_on_ms = p.now;}
      if (p.active && p.attached && p.powered() && p.mode != Plant::Mode::pps && p.mode != Plant::Mode::qc) st.on_unstepped_ms += p.cfg.dt_ms;
    }
    st.cp_on_ms = p.cp_on_ms; st.mean_ibus_on = ibus_on_n ? ibus_on_sum / ibus_on_n : 0;
    st.max_ibus = p.max_ibus; st.max_ibat = p.max_ibat; st.max_pack = p.max_pack; st.max_tdie = p.max_tdie;
    st.verify_sets = p.verify_sets; st.voltage_while_verify = p.voltage_while_verify; st.verify_left = p.verify != 0;
    st.verified = real ? real->m.verified != 0 : ctl.verified; st.legit = p.mipps_legit;
    st.final_budget = real ? piano_mipps_budget_ma(&real->m) : ctl.budget_ma; st.source_resets = p.source_resets;
    st.soc_end = std::max(p.pk[0].soc, p.pk[1].soc) * 100;
    st.violations = c.count; st.v = c.v;
    st.unstepped_starts = p.unstepped_starts;
    return st;
  }
};

struct Scenario {
  const char *name;
  uint32_t ms;
  std::function<void(Run &)> setup;
  std::function<void(Run &, uint64_t)> events;
  std::function<std::string(const Stats &)> expect;   // "" when the safe outcome holds
  uint32_t catches = 0;                                // the mutants this scenario must catch
  bool ref_only = false;                               // exercises a reference-only knob
};

static void attach_at(Run &r, uint64_t t, uint64_t at) {if (t == at) r.p.attach();}

static std::vector<Scenario> scenarios() {
  std::vector<Scenario> v;
  auto base = [](Run &r) {r.p.pk[0].soc = r.p.pk[1].soc = 0.30;};
  auto keyed = [](Run &r) {test_key(r.p.cfg.host_seed, r.p.cfg.host_key); r.p.cfg.host_has_key = true;
                           std::memcpy(r.p.src.seed, r.p.cfg.host_seed, 16); std::memcpy(r.p.src.key, r.p.cfg.host_key, 32);};
  auto on = [](const Stats &s, uint64_t min_ms) -> std::string {return s.cp_on_ms >= min_ms ? "" : "pump on only " + std::to_string(s.cp_on_ms) + " ms";};
  auto off = [](const Stats &s) -> std::string {return s.cp_on_ms == 0 ? "" : "pump ran " + std::to_string(s.cp_on_ms) + " ms";};
  auto ev_attach = [](Run &r, uint64_t t) {attach_at(r, t, 1000);};

  v.push_back({"nominal PPS 5 A", 120000, base, ev_attach,
    [=](const Stats &s) {std::string e = on(s, 60000); if (e.empty() && s.mean_ibus_on < 3000) e = "mean IBUS " + std::to_string(int(s.mean_ibus_on)); return e;}, 0});
  v.push_back({"MiPPS verified (6 A budget)", 90000, [=](Run &r) {base(r); keyed(r); r.p.src.mipps = Mipps::ok; r.p.src.apdo_max_ma = 6100;},
    ev_attach, [=](const Stats &s) {if (!s.verified || !s.legit) return std::string("not verified"); if (s.verify_left) return std::string("0x21002 left set"); return on(s, 30000);}, 0});
  v.push_back({"MiPPS wrong response -> 5 A", 90000, [=](Run &r) {base(r); keyed(r); r.p.src.mipps = Mipps::wrong_response; r.p.src.apdo_max_ma = 6100;},
    ev_attach, [=](const Stats &s) {if (s.verified) return std::string("verified a wrong answer"); if (s.final_budget != 5000) return std::string("budget not 5000");
                                    if (s.verify_left || !s.verify_sets) return std::string("0x21002 not set and cleared"); return on(s, 30000);}, M_TRUST_MIPPS});
  for (unsigned cmd = 1; cmd <= 7; ++cmd) {
    static char names[8][48];
    std::snprintf(names[cmd], sizeof names[cmd], "MiPPS timeout at command %u -> 5 A", cmd);
    v.push_back({names[cmd], 60000, [=](Run &r) {base(r); keyed(r); r.p.src.mipps = Mipps::timeout_cmd; r.p.src.mipps_fault_cmd = cmd; r.p.src.apdo_max_ma = 6100;},
      ev_attach, [=](const Stats &s) {if (s.verified) return std::string("verified"); if (s.verify_left) return std::string("0x21002 left set"); if (s.voltage_while_verify) return std::string("a voltage asked during MiPPS");
                                      return on(s, 15000);}, cmd == 5 ? uint32_t(M_NO_VERIFY_CLEAR) : 0u});
  }
  v.push_back({"MiPPS adapter stalls -> 5 A, never command 8", 60000, [=](Run &r) {base(r); keyed(r); r.p.src.mipps = Mipps::stall;}, ev_attach,
    [=](const Stats &s) {if (s.verified || s.verify_left) return std::string("verified or 0x21002 left"); return on(s, 15000);}, 0});
  v.push_back({"MiPPS ok but command 8 would stall", 60000, [=](Run &r) {base(r); keyed(r); r.p.src.mipps = Mipps::ok;}, ev_attach,
    [=](const Stats &s) {return s.verified ? std::string("") : std::string("not verified");}, M_SEND_CMD8});
  v.push_back({"MiPPS no key file -> never set 0x21002", 40000, base, ev_attach,
    [=](const Stats &s) {if (s.verify_sets) return std::string("0x21002 set without a key"); return on(s, 15000);}, 0});
  v.push_back({"MiPPS adapter ID 0 -> ID search", 60000, [=](Run &r) {base(r); keyed(r); r.p.src.mipps = Mipps::id_zero;}, ev_attach,
    [=](const Stats &s) {return s.verified ? std::string("") : std::string("ID search did not verify");}, 0});
  v.push_back({"QC3-B pump with 20 mV pulses", 90000, [=](Run &r) {base(r); r.p.src.kind = SrcKind::qc3b; r.p.src.qc_max_ma = 3000;}, ev_attach,
    [=](const Stats &s) {std::string e = on(s, 30000); if (e.empty() && s.max_ibat > 5600) e = "battery above the QC3-B limit"; return e;}, 0});
  v.push_back({"QC3.5 pump (4.2 A battery)", 90000, [=](Run &r) {base(r); r.p.src.kind = SrcKind::qc35; r.p.src.qc_max_ma = 3000;}, ev_attach,
    [=](const Stats &s) {std::string e = on(s, 30000); if (e.empty() && s.max_ibat > 4800) e = "battery above the QC3.5 limit"; return e;}, 0});
  v.push_back({"QC3 200 mV steps -> no pump", 40000, [=](Run &r) {base(r); r.p.src.kind = SrcKind::qc3;}, ev_attach, off, M_QC3_COARSE});
  v.push_back({"fixed PD 9 V only -> no pump", 40000, [=](Run &r) {base(r); r.p.src.kind = SrcKind::fixed_pd;}, ev_attach, off, 0});
  auto busfault = [&v, base](const char *n, BusFault k, double prob, uint32_t catches) {
    v.push_back({n, 90000, base, [=](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 30000) r.p.faults.push_back({k, 30000, k == BusFault::stuck ? 50000u : 40000u, prob, 60000, 0});},
                 [](const Stats &) {return std::string("");}, catches});
  };
  busfault("I2C NAK burst mid-session", BusFault::nak, 0.6, 0);
  busfault("I2C timeouts mid-session", BusFault::timeout, 0.5, 0);
  busfault("I2C bus stuck 20 s mid-session", BusFault::stuck, 1.0, M_NO_WD);
  busfault("I2C bit errors mid-session", BusFault::bitflip, 0.2, 0);
  busfault("I2C latency spikes", BusFault::latency, 0.3, 0);
  v.push_back({"gauge A lost", 60000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 30000) r.p.gauge_forced_invalid[0] = true;},
    [](const Stats &s) {return s.last_on_ms < 36000 ? std::string("") : "pump on until " + std::to_string(s.last_on_ms);}, M_NO_GAUGE});
  v.push_back({"gauge B lost", 60000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 30000) r.p.gauge_forced_invalid[1] = true;},
    [](const Stats &s) {return s.last_on_ms < 36000 ? std::string("") : "pump on until " + std::to_string(s.last_on_ms);}, M_NO_GAUGE});
  v.push_back({"source sags 1 V for 3 s", 60000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 30000) r.p.sag_mv = 1000; if (t == 33000) r.p.sag_mv = 0;},
    [](const Stats &) {return std::string("");}, 0});
  v.push_back({"source overshoots 900 mV", 60000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 30000) r.p.overshoot_mv = 900; if (t == 45000) r.p.overshoot_mv = 0;},
    [](const Stats &) {return std::string("");}, 0});
  v.push_back({"battery path resistance rises to 0.12 ohm", 120000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 30000) r.p.r_path = 0.12;},
    [](const Stats &) {return std::string("");}, M_NO_WINDOW});
  v.push_back({"APDO folds back to 2 A", 60000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 30000) r.p.apdo_override_ma = 2000;},
    [](const Stats &) {return std::string("");}, 0});
  v.push_back({"connector heats above 50 C", 90000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 20000) r.p.extra_conn_w = 0.6;},
    [](const Stats &) {return std::string("");}, M_NO_CONN});
  v.push_back({"die derate at 120 C", 120000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 20000) r.p.extra_die_w = 1.6;},
    [](const Stats &s) {return s.max_tdie < 125.5 ? std::string("") : "die " + std::to_string(s.max_tdie);}, 0});
  v.push_back({"die trip to 125 C", 120000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 20000) r.p.extra_die_w = 3.0;},
    [](const Stats &s) {return s.max_tdie < 126.5 ? std::string("") : "die " + std::to_string(s.max_tdie);}, M_NO_TDIE});
  v.push_back({"cell OCP, pack A takes most", 180000, [](Run &r) {r.p.pk[0].soc = r.p.pk[1].soc = 0.30; r.p.pk[0].r_ohm = 0.003; r.p.pk[1].r_ohm = 0.120; r.p.cfg.sys_mw = 300;
                                                            r.p.src.apdo_max_ma = 6100; test_key(r.p.cfg.host_seed, r.p.cfg.host_key); r.p.cfg.host_has_key = true;
                                                            std::memcpy(r.p.src.seed, r.p.cfg.host_seed, 16); std::memcpy(r.p.src.key, r.p.cfg.host_key, 32); r.p.src.mipps = Mipps::ok;},
    ev_attach, [](const Stats &s) {return s.max_pack < 7400 ? std::string("") : "pack " + std::to_string(int(s.max_pack));}, M_NO_CELL_OCP});
  v.push_back({"cell OCP, pack B takes most", 180000, [](Run &r) {r.p.pk[0].soc = r.p.pk[1].soc = 0.30; r.p.pk[1].r_ohm = 0.003; r.p.pk[0].r_ohm = 0.120; r.p.cfg.sys_mw = 300;
                                                            r.p.src.apdo_max_ma = 6100; test_key(r.p.cfg.host_seed, r.p.cfg.host_key); r.p.cfg.host_has_key = true;
                                                            std::memcpy(r.p.src.seed, r.p.cfg.host_seed, 16); std::memcpy(r.p.src.key, r.p.cfg.host_key, 32); r.p.src.mipps = Mipps::ok;},
    ev_attach, [](const Stats &s) {return s.max_pack < 7400 ? std::string("") : "pack " + std::to_string(int(s.max_pack));}, M_NO_CELL_OCP});
  v.push_back({"CV and taper, then the buck", 400000, [](Run &r) {r.p.pk[0].soc = r.p.pk[1].soc = 0.70; r.p.pk[0].r_ohm = r.p.pk[1].r_ohm = 0.13;}, ev_attach,
    [](const Stats &s) {return s.cp_on_ms > 5000 ? std::string("") : std::string("no pump");}, M_NO_CV});
  v.push_back({"SOC 92 % with the health policy off -> no pump", 40000, [](Run &r) {r.p.pk[0].soc = r.p.pk[1].soc = 0.92; r.p.cfg.health_policy = false;}, ev_attach, off, M_NO_SOC});
  v.push_back({"80 % hold during the pump (80/75 unchanged)", 300000, [](Run &r) {r.p.pk[0].soc = r.p.pk[1].soc = 0.785;}, ev_attach,
    [](const Stats &s) {return s.soc_end < 80.6 ? std::string("") : "SOC rose to " + std::to_string(s.soc_end);}, M_NO_HEALTH});
  v.push_back({"detach mid-session, then a fixed 9 V charger", 90000, base,
    [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 30000) r.p.detach(); if (t == 32000) {r.p.src.kind = SrcKind::fixed_pd; r.p.attach();}
                            if (t == 33000) r.p.write(prop::FIXED_MV, reinterpret_cast<const uint8_t *>("\x28\x23\x00\x00"), 4);},
    [](const Stats &s) {return s.on_unstepped_ms == 0 && s.unstepped_starts == 0 ? std::string("") :
                         "pump started " + std::to_string(s.unstepped_starts) + "x / on " + std::to_string(s.on_unstepped_ms) +
                         " ms on the fixed charger (CHG_EN left set over the detach)";}, M_NO_DETACH});
  v.push_back({"re-attach: a second session", 90000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 30000) r.p.detach(); if (t == 35000) r.p.attach();},
    [](const Stats &s) {return s.last_on_ms > 50000 ? std::string("") : std::string("no second session");}, 0});
  v.push_back({"sleep requested mid-session", 60000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 30000) r.sleep_req = true;},
    [](const Stats &s) {return s.slept ? std::string("") : std::string("never ready to sleep");}, M_NO_SLEEP});
  v.push_back({"host stall 8 s (the watchdog stops the pump)", 60000, base, [](Run &r, uint64_t t) {attach_at(r, t, 1000); if (t == 30000) r.stall_until = 38000;},
    [](const Stats &) {return std::string("");}, M_NO_WD});
  v.push_back({"read-back mismatch on BUSOCP -> no pump", 40000, [](Run &r) {r.p.pk[0].soc = r.p.pk[1].soc = 0.30; r.p.ignore_writes[reg::BUSOCP] = 1;}, ev_attach, off, M_NO_READBACK});
  v.push_back({"APDO 3 A below the request", 60000, [](Run &r) {r.p.pk[0].soc = r.p.pk[1].soc = 0.30; r.p.src.apdo_max_ma = 3000; r.p.src.cable_rating_ma = 3000;}, ev_attach,
    [](const Stats &s) {return s.max_ibus < 3150 ? std::string("") : "IBUS " + std::to_string(int(s.max_ibus));}, 0});
  v.push_back({"power cap (20 W cap to exercise the 67 W rule)", 60000, [](Run &r) {r.p.pk[0].soc = r.p.pk[1].soc = 0.30; r.p.cfg.power_cap_mw = 20000; r.ctl.cfg.power_cap_mw = 20000;}, ev_attach,
    [](const Stats &) {return std::string("");}, M_NO_POWER, true});
  v.push_back({"5 A cap without MiPPS (6.1 A APDO, stage 6 A)", 180000, [](Run &r) {r.p.pk[0].soc = r.p.pk[1].soc = 0.30; r.p.src.apdo_max_ma = 6100;}, ev_attach,
    [](const Stats &s) {return s.max_ibus < 5200 ? std::string("") : "IBUS " + std::to_string(int(s.max_ibus));}, M_NO_5A});
  return v;
}

struct Sens {const char *name; RealCfg rc; uint32_t wd_ms = kWdMs, en_ms = kEnLatencyMs; bool auto_inc = true; bool report_only = false;};
static void apply(Run &r, const Sens *x) {
  if (!x) return;
  r.real = std::make_unique<RealController>(r.p);
  r.real->rc = x->rc;
  r.p.wd_ms = x->wd_ms; r.p.en_latency_ms = x->en_ms; r.p.auto_inc = x->auto_inc;
}
static unsigned fuzz(unsigned fuzz_n, const struct Sens *x);
static Stats run_one(const Scenario &sc, uint32_t mut, uint64_t seed = 7, const Sens *x = nullptr) {
  Run r;
  if (const char *m = std::getenv("CPSIM_MUT")) mut = uint32_t(std::strtoul(m, nullptr, 0));
  r.p.rng.seed(seed);
  r.ctl.mut = mut;
  r.p.attached = false;
  if (sc.setup) sc.setup(r);
  apply(r, x);
  r.events = sc.events;
  return r.go(sc.ms);
}

// ---------------------------------------------------------------- the line-level slave against Gauge.h's I2cBus
static void line_bus_tests() {
  Plant p;
  p.pk[0].soc = p.pk[1].soc = 0.5;
  p.step();
  LineBus lb(p);
  const pianopower::I2cIo io = lb.io();
  pianopower::I2cBus bus(io);
  uint8_t id = 0;
  CHECK(bus.readRegisterByte(PIANO_CP_ADDRESS, PIANO_CP_REG_DEVICE, &id) == pianopower::I2cResult::ok && id == PIANO_CP_DEVICE_ID, "device id %02x", id);
  uint8_t ovp = 0;
  CHECK(bus.readRegisterByte(PIANO_CP_ADDRESS, PIANO_CP_REG_USBOVP, &ovp) == pianopower::I2cResult::ok && ovp == PIANO_CP_USBOVP_STOCK, "usbovp %02x", ovp);
  uint16_t mv = 0;
  CHECK(bus.readWord(pianopower::kGaugeAddress, pianopower::kGaugeVoltage, &mv) == pianopower::I2cResult::ok && std::abs(int(mv) - int(p.vnode_mv)) < 2,
        "gauge voltage %u vs %d", mv, int(p.vnode_mv));
  uint16_t soc = 0;
  CHECK(bus.readWord(pianopower::kGaugeAddress, pianopower::kGaugeSoc, &soc) == pianopower::I2cResult::ok && soc == 50, "soc %u", soc);
  CHECK(bus.readRegisterByte(0x33, 0x00, &id) == pianopower::I2cResult::nackAddress, "absent device NAKs");
  p.faults.push_back({BusFault::nak, 0, 100000, 1.0, 0, 0});
  CHECK(bus.readRegisterByte(PIANO_CP_ADDRESS, PIANO_CP_REG_DEVICE, &id) == pianopower::I2cResult::nackAddress, "NAK fault");
  p.faults.clear();
  p.faults.push_back({BusFault::stuck, 0, p.now + 5, 1.0, 0, 0});
  CHECK(bus.readRegisterByte(PIANO_CP_ADDRESS, PIANO_CP_REG_DEVICE, &id) == pianopower::I2cResult::busBusy, "stuck SDA: bus busy");
  p.now += 10; p.stuck = false;
  CHECK(bus.readRegisterByte(PIANO_CP_ADDRESS, PIANO_CP_REG_DEVICE, &id) == pianopower::I2cResult::ok && id == PIANO_CP_DEVICE_ID, "after the window %02x", id);
  p.faults.clear();
  p.faults.push_back({BusFault::timeout, p.now, p.now + 100000, 1.0, 0, 0});
  CHECK(bus.readRegisterByte(PIANO_CP_ADDRESS, PIANO_CP_REG_DEVICE, &id) == pianopower::I2cResult::stretchTimeout, "clock stretch timeout");
  p.faults.clear();
  CHECK(bus.readRegisterByte(PIANO_CP_ADDRESS, PIANO_CP_REG_DEVICE, &id) == pianopower::I2cResult::ok, "the bus back after a stretch timeout");
}

// ---------------------------------------------------------------- sanity of the plant itself
static void plant_tests() {
  // HMAC-SHA256: RFC 4231 test case 2
  const uint8_t key[] = {'J', 'e', 'f', 'e'};
  const char *msg = "what do ya want for nothing?";
  uint8_t mac[32];
  hmac_sha256(key, 4, reinterpret_cast<const uint8_t *>(msg), std::strlen(msg), mac);
  CHECK(mac[0] == 0x5b && mac[1] == 0xdc && mac[31] == 0x43, "RFC 4231 case 2 %02x %02x %02x", mac[0], mac[1], mac[31]);
  // the watchdog turns switching off in hardware
  Plant p; p.src.kind = SrcKind::pps; p.attach();
  for (int i = 0; i < 50; ++i) p.step();
  p.r[reg::CTRL3] = PIANO_CP_WD_ACTIVE; p.wd_kick = p.now;
  p.chip_write(reg::CTRL2, PIANO_CP_CHG_EN);
  p.write(prop::PPS_WORD, reinterpret_cast<const uint8_t *>("\x0a\x00\xd2\x01"), 4);   // not a real request: rejected below
  for (int i = 0; i < 700; ++i) p.step();
  CHECK(!p.active, "watchdog: switching off without a kick (flags %02x)", p.flags);
  // a write outside kCpAllowed is recorded
  Plant q; q.chip_write(PIANO_CP_REG_USBOVP, 0x00);
  CHECK(!q.write_violations.empty(), "VAC OVP write recorded");
}

int main(int argc, char **argv) {
  unsigned fuzz_n = argc > 1 ? unsigned(std::atoi(argv[1])) : 10000;
  plant_tests();
  line_bus_tests();
  const auto list = scenarios();
  unsigned pass = 0;
  const char *only = std::getenv("CPSIM_ONLY");
  for (const auto &sc : list) {
    if (only && !std::strstr(sc.name, only)) continue;
    const Stats s = run_one(sc, M_NONE);
    const std::string e = sc.expect(s);
    const bool ok = s.violations == 0 && e.empty();
    CHECK(ok, "[%s] %u violation(s)%s%s %s", sc.name, s.violations, e.empty() ? "" : ", outcome: ", e.c_str(), s.v.empty() ? "" : s.v[0].c_str());
    if (ok) ++pass;
    std::printf("  %-52s %s  on %6.1f s, mean IBUS %5.0f, max IBUS %5.0f, max pack %5.0f, max die %5.1f, budget %u%s\n", sc.name, ok ? "ok  " : "FAIL",
                s.cp_on_ms / 1000.0, s.mean_ibus_on, s.max_ibus, s.max_pack, s.max_tdie, s.final_budget, s.verified ? " verified" : "");
  }
  std::printf("scenarios: %u/%zu safe with the reference controller\n", pass, list.size());
  // mutants: each must be caught by the scenarios that name it (and by at least one scenario)
  const uint32_t mutants[] = {M_NO_WD, M_NO_TDIE, M_NO_CONN, M_NO_CELL_OCP, M_NO_SLEEP, M_NO_HEALTH, M_NO_POWER, M_NO_WINDOW,
                              M_NO_READBACK, M_NO_SOC, M_NO_CV, M_TRUST_MIPPS, M_NO_VERIFY_CLEAR, M_SEND_CMD8, M_NO_GAUGE,
                              M_QC3_COARSE, M_NO_DETACH};
  unsigned caught = 0;
  for (uint32_t m : mutants) {
    unsigned hits = 0, named = 0, named_hit = 0;
    for (const auto &sc : list) {
      if (!(sc.catches & m)) continue;
      ++named;
      const Stats s = run_one(sc, m);
      const bool failed = s.violations > 0 || !sc.expect(s).empty();
      if (failed) {++hits; ++named_hit;}
    }
    CHECK(named && named_hit == named, "mutant %s caught by %u of %u naming scenario(s)", mutant_name(m), named_hit, named);
    if (named && named_hit == named) ++caught;
    std::printf("  mutant %-26s caught by %u/%u\n", mutant_name(m), hits, named);
  }
  std::printf("mutants: %u/%zu caught\n", caught, sizeof mutants / sizeof mutants[0]);
  const unsigned bad_ref = fuzz(fuzz_n, nullptr);
  if (bad_ref) std::printf("  FINDING fuzz (reference oracle, report only): %u of %u episodes unsafe (bit-error corner cases of the oracle itself)\n", bad_ref, fuzz_n);
  // the integrated code (A's driver + C's session + D's MiPPS) on the same scenarios, plus the sensitivity cases
  const Sens sens[] = {
    {"integrated, defaults (WD 5 s, CHG_EN 200 ms, auto-increment, step 60 ms)", {}},
    {"WD 2 s", {}, 2000}, {"WD 10 s", {}, 10000}, {"no auto-increment", {}, kWdMs, kEnLatencyMs, false},
    {"CHG_EN delay 50 ms", {}, kWdMs, 50},
    {"system view 5 s stale (fresh packs, connectors, health, port each pass)", {5000, 60, false, true}, kWdMs, kEnLatencyMs, true, false},
    {"PPS step latency 0.7 s", {0, 700, false, true}}, {"PPS step latency 2 s", {0, 2000, false, true}},
    {"PPS step latency 5 s", {0, 5000, false, true}},
    {"pack current sign inverted", {0, 60, true, true}},
  };
  unsigned real_ok = 0, real_n = 0;
  for (const auto &x : sens) {
    if (only && !std::strstr(x.name, only) && !std::getenv("CPSIM_ALLSENS")) {}
    unsigned ok_n = 0, n = 0;
    std::string firstbad;
    for (const auto &sc : list) {
      if (sc.ref_only) continue;
      if (only && std::getenv("CPSIM_ONLY_SC") && !std::strstr(sc.name, std::getenv("CPSIM_ONLY_SC"))) continue;
      const Stats st = run_one(sc, M_NONE, 7, &x);
      const std::string e = sc.expect(st);
      const bool unsafe = st.violations > 0;
      ++n;
      if (!unsafe) ++ok_n; else if (firstbad.empty()) firstbad = std::string(sc.name) + ": " + st.v[0];
      if (std::getenv("CPSIM_VERBOSE")) std::printf("    [%s] %-50s %s%s on %6.1f s mean IBUS %5.0f max pack %5.0f %s\n", x.name, sc.name, unsafe ? "UNSAFE " : "safe ",
                                                   e.empty() ? "" : ("(" + e + ")").c_str(), st.cp_on_ms / 1000.0, st.mean_ibus_on, st.max_pack, unsafe ? st.v[0].c_str() : "");
    }
    if (x.report_only) {if (ok_n != n) std::printf("  FINDING integrated [%s]: %u of %u safe; first unsafe: %s\n", x.name, ok_n, n, firstbad.c_str());}
    else CHECK(ok_n == n, "integrated [%s]: %u of %u scenarios safe; first unsafe: %s", x.name, ok_n, n, firstbad.c_str());
    std::printf("  integrated [%-62s] %u/%u scenarios safe\n", x.name, ok_n, n);
    real_ok += ok_n; real_n += n;
  }
  const unsigned real_fuzz = std::getenv("CPSIM_REAL_FUZZ") ? unsigned(std::atoi(std::getenv("CPSIM_REAL_FUZZ"))) : fuzz_n;
  const unsigned bad_real = fuzz(real_fuzz, &sens[0]);
  CHECK(bad_real == 0, "fuzz (integrated): %u of %u episodes unsafe", bad_real, real_fuzz);
  std::printf("integrated: %u/%u scenario runs safe; fuzz %u episodes, %u unsafe\n", real_ok, real_n, real_fuzz, bad_real);
  std::printf("cp_sim_host: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}

static unsigned fuzz(unsigned fuzz_n, const Sens *x) {
  std::mt19937_64 g(20261123);
  unsigned bad = 0;
  std::string first;
  const char *ep = std::getenv("CPSIM_EP");
  for (unsigned n = 0; n < fuzz_n; ++n) {
    Run r;
    if (ep && unsigned(std::atoi(ep)) == n) {r.ctl.trace = true; r.p.wlog = std::getenv("CPSIM_WLOG") != nullptr;}
    r.p.rng.seed(g());
    std::uniform_real_distribution<double> u(0, 1);
    const SrcKind kinds[] = {SrcKind::pps, SrcKind::pps, SrcKind::pps, SrcKind::qc3b, SrcKind::qc35, SrcKind::fixed_pd, SrcKind::qc3};
    r.p.src.kind = kinds[g() % 7];
    r.p.src.apdo_max_ma = (const uint32_t[]){3000, 5000, 6100}[g() % 3];
    r.p.src.cable_rating_ma = r.p.src.apdo_max_ma == 3000 ? 3000 : 6000;
    r.p.src.cable_mohm = 100 + 150 * u(g);
    r.p.pk[0].soc = 0.1 + 0.78 * u(g); r.p.pk[1].soc = std::clamp(r.p.pk[0].soc + 0.04 * (u(g) - 0.5), 0.0, 1.0);
    r.p.pk[0].r_ohm = 0.01 + 0.06 * u(g); r.p.pk[1].r_ohm = 0.01 + 0.06 * u(g);
    r.p.pk[0].temp_c = r.p.pk[1].temp_c = 5 + 50 * u(g);
    if (u(g) < 0.5) {test_key(r.p.cfg.host_seed, r.p.cfg.host_key); r.p.cfg.host_has_key = true; std::memcpy(r.p.src.seed, r.p.cfg.host_seed, 16); std::memcpy(r.p.src.key, r.p.cfg.host_key, 32);}
    r.p.src.mipps = (const Mipps[]){Mipps::none, Mipps::ok, Mipps::wrong_response, Mipps::timeout_cmd, Mipps::stall, Mipps::id_zero}[g() % 6];
    r.p.src.mipps_fault_cmd = 1 + unsigned(g() % 7);
    struct Ev {uint64_t t; int kind; double a;};
    std::vector<Ev> evs;
    const unsigned ne = unsigned(g() % 6);
    for (unsigned i = 0; i < ne; ++i) evs.push_back({2000 + g() % 26000 / 10 * 10, int(g() % 13), u(g)});
    r.events = [evs](Run &rr, uint64_t t) {
      if (t == 1000) rr.p.attach();
      for (const auto &e : evs) {
        if (t != e.t) continue;
        switch (e.kind) {
          case 0: rr.p.detach(); break;
          case 1: rr.p.attach(); break;
          case 2: rr.p.sag_mv = 1500 * e.a; break;
          case 3: rr.p.overshoot_mv = 1000 * e.a; break;
          case 4: rr.p.apdo_override_ma = uint32_t(1000 + 3000 * e.a); break;
          case 5: rr.p.extra_conn_w = 1.0 * e.a; break;
          case 6: rr.p.extra_die_w = 4.0 * e.a; break;
          case 7: rr.p.gauge_forced_invalid[e.a < 0.5 ? 0 : 1] = true; break;
          case 8: rr.p.faults.push_back({(const BusFault[]){BusFault::nak, BusFault::timeout, BusFault::stuck, BusFault::bitflip, BusFault::latency}[int(e.a * 5) % 5],
                                         t, t + 2000 + uint64_t(8000 * e.a), 0.5, 40000, 0}); break;
          case 9: rr.sleep_req = true; break;
          case 10: rr.stall_until = t + 2000 + uint64_t(8000 * e.a); break;
          case 11: rr.p.ignore_writes[(const uint8_t[]){reg::BUSOCP, reg::BATOVP, reg::CTRL3, reg::ADC1}[int(e.a * 4) % 4]] = 1; break;
          case 12: rr.p.sag_mv = 0; rr.p.overshoot_mv = 0; rr.p.apdo_override_ma = 0; break;
        }
      }
    };
    apply(r, x);
    if (ep && unsigned(std::atoi(ep)) == n && r.real) {r.real->trace = true; r.p.wlog = true;}
    const Stats s = r.go(30000);
    if (s.violations) {
      ++bad;
      if (first.empty()) first = "episode " + std::to_string(n) + ": " + s.v[0];
      if (bad <= 4) {
        std::printf("  fuzz episode %u: src %d apdo %u mipps %d socA %.2f rA %.3f rB %.3f T %.0f;", n, int(r.p.src.kind), r.p.src.apdo_max_ma, int(r.p.src.mipps), r.p.pk[0].soc, r.p.pk[0].r_ohm, r.p.pk[1].r_ohm, r.p.pk[0].temp_c);
        for (const auto &e : evs) std::printf(" ev%d@%llu(%.2f)", e.kind, (unsigned long long)e.t, e.a);
        std::printf("\n    %s\n", s.v[0].c_str());
      }
    }
  }
  std::printf("fuzz%s: %u episodes, %u unsafe%s%s\n", x ? " (integrated)" : " (reference)", fuzz_n, bad, first.empty() ? "" : "; first: ", first.c_str());
  return bad;
}
