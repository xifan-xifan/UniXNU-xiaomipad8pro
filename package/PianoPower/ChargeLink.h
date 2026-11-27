// SPDX-License-Identifier: BSD-2-Clause-Patent
// ChargePolicy (ChargePolicy.h) wired to PianoMca's codec: what PianoPower does at level 2 with piano-power-policy=1
// (docs/drivers/charging-policy.md §8). Portable, header only, no I/O: the owner hands in what it read besides the codec (the
// charger_pd state, the two packs, the Type-C block, PianoThermal's board temperatures, the wake count) and acts on the outcome
// (relink the codec, set the charging-hold assertion, log). Host-tested with the DSP model of tests/power_host.cpp.
//
//   - Gates: nothing is decided until msm/adsp/charger_pd reports up and the codec's board configuration of this link is through
//     (taken, or given up after five cycles), so the stock board configuration always precedes the first policy write.
//   - Cadence: one decision per finished codec cycle (5 s, and the cycles a notification or a write's read-back start); while a sleep
//     or a shutdown is handed over, also one per write answer, until the plan says ready.
//   - Writes: in the plan's order through piano_mca_queue, whose allowlist (piano_mca_write_allowed_for) is the codec's, not the
//     policy's: a write it refuses is answered to the policy as refused (three refusals: given up until the next attach). Every
//     answer, a timeout's included, goes back through piano_charge_ack(), and so does a late answer to a write the owner's timer had
//     given up on (McaLateWritten: the owner calls answered() for it too). The codec's input limit bound follows the policy's
//     (piano_mca_allow_icl(piano_charge_icl_max)): above 1600 mA only for the stepped Type-C current (charging-policy.md §15); its
//     0x2003 = 0 only with battery health or the charging restart on (charging-policy.md §17).
//   - Read-back: the input limit, the enable bit and the bus voltage request are compared with the first cycle read after they were
//     taken (the policy itself writes a limit again after two disagreeing read-backs); the charge current has none (0x2005 reads 0).
//   - Resync (a wake, Ubuntu #18): the plan's writes are not queued; the owner relinks the codec (board configuration, port enable,
//     UCSI read, everything read again) and the new generation sends everything again.
#ifndef PIANO_POWER_CHARGE_LINK_H
#define PIANO_POWER_CHARGE_LINK_H
#include <stdint.h>
#include "Gauge.h"
#include "ChargerPmic.h"
#include "PianoBoardTemps.h"
#include "ChargePolicy.h"
#include "PianoMca.h"
#include "ChargeContract.h"

namespace pianopower {

struct LinkOptions {
  bool strictContract = false;     // production contract-bounded buck session
  bool writes = true;              // piano-power-write
  unsigned hv = 0;                 // piano-power-hv: 0, 1 fixed 9 V PDO, 2 also a PPS APDO
  bool qc3 = false;                // piano-power-b4pd: HVDCP3 9 V request (0x2011)
  bool rp = false;                 // piano-power-rp: the Type-C Rp current as the 5 V limit of a source without PD
  bool ramp = true;                // piano-power-ramp: that limit in steps from the row (ChargePolicy cfg.ramp, docs §15)
  unsigned rpMax = 0;              // piano-power-rp-max: the stepped limit's ceiling, mA (0: the 1600 mA cap; at most 3000)
  bool health = false;             // piano-power-health: stop at stopPct, resume at resumePct
  unsigned stopPct = 80, resumePct = 75;
  bool kick = true;                // piano-power-kick: the charging restart (ChargePolicy cfg.kick, charging-policy.md §17)
  // charge/pd1 (charging-policy.md §21), strict session only, each off by default:
  bool hvOrder = false;            // piano-power-hv-order: the other 9 V object when the mode's own is missing (cfg.hv_order)
  bool hvCaps = false;             // piano-power-hv-caps: the stock 9 V rows at a validated 9 V contract (cfg.hv_caps)
  bool qc = false;                 // piano-power-qc: QC2/QC3 9 V through 0x2011 (cfg.qc)
  bool fvHot = false;              // piano-power-fv-hot: 4060 mV from 48 degC and the warm hold (cfg.fv_hot)
  bool ownBus = false;             // piano-power-own-bus: our own VBUS (a USB device on the port) is no source (cfg.own_bus)
};

// What the owner knows besides the codec, for one decision.
struct LinkView {
  const PianoChargeContract *contract = nullptr;
  bool safetyFresh = false, cpOff = false, dtptValid = false, dtpt = false; // dtpt: a confirmed alert, even if another field is missing
  uint64_t nowMs = 0;
  bool chargerPdUp = false;                   // msm/adsp/charger_pd reports up (PianoQrtr.c's servreg listener)
  uint64_t chargerPdUpMs = 0;                 // when it was seen coming up (nowMs's clock; each up again moves it)
  const GaugeReading *packs = nullptr;        // the two packs' last reading, or none
  bool packsFresh = false;                    // that reading is recent enough to decide with
  uint64_t packsMs = 0;                       // when it was taken (nowMs's clock; 0: not known)
  const PortState *port = nullptr;            // the charger PMIC's Type-C block, or not readable
  const piano_board_temps *board = nullptr;   // PianoThermal's last round, or PianoThermal not there
  const piano_skin_temps *skin = nullptr;     // virtual skin reading for the PPS buck
  uint32_t wakes = 0;                         // system wakes so far
  bool willSleep = false, shutdown = false;   // a handover is asked for
  bool mippsBusy = false;                     // MiPPS runs or owes its 0x21002 = 0: no voltage step (an authentication)
  bool cpFirst = false;                       // charge/mipps2: the armed pump would start from this 5 V (no 9 V request now)
  bool mippsPending = false;                  // charge/mipps2: MiPPS will run on this attach and has not finished (no 9 V first)
};

struct LinkReadback {
  uint32_t property = 0, written = 0;
  int32_t read = 0;
  bool readable = false, agrees = false;
};

// What one step did.
struct LinkOutcome {
  bool decided = false;
  bool resync = false;            // relink the codec now
  bool waitingPd = false;         // not deciding: charger_pd is not up (once per wait)
  bool waitingBoard = false;      // not deciding: the board configuration is not through (once per link)
  bool planChanged = false;       // the class, a limit, the voltage, charging or the hold differ from the last one reported
  bool handoverReady = false;     // willSleep / shutdown: nothing is left to hand over
  uint32_t events = 0;            // PIANO_CHG_EV_*
  unsigned queued = 0, refused = 0, full = 0;
  uint32_t refusedProperty = 0, refusedValue = 0;
  LinkReadback readback[3];       // the input limit, the enable bit, the bus voltage request
  unsigned readbacks = 0;
};

inline int32_t milliToDeci(int32_t mc) { return mc >= 0 ? mc / 100 : -((-mc + 99) / 100); }

// The bus is our own boost's (usb-host.md section 6.4, Q17): a USB device on the port and the DSP's firmware sourcing VBUS for it.
// A source's Rp on the PMIC's Type-C block never is (a charger, a computer, a dock); else UCSI's fresh status decides (connected and
// we are the provider, not the sink); without one, the PMIC: a sink's Rd (we source). Unknown is never ours.
inline bool ownBus(const PianoChargeContract *c, const PortState *p, uint64_t nowMs)
{
  if (p && p->ok && p->sourceSeen) return false;
  if (c && piano_contract_status_fresh(c, nowMs)) return c->connected && !c->sink;
  return p && p->ok && p->sinkSeen;
}

// The contract's "the gauges' I2C idle" (docs/power.md 4): from the sleep handover's acknowledgement to the wake no report runs (the
// gauges' bit-banged I2C, the Type-C block and the RTC over SPMI), so the freeze cuts none of them in the middle; the report that
// fell due meanwhile runs right after the wake. The codec's DSP traffic is not paused (the DSP stays as it is). update() once per
// pass of the owner's loop: +1 when the reports stop, -1 when they run again, 0 otherwise.
struct SleepReports {
  bool quiet = false;
  int update(bool policyOn, bool handover, bool acknowledged)
  {
    const bool q = policyOn && handover && acknowledged;
    if (q == quiet) return 0;
    quiet = q;
    return q ? 1 : -1;
  }
};

class ChargeLink {
 public:
  PianoChargeConfig cfg{};
  PianoChargeState st{};
  PianoChargePlan plan{};
  PianoChargeInput in{};
  uint32_t decisions = 0, queuedTotal = 0, refusedTotal = 0, answers = 0, resyncs = 0;
  bool hold = false;              // the charging-hold assertion's level
  bool healthRefused = false;     // the thresholds were refused: battery health stays off

  // After piano_mca_init (and the allow_* calls): the policy's configuration, and the codec told to leave its built-in policy out.
  void configure(PianoMca &m, const LinkOptions &o)
  {
    piano_charge_config_default(&cfg);
    cfg.strict_contract = o.strictContract ? 1 : 0;
    cfg.writes = o.writes && m.policy ? 1 : 0;
    cfg.hv = uint8_t(o.hv <= 2 ? o.hv : 0);
    cfg.qc3 = o.qc3 ? 1 : 0;
    cfg.rp = o.rp ? 1 : 0;
    cfg.ramp = o.rp && o.ramp ? 1 : 0;
    cfg.rp_cap_ma = uint16_t(cfg.ramp && o.rpMax ? (o.rpMax > PIANO_CHG_RP_ABS_MAX ? PIANO_CHG_RP_ABS_MAX : o.rpMax) : 0);
    cfg.kick = o.kick ? 1 : 0;
    cfg.hv_order = o.strictContract && o.hvOrder ? 1 : 0;
    cfg.hv_caps = o.strictContract && o.hvCaps ? 1 : 0;
    cfg.qc = o.strictContract && o.qc ? 1 : 0;
    cfg.fv_hot = o.strictContract && o.fvHot ? 1 : 0;
    cfg.own_bus = o.ownBus ? 1 : 0;
    healthRefused = o.health && piano_charge_config_limit(&cfg, 1, o.stopPct, o.resumePct) != 0;
    piano_charge_init(&st);
    // 0x2003 = 0 through the codec only for battery health's stop and the charging restart's
    piano_mca_use_external_policy(&m, cfg.strict_contract || cfg.limit || cfg.kick, ppsEnabled(),
                                  cfg.qc3 || cfg.qc);
    piano_mca_allow_icl(&m, piano_charge_icl_max(&cfg));   // the codec's bound follows the policy's (1600 unless stepped higher)
    // charge/pd1: the charge current up to the stock 9 V rows (3500) and the warm float (0x2007 = 4400 / 4060) only with their options
    piano_mca_allow_fast(&m, cfg.hv_caps, cfg.fv_hot);
  }

  void syncPpsOption(PianoMca &m) const {m.pps_allowed = m.policy && ppsEnabled();}

  // The charger thermistor's reading was taken while charger_pd was up (at or after it came up this time).
  static bool chargerReadingCounts(const LinkView &v)
  {
    if (!v.chargerPdUp || !v.board) return false;
    const uint64_t age = v.board->age_ms[PIANO_BOARD_TEMP_CHARGER];
    return age <= v.nowMs && v.nowMs - age >= v.chargerPdUpMs;
  }

  static void fill(PianoChargeInput &x, const PianoMca &m, const LinkView &v, bool ownBusGuard = false)
  {
    x = PianoChargeInput();
    x.now_ms = v.nowMs;
    x.generation = m.links;
    x.wakes = v.wakes;
    x.sleep = v.willSleep ? PIANO_CHG_WILL_SLEEP : PIANO_CHG_AWAKE;
    auto has = [&](int f) { return piano_mca_has(&m, f) != 0; };
    if (has(McaBusVoltage)) {x.valid |= PIANO_CHG_IN_BUS; x.bus_mv = m.value[McaBusVoltage] / 1000;}
    if (has(McaBusCurrent)) {x.valid |= PIANO_CHG_IN_IBUS; x.ibus_ma = m.value[McaBusCurrent] / 1000;}
    if (has(McaType)) {x.valid |= PIANO_CHG_IN_TYPE; x.type = m.value[McaType];}
    if (has(McaIcl)) {x.valid |= PIANO_CHG_IN_ICL; x.icl_ma = m.value[McaIcl];}
    if (has(McaChargeEnable)) {x.valid |= PIANO_CHG_IN_ENABLE; x.enable = m.value[McaChargeEnable];}
    if (has(McaTermVolt)) {x.valid |= PIANO_CHG_IN_FV; x.fv_raw = m.value[McaTermVolt];}
    if (has(McaVerify)) {x.valid |= PIANO_CHG_IN_VERIFY; x.verify = m.value[McaVerify];}
    if (v.mippsBusy) {x.valid |= PIANO_CHG_IN_VERIFY; x.verify = 1;}
    x.mipps_pending = (v.mippsPending || v.mippsBusy) ? 1 : 0;
    x.cp_first = v.cpFirst ? 1 : 0;
    if (has(McaTbat)) {x.valid |= PIANO_CHG_IN_TBAT; x.tbat_c = m.value[McaTbat];}
    if (has(McaAiclStatus)) {x.valid |= PIANO_CHG_IN_AICL; x.aicl_ma = m.value[McaAiclStatus];}
    if (has(McaPdos)) {
      x.valid |= PIANO_CHG_IN_PDOS;
      x.pdos = m.pdos < 7 ? m.pdos : 7;
      for (unsigned i = 0; i < x.pdos; ++i)
        for (unsigned k = 0; k < 3; ++k) x.pdo[i][k] = m.pdo[i][k];
    }
    if (v.contract) {
      x.contract_notify_pending = v.contract->notify_pending;
      x.contract_notify_ms = v.contract->notify_ms;
    }
    // cfg.qc's evidence that the partner is no PD source: a fresh status of a connected sink in a non-PD operation mode
    if (v.contract && piano_contract_status_fresh(v.contract, v.nowMs) && v.contract->connected && v.contract->sink &&
        !v.contract->pd)
      x.valid |= PIANO_CHG_IN_NONPD;
    // charge/mipps2: a fresh PD sink status whose RDO does not decode against the firmware's list (a verified MiPPS adapter's vendor
    // object). 0x21011 is the firmware's own list, not the source's objects by position: cp-first2 had RDO object 7 with a 4-entry
    // list whose first slot was no 5000/5000 object, cp-first1 object 7 with 2 entries. So no position test here: any RDO object
    // 1-7 with this cycle's list read. Ubuntu's 5 V PD input (xiaomi_mca_5v_input, xiaomi_mca_pd_5v_ma) never looks at the RDO; the
    // policy gives the class's 5 V row from a 5000/5000 entry at any slot (else 500 mA) and never a contract (no 9 V, PPS or pump).
    if (v.contract && piano_contract_status_fresh(v.contract, v.nowMs) && v.contract->connected && v.contract->sink &&
        v.contract->pd && !v.contract->valid && v.contract->last_error == PIANO_CONTRACT_RDO && (x.valid & PIANO_CHG_IN_PDOS) &&
        x.pdos) {
      const unsigned object = (v.contract->rdo >> 28) & 15u;
      if (object >= 1 && object <= 7) {
        x.valid |= PIANO_CHG_IN_PD_UNMAPPED;
        x.rdo_object = uint8_t(object);
      }
    }
    if (v.contract) x.contract_observing = v.contract->linked && !v.contract->quarantined &&
        v.contract->stage != PianoContractIdle &&
        (v.contract->last_error == PIANO_CONTRACT_OK || v.contract->last_error == PIANO_CONTRACT_CHANGED);
    // Only the selected raw PDO + current RDO grant a contract. MCA triplets
    // remain diagnostics and never authorize a new HV request.
    if (v.contract && piano_contract_fresh(v.contract, v.nowMs)) {
      const PianoChargeContract &c = *v.contract;
      x.valid |= PIANO_CHG_IN_CONTRACT | PIANO_CHG_IN_SOURCE;
      x.contract_generation = c.generation;
      x.contract_link_generation = m.links;
      x.contract_mv = c.mv; x.contract_ma = c.ma; x.contract_protocol = c.protocol; x.contract_object = c.object;
      x.fixed9_ma = x.pps9_ma = 0;
      bool fixed5 = false;
      for (unsigned i = 0; i < c.pdos && i < 7; ++i) {
        const uint32_t raw = c.source_pdo[i];
        if ((raw >> 30) == 0) {
          const uint32_t mv = ((raw >> 10) & 1023) * 50, ma = (raw & 1023) * 10;
          if (mv == 5000 && ma >= 100) fixed5 = true;
          if (mv == 9000 && ma >= 100 && ma <= 5000) x.fixed9_ma = ma;
        } else if ((raw >> 28) == 12) {
          const uint32_t lo = ((raw >> 8) & 255) * 100, hi = ((raw >> 17) & 255) * 100, ma = (raw & 127) * 50;
          if (lo < hi && lo <= 9000 && hi >= 9000 && lo >= 3000 && hi <= 21000 && ma >= 500 && ma <= 5000) {
            // With firmware PDOs, check the selected slot against this cycle's list too. An older UCSI snapshot can
            // outlive a new firmware offer until the next agreeing GET_CONNECTOR_STATUS pair.
            const bool matches = c.pdo_origin != PIANO_CONTRACT_PDO_FIRMWARE ||
                                 (i < c.offer_n && piano_contract_firmware_pdo_word(c.offer_pdo[i][0],
                                      c.offer_pdo[i][1], c.offer_pdo[i][2]) == raw);
            if ((x.valid & PIANO_CHG_IN_PDOS) &&
                (i >= x.pdos || piano_contract_firmware_pdo_word(x.pdo[i][0], x.pdo[i][1], x.pdo[i][2]) !=
                  piano_contract_firmware_pdo_word(lo, hi, ma))) continue;
            if (matches && ma > x.pps9_ma) {
              x.pps9_ma = ma;
              x.pps9_object = uint8_t(i + 1);
            }
          }
        }
      }
      if (fixed5 && has(McaPdState) && m.value[McaPdState] == 2 &&
          (!has(McaType) || x.type == PIANO_CHG_T_UNKNOWN || x.type == PIANO_CHG_T_FLOAT ||
           x.type == PIANO_CHG_T_TYPEC || x.type == PIANO_CHG_T_DCP)) {
        x.type = c.protocol == PIANO_CONTRACT_PPS ? PIANO_CHG_T_PPS : PIANO_CHG_T_PD;
        x.valid |= PIANO_CHG_IN_TYPE;
      }
    }
    if (m.board_allowed && m.board_done == 7) x.safety |= PIANO_CHG_SAFE_BOARD;
    if (v.safetyFresh && v.cpOff) x.safety |= PIANO_CHG_SAFE_CP_OFF;
    if (has(McaLpd) && m.value[McaLpd] == 0) x.safety |= PIANO_CHG_SAFE_LPD_DRY;
    if (v.safetyFresh && v.dtptValid && !v.dtpt) x.safety |= PIANO_CHG_SAFE_DTPT;
    if (has(McaLpd) && m.value[McaLpd] != 0) x.faults |= 1;
    if (v.safetyFresh && v.dtpt) x.faults |= 2;
    // the gauges only when both packs read (one charger feeds both)
    if (v.packs && v.packsFresh && v.packs[0].ok && v.packs[1].ok) {
      const GaugeReading &a = v.packs[0], &b = v.packs[1];
      x.valid |= PIANO_CHG_IN_GAUGES;
      x.soc_max_pct = a.socPct > b.socPct ? a.socPct : b.socPct;
      x.soc_min_pct = a.socPct > b.socPct ? b.socPct : a.socPct;
      x.vbat_max_mv = a.voltageMv > b.voltageMv ? a.voltageMv : b.voltageMv;
      x.t_warm_dc = a.tempDeciC > b.tempDeciC ? a.tempDeciC : b.tempDeciC;
      x.t_cool_dc = a.tempDeciC > b.tempDeciC ? b.tempDeciC : a.tempDeciC;
      x.ibat_ma = a.currentMa + b.currentMa;
      x.gauge_ms = v.packsMs;
    }
    if (v.port && v.port->ok) {
      x.valid |= PIANO_CHG_IN_TYPEC;
      x.typec_source = v.port->sourceSeen ? 1 : 0;
      x.rp_ma = v.port->sourceSeen ? rpMilliamps(v.port->rpBits) : 0;
    }
    if (ownBusGuard && ownBus(v.contract, v.port, v.nowMs)) x.valid |= PIANO_CHG_IN_OWN_BUS;
    // PianoThermal's: each temperature only with its valid bit (missing = unknown, never cool). The charger thermistor only from a
    // reading taken after charger_pd came up: its pin is the DSP firmware's, and before that firmware runs the channel reads the
    // pin's load (59.5-60.8 C at an idle board, 2026-11-12 queue5 P12; PianoThermal also holds it out while the pin is not
    // high-impedance, AdcChannels.h kPinGates).
    if (v.board && v.board->version == PIANO_BOARD_TEMPS_VERSION) {
      static const struct {unsigned index; uint32_t bit;} kMap[] = {
          {PIANO_BOARD_TEMP_BATTERY, PIANO_CHG_IN_NTC}, {PIANO_BOARD_TEMP_CHARGER, PIANO_CHG_IN_CHGR},
          {PIANO_BOARD_TEMP_CONNECTOR, PIANO_CHG_IN_CONN1}, {PIANO_BOARD_TEMP_CONNECTOR2, PIANO_CHG_IN_CONN2}};
      int32_t *const dst[] = {&x.t_ntc_dc, &x.t_chgr_dc, &x.t_conn1_dc, &x.t_conn2_dc};
      for (unsigned i = 0; i < 4; ++i) {
        if (!(v.board->valid & (1u << kMap[i].index))) continue;
        if (kMap[i].index == PIANO_BOARD_TEMP_CHARGER && !chargerReadingCounts(v)) continue;
        x.valid |= kMap[i].bit;
        *dst[i] = milliToDeci(v.board->milli_c[kMap[i].index]);
      }
    }
    if (v.skin && v.skin->version == PIANO_SKIN_TEMPS_VERSION && v.skin->virtual_valid) {
      const int32_t dc = milliToDeci(v.skin->virtual_milli_c);
      if (dc > -400 && dc < 1250) {
        x.valid |= PIANO_CHG_IN_SKIN;
        x.t_skin_dc = int16_t(dc);
      }
    }
  }

  // A decision is due: a codec cycle ended since the last one, or (handing over) the first time and after each write answer.
  bool due(const PianoMca &m, const LinkView &v) const
  {
    if (!m.external) return false;
    if (force_ || m.cycles != lastCycles_ || v.wakes != st.wakes) return true;
    const bool handover = v.willSleep || v.shutdown;
    if (!handover && v.nowMs != in.now_ms && piano_charge_pps_due(&st, v.nowMs)) return true;
    return handover && (!lastHandover_ || answers != answersAtDecision_);
  }

  void step(PianoMca &m, const LinkView &v, LinkOutcome &o)
  {
    o = LinkOutcome();
    if (!m.external) return;
    const bool handover = v.willSleep || v.shutdown;
    if (!v.chargerPdUp) {
      if (st.volt_want_prop == PIANO_CHG_PPS) cancelQueued(m);
      if (!pdNoted_) {pdNoted_ = true; o.waitingPd = true;}
      if (handover) o.handoverReady = !st.disabled && !st.pending;
      hold = false;
      return;
    }
    pdNoted_ = false;
    if (m.links != boardLinks_) {boardLinks_ = m.links; boardNoted_ = false;}
    if (!piano_mca_board_settled(&m)) {
      if (!boardNoted_) {boardNoted_ = true; o.waitingBoard = true;}
      if (handover) o.handoverReady = false;
      return;
    }
    if (!due(m, v)) {
      o.handoverReady = handover && lastHandover_ && plan.sleep_ready && !m.queued && !m.sent_write;
      return;
    }
    force_ = false;
    const bool newCycle = m.cycles != lastCycles_;
    lastCycles_ = m.cycles;
    lastHandover_ = handover;
    answersAtDecision_ = answers;
    const bool buckPps = st.volt_want_prop == PIANO_CHG_PPS;
    syncPpsOption(m);
    const bool wasOwnBus = (in.valid & PIANO_CHG_IN_OWN_BUS) != 0;
    fill(in, m, v, cfg.own_bus != 0);
    if (!wasOwnBus && (in.valid & PIANO_CHG_IN_OWN_BUS)) cancelQueued(m);
    if (v.shutdown) piano_charge_shutdown(&cfg, &in, &st, &plan);
    else piano_charge_decide(&cfg, &in, &st, &plan);
    if (buckPps && (handover || st.volt_want_prop != PIANO_CHG_PPS ||
                    !(in.valid & PIANO_CHG_IN_CONTRACT))) cancelQueued(m);
    decisions++;
    o.decided = true;
    o.events = plan.events;
    if (newCycle) readBack(m, o);
    if (plan.resync && !v.shutdown) {
      // the writes go to the new generation instead (the policy forgets this one when the generation moves)
      o.resync = true;
      resyncs++;
      for (auto &r : rb_) r = Pending();
      hold = false;
      return;
    }
    for (unsigned i = 0; i < plan.writes; ++i) {
      const PianoChargeWrite &w = plan.write[i];
      const int r = piano_mca_queue(&m, w.property, w.value, w.bytes);
      if (!r) {o.queued++; queuedTotal++; continue;}
      if (r == -2) {o.full++; continue;}   // left pending: the policy plans it again after 10 s
      piano_charge_ack(&st, w.property, w.value, -1);
      o.refused++;
      refusedTotal++;
      o.refusedProperty = w.property;
      o.refusedValue = w.value;
    }
    hold = plan.hold_awake && !handover;
    o.handoverReady = handover && plan.sleep_ready;
    if (plan.cls != shown_.cls || plan.icl_ma != shown_.icl || plan.fcc_ma != shown_.fcc || plan.hv != shown_.hv ||
        plan.charge != shown_.charge || plan.icl_why != shown_.why || (hold ? 1 : 0) != shown_.hold) {
      shown_ = {plan.cls, plan.hv, plan.charge, plan.icl_why, uint8_t(hold ? 1 : 0), plan.icl_ma, plan.fcc_ma};
      o.planChanged = true;
    }
  }

  void requestDecision() {force_ = true;}

  // A configuration/contract change revokes unsent policy writes. The single
  // already-sent transaction is still matched and its ACK reported; board
  // setup is never cancelled. No stale unsent enable/HV/up-current escapes.
  void cancelQueued(PianoMca &m)
  {
    unsigned dst = m.sent_write ? 1 : 0;
    for (unsigned i = dst; i < m.queued; ++i) {
      const uint32_t prop = m.queue[i].property;
      if (prop == PIANO_MCA_CP_STATE || prop == PIANO_MCA_OTG_CFG || prop == PIANO_MCA_CID_CFG) {
        m.queue[dst++] = m.queue[i];
      } else {
        const int slot = prop == PIANO_CHG_ENABLE ? PIANO_CHG_S_ENABLE : prop == PIANO_CHG_FCC ? PIANO_CHG_S_FCC :
                         prop == PIANO_CHG_ICL ? PIANO_CHG_S_ICL : prop == PIANO_CHG_FV ? PIANO_CHG_S_FV :
                         prop == PIANO_CHG_PD_FIXED || prop == PIANO_CHG_PPS ? PIANO_CHG_S_VOLT : -1;
        if (slot >= 0) st.pending &= uint8_t(~(1u << slot));
        // Cancellation is not a firmware refusal. An unsent stop alone
        // creates no right to lift a firmware-owned disable afterwards.
        if (prop == PIANO_CHG_ENABLE && !m.queue[i].value && !disableObligation_) st.disabled = 0;
        force_ = true;
      }
    }
    m.queued = uint8_t(dst);
  }

  // Called only when the owner hands this serialized request to GLINK.
  void sent(const PianoMca &m)
  {
    if (m.sent_write && m.queued && m.queue[0].property == PIANO_CHG_ENABLE && !m.queue[0].value)
      disableObligation_ = true;
  }

  // The codec answered a write (McaWritten): the policy learns the result; a taken limit, enable or voltage is read back next cycle.
  void answered(const PianoMca &m, uint64_t now = 0, bool late = false)
  {
    if (!m.external) return;
    if (m.last_property == PIANO_MCA_PPS && late) return;   // a timed-out renew cannot extend the PPS deadline
    answers++;
    const int32_t result = m.last_property == PIANO_MCA_PPS && piano_charge_pps_expired(&st, now) ? -60 : m.last_result;
    piano_charge_ack(&st, m.last_property, m.last_value, result);
    if (result) return;
    if (m.last_property == PIANO_CHG_ENABLE) disableObligation_ = m.last_value == 0;
    const int slot = m.last_property == PIANO_MCA_ICL ? 0 : m.last_property == PIANO_MCA_CHARGE_ENABLE ? 1
                     : m.last_property == PIANO_MCA_PD_FIXED || m.last_property == PIANO_MCA_PPS ? 2 : -1;
    if (slot >= 0) rb_[slot] = Pending{m.last_property, m.last_value, m.cycles, true};
  }

  // The owner's timer gave up on the request in flight: a write's goes to the policy as no answer (-60, a refusal).
  void timeout(PianoMca &m)
  {
    const bool write = m.in_flight && m.sent_write;
    piano_mca_timeout(&m);
    if (write) answered(m);
  }

 private:
  bool ppsEnabled() const {return piano_charge_pps_enabled(&cfg);}
  struct Pending {uint32_t property = 0, value = 0, cycle = 0; bool armed = false;};
  struct Shown {uint8_t cls = 0xff, hv = 0xff, charge = 0xff, why = 0xff, hold = 0xff; uint32_t icl = 0, fcc = 0;};
  uint32_t lastCycles_ = 0, answersAtDecision_ = 0, boardLinks_ = 0;
  bool force_ = false, disableObligation_ = false;
  bool lastHandover_ = false, pdNoted_ = false, boardNoted_ = false;
  Pending rb_[3];
  Shown shown_;

  void readBack(const PianoMca &m, LinkOutcome &o)
  {
    for (Pending &p : rb_) {
      if (!p.armed || m.cycles <= p.cycle || o.readbacks >= 3) continue;
      LinkReadback &r = o.readback[o.readbacks++];
      r.property = p.property;
      r.written = p.value;
      if (p.property == PIANO_MCA_ICL) {
        r.readable = piano_mca_has(&m, McaIcl) != 0;
        r.read = m.value[McaIcl];
        r.agrees = r.readable && r.read <= int32_t(p.value) + 50 && r.read + 50 >= int32_t(p.value);
      } else if (p.property == PIANO_MCA_CHARGE_ENABLE) {
        r.readable = piano_mca_has(&m, McaChargeEnable) != 0;
        r.read = m.value[McaChargeEnable];
        r.agrees = r.readable && r.read == int32_t(p.value);
      } else {
        // the bus voltage request: 9 V counts in 8.0-9.6 V, 5 V below 6 V (the PPS word asks for 9 V)
        r.readable = piano_mca_has(&m, McaBusVoltage) != 0;
        r.read = m.value[McaBusVoltage] / 1000;
        const bool nine = p.property == PIANO_MCA_PPS || p.value == 9000;
        r.agrees = r.readable && (nine ? r.read >= 8000 && r.read <= 9600 : r.read < 6000);
      }
      p.armed = false;
    }
  }
};

}  // namespace pianopower
#endif
