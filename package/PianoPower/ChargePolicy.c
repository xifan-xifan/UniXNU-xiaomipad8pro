/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The charging policy (ChargePolicy.h; docs/drivers/charging-policy.md). Written fresh from the behaviour and the numbers of the
 * Ubuntu history pages and the GPL xiaomi_mca driver read as a specification; no code taken. */
#include "ChargePumpSession.h"   /* charge/cp-sess: the glue at the end of this file */
#include "ChargePolicy.h"

#define ONLINE_MV 4000           /* a bus above 4 V: a source is attached */
#define HV_MV 9000u
#define HV_MIN_MV 8000           /* 9 V counts as reached in 8.0-9.6 V */
#define HV_MAX_MV 9600
#define HV_5V_MAX_MV 6000        /* below: the bus is at 5 V */
#define HV_WAIT_MS 15000u        /* after an attach, a wake or a new generation (stock waits 15 s for the authentication) */
#define HV_SETTLE_MS 15000u      /* the bus has this long (and three decisions) to reach 9 V */
#define HV_SETTLE_DECISIONS 3
#define HV_RENEG_MS 10000u       /* a source back at 5 V by itself is asked again after this, */
#define HV_RENEG_MAX 3           /* this often per attach */
#define HV_RETRY_MS 60000u       /* a detach while 9 V was asked (a hard reset in answer looks like one) */
#define HV_FIXED_MIN_MA 100u
#define HV_PPS_MIN_MA 500u
#define HV_PPS_MAX_MA 3000u
#define NOTIFY_HOLD_MS 3000u      /* T-14b: a UCSI notification in flight younger than this holds the limits in force, */
#define NOTIFY_HOLD_MAX 3u        /* for at most this many decisions in a row; then the contract counts as lost */
#define HV_PD_ROW_MA 1600u       /* stock in_pd: a fixed 9 V PD source's input row; above it only with T-14 9 V's connector rule */
#define FCC_TBAT_MIN 15
#define FCC_DERATE_FROM 48
#define FCC_DERATE_STEP 300u
#define FCC_RAISE_MS 30000u
#define TERM_HOT_C 48
#define TERM_COOL_C 46
#define LIMIT_HOT_C 55           /* the battery-health mode's own stop window, inside the firmware's 0-55 degC JEITA */
#define LIMIT_HOT_RELEASE_C 53
#define LIMIT_COLD_C 0
#define LIMIT_COLD_RELEASE_C 2
#define SOC_FAILURES_STOP 2u
#define STOP_CHECK_MS 60000u
#define STOP_LEAK_MA 100
#define PENDING_MS 10000u        /* a planned write with no answer is planned again after this (the owner dropped it) */
#define TRIES 3
#define MISMATCHES 2
#define REWRITES 3
#define ICL_SLACK_MA 50
#define WEDGE_MS 30000u
#define WEDGE_DECISIONS 3
#define WEDGE_RESYNCS 2
#define UNKNOWN_ENABLE 0xffu
/* The Type-C current (cfg.rp). The firmware's own input-current limiting (AICL) reads off on piano (0x2012 = 0), so nothing would
 * walk a limit down that the source cannot hold: a bus below 4.5 V under an Rp limit, or two detaches within 30 s of one taking,
 * end the trust for the boot. */
#define RP_MIN_MA 1500u          /* above the default USB power: the 1.5 A and 3.0 A advertisements */
#define RP_SAG_MV 4500
#define RP_QUICK_MS 30000u
#define RP_STRIKES 2
/* The stepped Rp limit (cfg.ramp, docs/drivers/charging-policy.md §15): with that limiting off, the policy finds what the source
 * and the cable hold by itself, one step at a time, demand-driven, with a bounded cool-down after a failed step. 2026-11-12 on the
 * Mac's port (CDP, Rp 3.0 A): 900 mA in -> the bus at 5.05 V and 897 mA, 1600 mA in -> 4.89 V and 1594 mA (about 0.21 Ohm from
 * the source to the charger), so 3 A would leave about 4.6 V. */
#define RAMP_HOLD_MS 10000u      /* a step taken is held this long, */
#define RAMP_HOLD_DECISIONS 2    /* and this many decisions (a charger cycle read under it), before the next */
#define RAMP_UP_MV 4650          /* the bus under a step must read this much for the next (the sag rule's 4.5 V plus headroom) */
#define RAMP_BC12_MA 1500u       /* up to here without a connector reading: BC 1.2's CDP/DCP current and the 1.5 A Rp, as the DCP row */
#define RAMP_USED_MIN_MA 100u    /* the input counts as used up within max(this, an eighth of the step) of the step */
#define RAMP_AICL_SETTLE_MS 15000u   /* 0x2029 below the step and not risen for this long: the firmware's own limit is the one to use up
                                      * (2026-11-12, chg-ramp: 1750-1800 under 2000 asked for eight minutes, the bus at 1.33-1.71 A) */
static const uint32_t kRampSteps[] = {1500u, 2000u, 2500u, 3000u};
/* The charging restart (cfg.kick, docs/drivers/charging-policy.md §17). 2026-11-12: in every native boot the charger stopped charging
 * at about 40 s of uptime (level 1, the PMIC alone: +476 mA, then 0 or less for the rest of the boot), and once the DSP firmware ran
 * and took the limits, the packs stayed at exactly 0 mA (the firmware's own 0x2027 too) while the bus current moved with the system
 * under the limit: the input on, the battery not charged. 0x2003 is the firmware's charging enable (the stock AP driver's). */
#define KICK_IDLE_MA 30          /* the packs within this of 0 mA: neither charged nor discharged */
#define KICK_IDLE_MS 20000u      /* so over two gauge readings at least this far apart */
#define KICK_SPARE_MA 150        /* the input at least this far below the limit it is held to */
#define KICK_SOC_MAX 95          /* not near full: there 0 mA is the charger's termination */
#define KICK_FV_MARGIN_MV 30     /* the higher pack at least this far below the float voltage in force */
#define KICK_T_MIN_C 15          /* the coolest reading at least (the charge current's own floor below) */
#define KICK_T_MAX_C 45          /* the warmest at most (the firmware's JEITA window is 0-55) */
#define KICK_SETTLE_MS 15000u    /* the first gauge reading this long after the resume judges the restart */
#define KICK_CHARGING_MA 50      /* the packs taking this much: charging again */
#define KICK_GAP_MS 120000u      /* between two restarts */
#define KICK_MAX 3               /* per attach, wake or generation */
#define KICK_STAGE_MS 60000u     /* the restart's write not through within this: abandoned (the enable's rules still end at 1) */
#define KICK_OFF_MS 5000u        /* the 0 held this long before the enable's rule lifts it (one charger cycle) */
/* The board thermistors (docs/drivers/thermal.md 12.7): only ever more conservative. */
#define CONN_HOT_C 60            /* a connector at or above: the input at 100 mA (the detach value) */
#define CONN_COOL_C 50           /* until both read below; also the most an Rp limit is allowed with */
#define CONN_ICL_MA 100u
#define CHGR_HOT_C 85            /* the charger at or above: the input at 500 mA at most */
#define CHGR_COOL_C 75
#define CHGR_ICL_MA 500u
#define NTC_APART_C 10           /* the battery thermistor this far from the packs: logged (the warmer counts anyway) */

static void zero(void *p, unsigned n) {uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0;}
static uint32_t umin(uint32_t a, uint32_t b) {return a < b ? a : b;}
static uint32_t umax(uint32_t a, uint32_t b) {return a > b ? a : b;}
static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi) {return v < lo ? lo : v > hi ? hi : v;}
static int32_t floor_c(int32_t dc) {return dc >= 0 ? dc / 10 : -((-dc + 9) / 10);}
static int is_pd(int cls) {return cls == PIANO_CHG_C_PD || cls == PIANO_CHG_C_PD_PPS;}
static uint32_t icl_cap(const PianoChargeConfig *c) {return c->icl_cap_ma ? umin(c->icl_cap_ma, PIANO_CHG_ICL_ABS_MAX) : PIANO_CHG_ICL_CAP;}
static uint32_t fcc_cap(const PianoChargeConfig *c) {return c->fcc_cap_ma ? umin(c->fcc_cap_ma, PIANO_CHG_FCC_ABS_MAX) : PIANO_CHG_FCC_CAP;}
/* The Rp limit's ceiling: the stepped one may go to rp_cap_ma (within the Type-C current's 3 A), the immediate one stays in icl_cap. */
static int ramped(const PianoChargeConfig *c) {return c->rp && c->ramp;}
static uint32_t rp_cap(const PianoChargeConfig *c)
{
    return ramped(c) && c->rp_cap_ma ? umin(c->rp_cap_ma, PIANO_CHG_RP_ABS_MAX) : icl_cap(c);
}
/* cfg.hv_caps (charge/pd1): the stock 9 V rows at a validated 9 V contract, only in the strict session */
static int hv_caps(const PianoChargeConfig *c) {return c->strict_contract && c->hv_caps;}
uint32_t piano_charge_icl_max(const PianoChargeConfig *c)
{
    return umax(umax(icl_cap(c), ramped(c) ? rp_cap(c) : 0), hv_caps(c) ? PIANO_CHG_ICL_ABS_MAX : 0);
}

/* ---------------------------------------------------------------------------------------------------------------- configuration */
void piano_charge_config_default(PianoChargeConfig *c)
{
    zero(c, sizeof *c);
    c->writes = 1;
    c->stop_pct = 80;
    c->resume_pct = 75;
    c->icl_cap_ma = PIANO_CHG_ICL_CAP;
    c->fcc_cap_ma = PIANO_CHG_FCC_CAP;
    c->kick = 1;
}

int piano_charge_config_limit(PianoChargeConfig *c, unsigned on, unsigned stop_pct, unsigned resume_pct)
{
    if (!on) {c->limit = 0; return 0;}
    if (!resume_pct && stop_pct >= 45) resume_pct = stop_pct - 5;
    if (stop_pct < 50 || stop_pct > 100 || resume_pct < 40 || resume_pct >= stop_pct) return -1;
    c->limit = 1;
    c->stop_pct = (uint8_t)stop_pct;
    c->resume_pct = (uint8_t)resume_pct;
    return 0;
}

int piano_charge_config_transaction(PianoChargeConfig *c, unsigned on, unsigned stop_pct, unsigned resume_pct,
                                    unsigned mode, unsigned admin_stop)
{
    PianoChargeConfig next = *c;
    if (on > 1 || mode > 2 || admin_stop > 1) return -1;
    /* Validate thresholds even when disabling: a transaction always carries a complete usable configuration. */
    if (piano_charge_config_limit(&next, 1, stop_pct, resume_pct)) return -1;
    next.limit = (uint8_t)on;
    next.hv = (uint8_t)mode;
    next.admin_stop = (uint8_t)admin_stop;
    *c = next;
    return 0;
}

/* ----------------------------------------------------------------------------------------------------------------- the tables */
int piano_charge_class(int32_t type)
{
    switch (type) {
    case PIANO_CHG_T_SDP: return PIANO_CHG_C_SDP;
    case PIANO_CHG_T_CDP: return PIANO_CHG_C_CDP;
    case PIANO_CHG_T_DCP: case PIANO_CHG_T_HVDCP: case PIANO_CHG_T_HVDCP3: case PIANO_CHG_T_HVDCP3_B: case PIANO_CHG_T_HVDCP3P5:
        return PIANO_CHG_C_DCP;   /* stock maps QC to DCP at 5 V */
    case PIANO_CHG_T_FLOAT: case PIANO_CHG_T_OCP: return PIANO_CHG_C_FLOAT;
    case PIANO_CHG_T_TYPEC: return PIANO_CHG_C_TYPEC;
    case PIANO_CHG_T_PD: return PIANO_CHG_C_PD;
    case PIANO_CHG_T_PD_VERIFY: case PIANO_CHG_T_PPS: return PIANO_CHG_C_PD_PPS;
    default: return PIANO_CHG_C_REFUSED;
    }
}

const char *piano_charge_class_name(int cls)
{
    static const char *const names[] = {"none", "SDP", "CDP", "DCP/QC", "float/OCP", "Type-C", "PD", "PD PPS", "refused"};
    return cls >= 0 && cls <= PIANO_CHG_C_REFUSED ? names[cls] : "?";
}

uint32_t piano_charge_icl_5v(int cls, uint32_t pdo5v_ma)
{
    switch (cls) {
    case PIANO_CHG_C_SDP: return 500;
    case PIANO_CHG_C_CDP: return 900;
    case PIANO_CHG_C_DCP: return 1500;
    case PIANO_CHG_C_FLOAT: case PIANO_CHG_C_TYPEC: return 500;
    case PIANO_CHG_C_PD: case PIANO_CHG_C_PD_PPS: return pdo5v_ma ? clampu(pdo5v_ma, PIANO_CHG_ICL_DEFAULT, 1500) : 500;
    default: return 0;
    }
}

uint32_t piano_charge_icl_rp(int cls, uint32_t row_ma, uint32_t rp_ma, uint32_t cap_ma)
{
    switch (cls) {
    case PIANO_CHG_C_SDP: case PIANO_CHG_C_CDP: case PIANO_CHG_C_DCP: case PIANO_CHG_C_FLOAT: case PIANO_CHG_C_TYPEC: break;
    default: return row_ma;   /* a PD contract governs a PD source; nothing for an unknown one */
    }
    if (rp_ma < RP_MIN_MA) return row_ma;
    const uint32_t ma = umin(rp_ma, cap_ma);
    return ma > row_ma ? ma : row_ma;
}

uint32_t piano_charge_ramp_next(uint32_t from_ma, uint32_t goal_ma)
{
    for (unsigned i = 0; i < sizeof kRampSteps / sizeof kRampSteps[0]; ++i)
        if (kRampSteps[i] > from_ma) return umin(kRampSteps[i], goal_ma);
    return umin(umax(from_ma, goal_ma), PIANO_CHG_RP_ABS_MAX);
}

uint32_t piano_charge_ramp_back(uint32_t from_ma, uint32_t row_ma)
{
    uint32_t back = row_ma;
    for (unsigned i = 0; i < sizeof kRampSteps / sizeof kRampSteps[0]; ++i)
        if (kRampSteps[i] < from_ma && kRampSteps[i] > back) back = kRampSteps[i];
    return back < from_ma ? back : umin(from_ma, row_ma);
}

uint32_t piano_charge_icl_9v(int cls, uint32_t source_ma)
{
    const uint32_t ceiling = cls == PIANO_CHG_C_PD ? 1600u : cls == PIANO_CHG_C_PD_PPS ? 2500u : 0u;
    return ceiling && source_ma ? clampu(source_ma, PIANO_CHG_ICL_DEFAULT, ceiling) : 0;
}

uint32_t piano_charge_icl_qc3_9v(uint32_t source_ma)
{
    return source_ma ? clampu(source_ma, PIANO_CHG_ICL_DEFAULT, 1600u) : 0;
}

int piano_charge_is_qc3(int32_t type)
{
    return type == PIANO_CHG_T_HVDCP3 || type == PIANO_CHG_T_HVDCP3_B || type == PIANO_CHG_T_HVDCP3P5;
}

int piano_charge_is_qc(int32_t type) {return type == PIANO_CHG_T_HVDCP || piano_charge_is_qc3(type);}
/* stock qc2 / qc3 9 V rows (the Ubuntu port's xiaomi_mca hv limits, read as a specification) */
uint32_t piano_charge_qc_icl_9v(int32_t type) {return type == PIANO_CHG_T_HVDCP ? 1200u : piano_charge_is_qc3(type) ? 1600u : 0u;}
uint32_t piano_charge_qc_fcc_9v(int32_t type)
{
    return type == PIANO_CHG_T_HVDCP ? 2600u : piano_charge_is_qc3(type) ? PIANO_CHG_FCC_ABS_MAX : 0u;
}

uint32_t piano_charge_fcc_full(int cls, int hv_on)
{
    switch (cls) {
    case PIANO_CHG_C_SDP: return 500;
    case PIANO_CHG_C_CDP: return 900;
    case PIANO_CHG_C_DCP: return 2000;
    case PIANO_CHG_C_FLOAT: return 1000;
    case PIANO_CHG_C_TYPEC: return 500;
    case PIANO_CHG_C_PD: return hv_on ? 3000 : 2000;
    case PIANO_CHG_C_PD_PPS: return hv_on ? 3500 : 2000;
    default: return 0;
    }
}

uint32_t piano_charge_fcc_thermal(uint32_t full, int t_valid, int32_t t_c)
{
    if (!full) return 0;
    if (!t_valid || t_c < FCC_TBAT_MIN) return PIANO_CHG_FCC_DEFAULT;
    if (t_c <= FCC_DERATE_FROM || full <= PIANO_CHG_FCC_DEFAULT) return full;
    const uint32_t degrees = (uint32_t)(t_c - FCC_DERATE_FROM);
    if (degrees >= 20) return PIANO_CHG_FCC_DEFAULT;
    const uint32_t derate = degrees * FCC_DERATE_STEP;
    return derate < full - PIANO_CHG_FCC_DEFAULT ? full - derate : PIANO_CHG_FCC_DEFAULT;
}

uint32_t piano_charge_pdo_5v_ma(const uint32_t pdo[][3], unsigned n)
{
    uint32_t ma = 0;
    for (unsigned i = 0; i < n && i < 7; ++i)
        if (pdo[i][0] <= 5000 && pdo[i][1] >= 5000 && pdo[i][2] && (!ma || pdo[i][2] < ma)) ma = pdo[i][2];
    return ma;
}

uint32_t piano_charge_pdo_9v_ma(const uint32_t pdo[][3], unsigned n, int allow_pps, int *pps)
{
    uint32_t apdo = 0;
    *pps = 0;
    for (unsigned i = 0; i < n && i < 7; ++i) {
        if (pdo[i][0] == HV_MV && pdo[i][1] == HV_MV && pdo[i][2] >= HV_FIXED_MIN_MA) return pdo[i][2];
        if (pdo[i][0] < pdo[i][1] && pdo[i][0] <= HV_MV && pdo[i][1] >= HV_MV) apdo = umax(apdo, pdo[i][2]);
    }
    if (!allow_pps || apdo < HV_PPS_MIN_MA) return 0;
    *pps = 1;
    return umin(apdo, HV_PPS_MAX_MA);
}

uint32_t piano_charge_pps_word(uint32_t mv, uint32_t ma) {return (mv / 20) << 16 | (ma / 50);}

uint32_t piano_charge_fv_target(uint8_t *term_hot, int t_valid, int32_t t_warm_c)
{
    if (!t_valid || t_warm_c >= TERM_HOT_C) *term_hot = 1;
    else if (t_warm_c < TERM_COOL_C) *term_hot = 0;
    return *term_hot ? PIANO_CHG_FV_HOT : PIANO_CHG_FV_FULL;
}

int piano_charge_soc_hold(int hold, int32_t soc_pct, unsigned stop_pct, unsigned resume_pct)
{
    if (stop_pct >= 100) return 0;
    if (soc_pct >= (int32_t)stop_pct) return 1;
    if (soc_pct <= (int32_t)resume_pct) return 0;
    return hold;
}

uint32_t piano_charge_used_limit(uint32_t step_ma, uint32_t aicl_ma, int settled)
{
    return settled && aicl_ma >= PIANO_CHG_ICL_DEFAULT && aicl_ma < step_ma ? aicl_ma : step_ma;
}

/* ----------------------------------------------------------------------------------------------------------- the balance line */
#define BAL_MOVING_MA 50
#define BAL_SPARE_MA 150

void piano_charge_balance(const PianoChargeBalanceIn *in, PianoChargeBalance *o)
{
    zero(o, sizeof *o);
    const int input = in->input_valid && in->bus_mv >= ONLINE_MV;
    if (input) o->in_mw = (int32_t)((int64_t)in->bus_mv * in->ibus_ma / 1000);
    if (in->battery_valid) o->bat_mw = (int32_t)((int64_t)in->vbat_mv * in->ibat_ma / 1000);
    o->spare_ma = input && in->limit_ma > 0 ? in->limit_ma - in->ibus_ma : 0;
    if (!in->battery_valid) {o->verdict = PIANO_CHG_BAL_UNKNOWN; return;}
    /* the system: what the charger passes on (about 90 % of the input; not read) less what the battery takes */
    o->sys_mw = (input ? o->in_mw / 10 * 9 : 0) - o->bat_mw;
    if (!input) o->verdict = PIANO_CHG_BAL_ON_BATTERY;
    else if (in->ibat_ma >= BAL_MOVING_MA) o->verdict = PIANO_CHG_BAL_CHARGING;
    else if (in->ibat_ma <= -BAL_MOVING_MA) o->verdict = PIANO_CHG_BAL_DRAINING;
    else if (in->limit_ma > 0 && o->spare_ma >= BAL_SPARE_MA) o->verdict = PIANO_CHG_BAL_SPARE;
    else o->verdict = PIANO_CHG_BAL_USED_UP;
}

const char *piano_charge_balance_name(int verdict)
{
    static const char *const names[] = {"no battery reading", "on battery", "charging", "NOT CHARGING with input to spare",
                                        "input used up by the system", "draining: the system takes more than the input"};
    return verdict >= 0 && verdict <= PIANO_CHG_BAL_DRAINING ? names[verdict] : "?";
}

int piano_charge_write_allowed(const PianoChargeConfig *c, uint32_t property, uint32_t value, uint8_t bytes)
{
    switch (property) {
    case PIANO_CHG_ENABLE: return bytes == 1 && value <= 1;
    case PIANO_CHG_FCC:
        return bytes == 4 && value >= 100 &&
               value <= (hv_caps(c) ? PIANO_CHG_FCC_ABS_MAX : c->strict_contract ? umin(fcc_cap(c),1500) : fcc_cap(c));
    case PIANO_CHG_ICL: return bytes == 4 && value >= PIANO_CHG_ICL_DEFAULT && value <= piano_charge_icl_max(c);
    case PIANO_CHG_FV:
        return bytes == 4 && (value == PIANO_CHG_FV_DEFAULT ||
                              (c->full_charge && !c->strict_contract && (value == PIANO_CHG_FV_FULL || value == PIANO_CHG_FV_HOT)) ||
                              (c->fv_hot && c->strict_contract && value == PIANO_CHG_FV_HOT));
    case PIANO_CHG_PD_FIXED: return bytes == 4 && (value == 5000 || (c->hv && value == HV_MV));
    case PIANO_CHG_QC_VOLT: return bytes == 4 && (c->qc3 || (c->qc && c->strict_contract)) && (value == 5000 || value == HV_MV);
    case PIANO_CHG_PPS: {
        const uint32_t mv = (value >> 16) * 20, ma = (value & 0xffffu) * 50;
        return bytes == 4 && (c->hv >= 2 || (c->hv && c->hv_order && c->strict_contract)) && mv == HV_MV &&
               ma >= HV_PPS_MIN_MA && ma <= HV_PPS_MAX_MA;
    }
    default: return 0;
    }
}

/* ------------------------------------------------------------------------------------------------------------------ the state */
static int slot_of(uint32_t property)
{
    switch (property) {
    case PIANO_CHG_ENABLE: return PIANO_CHG_S_ENABLE;
    case PIANO_CHG_FCC: return PIANO_CHG_S_FCC;
    case PIANO_CHG_ICL: return PIANO_CHG_S_ICL;
    case PIANO_CHG_PD_FIXED: case PIANO_CHG_PPS: case PIANO_CHG_QC_VOLT: return PIANO_CHG_S_VOLT;   /* QC: charge/pd1 */
    case PIANO_CHG_FV: return PIANO_CHG_S_FV;
    default: return -1;
    }
}

static void reset_tries(PianoChargeState *s)
{
    for (unsigned i = 0; i < PIANO_CHG_SLOTS; ++i) s->tries[i] = s->mismatch[i] = s->rewrites[i] = 0;
}

/* A new step (or none): what was seen at the old one does not count for it. */
static void ramp_moved(PianoChargeState *s)
{
    s->ramp_taken_ms = 0;
    s->ramp_waits_shown = 0;
    s->ramp_aicl_ma = 0;
    s->ramp_aicl_ms = 0;
}

static void ramp_restart(PianoChargeState *s)
{
    s->ramp_ma = 0;
    s->ramp_step_ms = 0;
    s->ramp_decisions = 0;
    ramp_moved(s);
}

/* The charging restart back to idle; with count also its budget (a new attach, a wake, a new generation). */
static void kick_reset(PianoChargeState *s, int count)
{
    s->kick_stage = PIANO_CHG_KICK_IDLE;
    s->kick_idle_ms = 0;
    if (count) {s->kick_count = 0; s->kick_ms = 0; s->kick_result = 0;}
}

/* The restart's 0 is held KICK_OFF_MS before the enable's rule lifts it (only there: a detach, a sleep or a shutdown lifts it at once). */
static int kick_holding(const PianoChargeState *s, uint64_t now)
{
    return s->kick_stage == PIANO_CHG_KICK_STOPPING && now < s->kick_ms + KICK_OFF_MS;
}

/* The firmware is on its defaults (a new generation) or has to be told everything again (a wake): forget what it took. What is
 * about the battery (the battery-health hysteresis, a stop of ours that may still be in force) is kept. */
static void forget(PianoChargeState *s, uint64_t now)
{
    s->icl_set = s->fcc_set = s->fv_set = 0;
    s->enable_set = UNKNOWN_ENABLE;
    s->volt_set_prop = s->volt_set_value = s->volt_want_prop = s->volt_want_value = 0;
    s->touched = s->pending = 0;
    reset_tries(s);
    s->fcc_full = 0;
    s->hv = PIANO_CHG_HV_OFF;
    s->hv_qc = 0;
    s->hv_renegs = s->hv_checks = 0;
    if (s->hv_retry_ms < now + HV_WAIT_MS) s->hv_retry_ms = now + HV_WAIT_MS;
    s->attached = 0;
    s->type = -1;
    s->handover_fault = 0;
    s->wedge_checks = 0;
    s->wedge_since_ms = 0;
    s->rp_taken_ms = 0;   /* the trust itself and its strikes are about the source and stay */
    ramp_restart(s);      /* start from the row; the failed step's ceiling and retry deadline stay */
    kick_reset(s, 1);     /* a stop of the restart that may be in force is s->disabled's: lifted by the enable's rules */
}

void piano_charge_init(PianoChargeState *s)
{
    zero(s, sizeof *s);
    s->type = -1;
    s->enable_set = UNKNOWN_ENABLE;
    s->charge_ok = 1;
}

void piano_charge_ack(PianoChargeState *s, uint32_t property, uint32_t value, int32_t retcode)
{
    const int slot = slot_of(property);
    if (slot < 0) return;
    s->pending &= (uint8_t)~(1u << slot);
    if (retcode) {
        if (s->sleeping) s->handover_fault = 1;
        if (s->tries[slot] < 255) s->tries[slot]++;
        if (s->tries[slot] == TRIES) s->events |= PIANO_CHG_EV_GAVE_UP;
        return;
    }
    s->tries[slot] = 0;
    s->mismatch[slot] = 0;
    s->touched |= (uint8_t)(1u << slot);
    switch (slot) {
    case PIANO_CHG_S_ENABLE:
        s->enable_set = (uint8_t)value;
        if (value) s->disabled = 0;
        break;
    case PIANO_CHG_S_FCC: s->fcc_set = value; break;
    case PIANO_CHG_S_ICL: s->icl_set = value; break;
    case PIANO_CHG_S_VOLT: s->volt_set_prop = property; s->volt_set_value = value; break;
    case PIANO_CHG_S_FV:
        s->fv_set = value;
        if (value == PIANO_CHG_FV_DEFAULT) s->touched &= (uint8_t)~(1u << PIANO_CHG_S_FV);   /* the firmware's own again */
        break;
    }
}

/* One write into the plan, if it is not in flight, not given up, allowed and there is room. */
static void plan_write(const PianoChargeConfig *c, PianoChargeState *s, PianoChargePlan *p, uint64_t now, int slot,
                       uint32_t property, uint32_t value, uint8_t bytes)
{
    if (!c->writes) return;
    if (s->want[slot] != value) {s->want[slot] = value; s->tries[slot] = 0; s->rewrites[slot] = 0;}
    if (s->pending & (1u << slot)) return;
    if (s->tries[slot] >= TRIES) return;
    if (!piano_charge_write_allowed(c, property, value, bytes)) {p->events |= PIANO_CHG_EV_REFUSED; return;}
    if (p->writes >= PIANO_CHG_MAX_WRITES) return;
    PianoChargeWrite *w = &p->write[p->writes++];
    w->property = property;
    w->value = value;
    w->bytes = bytes;
    w->slot = (uint8_t)slot;
    s->pending |= (uint8_t)(1u << slot);
    s->pending_ms[slot] = now;
    if (slot == PIANO_CHG_S_ENABLE && !value) s->disabled = 1;   /* a disable may reach the firmware: ours to lift */
}

/* A value the firmware took, held against its read-back: two disagreeing read-backs in a row forget it (it is written again),
 * at most REWRITES times; then the slot is left alone. */
static int still_taken(PianoChargeState *s, PianoChargePlan *p, int slot, int readback_valid, int agrees)
{
    if (!readback_valid || agrees) {s->mismatch[slot] = 0; return 1;}
    if (++s->mismatch[slot] < MISMATCHES) return 1;
    s->mismatch[slot] = 0;
    if (++s->rewrites[slot] >= REWRITES) {
        if (s->tries[slot] < TRIES) p->events |= PIANO_CHG_EV_READBACK;
        s->tries[slot] = TRIES;
        return 1;
    }
    return 0;
}

static int volt_raised(const PianoChargeState *s)
{
    const int want = s->volt_want_prop && !((s->volt_want_prop == PIANO_CHG_PD_FIXED || s->volt_want_prop == PIANO_CHG_QC_VOLT) &&
                                            s->volt_want_value == 5000);
    const int set = s->volt_set_prop && !((s->volt_set_prop == PIANO_CHG_PD_FIXED || s->volt_set_prop == PIANO_CHG_QC_VOLT) &&
                                           s->volt_set_value == 5000);
    return want || set || s->hv == PIANO_CHG_HV_ASKED || s->hv == PIANO_CHG_HV_ON;
}

static void want_volt(PianoChargeState *s, uint32_t property, uint32_t value)
{
    s->volt_want_prop = property;
    s->volt_want_value = value;
}

/* The 5 V request of the session in force: 0x2011 for a QC source (cfg.qc), vSafe5V (0x21018) otherwise. */
static uint32_t withdraw_prop(const PianoChargeState *s)
{
    return s->hv_qc || s->volt_set_prop == PIANO_CHG_QC_VOLT || s->volt_want_prop == PIANO_CHG_QC_VOLT ? PIANO_CHG_QC_VOLT
                                                                                                       : PIANO_CHG_PD_FIXED;
}

static void want_5v(PianoChargeState *s)
{
    if (volt_raised(s)) want_volt(s, s->hv_qc ? PIANO_CHG_QC_VOLT : PIANO_CHG_PD_FIXED, 5000);
    s->hv = s->hv == PIANO_CHG_HV_BLOCKED ? PIANO_CHG_HV_BLOCKED : PIANO_CHG_HV_OFF;
}

static void plan_volt(const PianoChargeConfig *c, PianoChargeState *s, PianoChargePlan *p, uint64_t now)
{
    if (!s->volt_want_prop) return;
    if (s->volt_set_prop == s->volt_want_prop && s->volt_set_value == s->volt_want_value) return;
    plan_write(c, s, p, now, PIANO_CHG_S_VOLT, s->volt_want_prop, s->volt_want_value, 4);
}

/* The temperatures: the warmest and the coolest of the firmware's pack reading, the two gauges and the battery thermistor. */
typedef struct {int valid; int32_t warm, cool;} Temps;
static int packs_valid(const PianoChargeInput *in)
{
    return (in->valid & PIANO_CHG_IN_GAUGES) && in->t_warm_dc > -400 && in->t_warm_dc < 900 && in->t_cool_dc > -400 &&
           in->t_cool_dc <= in->t_warm_dc;
}
static int ntc_valid(const PianoChargeInput *in) {return (in->valid & PIANO_CHG_IN_NTC) && in->t_ntc_dc > -400 && in->t_ntc_dc < 900;}
static Temps temps(const PianoChargeInput *in)
{
    Temps t = {0, 0, 0};
    if ((in->valid & PIANO_CHG_IN_TBAT) && in->tbat_c > -20 && in->tbat_c < 80) {t.valid = 1; t.warm = t.cool = in->tbat_c;}
    if (packs_valid(in)) {
        const int32_t w = floor_c(in->t_warm_dc), k = floor_c(in->t_cool_dc);
        if (!t.valid || w > t.warm) t.warm = w;
        if (!t.valid || k < t.cool) t.cool = k;
        t.valid = 1;
    }
    if (ntc_valid(in)) {
        const int32_t n = floor_c(in->t_ntc_dc);
        if (!t.valid || n > t.warm) t.warm = n;
        if (!t.valid || n < t.cool) t.cool = n;
        t.valid = 1;
    }
    return t;
}

/* A board thermistor's reading in whole degC, or 0 when its bit is off or it is outside the part's range (-40..125 degC). */
static int board_c(const PianoChargeInput *in, uint32_t bit, int32_t dc, int32_t *c)
{
    if (!(in->valid & bit) || dc <= -400 || dc >= 1250) return 0;
    *c = floor_c(dc);
    return 1;
}

/* The board thermistors' holds. A hold starts on one reading and ends only on readings: a connector that stops reading while hot
 * keeps the hold (unknown, never cool). */
static void board_check(const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p)
{
    int32_t c1 = 0, c2 = 0, ch = 0;
    const int v1 = board_c(in, PIANO_CHG_IN_CONN1, in->t_conn1_dc, &c1), v2 = board_c(in, PIANO_CHG_IN_CONN2, in->t_conn2_dc, &c2);
    if ((v1 && c1 >= CONN_HOT_C) || (v2 && c2 >= CONN_HOT_C)) {
        if (!s->conn_hot) p->events |= PIANO_CHG_EV_CONN_HOT;
        s->conn_hot = 1;
    } else if (s->conn_hot && v1 && v2 && c1 < CONN_COOL_C && c2 < CONN_COOL_C) {
        s->conn_hot = 0;
        p->events |= PIANO_CHG_EV_CONN_COOL;
    }
    const int vc = board_c(in, PIANO_CHG_IN_CHGR, in->t_chgr_dc, &ch);
    if (vc && ch >= CHGR_HOT_C) {
        if (!s->chgr_hot) p->events |= PIANO_CHG_EV_CHGR_HOT;
        s->chgr_hot = 1;
    } else if (s->chgr_hot && vc && ch < CHGR_COOL_C) {
        s->chgr_hot = 0;
        p->events |= PIANO_CHG_EV_CHGR_COOL;
    }
    /* the battery thermistor against the packs (else the firmware's reading): logged once per episode */
    if (ntc_valid(in) && (packs_valid(in) || ((in->valid & PIANO_CHG_IN_TBAT) && in->tbat_c > -20 && in->tbat_c < 80))) {
        const int32_t n = floor_c(in->t_ntc_dc), other = packs_valid(in) ? floor_c(in->t_warm_dc) : in->tbat_c;
        const int32_t d = n > other ? n - other : other - n;
        if (d > NTC_APART_C) {
            if (!s->ntc_apart) p->events |= PIANO_CHG_EV_NTC_APART;
            s->ntc_apart = 1;
        } else {
            s->ntc_apart = 0;
        }
    }
}

/* The Rp limit needs a connector that reads, every one that reads below 50 degC, and no hot hold. */
static int conn_allows_rp(const PianoChargeInput *in, const PianoChargeState *s)
{
    int32_t c1 = 0, c2 = 0;
    const int v1 = board_c(in, PIANO_CHG_IN_CONN1, in->t_conn1_dc, &c1), v2 = board_c(in, PIANO_CHG_IN_CONN2, in->t_conn2_dc, &c2);
    return !s->conn_hot && (v1 || v2) && (!v1 || c1 < CONN_COOL_C) && (!v2 || c2 < CONN_COOL_C);
}

static int strict_contract(const PianoChargeInput *in);

/* T-14 (owner 2026-11-23 18:45/18:47): with a 9 V mode (hv 1/2) the 5 V contract budget too, but only once this source is
 * settled at 5 V: the 9 V request asked and not reached (BLOCKED: 5 V until a detach), no 9 V object (SOURCE), or the health
 * stop (HEALTH). Never while a 9 V request may still be planned (OFF before its wait, a retry), asked or in force: the barrier's
 * reduced currents then come first. Mode 0 as before. */
static int hv_settled_5v(const PianoChargeConfig *c, const PianoChargeState *s, uint8_t fallback)
{
    if (!c->hv) return 1;
    if (s->hv == PIANO_CHG_HV_BLOCKED) return 1;
    return s->hv == PIANO_CHG_HV_OFF && (fallback == PIANO_CHG_FALLBACK_SOURCE || fallback == PIANO_CHG_FALLBACK_HEALTH);
}

/* At fixed 5 V the selected RDO, rather than an MCA offer or Rp, authorizes PD current. Keep the same opt-in ceiling,
 * connector check and measured steps as the non-PD path. Voltage withdrawal must finish before this budget is used. */
static uint32_t contract_5v_budget(const PianoChargeConfig *c, const PianoChargeInput *in, const PianoChargeState *s,
                                   uint8_t fallback)
{
    if (!c->strict_contract || !hv_settled_5v(c, s, fallback) || !ramped(c) || !strict_contract(in) ||
        !(in->valid & PIANO_CHG_IN_SOURCE) || in->contract_protocol != 1 || in->contract_mv != 5000 ||
        !(in->valid & PIANO_CHG_IN_BUS) || in->bus_mv < ONLINE_MV || in->bus_mv >= HV_5V_MAX_MV ||
        s->contract_transition || s->contract_mode_change || volt_raised(s)) return 0;
    return umin(in->contract_ma, rp_cap(c));
}

/* The 5 V input limit of a class: its row (a PD source's 5 V offer), or with cfg.rp (and with_rp) the Type-C current. */
static uint32_t icl_at_5v(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p, int cls,
                          int with_rp)
{
    const unsigned pdos = (in->valid & PIANO_CHG_IN_PDOS) ? in->pdos : 0;
    const uint32_t row = umin(piano_charge_icl_5v(cls, piano_charge_pdo_5v_ma(in->pdo, pdos)), icl_cap(c));
    p->icl_why = PIANO_CHG_ICL_ROW;
    p->rp_ma = 0;
    const uint32_t contract_ma = with_rp && is_pd(cls) ? contract_5v_budget(c, in, s, p->fallback) : 0;
    if (contract_ma > row) {
        const uint32_t target = conn_allows_rp(in, s) ? contract_ma : umin(contract_ma, RAMP_BC12_MA);
        if (target > row) {p->icl_why = PIANO_CHG_ICL_CONTRACT_5V; return target;}
    }
    if (!with_rp || !c->rp || s->rp_distrust || !(in->valid & PIANO_CHG_IN_TYPEC) || !in->typec_source) return row;
    uint32_t rp = piano_charge_icl_rp(cls, row, in->rp_ma, rp_cap(c));
    if (rp <= row) return row;
    if (!conn_allows_rp(in, s)) {
        if (!s->rp_noted) {s->rp_noted = 1; p->events |= PIANO_CHG_EV_RP_WITHHELD;}
        /* stepped: up to 1.5 A still (the current BC 1.2 and a 1.5 A Rp promise, which the DCP row takes without a thermistor) */
        if (!ramped(c) || umin(rp, RAMP_BC12_MA) <= row) return row;
        rp = umin(rp, RAMP_BC12_MA);
    }
    p->icl_why = PIANO_CHG_ICL_RP;
    p->rp_ma = in->rp_ma;
    return rp;
}

/* Which wait reasons are worth a line: each once per step. */
static void ramp_wait(PianoChargeState *s, PianoChargePlan *p, uint8_t wait)
{
    p->ramp_wait = wait;
    if (wait < PIANO_CHG_RAMP_BUS_LOW) return;   /* writing and holding pass by themselves */
    const uint8_t bit = (uint8_t)(1u << (wait - PIANO_CHG_RAMP_BUS_LOW));
    if (!(s->ramp_waits_shown & bit)) {s->ramp_waits_shown |= bit; p->events |= PIANO_CHG_EV_RAMP_WAIT;}
}

/* One step back immediately. A boot-time transient must not pin this source for the rest of the boot. */
static void ramp_back(PianoChargeState *s, PianoChargePlan *p, uint8_t why, int32_t bus_mv, uint64_t now)
{
    const uint32_t back = piano_charge_ramp_back(s->ramp_ma, s->ramp_row_ma);
    s->ramp_back_why = why;
    s->ramp_back_from_ma = s->ramp_ma;
    s->ramp_back_mv = bus_mv;
    if (!s->ramp_ceiling_ma || back < s->ramp_ceiling_ma) s->ramp_ceiling_ma = back;
    s->ramp_retry_ms = now + PIANO_CHG_RAMP_RETRY_MS;
    s->ramp_ma = back;
    s->ramp_step_ms = 0;
    s->ramp_decisions = 0;
    ramp_moved(s);
    p->events |= PIANO_CHG_EV_RAMP_BACK;
    p->ramp_back_why = why;
    p->ramp_back_from_ma = s->ramp_back_from_ma;
    p->ramp_back_mv = bus_mv;
    p->ramp_ceiling_ma = s->ramp_ceiling_ma;
}

/* The stepped Rp limit (cfg.ramp): from the class row to target (the Rp current within rp_cap and the connector rule) one step at
 * a time. A step goes up only when the one in force was taken, held RAMP_HOLD_MS and RAMP_HOLD_DECISIONS, the bus reads at least
 * RAMP_UP_MV under it and the bus current is within max(100 mA, an eighth) of the limit it is held to (the step, or the firmware's
 * own 0x2029 once that has settled below the step: 1750-1800 under 2000 on the Mac's port, chg-ramp): the system and the battery
 * take what it gives, so more is of use, and the source has just shown it holds this much. A lower target applies at once. A sag
 * below RP_SAG_MV under a step above the row, or the firmware refusing a step three times: one step back and a temporary ceiling (a
 * quick detach: in detached()). After the cool-down the same guards authorize each retry. Returns the input limit to plan. */
static uint32_t ramp(const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p, uint64_t now, uint32_t row,
                     uint32_t target)
{
    if (s->ramp_ceiling_ma && now >= s->ramp_retry_ms) {
        s->ramp_ceiling_ma = 0;
        s->ramp_retry_ms = 0;
        s->ramp_waits_shown = 0;
        /* A refused higher value may still own want[] while the lower value was already taken: plan_icl returned without a
         * new write. Give only the input slot a new bounded attempt; keep ACK state and pending writes intact. */
        s->tries[PIANO_CHG_S_ICL] = s->mismatch[PIANO_CHG_S_ICL] = s->rewrites[PIANO_CHG_S_ICL] = 0;
    }
    p->ramp_target_ma = target;
    p->ramp_ceiling_ma = s->ramp_ceiling_ma;
    if (s->ramp_ceiling_ma && target > s->ramp_ceiling_ma) target = umax(s->ramp_ceiling_ma, row);
    s->ramp_row_ma = row;
    if (target <= row) {
        if (s->ramp_ma != row) ramp_moved(s);
        s->ramp_ma = row;
        s->ramp_taken_ms = s->ramp_step_ms = 0;
        p->ramp_ma = row;
        ramp_wait(s, p, s->ramp_ceiling_ma ? PIANO_CHG_RAMP_COOLING : PIANO_CHG_RAMP_TOP);
        return row;
    }
    if (s->ramp_ma < row) {s->ramp_ma = row; ramp_moved(s);}      /* the start (or a higher row) */
    if (s->ramp_ma > target) {s->ramp_ma = target; ramp_moved(s);}   /* a lower target: at once */
    const int taken = s->icl_set == s->ramp_ma;
    if (!taken) s->ramp_taken_ms = 0;
    else if (!s->ramp_taken_ms) {s->ramp_taken_ms = now ? now : 1; s->ramp_decisions = 0;}
    else if (s->ramp_decisions < 255) s->ramp_decisions++;
    /* the firmware's own limit under this step: its highest reading, and when that last rose */
    if (taken && (in->valid & PIANO_CHG_IN_AICL) && in->aicl_ma > (int32_t)s->ramp_aicl_ma) {
        s->ramp_aicl_ma = (uint32_t)in->aicl_ma;
        s->ramp_aicl_ms = now ? now : 1;
    }
    const uint32_t used = taken ? piano_charge_used_limit(s->ramp_ma, s->ramp_aicl_ma, s->ramp_aicl_ma &&
                                                          now >= s->ramp_aicl_ms + RAMP_AICL_SETTLE_MS) : s->ramp_ma;
    p->ramp_used_ma = used;
    if (s->ramp_ma > row) {
        const int sag = taken && (in->valid & PIANO_CHG_IN_BUS) && in->bus_mv < RP_SAG_MV;
        const int refused = !taken && s->want[PIANO_CHG_S_ICL] == s->ramp_ma && s->tries[PIANO_CHG_S_ICL] >= TRIES;
        if (sag || refused) {
            ramp_back(s, p, sag ? PIANO_CHG_BACK_SAG : PIANO_CHG_BACK_REFUSED, in->bus_mv, now);
            p->ramp_ma = s->ramp_ma;
            p->ramp_wait = PIANO_CHG_RAMP_WRITING;
            return s->ramp_ma;
        }
    }
    uint8_t wait;
    if (s->ramp_ma >= target) wait = s->ramp_ceiling_ma ? PIANO_CHG_RAMP_COOLING : PIANO_CHG_RAMP_TOP;
    else if (!taken) wait = PIANO_CHG_RAMP_WRITING;
    else if (now < s->ramp_taken_ms + RAMP_HOLD_MS || s->ramp_decisions < RAMP_HOLD_DECISIONS) wait = PIANO_CHG_RAMP_HOLDING;
    else if (!(in->valid & PIANO_CHG_IN_BUS) || in->bus_mv < RAMP_UP_MV) wait = PIANO_CHG_RAMP_BUS_LOW;
    else if (!(in->valid & PIANO_CHG_IN_IBUS)) wait = PIANO_CHG_RAMP_NO_CURRENT;
    else if (in->ibus_ma + (int32_t)umax(RAMP_USED_MIN_MA, used / 8) < (int32_t)used) wait = PIANO_CHG_RAMP_NOT_USED;
    else {
        p->ramp_from_ma = s->ramp_ma;
        s->ramp_ma = piano_charge_ramp_next(s->ramp_ma, target);
        s->ramp_step_ms = now ? now : 1;
        ramp_moved(s);
        p->events |= PIANO_CHG_EV_RAMP_STEP;
        wait = PIANO_CHG_RAMP_WRITING;
    }
    ramp_wait(s, p, wait);
    p->ramp_ma = s->ramp_ma;
    return s->ramp_ma;
}

/* The board holds on top of any limit. */
static uint32_t board_cap(const PianoChargeState *s, PianoChargePlan *p, uint32_t icl)
{
    if (s->conn_hot && icl > CONN_ICL_MA) {p->icl_why = PIANO_CHG_ICL_CONN; p->rp_ma = 0; return CONN_ICL_MA;}
    if (s->chgr_hot && icl > CHGR_ICL_MA) {p->icl_why = PIANO_CHG_ICL_CHGR; p->rp_ma = 0; return CHGR_ICL_MA;}
    return icl;
}

static int soc_valid(const PianoChargeInput *in)
{
    return (in->valid & PIANO_CHG_IN_GAUGES) && in->soc_max_pct >= 0 && in->soc_max_pct <= 100;
}

static int gauge_fresh(const PianoChargeInput *in, uint64_t now)
{
    return soc_valid(in) && packs_valid(in) && (!in->gauge_ms ||
           (in->gauge_ms <= now && now - in->gauge_ms <= 65000));
}

/* Current UCSI session authorization. Source advertisements alone never authorize draw. */
static int strict_contract(const PianoChargeInput *in);
static int gauge_fresh(const PianoChargeInput *in, uint64_t now);

/* T-14b (owner 2026-11-23 19:25, live y10 reads 19:26/19:32): a UCSI notification also reports the completion of our own periodic
 * status GET, and the contract is withheld from the input until its CCI read resolves it (ChargeContract.h, ChargeLink::fill). A
 * decision in that window used to count as "no contract": ICL 100 / FCC 500 (the notify-observing transition and the PD clamp),
 * and the next fresh snapshot armed the recheck barrier (ICL/FCC 500, fallback READBACK) - every cycle on a steady 9 V contract,
 * with the firmware's input walk restarting from 100 mA each time. Such a decision now holds the limits in force when everything
 * else is steady: the last decision had a valid contract, the notification is young and not repeated past NOTIFY_HOLD_MAX, no
 * transition, mode change or voltage write is under way, the bus is where the session is (8.0-9.6 V at 9 V, below 6 V at 5 V),
 * the gauges are fresh and LPD/DTPT are clear. Nothing goes up in a hold; the board holds and a stop still go down. */
static int notify_hold(const PianoChargeConfig *c, const PianoChargeInput *in, const PianoChargeState *s, uint64_t now,
                       int charge_ok)
{
    /* the burst's first notification bounds the hold (a renewed notify_ms never extends it; strict_hv's 5 s bound counts from it
     * too); a stop of any kind is no steady state: the normal path ends the session */
    const uint64_t since = s->contract_notify_seen && s->contract_notify_since < in->contract_notify_ms ? s->contract_notify_since
                                                                                                       : in->contract_notify_ms;
    if (!charge_ok || !c->strict_contract || !in->contract_notify_pending || (in->valid & PIANO_CHG_IN_CONTRACT) || !s->contract_ok ||
        !s->attached || !s->icl_set || !s->fcc_set || s->notify_holds >= NOTIFY_HOLD_MAX ||
        s->contract_transition || s->contract_mode_change || s->contract_recheck || (s->pending & (1u << PIANO_CHG_S_VOLT)) ||
        now < since || now - since >= NOTIFY_HOLD_MS || !(in->valid & PIANO_CHG_IN_BUS) ||
        !gauge_fresh(in, now) ||
        (in->safety & (PIANO_CHG_SAFE_LPD_DRY | PIANO_CHG_SAFE_DTPT)) != (PIANO_CHG_SAFE_LPD_DRY | PIANO_CHG_SAFE_DTPT) ||
        ((in->valid & PIANO_CHG_IN_TYPE) && in->type != s->type)) return 0;
    if (s->hv == PIANO_CHG_HV_ON) return in->bus_mv >= HV_MIN_MV && in->bus_mv <= HV_MAX_MV;
    return (s->hv == PIANO_CHG_HV_OFF || s->hv == PIANO_CHG_HV_BLOCKED) && !volt_raised(s) &&
           in->bus_mv >= ONLINE_MV && in->bus_mv < HV_5V_MAX_MV;
}

static int strict_contract(const PianoChargeInput *in)
{
    return (in->valid & PIANO_CHG_IN_CONTRACT) && !in->contract_notify_pending && in->contract_generation &&
           in->contract_link_generation == in->generation &&
           (in->contract_protocol == 1 || in->contract_protocol == 2) &&
           in->contract_mv >= 4500 && in->contract_mv <= 10000 &&
           in->contract_ma >= PIANO_CHG_ICL_DEFAULT && in->contract_ma <= 5000;
}

static int strict_barrier(const PianoChargeInput *in, const PianoChargeState *s)
{
    const uint32_t ma = s->transition_ma ? s->transition_ma : 500;
    return s->icl_set && s->icl_set <= ma && s->fcc_set && s->fcc_set <= 500 &&
           (in->valid & PIANO_CHG_IN_ICL) && in->icl_ma >= 0 && (uint32_t)in->icl_ma <= ma &&
           !(s->pending & ((1u << PIANO_CHG_S_ICL) | (1u << PIANO_CHG_S_FCC)));
}

/* Pump state is read-only here. Neither raising nor lowering the source is safe while another stage may be pumping. */
static int strict_voltage_ready(const PianoChargeConfig *c, const PianoChargeInput *in, const PianoChargeState *s)
{
    return !c->strict_contract || ((in->safety & PIANO_CHG_SAFE_CP_OFF) &&
                                  (!s->contract_transition || strict_barrier(in, s)));
}

static void strict_fallback(const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p, uint8_t reason)
{
    p->fallback = reason;
    const int withdrawal_pending = s->volt_want_prop == PIANO_CHG_PD_FIXED && s->volt_want_value == 5000 &&
                                   (s->volt_set_prop != PIANO_CHG_PD_FIXED || s->volt_set_value != 5000 ||
                                    (s->pending & (1u << PIANO_CHG_S_VOLT)));
    const int raised = volt_raised(s) || withdrawal_pending ||
                       ((in->valid & PIANO_CHG_IN_BUS) && in->bus_mv >= HV_5V_MAX_MV) ||
                       (strict_contract(in) && in->contract_mv >= HV_5V_MAX_MV);
    s->contract_transition = (uint8_t)raised;
    s->transition_ma = 500;
    if (strict_contract(in)) s->transition_ma = umin(s->transition_ma, in->contract_ma);
    if (!strict_contract(in)) s->transition_ma = PIANO_CHG_ICL_DEFAULT;
    if (raised) {
        /* a QC source (cfg.qc) goes back through its own request, a PD source through vSafe5V */
        want_volt(s, withdraw_prop(s), 5000);
        s->hv = s->hv == PIANO_CHG_HV_BLOCKED ? PIANO_CHG_HV_BLOCKED : PIANO_CHG_HV_OFF;
    }
}

/* Bounded buck session. A voltage transition is only planned after the reduced currents are ACKed and ICL is read back;
 * the actual new RDO plus VBUS, not that voltage request's ACK, authorizes the larger input current. */
static uint32_t strict_hv(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                          const Temps *t, uint64_t now, int charge_ok)
{
    s->voltage_observe_hold = 0;
    if (!in->contract_notify_pending) {s->contract_notify_seen = 0; s->contract_notify_since = 0;}
    else if (!s->contract_notify_seen) {
        s->contract_notify_seen = 1;
        s->contract_notify_since = in->contract_notify_ms;
    } else if (in->contract_notify_ms < s->contract_notify_since) s->contract_notify_since = in->contract_notify_ms;
    const int contract = strict_contract(in);
    const int at5 = contract && in->contract_protocol == 1 && in->contract_mv < HV_5V_MAX_MV &&
                    (in->valid & PIANO_CHG_IN_BUS) && in->bus_mv < HV_5V_MAX_MV;
    /* With no PD contract, a fresh measured5V plus our explicit5V ACK can finish withdrawal; it never authorizes a new HV. */
    const int returned5 = (in->valid & PIANO_CHG_IN_BUS) && in->bus_mv >= ONLINE_MV && in->bus_mv < HV_5V_MAX_MV &&
                          (!contract || (in->contract_protocol == 1 && in->contract_mv < HV_5V_MAX_MV));
    const int at9 = contract && in->contract_mv >= HV_MIN_MV && in->contract_mv <= HV_MAX_MV &&
                    (in->valid & PIANO_CHG_IN_BUS) && in->bus_mv >= HV_MIN_MV && in->bus_mv <= HV_MAX_MV;
    /* T-14b: only a fresh snapshot carries a generation (ChargeLink omits the contract while a UCSI notification is in flight,
     * e.g. our own periodic status GET's); a decision without one is no change of contract and must not arm the recheck
     * barrier. A real change bumps the generation (ChargeContract.h piano_contract_invalidate, the re-read's `changed`). */
    if (in->contract_generation) {
        if (s->contract_generation && s->contract_generation != in->contract_generation) s->contract_recheck = 1;
        s->contract_generation = in->contract_generation;
    }
    if (contract && s->contract_restore_generation != in->contract_generation) {
        s->contract_restore_generation = in->contract_generation;
        if (s->contract_mode_change || (s->volt_want_prop == PIANO_CHG_PD_FIXED && s->volt_want_value == 5000)) {
            /* A new validated source gets its own bounded withdrawal attempts; failed SDP writes cannot poison the next charger. */
            s->tries[PIANO_CHG_S_VOLT] = s->mismatch[PIANO_CHG_S_VOLT] = s->rewrites[PIANO_CHG_S_VOLT] = 0;
        }
    }
    if (s->contract_mode != c->hv) {
        if (volt_raised(s) || at9 || s->contract_mode_change) s->contract_mode_change = 1;
        s->contract_mode = c->hv;
        s->hv_retry_ms = now + HV_WAIT_MS;
        if (s->hv == PIANO_CHG_HV_BLOCKED) s->hv = PIANO_CHG_HV_OFF;
    }
    if (s->contract_mode_change) {
        strict_fallback(in, s, p, PIANO_CHG_FALLBACK_MODE);
        /* The mode-change obligation survives cable loss, including detach clearing the old request and its ACK. */
        s->contract_transition = 1;
        s->transition_ma = contract ? umin(500, in->contract_ma) : PIANO_CHG_ICL_DEFAULT;
        want_volt(s, PIANO_CHG_PD_FIXED, 5000);
        if (returned5 && s->volt_set_prop == PIANO_CHG_PD_FIXED && s->volt_set_value == 5000 &&
            !(s->pending & (1u << PIANO_CHG_S_VOLT)) && strict_barrier(in, s)) {
            s->contract_mode_change = s->contract_transition = 0;
            s->hv = PIANO_CHG_HV_OFF;
            s->hv_retry_ms = now + HV_WAIT_MS;
        }
        return 0;
    }
    if (!charge_ok || !c->hv || !c->writes) {
        strict_fallback(in, s, p, !charge_ok ? PIANO_CHG_FALLBACK_HEALTH : PIANO_CHG_FALLBACK_MODE);
        return 0;
    }
    /* A withdrawn voltage must reach5V before any re-adoption of HV. Otherwise a recovered safety input could cancel the
     * barrier while an older5V request is still pending, raising current immediately before that request executes. */
    if (s->volt_want_prop == PIANO_CHG_PD_FIXED && s->volt_want_value == 5000 &&
        (!returned5 || s->volt_set_prop != PIANO_CHG_PD_FIXED || s->volt_set_value != 5000 ||
         (s->pending & (1u << PIANO_CHG_S_VOLT)))) {
        strict_fallback(in, s, p, PIANO_CHG_FALLBACK_READBACK);
        return 0;
    }
    if (!contract) {
        const int healthy = gauge_fresh(in, now) && t->valid &&
                            (in->safety & PIANO_CHG_SAFE_ALL) == PIANO_CHG_SAFE_ALL && !in->faults &&
                            t->cool >= 15 && t->warm <= 45 && !s->conn_hot && !s->chgr_hot &&
                            (in->valid & PIANO_CHG_IN_VERIFY) && in->verify == 0 &&
                            (in->valid & PIANO_CHG_IN_FV) && in->fv_raw == piano_charge_fv_raw(PIANO_CHG_FV_DEFAULT);
        const int notify_observing = in->contract_notify_pending && s->hv == PIANO_CHG_HV_ON &&
                                     now >= in->contract_notify_ms && now >= s->contract_notify_since &&
                                     now - s->contract_notify_since < 5000 && healthy &&
                                     (in->valid & PIANO_CHG_IN_BUS) && in->bus_mv >= HV_MIN_MV && in->bus_mv <= HV_MAX_MV;
        if (notify_observing) {
            s->voltage_observe_hold = 1;
            s->contract_transition = 1;
            s->transition_ma = PIANO_CHG_ICL_DEFAULT;
            p->fallback = PIANO_CHG_FALLBACK_NO_CONTRACT;
            return 0;
        }
        /* A requested voltage change necessarily invalidates the old RDO while UCSI rereads it. This is a short observation
         * gap, not authority to draw or request more: only the already issued request may settle behind a 100mA limit. */
        const int observing = in->contract_observing && s->hv == PIANO_CHG_HV_ASKED &&
                              now < s->hv_asked_ms + HV_SETTLE_MS && healthy;
        if (observing) {
            s->contract_transition = 1;
            s->transition_ma = PIANO_CHG_ICL_DEFAULT;
            p->fallback = PIANO_CHG_FALLBACK_NO_CONTRACT;
            return 0;
        }
        const int expired = (s->hv == PIANO_CHG_HV_ASKED && now >= s->hv_asked_ms + HV_SETTLE_MS) ||
                            (s->hv == PIANO_CHG_HV_ON && in->contract_notify_pending &&
                             now >= s->contract_notify_since && now - s->contract_notify_since >= 5000);
        if (s->hv == PIANO_CHG_HV_ASKED) s->hv = PIANO_CHG_HV_BLOCKED;
        strict_fallback(in, s, p, expired ? PIANO_CHG_FALLBACK_TIMEOUT : PIANO_CHG_FALLBACK_NO_CONTRACT);
        return 0;
    }
    if (!gauge_fresh(in, now) || !t->valid) {
        strict_fallback(in, s, p, PIANO_CHG_FALLBACK_GAUGE); return 0;
    }
    if ((in->safety & PIANO_CHG_SAFE_ALL) != PIANO_CHG_SAFE_ALL || in->faults ||
        t->cool < 15 || t->warm > 45 || s->conn_hot || s->chgr_hot ||
        !(in->valid & PIANO_CHG_IN_VERIFY) || in->verify ||
        !(in->valid & PIANO_CHG_IN_FV) || in->fv_raw != piano_charge_fv_raw(PIANO_CHG_FV_DEFAULT)) {
        strict_fallback(in, s, p, PIANO_CHG_FALLBACK_SAFETY); return 0;
    }
    int pps = c->hv == 2;
    const int source = (in->valid & PIANO_CHG_IN_SOURCE) != 0;
    const uint32_t fixed9 = source ? umin(in->fixed9_ma, 5000) : 0, apdo9 = source ? umin(in->pps9_ma, HV_PPS_MAX_MA) / 50 * 50 : 0;
    if (c->hv_order) {
        /* the Ubuntu port's order: the mode's own object, else the other one this source offers at 9 V; an object once asked
         * for stays the one judged (a change of offer ends it below, as the budget goes to 0) */
        if (s->hv == PIANO_CHG_HV_ASKED || s->hv == PIANO_CHG_HV_ON) pps = s->hv_pps;
        else if (pps && apdo9 < HV_PPS_MIN_MA && fixed9 >= HV_FIXED_MIN_MA) pps = 0;
        else if (!pps && fixed9 < HV_FIXED_MIN_MA && apdo9 >= HV_PPS_MIN_MA) pps = 1;
    }
    const uint32_t budget = pps ? apdo9 : fixed9;
    if (budget < (pps ? HV_PPS_MIN_MA : HV_FIXED_MIN_MA)) {
        strict_fallback(in, s, p, PIANO_CHG_FALLBACK_SOURCE); return 0;
    }
    if (at9 && in->contract_protocol == (pps ? 2 : 1)) {
        if (s->contract_recheck) {
            s->contract_transition = 1;
            s->transition_ma = umin(500, in->contract_ma);
            if (!strict_barrier(in, s)) {p->fallback = PIANO_CHG_FALLBACK_READBACK; return 0;}
            s->contract_recheck = 0;
        }
        if (s->hv == PIANO_CHG_HV_ASKED && (s->pending & (1u << PIANO_CHG_S_VOLT))) return 0;
        s->contract_transition = 0;
        s->hv = PIANO_CHG_HV_ON;
        s->hv_pps = (uint8_t)pps;
        s->hv_ma = umin(in->contract_ma, budget);
        /* cfg.hv_caps: at most the buck's highest stock input at 9 V (in_pps 2500) within the RDO and the object; the decision
         * applies the class row of a fixed PD source (in_pd 1600) unless T-14 9 V's connector rule allows more */
        return umin(s->hv_ma, hv_caps(c) ? PIANO_CHG_ICL_ABS_MAX : PIANO_CHG_ICL_CAP);
    }
    if (s->hv == PIANO_CHG_HV_ASKED) {
        if (now < s->hv_asked_ms + HV_SETTLE_MS) return 0;
        s->hv = PIANO_CHG_HV_BLOCKED;
        strict_fallback(in, s, p, PIANO_CHG_FALLBACK_TIMEOUT);
        return 0;
    }
    if (!at5 || s->hv == PIANO_CHG_HV_ON) {
        strict_fallback(in, s, p, PIANO_CHG_FALLBACK_READBACK); return 0;
    }
    if (s->hv == PIANO_CHG_HV_BLOCKED) {
        /* T-14: asked and not reached, 5 V until a detach. The withdrawal is confirmed here (5 V asked, taken and back on the bus,
         * checked above), so the transition is recomputed as every other fallback does: without this the barrier's 500 mA stayed
         * in force for the rest of the attach, below the 5 V contract's own row. */
        strict_fallback(in, s, p, PIANO_CHG_FALLBACK_TIMEOUT);
        return 0;
    }
    if (now < s->attach_ms + HV_WAIT_MS || now < s->hv_retry_ms) return 0;
    s->transition_ma = umin(500, umin(in->contract_ma, budget));
    s->contract_transition = 1;
    if (!strict_barrier(in, s)) {p->fallback = PIANO_CHG_FALLBACK_READBACK; return 0;}
    s->hv = PIANO_CHG_HV_ASKED;
    s->hv_pps = (uint8_t)pps;
    s->hv_qc = 0;
    s->hv_ma = budget;
    s->hv_asked_ms = now;
    want_volt(s, pps ? PIANO_CHG_PPS : PIANO_CHG_PD_FIXED,
              pps ? piano_charge_pps_word(HV_MV, budget) : HV_MV);
    p->events |= PIANO_CHG_EV_HV_ASKED;
    return 0;
}

/* cfg.qc (charge/pd1): the strict session's QC2/QC3 9 V. No PD contract exists for an HVDCP source, so what stands in for it is the
 * firmware's own classification (type 5-8, read every cycle) and a fresh UCSI status of a connected non-PD sink; everything else is
 * the PD session's: the same health, temperature (15-45 degC), board, CP-off, LPD/DTPT, verify and float gates, the reduced-current
 * barrier before the request, 15 s for the bus to reach 8.0-9.6 V, and 0x2011 = 5000 on any loss (a health stop or mode 0 may ask
 * again later; a lost or unreached 9 V waits for the next attach). Ubuntu's request_volt/hv_step for a QC type, read as a
 * specification. */
static int qc_session(const PianoChargeConfig *c, const PianoChargeState *s)
{
    return c->strict_contract && c->qc && (piano_charge_is_qc(s->type) || s->hv_qc ||
                                           s->volt_want_prop == PIANO_CHG_QC_VOLT || s->volt_set_prop == PIANO_CHG_QC_VOLT);
}

static uint32_t strict_qc(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                          const Temps *t, uint64_t now, int charge_ok)
{
    s->voltage_observe_hold = 0;
    const int bus = (in->valid & PIANO_CHG_IN_BUS) != 0;
    const int at9 = bus && in->bus_mv >= HV_MIN_MV && in->bus_mv <= HV_MAX_MV;
    const int at5 = bus && in->bus_mv >= ONLINE_MV && in->bus_mv < HV_5V_MAX_MV;
    const int qc = (in->valid & PIANO_CHG_IN_TYPE) && piano_charge_is_qc(in->type);
    const int nonpd = (in->valid & PIANO_CHG_IN_NONPD) && !(in->valid & PIANO_CHG_IN_CONTRACT);
    const int healthy = gauge_fresh(in, now) && t->valid && (in->safety & PIANO_CHG_SAFE_ALL) == PIANO_CHG_SAFE_ALL &&
                        !in->faults && t->cool >= 15 && t->warm <= 45 && !s->conn_hot && !s->chgr_hot &&
                        (in->valid & PIANO_CHG_IN_VERIFY) && in->verify == 0 &&
                        (in->valid & PIANO_CHG_IN_FV) && in->fv_raw == piano_charge_fv_raw(PIANO_CHG_FV_DEFAULT);
    const int wanted = charge_ok && c->hv && c->writes;
    const uint8_t why = !charge_ok ? PIANO_CHG_FALLBACK_HEALTH : !c->hv || !c->writes ? PIANO_CHG_FALLBACK_MODE :
                        !qc || !nonpd ? PIANO_CHG_FALLBACK_SOURCE : PIANO_CHG_FALLBACK_SAFETY;
    if (s->contract_mode != c->hv) {s->contract_mode = c->hv; if (s->hv == PIANO_CHG_HV_OFF) s->hv_retry_ms = now + HV_WAIT_MS;}
    if (s->hv == PIANO_CHG_HV_ON || s->hv == PIANO_CHG_HV_ASKED) {
        if (wanted && healthy && qc && nonpd) {
            if (at9 && !(s->pending & (1u << PIANO_CHG_S_VOLT)) && s->volt_set_prop == PIANO_CHG_QC_VOLT &&
                s->volt_set_value == HV_MV) {
                if (s->hv == PIANO_CHG_HV_ASKED) p->events |= PIANO_CHG_EV_HV_ON;
                s->hv = PIANO_CHG_HV_ON;
                s->contract_transition = 0;
                return s->hv_ma;
            }
            if (s->hv == PIANO_CHG_HV_ASKED && now < s->hv_asked_ms + HV_SETTLE_MS) {
                s->contract_transition = 1;
                s->transition_ma = 500;
                return 0;
            }
        }
        /* lost, unreached in time, or a gate: back to 5 V (a health stop or mode 0 may ask again; anything else waits for a detach) */
        const int retry = !wanted;
        p->fallback = s->hv == PIANO_CHG_HV_ASKED && now >= s->hv_asked_ms + HV_SETTLE_MS ? PIANO_CHG_FALLBACK_TIMEOUT :
                      wanted && healthy && qc && nonpd ? PIANO_CHG_FALLBACK_READBACK : why;
        s->hv = retry ? PIANO_CHG_HV_OFF : PIANO_CHG_HV_BLOCKED;
        if (retry) s->hv_retry_ms = now + HV_RENEG_MS;
        if (!retry) p->events |= PIANO_CHG_EV_HV_BLOCKED;
        want_volt(s, PIANO_CHG_QC_VOLT, 5000);
        s->contract_transition = 1;
        s->transition_ma = 500;
        return 0;
    }
    /* a withdrawal in progress: the 5 V request taken and the bus back below 6 V before anything else */
    if (s->volt_want_prop == PIANO_CHG_QC_VOLT && s->volt_want_value == 5000 &&
        (!at5 || s->volt_set_prop != PIANO_CHG_QC_VOLT || s->volt_set_value != 5000 || (s->pending & (1u << PIANO_CHG_S_VOLT)))) {
        s->contract_transition = 1;
        s->transition_ma = 500;
        p->fallback = PIANO_CHG_FALLBACK_READBACK;
        return 0;
    }
    s->contract_transition = 0;
    s->hv_qc = 0;   /* at 5 V: the session ends here */
    if (!qc && s->volt_want_prop == PIANO_CHG_QC_VOLT && s->volt_want_value == 5000) {
        /* the type is no QC any more: the taken 5 V request is the last of this session; the PD session decides next */
        s->volt_want_prop = s->volt_want_value = s->volt_set_prop = s->volt_set_value = 0;
        if (s->hv == PIANO_CHG_HV_BLOCKED) s->hv = PIANO_CHG_HV_OFF;
    }
    if (!(wanted && healthy && qc && nonpd)) {p->fallback = why; return 0;}
    if (s->hv == PIANO_CHG_HV_BLOCKED || !at5 || now < s->attach_ms + HV_WAIT_MS || now < s->hv_retry_ms) return 0;
    s->contract_transition = 1;
    s->transition_ma = 500;
    if (!strict_barrier(in, s)) {p->fallback = PIANO_CHG_FALLBACK_READBACK; return 0;}
    s->hv = PIANO_CHG_HV_ASKED;
    s->hv_qc = 1;
    s->hv_pps = 0;
    s->hv_ma = piano_charge_qc_icl_9v(in->type);
    s->hv_asked_ms = now;
    want_volt(s, PIANO_CHG_QC_VOLT, HV_MV);
    p->events |= PIANO_CHG_EV_HV_ASKED;
    return 0;
}

/* The 9 V step (Ubuntu's hv_charge machine; one step per decision). Returns the source's current at 9 V while the bus is there. */
static uint32_t hv_step(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                        uint64_t now, int cls, int charge_ok)
{
    const int32_t bus = in->bus_mv;
    const int at_9v = bus >= HV_MIN_MV && bus <= HV_MAX_MV;
    const int qc = c->qc3 && piano_charge_is_qc3(s->type);
    int pps = 0;
    uint32_t ma = 0;
    if (is_pd(cls) && (in->valid & PIANO_CHG_IN_PDOS)) ma = piano_charge_pdo_9v_ma(in->pdo, in->pdos, c->hv >= 2, &pps);
    else if (is_pd(cls) && (s->hv == PIANO_CHG_HV_ASKED || s->hv == PIANO_CHG_HV_ON)) {ma = s->hv_ma; pps = s->hv_pps;}   /* a failed
                                                                                          * PDO read is no change of offer */
    else if (qc) {ma = 1600; pps = 0;}
    /* the source has to keep offering 9 V the way it was asked for */
    if (s->hv != PIANO_CHG_HV_OFF && s->hv != PIANO_CHG_HV_BLOCKED && (pps != s->hv_pps || qc != s->hv_qc)) ma = 0;
    if (!c->writes) return 0;   /* nothing is sent: the machine stays at 5 V rather than log requests that never go out */
    if (!c->hv || !charge_ok) {
        if (volt_raised(s)) want_5v(s);
        return 0;
    }
    switch (s->hv) {
    case PIANO_CHG_HV_OFF:
        if (!ma) return 0;
        if (at_9v && !s->volt_want_prop) {
            /* already at 9 V without a request of ours (a wake without a handover): take it as asked */
            s->hv = PIANO_CHG_HV_ON;
            s->hv_pps = (uint8_t)pps;
            s->hv_qc = (uint8_t)qc;
            s->hv_ma = ma;
            p->events |= PIANO_CHG_EV_HV_ON;
            return ma;
        }
        if (now < s->hv_retry_ms || now < s->attach_ms + HV_WAIT_MS || bus >= HV_5V_MAX_MV) return 0;
        if (!(in->valid & PIANO_CHG_IN_VERIFY) || in->verify) return 0;   /* never in the middle of an authentication (#20) */
        s->hv = PIANO_CHG_HV_ASKED;
        s->hv_pps = (uint8_t)pps;
        s->hv_qc = (uint8_t)qc;
        s->hv_ma = ma;
        s->hv_checks = 0;
        s->hv_asked_ms = now;
        want_volt(s, qc ? PIANO_CHG_QC_VOLT : pps ? PIANO_CHG_PPS : PIANO_CHG_PD_FIXED,
                  qc || !pps ? HV_MV : piano_charge_pps_word(HV_MV, ma));
        p->events |= PIANO_CHG_EV_HV_ASKED;
        return 0;
    case PIANO_CHG_HV_ASKED:
        if (ma && at_9v) {
            s->hv = PIANO_CHG_HV_ON;
            p->events |= PIANO_CHG_EV_HV_ON;
            return ma;
        }
        if (s->hv_checks < 255) s->hv_checks++;
        if (ma && (now < s->hv_asked_ms + HV_SETTLE_MS || s->hv_checks < HV_SETTLE_DECISIONS)) return 0;
        break;
    case PIANO_CHG_HV_ON:
        if (ma && at_9v) return ma;
        if (ma && bus < HV_5V_MAX_MV && s->hv_renegs < HV_RENEG_MAX) {
            /* back at 5 V by itself (a Xiaomi charger does after its authentication): asked again after a pause */
            s->hv_renegs++;
            s->hv = PIANO_CHG_HV_OFF;
            s->hv_qc = 0;
            s->volt_want_prop = s->volt_want_value = 0;
            s->volt_set_prop = s->volt_set_value = 0;
            s->hv_retry_ms = now + HV_RENEG_MS;
            p->events |= PIANO_CHG_EV_HV_RETRY;
            return 0;
        }
        break;
    default:
        return 0;
    }
    /* not reached, dropped or refused: 5 V until the source is detached */
    want_volt(s, s->hv_qc ? PIANO_CHG_QC_VOLT : PIANO_CHG_PD_FIXED, 5000);
    s->hv = PIANO_CHG_HV_BLOCKED;
    p->events |= PIANO_CHG_EV_HV_BLOCKED;
    return 0;
}

/* The charge current with its thermal steps: down at once, up one step (300 mA) per 30 s with a degree to spare. */
static uint32_t fcc_target(PianoChargeState *s, uint32_t full, const Temps *t, uint64_t now)
{
    uint32_t fcc = piano_charge_fcc_thermal(full, t->valid, t->warm);
    if (s->fcc_set && full == s->fcc_full && fcc > s->fcc_set) {
        if (now < s->fcc_raise_ms) return s->fcc_set;
        fcc = umin(piano_charge_fcc_thermal(full, t->valid, t->warm + 1), s->fcc_set + FCC_DERATE_STEP);
        if (fcc < s->fcc_set) fcc = s->fcc_set;
    }
    return fcc;
}

static void plan_fcc(const PianoChargeConfig *c, PianoChargeState *s, PianoChargePlan *p, uint64_t now, uint32_t fcc, uint32_t full)
{
    if (!fcc || s->fcc_set == fcc) return;
    const uint8_t before = p->writes;
    plan_write(c, s, p, now, PIANO_CHG_S_FCC, PIANO_CHG_FCC, fcc, 4);
    if (p->writes != before) {s->fcc_full = full; s->fcc_raise_ms = now + FCC_RAISE_MS;}
}

static void plan_icl(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                     uint64_t now, uint32_t icl)
{
    if (!icl) return;
    if (s->icl_set == icl) {
        const int32_t rb = in->icl_ma;
        const int agrees = rb <= (int32_t)icl + ICL_SLACK_MA && rb + ICL_SLACK_MA >= (int32_t)icl;
        if (still_taken(s, p, PIANO_CHG_S_ICL, (in->valid & PIANO_CHG_IN_ICL) != 0, agrees)) return;
        s->icl_set = 0;
    }
    plan_write(c, s, p, now, PIANO_CHG_S_ICL, PIANO_CHG_ICL, icl, 4);
}

static void plan_enable(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                        uint64_t now, uint32_t value)
{
    if (s->enable_set == value) {
        const int valid = (in->valid & PIANO_CHG_IN_ENABLE) != 0;
        if (still_taken(s, p, PIANO_CHG_S_ENABLE, valid, valid && in->enable == (int32_t)value)) return;
        s->enable_set = UNKNOWN_ENABLE;
    }
    plan_write(c, s, p, now, PIANO_CHG_S_ENABLE, PIANO_CHG_ENABLE, value, 1);
}

static void plan_fv(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                    uint64_t now, const Temps *t)
{
    uint32_t target = 0;
    if (c->full_charge && !c->strict_contract) target = piano_charge_fv_target(&s->term_hot, t->valid, t->warm);
    else if (c->strict_contract && c->fv_hot && s->term_hot) target = PIANO_CHG_FV_HOT;   /* protection() keeps term_hot */
    else if (s->touched & (1u << PIANO_CHG_S_FV)) target = PIANO_CHG_FV_DEFAULT;   /* the option went off: the firmware's own */
    p->fv_mv = target;
    if (!target) return;
    const int valid = (in->valid & PIANO_CHG_IN_FV) != 0;
    const int agrees = valid && in->fv_raw == piano_charge_fv_raw(target);
    if (agrees && s->fv_set != target) {
        /* already in force (the firmware kept it across a wake): nothing to send */
        s->fv_set = target;
        if (target != PIANO_CHG_FV_DEFAULT) s->touched |= 1u << PIANO_CHG_S_FV;
        else s->touched &= (uint8_t)~(1u << PIANO_CHG_S_FV);
    }
    if (s->fv_set == target) {
        if (still_taken(s, p, PIANO_CHG_S_FV, valid, agrees)) return;
        s->fv_set = 0;
    }
    plan_write(c, s, p, now, PIANO_CHG_S_FV, PIANO_CHG_FV, target, 4);
}

static void expire_pending(PianoChargeState *s, uint64_t now)
{
    for (unsigned i = 0; i < PIANO_CHG_SLOTS; ++i)
        if ((s->pending & (1u << i)) && now >= s->pending_ms[i] + PENDING_MS) {
            s->pending &= (uint8_t)~(1u << i);
            if (s->sleeping) s->handover_fault = 1;
        }
}

static int limit_on(const PianoChargeConfig *c) {return c->limit && c->stop_pct < 100;}

/* An already enabled 5 V session can leave termination to the firmware
 * while the gauges recover. This never authorizes an enable or a current rise. */
static int stale_soc_stop(const PianoChargeConfig *c, const PianoChargeInput *in, const PianoChargeState *s)
{
    if (s->soc_failures < SOC_FAILURES_STOP) return 0;
    const uint32_t needed = PIANO_CHG_IN_BUS | PIANO_CHG_IN_ENABLE;
    const int at5 = (in->valid & needed) == needed && in->bus_mv >= ONLINE_MV && in->bus_mv < HV_5V_MAX_MV &&
                    (!(in->valid & PIANO_CHG_IN_CONTRACT) ||
                     (in->contract_protocol == 1 && in->contract_mv < HV_5V_MAX_MV));
    return !(c->strict_contract && !c->hv && at5 && s->attached && in->enable == 1 && !s->disabled &&
             !volt_raised(s) && !s->contract_transition && !s->contract_mode_change);
}

/* Battery health: the charge hysteresis, the mode's hot/cold stops, and what happens without a charge reading. Returns charge_ok. */
static int protection(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                      const Temps *t, uint64_t now)
{
    if (!limit_on(c)) s->hold = 0;   /* disabling health clears only its SOC vote */
    if (gauge_fresh(in, now)) {
        if (limit_on(c)) {
            const uint8_t hold = (uint8_t)piano_charge_soc_hold(s->hold, in->soc_max_pct, c->stop_pct, c->resume_pct);
            if (hold != s->hold) p->events |= hold ? PIANO_CHG_EV_LIMIT_STOP : PIANO_CHG_EV_LIMIT_RESUME;
            s->hold = hold;
        }
        s->soc_lost = s->soc_failures = 0;
    } else if (limit_on(c) || c->strict_contract) {
        if (!s->soc_lost) {s->soc_lost = 1; s->soc_lost_ms = now;}
        if (s->soc_failures < 255) s->soc_failures++;
        if (s->soc_failures == SOC_FAILURES_STOP) p->events |= PIANO_CHG_EV_SOC_LOST;
        /* An already established SOC hold is retained indefinitely; an unknown SOC never grants a resume. */
    }

    if (t->valid && (limit_on(c) || c->strict_contract || s->hot || s->cold)) {
        if (t->warm >= LIMIT_HOT_C) {if (!s->hot) p->events |= PIANO_CHG_EV_HOT; s->hot = 1;}
        else if (t->warm < LIMIT_HOT_RELEASE_C) s->hot = 0;
        if (t->cool < LIMIT_COLD_C) {if (!s->cold) p->events |= PIANO_CHG_EV_COLD; s->cold = 1;}
        else if (t->cool >= LIMIT_COLD_RELEASE_C) s->cold = 0;
    }
    /* cfg.fv_hot (charge/pd1): the warm row's float, 4060 mV from 48 degC (the warmest reading) until below 46; a missing
     * temperature keeps the last state (the strict session holds the currents at their floors without one). The higher pack at
     * that float (within 5 mV) holds charging until 150 mV below it or the warm row ends (the Ubuntu port's voltage hold). */
    if (c->strict_contract && c->fv_hot) {
        if (t->valid && t->warm >= TERM_HOT_C) s->term_hot = 1;
        else if (t->valid && t->warm < TERM_COOL_C) s->term_hot = 0;
        if (!s->term_hot) s->fv_hold = 0;
        else if (gauge_fresh(in, now)) {
            if (in->vbat_max_mv >= (int32_t)PIANO_CHG_FV_HOT - 5) s->fv_hold = 1;
            else if (in->vbat_max_mv < (int32_t)PIANO_CHG_FV_HOT - 150) s->fv_hold = 0;
        }
    } else {
        s->fv_hold = 0;
    }
    p->why_off = s->hold ? PIANO_CHG_OFF_LIMIT : s->hot ? PIANO_CHG_OFF_HOT : s->cold ? PIANO_CHG_OFF_COLD :
                 s->fv_hold ? PIANO_CHG_OFF_WARM_FV :
                 c->admin_stop ? PIANO_CHG_OFF_ADMIN : in->faults ? PIANO_CHG_OFF_FAULT :
                 stale_soc_stop(c, in, s) ? PIANO_CHG_OFF_STALE :
                 c->strict_contract && s->fw_block ? PIANO_CHG_OFF_FIRMWARE : PIANO_CHG_OFF_NONE;
    return p->why_off == PIANO_CHG_OFF_NONE;
}

/* The firmware says no source while the PMIC's Type-C block sees one (Ubuntu #18: after a resume the firmware reported online 0,
 * VBUS 0, ignored replugs; a rebind, which sends everything again, recovered it). */
static void wedge_check(const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p, uint64_t now)
{
    if (!(in->valid & PIANO_CHG_IN_TYPEC) || !in->typec_source) {
        s->wedge_since_ms = 0;
        s->wedge_checks = 0;
        if (in->valid & PIANO_CHG_IN_TYPEC) s->wedge_resyncs = s->wedge_reported = 0;
        return;
    }
    if (!s->wedge_since_ms) {s->wedge_since_ms = now ? now : 1; s->wedge_checks = 0;}
    if (s->wedge_checks < 255) s->wedge_checks++;
    if (now < s->wedge_since_ms + WEDGE_MS || s->wedge_checks < WEDGE_DECISIONS) return;
    if (s->wedge_resyncs < WEDGE_RESYNCS) {
        s->wedge_resyncs++;
        p->resync = 1;
        p->events |= PIANO_CHG_EV_WEDGE_RESYNC;
        s->wedge_since_ms = now ? now : 1;
        s->wedge_checks = 0;
    } else if (!s->wedge_reported) {
        s->wedge_reported = 1;
        p->events |= PIANO_CHG_EV_WEDGED;
    }
}

static void detached(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p, uint64_t now)
{
    if (s->attached) {
        p->events |= PIANO_CHG_EV_DETACH;
        if (s->hv == PIANO_CHG_HV_ASKED && s->hv_retry_ms < now + HV_RETRY_MS) s->hv_retry_ms = now + HV_RETRY_MS;
        reset_tries(s);
        /* gone soon after an Rp limit took (a port's over-current cut looks like this): twice ends the trust */
        if (s->rp_taken_ms && now < s->rp_taken_ms + RP_QUICK_MS && ++s->rp_strikes >= RP_STRIKES && !s->rp_distrust) {
            s->rp_distrust = 1;
            p->events |= PIANO_CHG_EV_RP_DISTRUST;
        }
        /* stepped: gone within 30 s of a step up above the row (planned or taken): back off before retrying */
        if (s->ramp_ma > s->ramp_row_ma && s->ramp_step_ms && now < s->ramp_step_ms + RP_QUICK_MS)
            ramp_back(s, p, PIANO_CHG_BACK_DETACH, (in->valid & PIANO_CHG_IN_BUS) ? in->bus_mv : 0, now);
    }
    s->rp_taken_ms = 0;
    ramp_restart(s);
    kick_reset(s, 1);   /* a stop of a restart in force is lifted below with any stop of ours */
    s->attached = 0;
    s->type = -1;
    s->hv = PIANO_CHG_HV_OFF;
    s->hv_qc = 0;
    s->hv_renegs = 0;
    s->volt_set_prop = s->volt_set_value = s->volt_want_prop = s->volt_want_value = 0;   /* the next source starts at 5 V */
    s->fcc_full = 0;
    s->stop_noted = s->fw_noted = 0;
    /* The firmware keeps the last values for the next source (a weak port would get them): its own back, only where we wrote. A
     * stop of ours is lifted too: the next attach decides again within a cycle, and a stop must never outlive us. */
    const int rb_raised = (in->valid & PIANO_CHG_IN_ICL) && in->icl_ma > (int32_t)PIANO_CHG_ICL_DEFAULT + ICL_SLACK_MA;
    if (((s->touched & (1u << PIANO_CHG_S_ICL)) || rb_raised) && s->icl_set != PIANO_CHG_ICL_DEFAULT)
        plan_write(c, s, p, now, PIANO_CHG_S_ICL, PIANO_CHG_ICL, PIANO_CHG_ICL_DEFAULT, 4);
    if ((s->touched & (1u << PIANO_CHG_S_FCC)) && s->fcc_set != PIANO_CHG_FCC_DEFAULT)
        plan_write(c, s, p, now, PIANO_CHG_S_FCC, PIANO_CHG_FCC, PIANO_CHG_FCC_DEFAULT, 4);
    if (s->disabled) plan_write(c, s, p, now, PIANO_CHG_S_ENABLE, PIANO_CHG_ENABLE, 1, 1);
    p->icl_ma = PIANO_CHG_ICL_DEFAULT;
    p->fcc_ma = PIANO_CHG_FCC_DEFAULT;
    p->charge = 1;
    wedge_check(in, s, p, now);
}

/* The sleep handover (Ubuntu v1.1.0): the attached type's 5 V limits (not 100 mA), the source back at 5 V, and with battery health
 * on and the limit not reached, charging stopped for the sleep (the limit cannot be watched while the system is suspended). */
static void handover(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                     const Temps *t, uint64_t now)
{
    if (!s->sleeping) {s->sleeping = 1; p->events |= PIANO_CHG_EV_SLEEP;}
    s->hold_awake = 0;
    const int cls = c->strict_contract && strict_contract(in) ?
                    (in->contract_protocol == 2 ? PIANO_CHG_C_PD_PPS : PIANO_CHG_C_PD) :
                    s->attached && s->type >= 0 ? piano_charge_class(s->type) : PIANO_CHG_C_NONE;
    p->cls = (uint8_t)cls;
    if (c->writes && cls != PIANO_CHG_C_NONE && (cls != PIANO_CHG_C_REFUSED || c->strict_contract)) {
        /* the row, not the Rp current: nothing watches the bus while the system is suspended */
        uint32_t icl = board_cap(s, p, icl_at_5v(c, in, s, p, cls, 0));
        s->rp_taken_ms = 0;
        uint32_t fcc = piano_charge_fcc_thermal(umin(piano_charge_fcc_full(cls, 0), fcc_cap(c)), t->valid, t->warm);
        if (c->strict_contract) {
            if (!icl) icl = PIANO_CHG_ICL_DEFAULT;
            if (!fcc) fcc = PIANO_CHG_FCC_DEFAULT;
        }
        if (c->strict_contract && (volt_raised(s) || in->bus_mv >= HV_5V_MAX_MV)) {
            s->contract_transition = 1;
            s->transition_ma = strict_contract(in) ? umin(500, in->contract_ma) : PIANO_CHG_ICL_DEFAULT;
            icl = umin(icl, s->transition_ma); fcc = umin(fcc, 500);
        }
        const int stale_stop = stale_soc_stop(c, in, s);
        const int stop = s->hot || s->cold || c->admin_stop || in->faults || stale_stop ||
                         (c->strict_contract && s->fw_block) || (limit_on(c) && (s->hold || !c->sleep_charge));
        if (stop) {
            p->why_off = s->hold ? PIANO_CHG_OFF_LIMIT : s->hot ? PIANO_CHG_OFF_HOT : s->cold ? PIANO_CHG_OFF_COLD :
                         c->admin_stop ? PIANO_CHG_OFF_ADMIN : in->faults ? PIANO_CHG_OFF_FAULT :
                         stale_stop ? PIANO_CHG_OFF_STALE :
                         c->strict_contract && s->fw_block ? PIANO_CHG_OFF_FIRMWARE : PIANO_CHG_OFF_SLEEP;
            if (s->enable_set != 0 && !(c->strict_contract && s->fw_block && !s->disabled)) plan_write(c, s, p, now, PIANO_CHG_S_ENABLE, PIANO_CHG_ENABLE, 0, 1);
        } else if (s->disabled && (!c->strict_contract || gauge_fresh(in, now))) {
            /* a charging restart's stop (or one of battery health's being lifted): charging stays on for the sleep */
            plan_write(c, s, p, now, PIANO_CHG_S_ENABLE, PIANO_CHG_ENABLE, 1, 1);
        }
        kick_reset(s, 0);
        if ((s->touched & (1u << PIANO_CHG_S_FCC)) && s->fcc_set != fcc) plan_write(c, s, p, now, PIANO_CHG_S_FCC, PIANO_CHG_FCC, fcc, 4);
        if ((s->touched & (1u << PIANO_CHG_S_ICL)) && s->icl_set != icl) plan_write(c, s, p, now, PIANO_CHG_S_ICL, PIANO_CHG_ICL, icl, 4);
        if (c->strict_contract && in->bus_mv >= HV_5V_MAX_MV) want_volt(s, withdraw_prop(s), 5000);
        else want_5v(s);
        if (strict_voltage_ready(c, in, s)) plan_volt(c, s, p, now);
        p->icl_ma = icl;
        p->fcc_ma = fcc;
        p->charge = (uint8_t)!stop;
        p->bus_mv = 5000;
    }
    /* Hardware refusal or a lost completion remains a fault; elapsed time never proves a handover. */
    p->sleep_ready = p->writes == 0 && s->pending == 0 && !s->handover_fault;
    if (c->strict_contract && p->sleep_ready && s->attached) {
        const int stopped = !p->charge;
        p->sleep_ready = (in->valid & PIANO_CHG_IN_ENABLE) && in->enable == (stopped ? 0 : 1) &&
                         (in->valid & PIANO_CHG_IN_BUS) && in->bus_mv < HV_5V_MAX_MV &&
                         (!is_pd(cls) || (strict_contract(in) && in->contract_mv < HV_5V_MAX_MV));
    }
}

/* The float voltage in force: 0x2007 as read (2 mV - 3600), else the firmware's own 4400 mV. */
static int32_t fv_in_force(const PianoChargeInput *in)
{
    return (in->valid & PIANO_CHG_IN_FV) && in->fv_raw > 0 ? (in->fv_raw + 3600) / 2 : (int32_t)PIANO_CHG_FV_DEFAULT;
}

/* The charging restart (cfg.kick, docs §17), after the enable's own rules in a decision. It starts only when those planned nothing
 * for 0x2003 and no stop of ours is in force, so the stop is the restart's alone; the resume is the enable's own rule (a stop of
 * ours is lifted at the next decision once the 0 is through), so a restart cut short anywhere (a refusal, no answer, a detach, a
 * sleep, a shutdown, a new generation) still ends with 0x2003 = 1. Conditions to start: the policy's input limit and charge current
 * taken and nothing of them in flight, charging allowed (battery health, its hot and cold stops), the firmware's own enable reading
 * 1 (a 0 of its own is never overridden), both packs read at most 95 % and 30 mV below the float voltage in force, 15-45 degC, the
 * packs within 30 mA of 0 on two gauge readings 20 s apart or more, and at the decision the bus at 4.65 V or more and its current
 * 150 mA or more below the limit it is held to (limit_ma). */
static void kick(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p, uint64_t now,
                 const Temps *t, int charge_ok, uint32_t limit_ma)
{
    const uint64_t g = in->gauge_ms ? in->gauge_ms : now;
    const uint8_t enable_bit = (uint8_t)(1u << PIANO_CHG_S_ENABLE);
    if (!c->kick || !c->writes) {kick_reset(s, 0); goto out;}
    /* the 0 held its time: the enable's rule plans the 1 (this decision already, or the next), whatever the 0's answer was */
    if (s->kick_stage == PIANO_CHG_KICK_STOPPING && !kick_holding(s, now)) s->kick_stage = PIANO_CHG_KICK_RESUMING;
    if (s->kick_stage == PIANO_CHG_KICK_RESUMING) {
        if (s->enable_set == 1 && !s->disabled) {
            s->kick_stage = PIANO_CHG_KICK_WATCHING;
            s->kick_resumed_ms = now ? now : 1;
            s->fcc_set = 0;   /* the charge current again (no read-back: the firmware may have dropped it with the enable) */
            p->kick_events |= PIANO_CHG_KICK_EV_RESUMED;
        } else if (now >= s->kick_ms + 2 * KICK_STAGE_MS) {
            kick_reset(s, 0);
            p->kick_events |= PIANO_CHG_KICK_EV_ABANDONED;
        }
        goto out;
    }
    if (s->kick_stage == PIANO_CHG_KICK_WATCHING) {
        if (now >= s->kick_resumed_ms + 2 * KICK_STAGE_MS) {kick_reset(s, 0); goto out;}   /* no gauge reading to judge it by */
        if (!soc_valid(in) || g < s->kick_resumed_ms + KICK_SETTLE_MS) goto out;
        s->kick_ibat_ma = in->ibat_ma;
        p->kick_ibat_ma = in->ibat_ma;
        if (in->ibat_ma >= KICK_CHARGING_MA) {
            s->kick_result = 1;
            p->kick_events |= PIANO_CHG_KICK_EV_CHARGING;
        } else {
            s->kick_result = 2;
            p->kick_events |= PIANO_CHG_KICK_EV_NONE;
            if (s->kick_count >= KICK_MAX) p->kick_events |= PIANO_CHG_KICK_EV_GAVE_UP;
        }
        kick_reset(s, 0);
        goto out;
    }
    if (s->kick_stage != PIANO_CHG_KICK_IDLE) goto out;
    {
        const int32_t spare = (int32_t)limit_ma - in->ibus_ma;
        const int ready = charge_ok && !s->disabled && !(s->pending & (enable_bit | (1u << PIANO_CHG_S_FCC) | (1u << PIANO_CHG_S_ICL))) &&
                          s->icl_set && s->fcc_set && s->kick_count < KICK_MAX && (!s->kick_count || now >= s->kick_ms + KICK_GAP_MS) &&
                          (in->valid & PIANO_CHG_IN_ENABLE) && in->enable == 1 && s->hv != PIANO_CHG_HV_ASKED &&
                          soc_valid(in) && in->soc_max_pct <= KICK_SOC_MAX && in->vbat_max_mv > 0 &&
                          in->vbat_max_mv + KICK_FV_MARGIN_MV <= fv_in_force(in) &&
                          t->valid && t->cool >= KICK_T_MIN_C && t->warm <= KICK_T_MAX_C;
        const int idle = in->ibat_ma >= -KICK_IDLE_MA && in->ibat_ma <= KICK_IDLE_MA;
        if (!ready || !idle) {s->kick_idle_ms = 0; goto out;}
        if (!s->kick_idle_ms) {s->kick_idle_ms = g ? g : 1; goto out;}
        const int spare_ok = (in->valid & PIANO_CHG_IN_BUS) && in->bus_mv >= RAMP_UP_MV && (in->valid & PIANO_CHG_IN_IBUS) &&
                             limit_ma && spare >= KICK_SPARE_MA;
        if (g < s->kick_idle_ms + KICK_IDLE_MS || !spare_ok) goto out;
        plan_write(c, s, p, now, PIANO_CHG_S_ENABLE, PIANO_CHG_ENABLE, 0, 1);
        if (!(s->pending & enable_bit)) goto out;   /* not planned (the plan is full): the next decision */
        s->kick_stage = PIANO_CHG_KICK_STOPPING;
        s->kick_count++;
        s->kick_ms = now ? now : 1;
        s->kick_idle_ms = 0;
        s->kick_ibat_ma = in->ibat_ma;
        p->kick_events |= PIANO_CHG_KICK_EV_START;
        p->kick_ibat_ma = in->ibat_ma;
        p->kick_spare_ma = spare;
    }
out:
    p->kick_stage = s->kick_stage;
    p->kick_count = s->kick_count;
}

void piano_charge_decide(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p)
{
    zero(p, sizeof *p);
    const uint64_t now = in->now_ms;
    s->decisions++;
    p->events = s->events;
    s->events = 0;
    if (!s->started) {
        s->started = 1;
        s->generation = in->generation;
        s->wakes = in->wakes;
        forget(s, now);
    }
    if (in->generation != s->generation) {
        s->generation = in->generation;
        forget(s, now);
        p->events |= PIANO_CHG_EV_GENERATION;
    }
    if (in->wakes != s->wakes) {
        s->wakes = in->wakes;
        forget(s, now);
        s->sleeping = 0;
        s->wedge_resyncs = s->wedge_reported = 0;
        p->resync = 1;
        p->events |= PIANO_CHG_EV_WAKE;
    }
    expire_pending(s, now);
    const Temps t = temps(in);
    board_check(in, s, p);
    if (c->strict_contract && (in->valid & PIANO_CHG_IN_ENABLE)) {
        if (in->enable == 0 && !s->disabled && s->enable_set != 0) s->fw_block = 1;
        else if (in->enable == 1) s->fw_block = 0;
    }
    const int charge_ok = protection(c, in, s, p, &t, now);
    if (charge_ok != s->charge_ok) {s->charge_ok = (uint8_t)charge_ok; s->off_since_ms = now; s->stop_noted = 0;}
    if (in->sleep == PIANO_CHG_WILL_SLEEP) {handover(c, in, s, p, &t, now); return;}
    s->sleeping = 0;
    p->hold_awake = s->hold_awake;
    p->charge = (uint8_t)charge_ok;
    if (!charge_ok) kick_reset(s, 0);
    if (!(in->valid & PIANO_CHG_IN_BUS)) {
        if (!charge_ok && p->why_off != PIANO_CHG_OFF_FIRMWARE) plan_enable(c, in, s, p, now, 0);
        if (c->strict_contract && volt_raised(s)) {
            s->contract_transition = 1; s->transition_ma = PIANO_CHG_ICL_DEFAULT;
            want_5v(s); plan_icl(c, in, s, p, now, PIANO_CHG_ICL_DEFAULT);
            plan_fcc(c, s, p, now, PIANO_CHG_FCC_DEFAULT, PIANO_CHG_FCC_DEFAULT);
            if (strict_voltage_ready(c, in, s)) plan_volt(c, s, p, now);
            p->fallback = PIANO_CHG_FALLBACK_READBACK; p->transition = 1;
        }
        return;
    }   /* no bus reading is neither a detach nor an attach: nothing changes */
    if (in->bus_mv < ONLINE_MV && c->strict_contract &&
        (((in->valid & PIANO_CHG_IN_TYPEC) && in->typec_source) || strict_contract(in))) {
        /* A live connector with a missing bus reading is not a detach and cannot lift a safety stop. */
        if (!charge_ok && p->why_off != PIANO_CHG_OFF_FIRMWARE) plan_enable(c, in, s, p, now, 0);
        strict_fallback(in, s, p, PIANO_CHG_FALLBACK_READBACK);
        s->transition_ma = PIANO_CHG_ICL_DEFAULT;
        plan_icl(c, in, s, p, now, PIANO_CHG_ICL_DEFAULT);
        plan_fcc(c, s, p, now, 500, 500);
        if (strict_voltage_ready(c, in, s)) plan_volt(c, s, p, now);
        return;
    }
    if (in->bus_mv < ONLINE_MV) {
        detached(c, in, s, p, now);
        s->hold_awake = p->hold_awake = 0;
        return;
    }
    s->wedge_since_ms = 0;
    s->wedge_checks = s->wedge_resyncs = s->wedge_reported = 0;
    if (!s->attached) {
        s->attached = 1;
        s->attach_ms = now;
        s->type = -1;
        s->hv = PIANO_CHG_HV_OFF;
        s->hv_qc = 0;
        s->hv_renegs = s->hv_checks = 0;
        s->fcc_full = 0;
        s->stop_noted = s->fw_noted = 0;
        s->rp_noted = 0;
        s->rp_taken_ms = 0;
        reset_tries(s);
        kick_reset(s, 1);
        p->events |= PIANO_CHG_EV_ATTACH;
    }
    /* the type: a failed read changes nothing; a new type starts its limits afresh */
    if (in->valid & PIANO_CHG_IN_TYPE) {
        if (in->type != s->type) {
            const int was = s->type >= 0 ? piano_charge_class(s->type) : PIANO_CHG_C_NONE;
            const int now_cls = piano_charge_class(in->type);
            if (s->type >= 0) p->events |= PIANO_CHG_EV_TYPE;
            /* a PD classification back after an Unknown sample must not stay blocked: the same retry budget */
            if (is_pd(now_cls) && s->type >= 0 && !is_pd(was) && s->hv == PIANO_CHG_HV_BLOCKED && s->hv_renegs < HV_RENEG_MAX) {
                s->hv_renegs++;
                s->hv = PIANO_CHG_HV_OFF;
                if (s->hv_retry_ms < now + HV_RENEG_MS) s->hv_retry_ms = now + HV_RENEG_MS;
            }
            s->type = in->type;
            reset_tries(s);
            s->fcc_full = 0;
        }
    }
    if (notify_hold(c, in, s, now, charge_ok)) {
        /* T-14b: the limits in force stay; only a board hold may act in this decision. The burst's start is latched as strict_hv
         * does, so its 5 s bound on a notification storm still counts from the first one. */
        s->notify_holds++;
        if (!s->contract_notify_seen) {s->contract_notify_seen = 1; s->contract_notify_since = in->contract_notify_ms;}
        else if (in->contract_notify_ms < s->contract_notify_since) s->contract_notify_since = in->contract_notify_ms;
        p->hv = s->hv;
        p->cls = (uint8_t)(s->hv == PIANO_CHG_HV_ON ? (s->hv_pps ? PIANO_CHG_C_PD_PPS : PIANO_CHG_C_PD) : piano_charge_class(s->type));
        p->charge = (uint8_t)charge_ok;
        const uint32_t held_icl = board_cap(s, p, s->icl_set);
        const uint32_t held_fcc = held_icl < s->icl_set ? umin(s->fcc_set, umax(2 * held_icl, PIANO_CHG_FCC_DEFAULT)) : s->fcc_set;
        p->icl_ma = held_icl;
        p->fcc_ma = held_fcc;
        p->bus_mv = s->hv == PIANO_CHG_HV_ON ? HV_MV : 5000;
        if (held_icl < s->icl_set) {
            plan_fcc(c, s, p, now, held_fcc, held_fcc);
            plan_icl(c, in, s, p, now, held_icl);
        }
        plan_fv(c, in, s, p, now, &t);
        s->hold_awake = p->hold_awake = (uint8_t)(c->writes && limit_on(c) && charge_ok && soc_valid(in));
        return;
    }
    s->notify_holds = 0;
    if (s->type < 0 && !c->strict_contract) {
        if (!charge_ok) plan_enable(c, in, s, p, now, 0);
        return;
    }
    const int cls = c->strict_contract && strict_contract(in) ?
                    (in->contract_protocol == 2 ? PIANO_CHG_C_PD_PPS : PIANO_CHG_C_PD) : piano_charge_class(s->type);
    p->cls = (uint8_t)cls;
    if (cls == PIANO_CHG_C_REFUSED && !c->strict_contract) {   /* classification never bypasses a known stop */
        if (!charge_ok) plan_enable(c, in, s, p, now, 0);
        s->hold_awake = p->hold_awake = 0;
        return;
    }
    p->charge = (uint8_t)charge_ok;
    const uint32_t hv_ma = !c->strict_contract ? hv_step(c, in, s, p, now, cls, charge_ok) :
                           qc_session(c, s) ? strict_qc(c, in, s, p, &t, now, charge_ok) :
                                              strict_hv(c, in, s, p, &t, now, charge_ok);
    const int hv_on = s->hv == PIANO_CHG_HV_ON && hv_ma;
    if (c->strict_contract) s->contract_ok = (uint8_t)strict_contract(in);   /* T-14b: what a notification hold may keep */
    p->hv = s->hv;
    uint32_t icl;
    if (hv_on) {
        icl = umin(s->hv_qc ? piano_charge_icl_qc3_9v(hv_ma) : piano_charge_icl_9v(cls, hv_ma),
                   hv_caps(c) ? PIANO_CHG_ICL_ABS_MAX : icl_cap(c));
        /* T-14 9 V (owner 2026-11-23 19:22): with cfg.hv_caps a fixed 9 V PD contract gets what it advertises, its RDO within the
         * object (hv_ma), up to the buck's highest stock input at 9 V (in_pps 2500; the Ubuntu port's fixed-PD rule, its
         * MCA_ICL_HV_MAX_MA), instead of the in_pd row 1600; above the row only with a connector that reads below 50 degC and no
         * hot hold (the 5 V rule), else the row. QC and PPS keep their rows. */
        if (hv_caps(c) && !s->hv_qc && cls == PIANO_CHG_C_PD) {
            icl = umin(hv_ma, PIANO_CHG_ICL_ABS_MAX);
            if (icl > HV_PD_ROW_MA && !conn_allows_rp(in, s)) icl = HV_PD_ROW_MA;
        }
        p->icl_why = PIANO_CHG_ICL_9V;
        p->rp_ma = 0;
    } else {
        icl = icl_at_5v(c, in, s, p, cls, 1);
        if (ramped(c) && (p->icl_why == PIANO_CHG_ICL_RP || p->icl_why == PIANO_CHG_ICL_CONTRACT_5V)) {
            /* stepped from the row; the row is computed after the Rp target so that icl_why and rp_ma stay the Rp's */
            const uint8_t why = p->icl_why;
            const uint32_t rp_ma = p->rp_ma;
            const uint32_t row = icl_at_5v(c, in, s, p, cls, 0);
            p->icl_why = why;
            p->rp_ma = rp_ma;
            icl = ramp(in, s, p, now, row, icl);
        } else if (s->ramp_ma) {
            ramp_restart(s);   /* no Rp limit above the row now (the Rp gone, a PD contract, a hold): the next one starts over */
        }
    }
    if (hv_on && s->ramp_ma) ramp_restart(s);
    if (c->strict_contract) {
        if (!icl) icl = PIANO_CHG_ICL_DEFAULT;
        if (is_pd(cls)) {
            const int contract = strict_contract(in);
            const uint32_t cap = p->icl_why == PIANO_CHG_ICL_CONTRACT_5V ? piano_charge_icl_max(c) :
                                 hv_on && hv_caps(c) ? PIANO_CHG_ICL_ABS_MAX : PIANO_CHG_ICL_CAP;
            icl = contract ? umin(icl, umin(in->contract_ma, cap)) : PIANO_CHG_ICL_DEFAULT;
        }
        if (s->contract_transition) icl = umin(icl, s->transition_ma);
        if (!gauge_fresh(in, now) || !t.valid) icl = umin(icl, PIANO_CHG_ICL_DEFAULT);
        const uint32_t live_safety = PIANO_CHG_SAFE_LPD_DRY | PIANO_CHG_SAFE_DTPT;
        if ((in->safety & live_safety) != live_safety ||
            (is_pd(cls) && (!(in->valid & PIANO_CHG_IN_SOURCE) || !strict_contract(in))))
            icl = umin(icl, s->icl_set ? s->icl_set : PIANO_CHG_ICL_DEFAULT);
        p->transition = s->contract_transition;
    }
    icl = board_cap(s, p, icl);
    if (s->ramp_ma && (p->icl_why == PIANO_CHG_ICL_CONN || p->icl_why == PIANO_CHG_ICL_CHGR)) {
        ramp_restart(s);   /* a board hold: afterwards the steps start again from the row, never straight back up */
        p->ramp_ma = p->ramp_from_ma = 0;
        p->ramp_wait = PIANO_CHG_RAMP_OFF;
    }
    /* an Rp limit in force (not stepped): a bus that sags under it ends the trust (the row from this decision on) */
    if (ramped(c)) {
        s->rp_taken_ms = 0;   /* the steps have their own sag and detach rules */
    } else if (p->icl_why == PIANO_CHG_ICL_RP && s->icl_set == icl) {
        if (!s->rp_taken_ms) s->rp_taken_ms = now ? now : 1;
        if (in->bus_mv < RP_SAG_MV) {
            s->rp_distrust = 1;
            s->rp_taken_ms = 0;
            p->events |= PIANO_CHG_EV_RP_DISTRUST;
            icl = board_cap(s, p, icl_at_5v(c, in, s, p, cls, 0));
        }
    } else if (p->icl_why != PIANO_CHG_ICL_RP) {
        s->rp_taken_ms = 0;
    }
    /* the charge current: the class's; a source whose Type-C current the steps follow above its row is a 1.5 / 3 A source, so the
     * DCP row's (2000, within the ceiling: 1500 at stage A); the derating is the same, and the input limit still governs the input */
    uint32_t full = piano_charge_fcc_full(cls, hv_on);
    if (!hv_on && ramped(c) && p->icl_why == PIANO_CHG_ICL_RP) full = umax(full, piano_charge_fcc_full(PIANO_CHG_C_DCP, 0));
    /* cfg.hv_caps at 9 V: the stock rows (PD 3000, PPS 3500, QC3 3500, QC2 2600), at most twice the input limit in force (what
     * the buck can pass from 9 V into a 4 V pack); 5 V and every transition keep the stage A ceilings */
    const int fast = hv_on && hv_caps(c);
    if (fast && s->hv_qc) full = piano_charge_qc_fcc_9v(s->type);
    /* T-14 9 V: a fixed PD source above its row: the buck's stock maximum at 9 V (chg_pps 3500; the Ubuntu port: 2 x ICL, at most
     * 3600), still at most twice the input limit below and under every derating and hold */
    if (fast && !s->hv_qc && cls == PIANO_CHG_C_PD && icl > HV_PD_ROW_MA) full = PIANO_CHG_FCC_ABS_MAX;
    full = umin(full, fast ? PIANO_CHG_FCC_ABS_MAX : fcc_cap(c));
    if (c->strict_contract) {
        full = umin(full ? full : PIANO_CHG_FCC_DEFAULT, fast ? umin(PIANO_CHG_FCC_ABS_MAX, 2 * icl) : PIANO_CHG_FCC_CAP);
        const uint32_t live_safety = PIANO_CHG_SAFE_LPD_DRY | PIANO_CHG_SAFE_DTPT;
        if (s->contract_transition || !gauge_fresh(in, now) || !t.valid ||
            (in->safety & live_safety) != live_safety ||
            (is_pd(cls) && (!(in->valid & PIANO_CHG_IN_SOURCE) || !strict_contract(in))))
            full = umin(full, s->fcc_set ? umin(s->fcc_set, PIANO_CHG_FCC_DEFAULT) : PIANO_CHG_FCC_DEFAULT);
    }
    const uint32_t fcc = fcc_target(s, full, &t, now);
    p->icl_ma = icl;
    p->fcc_ma = fcc;
    p->bus_mv = s->hv == PIANO_CHG_HV_ASKED || s->hv == PIANO_CHG_HV_ON ? HV_MV : 5000;
    /* the writes, in this order: a stop first, the currents, the bus voltage, a resume, the float voltage. The currents: an input
     * limit that goes up first (the system gets the input before the battery asks for more), else the charge current first. On
     * 2026-11-12 the firmware answered a first charge current write made while its input was still at the 100 mA request only
     * after the codec's window, twice (P1, P2), and every one made after the input limit at once (I, usbhost-u0). */
    if (!charge_ok && p->why_off != PIANO_CHG_OFF_FIRMWARE) plan_enable(c, in, s, p, now, 0);
    if (icl > s->icl_set) {
        plan_icl(c, in, s, p, now, icl);
        plan_fcc(c, s, p, now, fcc, full);
    } else {
        plan_fcc(c, s, p, now, fcc, full);
        plan_icl(c, in, s, p, now, icl);
    }
    const int new_hv_write = s->volt_want_prop == PIANO_CHG_PPS ||
                             (s->volt_want_prop == PIANO_CHG_PD_FIXED && s->volt_want_value != 5000);
    if (strict_voltage_ready(c, in, s) && (!c->strict_contract || !s->voltage_observe_hold) &&
        (!c->strict_contract || !new_hv_write || strict_contract(in))) plan_volt(c, s, p, now);
    if (charge_ok) {
        if (s->disabled && !kick_holding(s, now)) {
            if (!c->strict_contract || gauge_fresh(in, now)) plan_enable(c, in, s, p, now, 1);
        }
        else if ((in->valid & PIANO_CHG_IN_ENABLE) && !in->enable && !s->fw_noted) {s->fw_noted = 1; p->events |= PIANO_CHG_EV_FW_DISABLED;}
    }
    /* the restart: the input measured against the limit it is held to (the one taken, or the firmware's own when lower) */
    uint32_t held = s->icl_set;
    if ((in->valid & PIANO_CHG_IN_AICL) && in->aicl_ma >= (int32_t)PIANO_CHG_ICL_DEFAULT && (uint32_t)in->aicl_ma < held)
        held = (uint32_t)in->aicl_ma;
    kick(c, in, s, p, now, &t, charge_ok, held);
    plan_fv(c, in, s, p, now, &t);
    /* the stop's read-back: 0x2003 reads 0 and yet the packs take current a minute later */
    if (!charge_ok && !s->stop_noted && s->enable_set == 0 && (in->valid & PIANO_CHG_IN_ENABLE) && !in->enable &&
        (in->valid & PIANO_CHG_IN_GAUGES) && in->ibat_ma > STOP_LEAK_MA && now >= s->off_since_ms + STOP_CHECK_MS) {
        s->stop_noted = 1;
        p->events |= PIANO_CHG_EV_STOP_LEAK;
    }
    /* the charging hold: protected charging below the limit keeps the system out of idle sleep, so the stop comes on time */
    s->hold_awake = p->hold_awake = (uint8_t)(c->writes && limit_on(c) && charge_ok && soc_valid(in));
}

/* The charge indicator's path (the Ubuntu port's top-bar protocol, here for the registry and the log): from the plan's class and
 * the 9 V state, never from a request alone. */
int piano_charge_path(const PianoChargeState *s, const PianoChargePlan *p, const PianoChargeInput *in)
{
    if (!(in->valid & PIANO_CHG_IN_BUS) || in->bus_mv < ONLINE_MV || !s->attached) return PIANO_CHG_PATH_NONE;
    if (s->hv == PIANO_CHG_HV_ON) return s->hv_qc ? PIANO_CHG_PATH_QC_9V : s->hv_pps ? PIANO_CHG_PATH_PPS_9V : PIANO_CHG_PATH_PD_9V;
    if (s->hv == PIANO_CHG_HV_ASKED) return PIANO_CHG_PATH_ASKING;
    return is_pd(p->cls) ? PIANO_CHG_PATH_PD_5V : PIANO_CHG_PATH_USB_5V;
}

const char *piano_charge_path_name(int path)
{
    static const char *const names[] = {"none", "USB 5 V", "PD 5 V", "PD 9 V", "PPS 9 V", "QC 9 V", "asking 9 V"};
    return path >= 0 && path <= PIANO_CHG_PATH_ASKING ? names[path] : "?";
}

void piano_charge_shutdown(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p)
{
    zero(p, sizeof *p);
    const uint64_t now = in->now_ms;
    p->events = s->events;
    s->events = 0;
    expire_pending(s, now);
    if (c->strict_contract) s->sleeping = 1;
    s->hold_awake = 0;
    if (s->disabled) plan_write(c, s, p, now, PIANO_CHG_S_ENABLE, PIANO_CHG_ENABLE, 1, 1);
    if (s->attached && (volt_raised(s) || (c->strict_contract && in->bus_mv >= HV_5V_MAX_MV))) {
        if (c->strict_contract) {
            s->contract_transition = 1;
            s->transition_ma = strict_contract(in) ? umin(500, in->contract_ma) : PIANO_CHG_ICL_DEFAULT;
            plan_fcc(c, s, p, now, 500, 500);
            plan_icl(c, in, s, p, now, s->transition_ma);
            want_volt(s, withdraw_prop(s), 5000);
        } else want_5v(s);
        if (strict_voltage_ready(c, in, s)) plan_volt(c, s, p, now);
    }
    p->sleep_ready = p->writes == 0 && s->pending == 0 && !s->handover_fault;
    if (c->strict_contract && p->sleep_ready && s->attached)
        p->sleep_ready = (in->valid & PIANO_CHG_IN_ENABLE) && in->enable == 1 &&
                         (in->valid & PIANO_CHG_IN_BUS) && in->bus_mv < HV_5V_MAX_MV;
}

/* --------------------------------------------------------------------------------------------------- the direct-charging curve */
#define CP_VBAT_MIN_MV 3450
#define CP_VBAT_NORMAL_MV 4473     /* the normal table's last step (with the float option on) */
#define CP_VBAT_DEFAULT_MV 4387    /* the firmware's 4400 mV float less 13 mV of hysteresis (float option off) */
#define CP_VBAT_FFC_MV 4540
#define CP_VBAT_HYS_MV 13
#define CP_CELL_OCP_MA 7000
#define CP_TBAT_MIN 15
#define CP_TBAT_FULL 18
#define CP_TBAT_WARM 40
#define CP_TBAT_EXIT 45
#define CP_TBAT_HYS 2
#define CP_HOLD_MS 30000u
#define CP_RAMP_MS 15000u
#define CP_RAMP_MA 500u
#define CP_REDUCED_MA 4500u
#define CP_IBAT_MAX_MA 12000u      /* the 2:1 stage's battery side */
#define CP_SOURCE_MAX_MA 6000u
#define CP_POWER_MW 67000u
#define CP_FFC_SOC 95

static const struct {uint32_t mv, ma;} kCpNormal[] = {{4150, 8000}, {4473, 7230}};
static const struct {uint32_t mv, ma;} kCpFfc[] = {{4144, 12400}, {4300, 10650}, {4475, 9040}, {4540, 7230}};

uint32_t piano_charge_cp_ibat_ma(const PianoChargeCpInput *in, PianoChargeCpState *s)
{
    if (!in->verified || !in->t_valid) return 0;
    const uint8_t profile = in->full_charge && in->soc_valid && in->soc_max_pct < CP_FFC_SOC && in->t_cool_c >= CP_TBAT_FULL &&
                            in->t_warm_c < 48 ? PIANO_CHG_CP_FFC : PIANO_CHG_CP_NORMAL;
    if (profile != s->profile) {s->profile = profile; s->stage = 0;}
    const int32_t vbat_max = profile == PIANO_CHG_CP_FFC ? CP_VBAT_FFC_MV : in->full_charge ? CP_VBAT_NORMAL_MV : CP_VBAT_DEFAULT_MV;
    const int32_t cell = in->cell_ma_max < 0 ? -in->cell_ma_max : in->cell_ma_max;
    if (in->running) {
        if (in->t_cool_c < CP_TBAT_MIN || in->t_warm_c >= CP_TBAT_EXIT || in->vbat_max_mv >= vbat_max || cell >= CP_CELL_OCP_MA) return 0;
    } else if (in->vbat_min_mv < CP_VBAT_MIN_MV || in->vbat_max_mv >= vbat_max - CP_VBAT_HYS_MV || cell >= CP_CELL_OCP_MA ||
               in->t_cool_c < CP_TBAT_MIN || in->t_warm_c >= CP_TBAT_WARM - CP_TBAT_HYS) {
        return 0;
    }
    /* the step table: the stage only moves on while the voltage passes each step */
    const unsigned steps = profile == PIANO_CHG_CP_FFC ? 4 : 2;
    while (s->stage + 1u < steps && in->vbat_max_mv >= (int32_t)(profile == PIANO_CHG_CP_FFC ? kCpFfc[s->stage].mv : kCpNormal[s->stage].mv))
        s->stage++;
    uint32_t ma = umin(CP_IBAT_MAX_MA, profile == PIANO_CHG_CP_FFC ? kCpFfc[s->stage].ma : kCpNormal[s->stage].ma);
    /* the source: twice its APDO current on the battery side, within 67 W at the voltage asked */
    uint32_t source = umin(in->apdo_ma, CP_SOURCE_MAX_MA);
    source = umin(source, CP_POWER_MW * 1000u / umax(in->vbus_mv, 6000));
    ma = umin(ma, umin(source, CP_IBAT_MAX_MA / 2) * 2);
    /* warm: 4.5 A from 40 degC, held 30 s, then back 0.5 A per 15 s once 2 degC cooler */
    if (!s->warm_valid) {s->warm_valid = 1; s->warm_ma = CP_IBAT_MAX_MA; s->warm_ms = in->now_ms;}
    if (in->t_warm_c >= CP_TBAT_WARM) {
        s->warm_ma = CP_REDUCED_MA;
        s->warm_ms = in->now_ms;
    } else if (s->warm_ma < CP_IBAT_MAX_MA && in->t_warm_c <= CP_TBAT_WARM - CP_TBAT_HYS &&
               in->now_ms >= s->warm_ms + (s->warm_ma == CP_REDUCED_MA ? CP_HOLD_MS : CP_RAMP_MS)) {
        s->warm_ma = umin(s->warm_ma + CP_RAMP_MA, CP_IBAT_MAX_MA);
        s->warm_ms = in->now_ms;
    }
    ma = umin(ma, s->warm_ma);
    if (in->t_cool_c < CP_TBAT_FULL) ma = umin(ma, CP_REDUCED_MA);
    return ma;
}

/* ---------------------------------------------------------------------------------- the charge-pump session's buck side ---- */
/* charge/cp-sess (ChargePumpSession.h): the system side of the pump session from this policy's view, and the hand-back of the
 * source when a session ends. The session itself plans nothing here: while it holds the source, the worker clears
 * PIANO_CHG_SAFE_CP_OFF, and the strict session above plans no voltage and holds the reduced-current barrier. */
static int temp_ok(int32_t dc) {return dc > -400 && dc < 900;}

void piano_charge_cp_sys(const PianoChargeConfig *c, const PianoChargeInput *in, const PianoChargeState *s, PianoCpSys *y)
{
    zero(y, sizeof *y);
    const uint64_t now = in->now_ms;
    y->now_ms = now;
    y->attached = s->attached;
    y->sleep_pending = in->sleep == PIANO_CHG_WILL_SLEEP;
    y->awake = !y->sleep_pending && !s->sleeping;
    /* the 80/75 health limit: its hold ends a session, and no session starts within 3 % of its stop */
    y->health_hold = (uint8_t)(s->hold || (limit_on(c) && soc_valid(in) && in->soc_max_pct >= (int32_t)c->stop_pct));
    if (limit_on(c)) y->soc_entry_max = (uint8_t)(c->stop_pct > 3 ? c->stop_pct - 3 : 0);
    if (limit_on(c) && !y->soc_entry_max) y->health_hold = 1;
    /* the source: a validated contract, and the APDO (min != max) reaching furthest above 9 V with 2 A or more */
    const int contract = c->strict_contract && strict_contract(in);
    y->contract_valid = (uint8_t)contract;
    y->contract_pps = (uint8_t)(contract && in->contract_protocol == 2);
    y->contract_mv = contract ? in->contract_mv : 0;
    y->contract_ma = contract ? in->contract_ma : 0;
    if (contract && (in->valid & PIANO_CHG_IN_PDOS)) {
        const unsigned n = in->pdos < 7 ? in->pdos : 7;
        for (unsigned i = 0; i < n; i++) {
            const uint32_t lo = in->pdo[i][0], hi = in->pdo[i][1], ma = umin(in->pdo[i][2], 6000);
            if (lo == hi || hi < HV_MV || lo > hi || ma < 2000) continue;
            if (y->src_kind == PIANO_CPS_SRC_PPS && (hi < y->apdo_max_mv || (hi == y->apdo_max_mv && ma <= y->apdo_ma))) continue;
            y->src_kind = PIANO_CPS_SRC_PPS;
            y->apdo_min_mv = lo; y->apdo_max_mv = hi; y->apdo_ma = ma;
        }
    }
    /* QC3-B / QC3.5: the buck's QC session's evidence (the firmware type and a fresh non-PD UCSI status) stands in for a contract;
     * only with that session on (its 0x2011 = 5000 is what takes the source back) */
    if (!y->src_kind && c->qc && c->strict_contract && (in->valid & PIANO_CHG_IN_TYPE) && (in->valid & PIANO_CHG_IN_NONPD)) {
        if (in->type == PIANO_CHG_T_HVDCP3_B) y->src_kind = PIANO_CPS_SRC_QC3B;
        else if (in->type == PIANO_CHG_T_HVDCP3P5) y->src_kind = PIANO_CPS_SRC_QC35;
        if (y->src_kind) y->contract_valid = 1;
    }
    y->bus_valid = (in->valid & PIANO_CHG_IN_BUS) != 0;
    y->vbus_mv = in->bus_mv;
    y->ibus_ma = (in->valid & PIANO_CHG_IN_IBUS) ? in->ibus_ma : 0;
    y->buck_icl_set_ma = s->icl_set;
    y->buck_fcc_set_ma = s->fcc_set;
    y->buck_pending = (s->pending & ((1u << PIANO_CHG_S_ICL) | (1u << PIANO_CHG_S_FCC))) != 0;
    y->volt_pending = (s->pending & (1u << PIANO_CHG_S_VOLT)) != 0;   /* the buck's own voltage write in flight */
    y->gauges_valid = (uint8_t)(packs_valid(in) && soc_valid(in) && gauge_fresh(in, now));
    y->soc_pct = in->soc_max_pct;
    y->vbat_max_mv = in->vbat_max_mv;
    y->t_cool_dc = in->t_cool_dc;
    y->t_warm_dc = in->t_warm_dc;
    if ((in->valid & PIANO_CHG_IN_NTC) && temp_ok(in->t_ntc_dc)) {
        if (in->t_ntc_dc > y->t_warm_dc) y->t_warm_dc = in->t_ntc_dc;
        if (in->t_ntc_dc < y->t_cool_dc) y->t_cool_dc = in->t_ntc_dc;
    }
    const int c1 = (in->valid & PIANO_CHG_IN_CONN1) && temp_ok(in->t_conn1_dc), c2 = (in->valid & PIANO_CHG_IN_CONN2) && temp_ok(in->t_conn2_dc);
    y->conn_valid = (uint8_t)(c1 || c2);
    y->t_conn_dc = c1 && c2 ? (in->t_conn1_dc > in->t_conn2_dc ? in->t_conn1_dc : in->t_conn2_dc) : c1 ? in->t_conn1_dc : in->t_conn2_dc;
    /* the worker's: vbat_min_mv, ibat_a_ma, ibat_b_ma (cells_valid), pps_taken_word, qc_pulses_taken, attach_seq,
     * mipps_cleanup_pending */
}

void piano_charge_cp_handback(PianoChargeState *s, uint32_t property, uint32_t value, uint64_t now)
{
    const int slot = PIANO_CHG_S_VOLT;
    s->volt_set_prop = property;
    s->volt_set_value = value;
    s->touched |= (uint8_t)(1u << slot);
    s->pending &= (uint8_t)~(1u << slot);
    s->tries[slot] = s->mismatch[slot] = s->rewrites[slot] = 0;
    s->contract_transition = 1;
    s->transition_ma = 500;
    s->hv_retry_ms = now + HV_WAIT_MS;
    if (s->hv != PIANO_CHG_HV_BLOCKED) s->hv = PIANO_CHG_HV_OFF;
    if (property == PIANO_CHG_QC_VOLT) {
        s->hv_qc = 1; s->hv_pps = 0;
        want_volt(s, PIANO_CHG_QC_VOLT, 5000);
    } else {
        /* the mode-change path: the barrier, vSafe5V, the bus back below 6 V, and only then (after HV_WAIT_MS) a new 9 V */
        s->hv_qc = 0; s->hv_pps = (uint8_t)(property == PIANO_CHG_PPS);
        s->contract_mode_change = 1;
        want_volt(s, PIANO_CHG_PD_FIXED, 5000);
    }
}
