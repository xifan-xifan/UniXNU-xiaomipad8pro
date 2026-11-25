// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano: the SC8541 2:1 charge-pump (direct charging) session, a pure function of its inputs (charge/cp-sess).
//
// No I/O here. PianoPower's worker owns the pump (ChargePump.h: the driver, struct piano_cp_in / piano_cp_out) and the MiPPS
// engine (ChargeMipps.h). It calls piano_cp_decide() at least every 100 ms while a session runs (every codec cycle while idle),
// executes plan.out (START / STOP / SET_BUSOCP, the PPS word) and plan.qc_pulse, and reports back what was taken.
//
// The buck policy (ChargePolicy.c) keeps every limit of its own. While the session holds the source (plan.owns_volt) the worker
// clears PIANO_CHG_SAFE_CP_OFF, so the strict buck session plans no voltage and holds its input and charge current at the
// reduced-current barrier (500 mA; plan.out.buck_icl_ma / buck_fcc_ma say the same). When a session ends it first stops the pump
// (STOP, then waits for CHG_EN to read clear), then hands the raised request back (plan.handback ->
// piano_charge_cp_handback()), and the buck session takes the source to vSafe5V (or 5 V QC) through its barrier before anything
// is raised again.
//
// Behaviour and numbers: the stock Android / Ubuntu v1.4.0 pump session read as a specification (STACK.md §2.2, CP-SPEC.md);
// where the two differ the safer value is used (CP-SESSION.md lists both). Off unless piano-power-cp=1 AND the per-boot root arm.
#ifndef PIANO_CHARGE_PUMP_SESSION_H
#define PIANO_CHARGE_PUMP_SESSION_H
#include <stdint.h>
#include "ChargePump.h"
#include "ChargePolicy.h"
#ifdef __cplusplus
extern "C" {
#endif

/* the source the pump runs from */
enum {PIANO_CPS_SRC_NONE = 0, PIANO_CPS_SRC_PPS = 1, PIANO_CPS_SRC_QC3B = 2, PIANO_CPS_SRC_QC35 = 3};

/* the session's states */
enum {
    PIANO_CPS_IDLE = 0,      /* not running; the gates are checked every call */
    PIANO_CPS_BUCK_HOLD,     /* the source is ours; waiting for the buck's 500 mA barrier to be taken */
    PIANO_CPS_SET_START,     /* the start voltage asked (PPS word, or QC pulses); waiting for the contract and the bus */
    PIANO_CPS_ENABLING,      /* START asked; waiting for CHG_EN to read set */
    PIANO_CPS_OPENING,       /* +40 mV steps until the pump carries 600 mA on the bus */
    PIANO_CPS_CC,            /* constant current, 700 ms steps */
    PIANO_CPS_CV,            /* constant voltage / taper, 500 ms steps */
    PIANO_CPS_STOPPING,      /* STOP asked; the source held until CHG_EN reads clear */
    PIANO_CPS_LATCHED,       /* too many fault exits this attach (or the driver lost the pump): off until the next attach */
    PIANO_CPS_STATES
};

/* why a session ended (plan.exit_reason; also out.stop_reason) */
enum {
    PIANO_CPS_EXIT_NONE = 0, PIANO_CPS_EXIT_DETACH, PIANO_CPS_EXIT_SLEEP, PIANO_CPS_EXIT_DISARM, PIANO_CPS_EXIT_HEALTH,
    PIANO_CPS_EXIT_GAUGE, PIANO_CPS_EXIT_TBAT, PIANO_CPS_EXIT_CONN, PIANO_CPS_EXIT_TAPER, PIANO_CPS_EXIT_CONTRACT,
    PIANO_CPS_EXIT_BUCK,
    /* from here: faults (counted towards the latch) */
    PIANO_CPS_EXIT_FAULT_FIRST,
    PIANO_CPS_EXIT_PUMP_FAULT = PIANO_CPS_EXIT_FAULT_FIRST,   /* the driver reported a fault (plan.exit_faults) */
    PIANO_CPS_EXIT_TDIE, PIANO_CPS_EXIT_CELL_OCP, PIANO_CPS_EXIT_BUS_HIGH, PIANO_CPS_EXIT_WATCHDOG, PIANO_CPS_EXIT_OPEN_FAIL,
    PIANO_CPS_EXIT_ENABLE_FAIL, PIANO_CPS_EXIT_START_FAIL, PIANO_CPS_EXIT_ERRORS, PIANO_CPS_EXIT_STUCK, PIANO_CPS_EXIT_LOST,
    PIANO_CPS_EXIT_REASONS
};

/* events of one call (plan.events) */
enum {
    PIANO_CPS_EV_START = 1u << 0, PIANO_CPS_EV_ENABLED = 1u << 1, PIANO_CPS_EV_OPENED = 1u << 2, PIANO_CPS_EV_CV = 1u << 3,
    PIANO_CPS_EV_EXIT = 1u << 4, PIANO_CPS_EV_HANDBACK = 1u << 5, PIANO_CPS_EV_LATCHED = 1u << 6, PIANO_CPS_EV_DERATE = 1u << 7,
    PIANO_CPS_EV_ERROR = 1u << 8, PIANO_CPS_EV_STUCK = 1u << 9, PIANO_CPS_EV_UNLATCHED = 1u << 10,
};

/* a QC pulse request rides in out.req_valid next to ChargePump.h's PPS/BUCK bits */
#define PIANO_CPS_REQ_QC_PULSE 0x100u

typedef struct {
    uint8_t qc3;             /* the QC3-B / QC3.5 pump path (0x201d pulses); PPS only without it */
    uint8_t full_charge;     /* the float option (FFC table to 4540 mV): never with the 80/75 health limit on */
    uint8_t latch_faults;    /* fault exits per attach before the latch (0: PIANO_CPS_LATCH_DEFAULT) */
    uint8_t soc_entry_max;   /* start only below this charge, % (0: 91, the stock gate); piano_charge_cp_sys() lowers it to the
                              * health limit's stop - 3 while that limit is on */
    uint8_t mipps;           /* the MiPPS engine runs on this boot: the session waits for drv->mipps_done (else 5 A, no wait) */
} PianoCpConfig;

/* the system side of one call (the buck policy, the gauges, the source); piano_charge_cp_sys() fills it from ChargePolicy */
typedef struct {
    uint64_t now_ms;
    uint32_t attach_seq;     /* bumped per attach: a new attach clears the latch */
    uint8_t attached, awake, sleep_pending;
    uint8_t health_hold;     /* the 80/75 battery-health stop is in force */
    uint8_t soc_entry_max;   /* 0: the config's */
    /* the source */
    uint8_t src_kind;        /* PIANO_CPS_SRC_* */
    uint32_t apdo_min_mv, apdo_max_mv, apdo_ma;   /* PPS: the APDO the session uses */
    uint8_t contract_valid;  /* a validated contract now (ChargePolicy's strict contract) */
    uint8_t contract_pps;    /* and it is a PPS one */
    uint32_t contract_mv, contract_ma;
    uint8_t mipps_cleanup_pending;   /* ChargeMipps.h: no voltage request while set */
    /* the firmware's bus reading */
    uint8_t bus_valid;
    int32_t vbus_mv, ibus_ma;
    /* the buck as the firmware took it */
    uint32_t buck_icl_set_ma, buck_fcc_set_ma;
    uint8_t buck_pending;    /* an input or charge current write of the buck not answered yet */
    /* the session's voltage requests as taken: the last PPS word acknowledged, a write in flight, QC pulses taken (net) */
    uint32_t pps_taken_word;
    uint8_t volt_pending;
    int32_t qc_pulses_taken;
    /* the gauges (both packs) */
    uint8_t gauges_valid;
    int32_t soc_pct;         /* the higher pack's */
    int32_t vbat_max_mv, vbat_min_mv;
    int32_t ibat_a_ma, ibat_b_ma;   /* + = charging */
    uint8_t cells_valid;            /* vbat_min_mv and the two packs' currents read from the gauges (the worker's; the glue
                                     * cannot know them: without them no session starts and a running one ends) */
    int32_t t_cool_dc, t_warm_dc;   /* 0.1 degC */
    /* the connectors' thermistor (the warmer), 0.1 degC */
    uint8_t conn_valid;
    int32_t t_conn_dc;
} PianoCpSys;

typedef struct {
    uint8_t state;
    uint32_t attach_seq;
    uint8_t attach_known;
    uint8_t faults_this_attach, errors;
    uint8_t exit_reason;
    uint32_t exit_faults;
    uint64_t state_ms, next_ms, retry_ms, awake_since_ms, request_ms, stop_ms;
    uint8_t was_awake, tries, start_tries, ocp_strikes, start_sent, busocp_set;
    uint32_t req_mv, req_ma, req_word, busocp_ma;
    int32_t qc_pulses_want;
    uint32_t ibat_target_ma, ibus_target_ma, derate_ma;
    PianoChargeCpState curve;
    uint32_t sessions;
    uint64_t contract_ok_ms;         /* the last call with a validated PPS contract (PPS sessions) */
} PianoCpState;

typedef struct {
    struct piano_cp_out out;     /* ChargePump.h: the driver action, the bus OCP, the PPS word, the buck share */
    uint8_t state;
    uint8_t owns_volt;           /* the session holds the source: SAFE_CP_OFF clear, the buck plans no voltage */
    uint8_t qc_pulse;            /* PIANO_CHG_QC_UP / PIANO_CHG_QC_DOWN, with PIANO_CPS_REQ_QC_PULSE */
    uint32_t pps_word;           /* with PIANO_CP_REQ_PPS: (mV / 20) << 16 | mA / 50 */
    uint32_t ibat_target_ma, ibus_target_ma;
    uint8_t handback;            /* the session ended: piano_charge_cp_handback(handback_prop, handback_value) this call */
    uint32_t handback_prop, handback_value;
    uint8_t exit_reason;
    uint32_t exit_faults;
    uint8_t sleep_ready;         /* the pump reads off and the session holds nothing */
    uint8_t latched;
    uint32_t events;
} PianoCpPlan;

#define PIANO_CPS_LATCH_DEFAULT 3u
#define PIANO_CPS_SOC_ENTRY 91            /* stock / Ubuntu: SOC < 91 */
#define PIANO_CPS_VBAT_MIN_MV 3450        /* stock min_vbat */
#define PIANO_CPS_VBAT_START_MAX_MV 4473  /* CP-SPEC: start VBAT 4473 (stock max_vbat 4550): the lower */
#define PIANO_CPS_TBAT_START_MIN 15       /* start 15-37 degC (the stock rows; Ubuntu's 8-55 gate is wider) */
#define PIANO_CPS_TBAT_START_MAX 37
#define PIANO_CPS_TBAT_EXIT_MIN 15        /* leave below 15 or from 45 degC */
#define PIANO_CPS_TBAT_EXIT_MAX 45
#define PIANO_CPS_CONN_MAX_C 50           /* the connectors: our own rule (the buck's above-1.5 A rule) */
#define PIANO_CPS_TDIE_START_MAX 115      /* start only below the derate's release */
#define PIANO_CPS_TDIE_DERATE 120         /* from 120 degC: -200 mA a poll, down to 1000 mA */
#define PIANO_CPS_TDIE_RELEASE 115        /* below: back a quarter as fast */
#define PIANO_CPS_TDIE_EXIT 125           /* stock die_temp_max */
#define PIANO_CPS_STAGE_IBUS_MA 3800u     /* (8000 / 2 - 200) */
#define PIANO_CPS_POWER_MW 67000u         /* stock max_power 67000 */
#define PIANO_CPS_IBAT_MAX_MA 12000u      /* CP-SPEC IBAT 12000 (Ubuntu 12400): the lower */
#define PIANO_CPS_CELL_OCP_MA 7000        /* CP-SPEC cell OCP 7000, one pack, absolute */
#define PIANO_CPS_EXIT_IBAT_MA 2500u      /* below this target the buck finishes (stock FFC iterm 2350: the earlier exit) */
#define PIANO_CPS_QC3B_IBAT_MA 5000u      /* stock qc3_ibat_max_limit */
#define PIANO_CPS_QC35_IBAT_MA 4200u      /* stock qc3p5 */
#define PIANO_CPS_QC3B_STEP_MV 200u       /* QC3: 200 mV a pulse */
#define PIANO_CPS_QC35_STEP_MV 20u        /* QC3.5: 20 mV a pulse */
#define PIANO_CPS_REQ_MIN_MV 5000u
#define PIANO_CPS_REQ_MAX_MV 11000u       /* = BUSOVP (CP-SPEC 11000): the request never reaches the trip */
#define PIANO_CPS_ERRORS_MAX 11u          /* Ubuntu: total errors >= 11 end the session */

void piano_cp_config_default(PianoCpConfig *c);
void piano_cp_init(PianoCpState *s);
/* One call (see above). Repeat while plan.sleep_ready is 0 during a sleep. */
void piano_cp_decide(const PianoCpConfig *c, const PianoCpSys *sys, const struct piano_cp_in *drv, PianoCpState *s,
                     PianoCpPlan *p);
/* The firmware writes the session may ever make: a PPS word 5000-11000 mV (20 mV steps) at 1000-6000 mA (50 mA steps), one QC
 * pulse up or down (only with cfg.qc3). Anything else: 0. */
int piano_cp_write_allowed(const PianoCpConfig *c, uint32_t property, uint32_t value, uint8_t bytes);
/* The battery current the session targets: the voltage table (normal, or FFC with cfg.full_charge), the temperature steps, the
 * source (twice the bus budget), the QC rows and the die-temperature derate (0: none). */
uint32_t piano_cp_ibat_target(const PianoCpConfig *c, const PianoCpSys *sys, const struct piano_cp_in *drv, PianoCpState *s,
                              int running, uint32_t vbus_mv);
/* The bus budget: min(APDO, the MiPPS budget 6000 verified / 5000), within 67 W at mv, and the stage limit 3800. */
uint32_t piano_cp_ibus_budget(const PianoCpSys *sys, const struct piano_cp_in *drv, uint32_t mv);
const char *piano_cp_state_name(int state);
const char *piano_cp_exit_name(int reason);

/* ---- the buck side (ChargePolicy.c): the glue, host-tested with the policy ---- */
/* The system side of a call from the buck policy's input and state (contract, source APDO covering the pump's range, the buck as
 * taken, the gauges, the connectors, the health hold; the latch's attach count is the caller's). */
void piano_charge_cp_sys(const PianoChargeConfig *c, const PianoChargeInput *in, const PianoChargeState *s, PianoCpSys *sys);
/* The session ended and the pump reads off: the request it left in force is recorded as taken, so the strict buck session's own
 * withdrawal (the reduced-current barrier, then vSafe5V or 5 V QC) runs before anything is raised again, and no 9 V before
 * HV_WAIT_MS. */
void piano_charge_cp_handback(PianoChargeState *s, uint32_t property, uint32_t value, uint64_t now);

#ifdef __cplusplus
}
#endif
#endif
