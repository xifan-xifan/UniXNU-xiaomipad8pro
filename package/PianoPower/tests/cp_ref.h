// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// T-CP fork B: the reference controller of the simulator, the specification (STACK.md 2.2, CP-SPEC.md, ChargeMipps.h) written as
// the simplest code that follows it, so the scenarios and invariants can be checked before forks A/C/D land (cp_sim_host.cpp also
// drives their code through adapters when present). Each guard can be switched off (a mutant): the harness must catch every one.
#pragma once
#include "cp_sim.h"
#include <cstdlib>

namespace cpsim {

enum Mutant : uint32_t {
  M_NONE = 0, M_NO_WD = 1u << 0, M_NO_TDIE = 1u << 1, M_NO_CONN = 1u << 2, M_NO_CELL_OCP = 1u << 3, M_NO_SLEEP = 1u << 4,
  M_NO_HEALTH = 1u << 5, M_NO_5A = 1u << 6, M_NO_POWER = 1u << 7, M_NO_WINDOW = 1u << 8, M_NO_READBACK = 1u << 9,
  M_NO_DETACH = 1u << 10, M_NO_SOC = 1u << 11, M_NO_CV = 1u << 12, M_NO_BUSERR = 1u << 13, M_TRUST_MIPPS = 1u << 14,
  M_NO_VERIFY_CLEAR = 1u << 15, M_SEND_CMD8 = 1u << 16, M_NO_GAUGE = 1u << 17, M_NO_APDO = 1u << 18, M_QC3_COARSE = 1u << 19,
};
inline const char *mutant_name(uint32_t m) {
  switch (m) {
    case M_NO_WD: return "no-watchdog"; case M_NO_TDIE: return "no-die-derate"; case M_NO_CONN: return "no-connector-rule";
    case M_NO_CELL_OCP: return "no-cell-ocp"; case M_NO_SLEEP: return "no-stop-before-sleep"; case M_NO_HEALTH: return "no-80%-hold";
    case M_NO_5A: return "no-5A-cap"; case M_NO_POWER: return "no-power-cap"; case M_NO_WINDOW: return "no-VBUS-window";
    case M_NO_READBACK: return "no-read-back"; case M_NO_DETACH: return "no-detach-stop"; case M_NO_SOC: return "no-SOC-gate";
    case M_NO_CV: return "no-CV-taper"; case M_NO_BUSERR: return "no-bus-error-stop"; case M_TRUST_MIPPS: return "trusts-a-wrong-MiPPS";
    case M_NO_VERIFY_CLEAR: return "leaves-0x21002"; case M_SEND_CMD8: return "sends-VDM-8"; case M_NO_GAUGE: return "no-gauge-gate";
    case M_NO_APDO: return "ignores-the-APDO-current"; case M_QC3_COARSE: return "pumps-on-200mV-QC3";
    default: return "none";
  }
}

struct RefCfg {
  bool mipps = true;
  uint32_t stage_ma = 8000 / 2 - 200;   // one SC8541's bus limit (STACK.md L 441)
  uint32_t power_cap_mw = 67000;
  uint32_t qc3b_ibat_ma = 5000, qc35_ibat_ma = 4200;
};

struct RefController final : Controller {
  RefCfg cfg;
  uint32_t mut = 0;
  const Config *pc = nullptr;   // the host's key (the TEST key file)
  enum class St { idle, mipps, open_wait, opening, cc, stopping, lost } st = St::idle;
  // the pump
  uint32_t req_mv = 0, target_ma = 0, tries = 0, errors = 0, bus_err_run = 0;
  uint64_t t_state = 0, t_kick = 0, t_step = 0, t_guard = 0, t_cv = 0, now_ms = 0;
  int saved_wd = -1;
  bool switching = false, guard_pending = false, confirmed_off = true, cv = false;
  uint32_t attach_seen = 0, mipps_attach = 0;
  bool qc = false;
  uint32_t budget_ma = 5000;
  bool verified = false;
  // MiPPS client (ChargeMipps.h's sequence)
  unsigned mstep = 0, mcmd = 0, mattempt = 0;
  uint64_t mdeadline = 0;
  bool mfail_after_version = false, mverified = false, mdone = false;
  uint8_t mchal[16] = {}, mans[16] = {};
  uint32_t mid = 0;
  std::mt19937 rnd{42};
  bool trace = std::getenv("CPSIM_TRACE") != nullptr;
  St last_st = St::idle;
  const char *name() const override {return mut ? mutant_name(mut) : "reference";}
  uint32_t period_ms() const override {return 100;}

  static uint32_t deadline_of(unsigned cmd, uint32_t flag) {
    switch (cmd) {case 1: case 2: return 500; case 3: return 1000; case 4: return 1500; case 5: return 2500; case 6: return flag == 1 ? 500 : 1000; default: return 1000;}
  }
  // ------------------------------------------------------------ chip helpers
  int rd(Bus &b, uint8_t r, uint8_t *v) {
    const int rc = b.read(PIANO_CP_ADDRESS, r, v, 1);
    if (rc) {if (rc == kStuck) b.recover(); ++bus_err_run;} else bus_err_run = 0;
    return rc;
  }
  bool wr(Bus &b, uint8_t r, uint8_t mask, uint8_t val) {   // write under mask, read back
    uint8_t cur = 0, cur2 = 0;
    if (rd(b, r, &cur)) return false;
    if (mask != 0xff && (rd(b, r, &cur2) || cur2 != cur)) return false;
    const uint8_t v = uint8_t((cur & ~mask) | (val & mask));
    if (b.write(PIANO_CP_ADDRESS, r, v)) {++bus_err_run; return false;}
    bus_err_run = 0;
    if (mut & M_NO_READBACK) return true;
    uint8_t back = 0;
    if (rd(b, r, &back)) return false;
    return (back & mask) == (val & mask);
  }
  uint8_t ctrl2_shadow = 0;   // CTRL2 as verified at the arm (EN_BYPASS 0), CHG_EN excluded
  void stop_chip(Bus &b) {   // CHG_EN = 0 with a known value (a corrupted read must not set EN_BYPASS), then the guard
    b.write(PIANO_CP_ADDRESS, PIANO_CP_REG_CTRL2, uint8_t(ctrl2_shadow & ~(PIANO_CP_CHG_EN | PIANO_CP_EN_BYPASS)));
    switching = false; guard_pending = true; confirmed_off = false; t_guard = now_ms;
  }
  int adc(Bus &b, unsigned ch, int32_t *v) {
    uint8_t x[2];
    const int rc = b.read(PIANO_CP_ADDRESS, uint8_t(PIANO_CP_REG_ADC_BASE + 2 * ch), x, 2);
    if (rc) {++bus_err_run; return rc;}
    bus_err_run = 0;
    *v = int32_t((uint32_t(x[0]) << 8 | x[1]) * reg::adc_lsb[ch] / (ch == PIANO_CP_ADC_TDIE ? 1 : 1000));
    return 0;
  }
  // ------------------------------------------------------------ gates
  int32_t vbat_max(const Sensors &s) const {return std::max(s.g[0].mv, s.g[1].mv);}
  bool gates(const Sensors &s, const char **why) const {
    const bool gok = (mut & M_NO_GAUGE) || (s.g[0].valid && s.g[1].valid);
    if (!s.attached && !(mut & M_NO_DETACH)) {*why = "no source"; return false;}
    if (!gok) {*why = "gauge"; return false;}
    if (s.health_hold && !(mut & M_NO_HEALTH)) {*why = "80 % hold"; return false;}
    if (s.sleep_request && !(mut & M_NO_SLEEP)) {*why = "sleep"; return false;}
    const int32_t soc = std::max(s.g[0].soc, s.g[1].soc);
    if (soc >= 91 && !(mut & M_NO_SOC)) {*why = "SOC"; return false;}
    const int32_t vb = vbat_max(s), vbmin = std::min(s.g[0].mv, s.g[1].mv);
    if (vbmin < 3450 || vb > 4473) {*why = "VBAT"; return false;}
    const int32_t tmin = std::min(s.g[0].temp_c10, s.g[1].temp_c10), tmax = std::max(s.g[0].temp_c10, s.g[1].temp_c10);
    if (tmin < 80 || tmax > 550) {*why = "cell temperature"; return false;}
    if ((s.conn_c10 >= 500 || s.conn2_c10 >= 500) && !(mut & M_NO_CONN)) {*why = "connector"; return false;}
    if (errors >= 11) {*why = "errors"; return false;}
    const bool pps = s.real_type == prop::T_PPS || s.real_type == prop::T_PD_VERIFIED;
    const bool qcok = s.real_type == prop::T_QC3B || s.real_type == prop::T_QC35 || ((mut & M_QC3_COARSE) && s.real_type == prop::T_QC3);
    if (mut & M_NO_DETACH) {} else if (pps) {
      const uint32_t v0 = uint32_t(2 * vb + 300);
      if (!s.apdo_max_ma || v0 < s.apdo_min_mv || v0 > s.apdo_max_mv) {*why = "no APDO covering the start"; return false;}
    } else if (!qcok) {*why = "no PPS/QC3.5 source"; return false;}
    return true;
  }
  uint32_t ibus_target(const Sensors &s, int32_t vbus_mv) const {
    uint32_t t = cfg.stage_ma;
    if (!(mut & M_NO_5A)) t = std::min(t, budget_ma);
    if (s.apdo_max_ma && !(mut & M_NO_APDO)) t = std::min(t, s.apdo_max_ma);
    if (qc) t = std::min<uint32_t>(t, (s.real_type == prop::T_QC35 ? cfg.qc35_ibat_ma : cfg.qc3b_ibat_ma) / 2);
    if (!(mut & M_NO_POWER) && vbus_mv > 5000) t = std::min<uint32_t>(t, uint32_t(uint64_t(std::min(cfg.power_cap_mw, pc ? pc->power_cap_mw : 67000u)) * 1000 / uint32_t(vbus_mv)) - 100);
    return t;
  }
  // ------------------------------------------------------------ MiPPS (fork D's sequence; the reference's own client)
  bool mipps_eligible(const Sensors &s) const {
    return cfg.mipps && pc && pc->host_has_key && (s.real_type == prop::T_PD || s.real_type == prop::T_PPS || s.real_type == prop::T_PD_VERIFIED) &&
           s.pdo_above_5v && mipps_attach != s.attach_seq;
  }
  void mipps_finish(Firmware &fw, bool ok) {
    if (!ok && mfail_after_version) {fw.set(prop::PD_VERIFIED, 0); const uint32_t f = 0; fw.write(prop::VDM + 6, reinterpret_cast<const uint8_t *>(&f), 4);}
    if (!(mut & M_NO_VERIFY_CLEAR)) fw.set(prop::VERIFY, 0);
    mverified = ok; verified = ok || (mut & M_TRUST_MIPPS && mstep >= 6); budget_ma = verified ? 6000 : 5000; mdone = true;
  }
  void mipps_send(Firmware &fw, uint64_t now, unsigned cmd, const uint8_t *b, unsigned n) {
    mcmd = cmd; mdeadline = now + deadline_of(cmd, n == 4 ? b[0] : 0);
    fw.write(prop::VDM + cmd - 1, b, n);
  }
  void mipps_tick(const Sensors &s, Firmware &fw) {
    const uint64_t now = s.now_ms;
    if (!s.attached) {mipps_finish(fw, false); st = St::idle; return;}
    uint32_t vb = 0;
    if (fw.get(prop::VBUS_UV, &vb) || vb < PIANO_MIPPS_VBUS_ONLINE_UV) {mipps_finish(fw, false); st = St::idle; return;}
    auto u32 = [](uint32_t v, uint8_t *b) {b[0] = uint8_t(v); b[1] = uint8_t(v >> 8); b[2] = uint8_t(v >> 16); b[3] = uint8_t(v >> 24);};
    if (mstep == 0) {   // start: 0x21002 = 1, command 1
      fw.set(prop::VERIFY, 1);
      uint8_t z[4] = {}; mattempt = 0; mipps_send(fw, now, 1, z, 4); mstep = 1; return;
    }
    if (mcmd) {   // waiting for the answer of mcmd
      uint32_t ans = 0;
      fw.get(prop::UVDM, &ans);
      if (ans != mcmd) {
        if (now < mdeadline) return;
        if (++mattempt < 2) {   // one resend
          uint8_t z[16] = {}; unsigned n = 4;
          if (mcmd == 4) {std::memcpy(z, pc->host_seed, 16); n = 16;}
          if (mcmd == 5) {std::memcpy(z, mchal, 16); n = 16;}
          if (mcmd == 6 || mcmd == 7) u32(1, z);
          mipps_send(fw, now, mcmd, z, n);
          return;
        }
        mipps_finish(fw, false); st = St::idle; return;
      }
      mattempt = 0;
      const unsigned done = mcmd; mcmd = 0;
      switch (done) {
      case 1: {
        uint32_t ver = 0;
        if (fw.get(prop::VDM, &ver) || ver == 0 || ver == 0xffffffffu) {mipps_finish(fw, false); st = St::idle; return;}
        mfail_after_version = true; fw.set(prop::PD_VERIFIED, 0);
        uint8_t z[4] = {}; mipps_send(fw, now, 2, z, 4); return;
      }
      case 2: {uint8_t z[4] = {}; mipps_send(fw, now, 3, z, 4); return;}
      case 3: mipps_send(fw, now, 4, pc->host_seed, 16); return;
      case 4: for (auto &x : mchal) x = uint8_t(rnd()); mipps_send(fw, now, 5, mchal, 16); return;
      case 5: {
        if (fw.read(prop::AUTH, mans, 16)) {mipps_finish(fw, false); st = St::idle; return;}
        fw.get(prop::ADAPTER_ID, &mid);
        bool match = false;
        uint8_t exp[16];
        if (mid) {mipps_answer(pc->host_key, mchal, mid, exp); match = !std::memcmp(exp, mans, 16);}
        else for (uint32_t id = 0; id < 65536 && !match; ++id) {mipps_answer(pc->host_key, mchal, id, exp); match = !std::memcmp(exp, mans, 16);}
        mstep = 6;
        if (!match && !(mut & M_TRUST_MIPPS)) {mipps_finish(fw, false); st = St::idle; return;}
        uint8_t f[4]; u32(1, f);
        if (mut & M_SEND_CMD8) {mipps_send(fw, now, 8, f, 4); mipps_finish(fw, false); st = St::idle; return;}
        mipps_send(fw, now, 7, f, 4); return;
      }
      case 7: {uint8_t f[4]; u32(1, f); mipps_send(fw, now, 6, f, 4); return;}
      case 6: fw.set(prop::PD_VERIFIED, 1); mipps_finish(fw, true); st = St::idle; return;
      default: mipps_finish(fw, false); st = St::idle; return;
      }
    }
  }
  // ------------------------------------------------------------ the session
  void exit_session(Bus &b, Firmware &fw, bool error) {
    if (switching || st == St::opening || st == St::cc || st == St::open_wait) {if (mut & M_NO_DETACH) {switching = false; confirmed_off = true;} else stop_chip(b);}
    if (error) ++errors;
    if (qc) fw.set(prop::QC_MV, 5000); else fw.set(prop::FIXED_MV, 5000);
    fw.set(prop::ICL, 1500); fw.set(prop::FCC, 3000);
    st = St::stopping; t_state = 0;
  }
  uint8_t ctrl3_shadow = 0;
  bool have_ctrl3 = false;
  bool kick(Bus &b, uint64_t now) {
    if (mut & M_NO_WD) {t_kick = now; return true;}
    if (have_ctrl3) {   // a known value, never one made from a read
      const uint8_t v = uint8_t((ctrl3_shadow & ~PIANO_CP_WD_MASK) | PIANO_CP_WD_ACTIVE);
      uint8_t back = 0;
      if (b.write(PIANO_CP_ADDRESS, PIANO_CP_REG_CTRL3, v)) {++bus_err_run; return false;}
      if (rd(b, PIANO_CP_REG_CTRL3, &back) || back != v) return false;
      t_kick = now; return true;
    }
    if (!wr(b, PIANO_CP_REG_CTRL3, PIANO_CP_WD_MASK, PIANO_CP_WD_ACTIVE)) return false;
    t_kick = now;
    return true;
  }
  bool arm(Bus &b, uint64_t now, uint32_t busocp) {
    uint8_t c3 = 0;
    if (rd(b, PIANO_CP_REG_CTRL3, &c3)) return false;
    uint8_t c3b = 0, c3c = 0;
    if (rd(b, PIANO_CP_REG_CTRL3, &c3b) || c3b != c3 || rd(b, PIANO_CP_REG_CTRL3, &c3c) || c3c != c3) return false;   // three agreeing reads
    if (saved_wd < 0) saved_wd = c3 & PIANO_CP_WD_MASK;
    ctrl3_shadow = c3; have_ctrl3 = true;
    if (!kick(b, now)) return false;
    uint8_t ovp = 0;
    if (rd(b, PIANO_CP_REG_USBOVP, &ovp) || (ovp & PIANO_CP_USBOVP_MASK) != PIANO_CP_USBOVP_STOCK) return false;
    return wr(b, PIANO_CP_REG_BATOVP, 0xff, PIANO_CP_ENC_BATOVP(PIANO_CP_BATOVP_MV)) &&
           wr(b, PIANO_CP_REG_BATOVP_ALM, 0xff, PIANO_CP_ENC_BATOVP(PIANO_CP_BATOVP_ALM_MV)) &&
           wr(b, PIANO_CP_REG_BUSOVP, 0xff, PIANO_CP_ENC_BUSOVP(PIANO_CP_BUSOVP_MV)) &&
           wr(b, PIANO_CP_REG_BUSOVP_ALM, 0xff, PIANO_CP_ENC_BUSOVP(PIANO_CP_BUSOVP_ALM_MV)) &&
           wr(b, PIANO_CP_REG_BUSOCP, 0x9f, PIANO_CP_ENC_BUSOCP(std::clamp<uint32_t>(busocp, 1000, 7000))) &&
           wr(b, PIANO_CP_REG_BATOCP, 0xff, PIANO_CP_ENC_BATOCP(PIANO_CP_BATOCP_MA)) &&
           wr(b, PIANO_CP_REG_TEMP_CTRL, PIANO_CP_TSHUT_DISABLE, 0) && wr(b, PIANO_CP_REG_CTRL2, PIANO_CP_EN_BYPASS, 0) &&
           wr(b, PIANO_CP_REG_ADC_CTRL2, 0xff, 0) && wr(b, PIANO_CP_REG_ADC_CTRL1, 0xc3, PIANO_CP_ADC_EN) && shadow_ctrl2(b);
  }
  bool shadow_ctrl2(Bus &b) {uint8_t a = 0, c = 0; if (rd(b, PIANO_CP_REG_CTRL2, &a) || rd(b, PIANO_CP_REG_CTRL2, &c) || a != c || (a & PIANO_CP_EN_BYPASS)) return false;
                             ctrl2_shadow = uint8_t(a & ~PIANO_CP_CHG_EN); return true;}
  void request_v(Firmware &fw, const Sensors &s, uint32_t mv, uint32_t ma) {
    if (qc) return;
    mv = std::clamp<uint32_t>(mv / 20 * 20, s.apdo_min_mv, s.apdo_max_mv);
    ma = std::max<uint32_t>(500, std::min<uint32_t>(ma, s.apdo_max_ma ? s.apdo_max_ma : ma)) / 50 * 50;
    if (mut & M_NO_APDO) ma = std::min<uint32_t>(ma + 1000, s.apdo_max_ma);
    fw.set(prop::PPS_WORD, prop::pps_word(mv, ma));
    req_mv = mv;
  }
  void qc_pulse(Firmware &fw, int n) {if (n) fw.set(prop::QC_PULSE, uint32_t(int32_t(n)));}

  void tick(const Sensors &s, Bus &b, Firmware &fw) override {
    tick2(s, b, fw);
    if (trace && st != last_st) {std::printf("    [%llu] state %d -> %d target %u req %u\n", (unsigned long long)s.now_ms, int(last_st), int(st), target_ma, req_mv); last_st = st;}
  }
  void tick2(const Sensors &s, Bus &b, Firmware &fw) {
    const uint64_t now = s.now_ms;
    now_ms = now;
    if (s.attach_seq != attach_seen) {   // a new attachment: a fresh session (errors are per attachment as in U)
      attach_seen = s.attach_seq; errors = 0; mdone = false; verified = false; budget_ma = 5000; mstep = 0; mcmd = 0;
      if (st == St::lost) {} else if (st != St::idle && !(mut & M_NO_DETACH)) {exit_session(b, fw, false);}
    }
    if (guard_pending && now >= t_guard + PIANO_CP_GUARD_MS) {
      uint8_t c2 = 0;
      if (!rd(b, PIANO_CP_REG_CTRL2, &c2)) {
        if (c2 & PIANO_CP_CHG_EN) {b.write(PIANO_CP_ADDRESS, PIANO_CP_REG_CTRL2, uint8_t(ctrl2_shadow & ~(PIANO_CP_CHG_EN | PIANO_CP_EN_BYPASS))); t_guard = now;}
        else {guard_pending = false; confirmed_off = true;
              if (saved_wd >= 0) {const uint8_t v = uint8_t((ctrl3_shadow & ~PIANO_CP_WD_MASK) | uint8_t(saved_wd)); uint8_t back = 0;
                                  if (!b.write(PIANO_CP_ADDRESS, PIANO_CP_REG_CTRL3, v) && !rd(b, PIANO_CP_REG_CTRL3, &back) && back == v) {saved_wd = -1; have_ctrl3 = false;}}}
      } else t_guard = now;
    }
    if (bus_err_run >= PIANO_CP_MAX_BUS_ERRORS && !(mut & M_NO_BUSERR) && st != St::lost && st != St::idle && st != St::mipps) {
      stop_chip(b); b.recover(); st = St::lost; return;   // the CP lost for this boot: the hardware watchdog backs the stop
    }
    if (st == St::lost) {if (guard_pending) return; return;}
    const char *why = nullptr;
    switch (st) {
    case St::idle:
      if (!s.attached) return;
      if (!mdone && mipps_eligible(s)) {mipps_attach = s.attach_seq; mstep = 0; mcmd = 0; mfail_after_version = false; st = St::mipps; mipps_tick(s, fw); return;}
      if (!mdone && !(cfg.mipps && pc && pc->host_has_key)) {mdone = true; budget_ma = 5000;}
      if (!mdone && mipps_attach == s.attach_seq) mdone = true;
      if (!confirmed_off) return;
      if (!gates(s, &why)) return;
      if (mut & M_NO_VERIFY_CLEAR) {uint32_t v = 0; fw.get(prop::VERIFY, &v); (void)v;}
      qc = s.real_type == prop::T_QC3B || s.real_type == prop::T_QC35 || s.real_type == prop::T_QC3;
      fw.set(prop::ICL, 500); fw.set(prop::FCC, 500);   // the buck held while the pump opens
      if (qc) {fw.set(prop::QC_MV, 9000);}
      else request_v(fw, s, uint32_t(2 * vbat_max(s) + 300), std::min<uint32_t>(ibus_target(s, 9000), s.apdo_max_ma));
      st = St::open_wait; t_state = now; tries = 0;
      return;
    case St::mipps: mipps_tick(s, fw); return;
    case St::open_wait: {
      if (!gates(s, &why) || (!s.attached && !(mut & M_NO_DETACH))) {exit_session(b, fw, false); return;}
      if (now < t_state + 400) return;
      if (qc) {   // QC3-B/3.5: from 9 V step down to 2 x VBAT + 300
        uint32_t vbus = 0; fw.get(prop::VBUS_UV, &vbus);
        const int32_t want = 2 * vbat_max(s) + 300, have = int32_t(vbus / 1000);
        const int step = s.real_type == prop::T_QC3 ? 200 : 20;
        const int n = std::clamp((want - have) / step, -50, 50);
        if (std::abs(want - have) > step && n) {qc_pulse(fw, n); t_state = now + 300; return;}
      }
      target_ma = ibus_target(s, 2 * vbat_max(s) + 300);
      if (!arm(b, now, qc ? std::min<uint32_t>(target_ma + 1000, 2500) : target_ma + 1000)) {exit_session(b, fw, true); return;}
      if (!wr(b, PIANO_CP_REG_CTRL2, PIANO_CP_CHG_EN, PIANO_CP_CHG_EN) && !(mut & M_NO_READBACK)) {
        // CHG_EN reads back set only later: the opening checks it at its next step
      }
      switching = true; confirmed_off = false; st = St::opening; t_state = now; t_step = now; tries = 0;
      return;
    }
    case St::opening:
    case St::cc: {
      if (!s.attached && !(mut & M_NO_DETACH)) {exit_session(b, fw, false); return;}
      if (now - t_kick >= PIANO_CP_KICK_MS) {
        if (now - t_kick > PIANO_CP_KICK_LATE_MS && !(mut & M_NO_WD)) {exit_session(b, fw, true); return;}
        if (!kick(b, now)) {exit_session(b, fw, true); return;}
      }
      if (!gates(s, &why)) {exit_session(b, fw, false); return;}
      if ((s.real_type != prop::T_PPS && s.real_type != prop::T_PD_VERIFIED && !qc) && !(mut & M_NO_DETACH)) {exit_session(b, fw, false); return;}
      int32_t ibus = 0, vbus = 0, ibat = 0, tdie = 0;
      if (adc(b, PIANO_CP_ADC_IBUS, &ibus) || adc(b, PIANO_CP_ADC_VBUS, &vbus) || adc(b, PIANO_CP_ADC_IBAT, &ibat) || adc(b, PIANO_CP_ADC_TDIE, &tdie)) return;
      uint8_t c2 = 0, fl = 0;
      if (rd(b, PIANO_CP_REG_CTRL2, &c2)) return;
      const int32_t vb = vbat_max(s);
      if (st == St::opening) {
        if (now - t_state < 300) return;
        if (!(c2 & PIANO_CP_CHG_EN)) {if (now - t_state > 1200) {rd(b, reg::FLAG, &fl); exit_session(b, fw, true);} return;}
        if (ibus > 600) {st = St::cc; t_step = now; t_cv = now; cv = false; return;}
        if (now - t_step < 200) return;
        t_step = now;
        if (++tries > 10) {exit_session(b, fw, true); return;}
        if (qc) qc_pulse(fw, s.real_type == prop::T_QC3 ? 1 : 2); else request_v(fw, s, req_mv + 40, target_ma);
        return;
      }
      if (!(c2 & PIANO_CP_CHG_EN)) {rd(b, reg::FLAG, &fl); exit_session(b, fw, true); return;}   // a trip or the watchdog
      // limits
      if (tdie >= 1250 && !(mut & M_NO_TDIE)) {exit_session(b, fw, true); return;}
      const int32_t pmax = std::max(s.g[0].ma, s.g[1].ma);
      if (pmax > 7000 && !(mut & M_NO_CELL_OCP)) {exit_session(b, fw, true); return;}
      uint32_t t = ibus_target(s, vbus);
      if (tdie >= 1200 && !(mut & M_NO_TDIE)) target_ma = target_ma > 1200 ? target_ma - 200 : 1000;
      else if (tdie < 1150 && target_ma < t) target_ma = std::min(t, target_ma + 50);
      target_ma = std::min(target_ma, t);
      if (pmax > 6600 && !(mut & M_NO_CELL_OCP)) target_ma = target_ma > 600 ? target_ma - 300 : 300;
      if (s.g[0].ma + s.g[1].ma > 11500) target_ma = target_ma > 600 ? target_ma - 300 : 300;
      // battery stages: the taper as the cells rise (stock ffc rows, non-FFC part), CV at FV
      uint32_t ibat_cap = 12000;
      if (!(mut & M_NO_CV)) {if (vb >= 4144) ibat_cap = 10650; if (vb >= 4300) ibat_cap = 9040; if (vb >= 4380) cv = true;}
      target_ma = std::min<uint32_t>(target_ma, ibat_cap / 2);
      if (cv && !(mut & M_NO_CV)) {
        if (now - t_cv >= 500) {
          t_cv = now;
          if (vb > 4390) {if (qc) qc_pulse(fw, -1); else request_v(fw, s, req_mv - 20, target_ma);}
          if (ibat < 2000) {exit_session(b, fw, false); return;}   // the buck finishes
        }
        return;
      }
      if (now - t_step < 700) return;
      t_step = now;
      if (trace && now % 10000 < 700) std::printf("    [%llu] cc target %u ibus %d vbus %d vb %d req %u ibat %d\n", (unsigned long long)now, target_ma, ibus, vbus, vb, req_mv, ibat);
      const int32_t window = 2 * vb + 1200;
      if (vbus > window - 50 && !(mut & M_NO_WINDOW)) {if (qc) qc_pulse(fw, -1); else request_v(fw, s, req_mv - 40, target_ma); return;}
      if (int32_t(target_ma) - ibus > 200 && ((mut & M_NO_WINDOW) || vbus < window - 100)) {if (qc) qc_pulse(fw, 1); else if (req_mv + 20 <= uint32_t(2 * vb + 2000) || (mut & M_NO_WINDOW)) request_v(fw, s, req_mv + 20, target_ma);}
      else if (ibus > int32_t(target_ma) + 300) {const int down = std::min(10, (ibus - int32_t(target_ma)) / 140); if (qc) qc_pulse(fw, -std::max(1, down)); else request_v(fw, s, req_mv - 20u * uint32_t(std::max(1, down)), target_ma);}
      else if (ibus > int32_t(target_ma)) {if (qc) qc_pulse(fw, -1); else request_v(fw, s, req_mv - 20, target_ma);}
      const uint32_t ocp = std::clamp<uint32_t>(qc ? std::min<uint32_t>(target_ma + 1000, 2500) : target_ma + 1000, 1000, 7000);
      if (!wr(b, PIANO_CP_REG_BUSOCP, 0x9f, PIANO_CP_ENC_BUSOCP(ocp)) && !(mut & M_NO_READBACK)) exit_session(b, fw, true);
      return;
    }
    case St::stopping:
      if (!guard_pending) {st = St::idle; qc = false;}
      return;
    case St::lost: return;
    }
  }
  bool sleep_ready() override {
    if (mut & M_NO_SLEEP) return true;
    return (st == St::idle || st == St::lost) && !switching && confirmed_off && !guard_pending;
  }
};

}  // namespace cpsim
