// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano: the SC8541 charge-pump session (ChargePumpSession.h). Pure C: no I/O, no allocation, no clock but sys->now_ms.
#include "ChargePumpSession.h"

static void zero(void *p, unsigned n) {uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0;}
static uint32_t umin(uint32_t a, uint32_t b) {return a < b ? a : b;}
static uint32_t umax(uint32_t a, uint32_t b) {return a > b ? a : b;}
static int32_t iabs(int32_t v) {return v < 0 ? -v : v;}
static int32_t dc_floor(int32_t dc) {return dc >= 0 ? dc / 10 : -((-dc + 9) / 10);}
static int32_t dc_ceil(int32_t dc) {return dc >= 0 ? (dc + 9) / 10 : -(-dc / 10);}

#define WAKE_SETTLE_MS 10000u      /* awake this long before a session may start (and after every wake) */
#define BUCK_HOLD_MS 20000u        /* the buck's barrier taken within this, else the session gives up (no fault) */
#define START_TIMEOUT_MS 3000u     /* the start voltage: contract and bus within this of the request */
#define START_TRIES 2u
#define ENABLE_TIMEOUT_MS 1000u    /* CHG_EN reads set within this of START (the CHG_EN latency) */
#define OPEN_FIRST_MS 300u         /* the first opening step 300 ms after the enable, */
#define OPEN_STEP_MS 200u          /* then every 200 ms, */
#define OPEN_STEP_MV 40u           /* +40 mV each, */
#define OPEN_TRIES 10u             /* at most 10, */
#define OPEN_IBUS_MA 600           /* until the pump carries this on the bus */
#define CC_MS 700u
#define CV_MS 500u
#define REG_STEP_MV 20u
#define REG_DOWN_MAX_MV 200u
#define REG_BAND_BELOW_MA 200      /* the bus held within 200 mA below the target */
#define REG_ABOVE_MA 100
#define REG_WELL_ABOVE_MA 500
#define REG_IBUS_PER_STEP_MA 140   /* ~140 mA per 20 mV */
#define VBUS_STAGE_MARGIN_MV 1200  /* the pump's bus at most 2 x VBAT + 1200 at the stage */
#define VBUS_REQ_MARGIN_MV 2000    /* the request at most 2 x VBAT + 2000; the pump's bus above that: a fault */
#define START_OFFSET_MV 300u       /* the start voltage 2 x VBAT + 300 */
#define START_BUS_BELOW_MV 500     /* the bus at the start: within -500 / +300 mV of the request (the cable drop) */
#define START_BUS_ABOVE_MV 300
#define START_CONTRACT_MV 100
#define APDO_HEADROOM_MV 600u      /* the APDO must reach the start voltage plus this (the opening's 10 x 40 mV, and more) */
#define APDO_MIN_MA 2000u
#define VOLT_PENDING_MS 2000u      /* a voltage request not answered within this counts as an error */
#define STOP_STUCK_MS 2000u        /* CHG_EN still set this long after STOP: stuck (the driver's guard stops again) */
#define STOP_RESEND_MS 500u
#define PIANO_CPS_INPUT_LOST_MV 4000   /* the pump's VBUS below this: no source (a detach seen on the pump's own ADC) */
#define STOP_GUARD_MS (PIANO_CP_GUARD_MS + 100u)   /* the driver's guard re-reads CHG_EN 500 ms after a stop: the source waits for it */
#define RETRY_FAULT_MS 60000u      /* after a fault exit (below the latch) */
#define RETRY_TAPER_MS 300000u     /* after the taper hands over to the buck */
#define RETRY_OTHER_MS 30000u
#define CV_ENTER_MV 40             /* CV from this far below the session's voltage limit */
#define CV_HOLD_MV 10
#define CV_STEP_MA 100u
#define CELL_SOFT_MARGIN_MA 2000   /* one pack above half the target plus this, two polls in a row: cell OCP */
#define CELL_SOFT_STRIKES 2u
#define DERATE_STEP_MA 200u
#define DERATE_FLOOR_MA 1000u
#define DERATE_BACK_MA 50u         /* a quarter as fast */
#define BUSOCP_MARGIN_MA 1000u
#define BUSOCP_STEP_MA 250u        /* reprogram when the wanted value moves by at least one register step */
#define START_HYS_MV 13            /* start below the limit by this (the curve's own start hysteresis) */

/* the session's voltage limit (CV): the firmware's 4400 mV float less 13 mV, the stock normal table's 4473 with the float option, or
 * the FFC table's 4540 (only with the float option, which the 80/75 health limit never sets) */
static int32_t cv_mv(const PianoCpConfig *c) {return c->full_charge ? 4473 : 4387;}

void piano_cp_config_default(PianoCpConfig *c)
{
    zero(c, sizeof *c);
    c->latch_faults = PIANO_CPS_LATCH_DEFAULT;
}

void piano_cp_init(PianoCpState *s)
{
    zero(s, sizeof *s);
    s->derate_ma = PIANO_CPS_IBAT_MAX_MA;
}

static const char *const kStates[PIANO_CPS_STATES] = {
    "idle", "buck-hold", "set-start", "enabling", "opening", "cc", "cv", "stopping", "latched"};
const char *piano_cp_state_name(int state) {return state >= 0 && state < PIANO_CPS_STATES ? kStates[state] : "?";}

static const char *const kExits[PIANO_CPS_EXIT_REASONS] = {
    "none", "detach", "sleep", "disarm", "health", "gauge", "tbat", "connector", "taper", "contract", "buck",
    "pump-fault", "tdie", "cell-ocp", "bus-high", "watchdog", "open-fail", "enable-fail", "start-fail", "errors", "stuck", "lost"};
const char *piano_cp_exit_name(int reason) {return reason >= 0 && reason < PIANO_CPS_EXIT_REASONS ? kExits[reason] : "?";}

static int is_fault(int reason) {return reason >= PIANO_CPS_EXIT_FAULT_FIRST;}
static int in_session(int state) {return state >= PIANO_CPS_BUCK_HOLD && state <= PIANO_CPS_CV;}
static int qc_kind(uint8_t k) {return k == PIANO_CPS_SRC_QC3B || k == PIANO_CPS_SRC_QC35;}
static uint32_t qc_step_mv(uint8_t k) {return k == PIANO_CPS_SRC_QC3B ? PIANO_CPS_QC3B_STEP_MV : PIANO_CPS_QC35_STEP_MV;}
static uint32_t qc_ibat_ma(uint8_t k) {return k == PIANO_CPS_SRC_QC3B ? PIANO_CPS_QC3B_IBAT_MA : PIANO_CPS_QC35_IBAT_MA;}
static uint32_t make_word(uint32_t mv, uint32_t ma) {return (mv / 20u) << 16 | (ma / 50u);}

static int adc_has(const struct piano_cp_in *d, int ch) {return (int)((d->adc.valid >> ch) & 1u);}
/* every driver fault ends a session; UNARMED (a refused START) is reported as DISARM */
static uint32_t hard_faults(const struct piano_cp_in *d) {return d->faults & ~(uint32_t)PIANO_CP_FAULT_UNARMED;}

static uint32_t source_budget(const struct piano_cp_in *d)
{
    if (!d->mipps_verified) return PIANO_CP_BUDGET_STANDARD_MA;
    return umin(d->source_budget_ma ? d->source_budget_ma : PIANO_CP_BUDGET_VERIFIED_MA, PIANO_CP_BUDGET_VERIFIED_MA);
}

uint32_t piano_cp_ibus_budget(const PianoCpSys *sys, const struct piano_cp_in *drv, uint32_t mv)
{
    uint32_t ma;
    if (sys->src_kind == PIANO_CPS_SRC_PPS) ma = umin(sys->apdo_ma, source_budget(drv));
    else if (qc_kind(sys->src_kind)) ma = qc_ibat_ma(sys->src_kind) / 2u;
    else return 0;
    ma = umin(ma, PIANO_CPS_POWER_MW * 1000u / umax(mv, PIANO_CPS_REQ_MIN_MV));
    ma = umin(ma, PIANO_CPS_STAGE_IBUS_MA);
    return ma / 50u * 50u;
}

uint32_t piano_cp_ibat_target(const PianoCpConfig *c, const PianoCpSys *sys, const struct piano_cp_in *drv, PianoCpState *s,
                              int run, uint32_t vbus_mv)
{
    if (!sys->gauges_valid || !sys->cells_valid) return 0;
    PianoChargeCpInput ci;
    zero(&ci, sizeof ci);
    ci.now_ms = sys->now_ms;
    ci.t_valid = 1;
    ci.soc_valid = 1;
    ci.verified = 1;   /* the curve's own gate means "a session may run"; what verification changes is the budget below */
    ci.full_charge = c->full_charge;
    ci.running = (uint8_t)(run != 0);
    ci.vbat_max_mv = sys->vbat_max_mv;
    ci.vbat_min_mv = sys->vbat_min_mv;
    ci.t_cool_c = dc_floor(sys->t_cool_dc);
    ci.t_warm_c = dc_ceil(sys->t_warm_dc);
    ci.soc_max_pct = sys->soc_pct;
    ci.cell_ma_max = iabs(sys->ibat_a_ma) > iabs(sys->ibat_b_ma) ? sys->ibat_a_ma : sys->ibat_b_ma;
    ci.apdo_ma = piano_cp_ibus_budget(sys, drv, vbus_mv);
    ci.vbus_mv = vbus_mv;
    uint32_t ma = piano_charge_cp_ibat_ma(&ci, &s->curve);
    if (qc_kind(sys->src_kind)) ma = umin(ma, qc_ibat_ma(sys->src_kind));
    ma = umin(ma, PIANO_CPS_IBAT_MAX_MA);
    ma = umin(ma, 2u * PIANO_CPS_STAGE_IBUS_MA);
    ma = umin(ma, s->derate_ma);
    return ma;
}

int piano_cp_write_allowed(const PianoCpConfig *c, uint32_t property, uint32_t value, uint8_t bytes)
{
    if (property == PIANO_CHG_PPS) {
        const uint32_t mv = (value >> 16) * 20u, ma = (value & 0xffffu) * 50u;
        return bytes == 4 && mv >= PIANO_CPS_REQ_MIN_MV && mv <= PIANO_CPS_REQ_MAX_MV && ma >= 1000u &&
               ma <= PIANO_CP_BUDGET_VERIFIED_MA;
    }
    if (property == PIANO_CHG_QC_PULSE)
        return c->qc3 && (bytes == 1 || bytes == 4) && (value == PIANO_CHG_QC_UP || value == PIANO_CHG_QC_DOWN);
    return 0;
}

/* ---- one call's context ---- */
typedef struct {
    const PianoCpConfig *c;
    const PianoCpSys *sys;
    const struct piano_cp_in *drv;
    PianoCpState *s;
    PianoCpPlan *p;
    uint64_t now;
} Ctx;

static void set_state(Ctx *x, uint8_t st)
{
    x->s->state = st;
    x->s->state_ms = x->now;
}

static uint32_t ceiling_mv(const Ctx *x)
{
    uint32_t top = PIANO_CPS_REQ_MAX_MV;
    if (x->sys->src_kind == PIANO_CPS_SRC_PPS) top = umin(top, x->sys->apdo_max_mv);
    if (x->sys->vbat_max_mv > 0) top = umin(top, 2u * (uint32_t)x->sys->vbat_max_mv + VBUS_REQ_MARGIN_MV);
    return top;
}

static int volt_free(const Ctx *x) {return !x->sys->volt_pending && !x->sys->mipps_cleanup_pending;}

/* the request's current (PPS): the source budget, at least 1000 */
static uint32_t req_ma(const Ctx *x)
{
    return umax(1000u, umin(x->sys->apdo_ma, source_budget(x->drv)) / 50u * 50u);
}

/* PPS: one word for mv; QC: one pulse up or down. 1 when a request goes out this call. */
static int ask_pps(Ctx *x, uint32_t mv)
{
    if (!volt_free(x)) return 0;
    mv = mv / 20u * 20u;
    if (mv > ceiling_mv(x) || mv < PIANO_CPS_REQ_MIN_MV) return 0;
    const uint32_t ma = req_ma(x), word = make_word(mv, ma);
    x->s->req_mv = mv;
    x->s->req_ma = ma;
    x->s->req_word = word;
    x->s->request_ms = x->now;
    x->p->out.req_valid |= PIANO_CP_REQ_PPS;
    x->p->out.pps_mv = mv;
    x->p->out.pps_ma = ma;
    x->p->pps_word = word;
    return 1;
}

static int ask_pulse(Ctx *x, int up)
{
    if (!volt_free(x)) return 0;
    if (up && x->s->req_mv + qc_step_mv(x->sys->src_kind) > ceiling_mv(x)) return 0;
    x->p->out.req_valid |= PIANO_CPS_REQ_QC_PULSE;
    x->p->qc_pulse = up ? PIANO_CHG_QC_UP : PIANO_CHG_QC_DOWN;
    x->s->qc_pulses_want += up ? 1 : -1;
    x->s->req_mv = up ? x->s->req_mv + qc_step_mv(x->sys->src_kind) : x->s->req_mv - qc_step_mv(x->sys->src_kind);
    x->s->request_ms = x->now;
    return 1;
}

/* a step of the regulation: +1 up, -n down (n steps of 20 mV for PPS; one pulse for QC) */
/* charge/mipps2 ladder: a PPS step up only once the previous one is confirmed: the word acknowledged by the firmware, nothing in
 * flight, the bus read and within -1200 / +300 mV of the request (the cable drop allowance, Ubuntu MCA_CP_VBUS_MAX_DELTA_MV), and no
 * pump fault. Steps down are never held back. */
static int up_confirmed(const Ctx *x)
{
    const PianoCpSys *y = x->sys;
    if (qc_kind(y->src_kind)) return !y->volt_pending;
    if (y->volt_pending || y->pps_taken_word != x->s->req_word || !y->bus_valid || hard_faults(x->drv)) return 0;
    const int32_t req = (int32_t)x->s->req_mv;
    return y->vbus_mv >= req - 1200 && y->vbus_mv <= req + 300;
}

static void step(Ctx *x, int dir, unsigned n)
{
    if (!dir) return;
    if (dir > 0 && !up_confirmed(x)) {x->p->events |= PIANO_CPS_EV_UNCONFIRMED; return;}
    if (qc_kind(x->sys->src_kind)) {(void)ask_pulse(x, dir > 0); return;}
    const uint32_t d = REG_STEP_MV * (n ? n : 1u);
    const uint32_t mv = dir > 0 ? x->s->req_mv + REG_STEP_MV : (x->s->req_mv > d + PIANO_CPS_REQ_MIN_MV ? x->s->req_mv - d
                                                                                                     : PIANO_CPS_REQ_MIN_MV);
    (void)ask_pps(x, mv);
}

/* ---- the end of a session ---- */
static void finish(Ctx *x)
{
    PianoCpState *s = x->s;
    PianoCpPlan *p = x->p;
    /* the request the session left in force goes back to the buck's barrier */
    const int raised = qc_kind(x->sys->src_kind) ? (x->sys->qc_pulses_taken > 0 || s->qc_pulses_want > 0)
                                                 : (s->req_word != 0 || x->sys->pps_taken_word != 0);
    if (raised) {
        p->handback = 1;
        if (qc_kind(x->sys->src_kind)) {
            p->handback_prop = PIANO_CHG_QC_VOLT;
            p->handback_value = s->req_mv ? s->req_mv : 9000u;
        } else {
            p->handback_prop = PIANO_CHG_PPS;
            p->handback_value = x->sys->pps_taken_word ? x->sys->pps_taken_word : s->req_word;
        }
        p->events |= PIANO_CPS_EV_HANDBACK;
    }
    if (is_fault(s->exit_reason)) s->faults_this_attach++;
    const unsigned latch = x->c->latch_faults ? x->c->latch_faults : PIANO_CPS_LATCH_DEFAULT;
    s->req_word = 0; s->req_mv = 0; s->req_ma = 0; s->qc_pulses_want = 0;
    s->start_sent = 0; s->busocp_set = 0; s->busocp_ma = 0; s->tries = 0; s->start_tries = 0; s->ocp_strikes = 0;
    s->ibat_target_ma = s->ibus_target_ma = 0;
    s->derate_ma = PIANO_CPS_IBAT_MAX_MA;
    if (s->exit_reason == PIANO_CPS_EXIT_LOST || s->exit_reason == PIANO_CPS_EXIT_STUCK || s->faults_this_attach >= latch) {
        set_state(x, PIANO_CPS_LATCHED);
        p->events |= PIANO_CPS_EV_LATCHED;
    } else {
        set_state(x, PIANO_CPS_IDLE);
        s->retry_ms = x->now + (is_fault(s->exit_reason) ? RETRY_FAULT_MS :
                                s->exit_reason == PIANO_CPS_EXIT_TAPER ? RETRY_TAPER_MS :
                                s->exit_reason == PIANO_CPS_EXIT_SLEEP || s->exit_reason == PIANO_CPS_EXIT_DETACH ? 0 :
                                RETRY_OTHER_MS);
    }
}

/* end the session: the pump off FIRST, the source only once CHG_EN reads clear */
static void begin_exit(Ctx *x, uint8_t reason, uint32_t faults)
{
    PianoCpState *s = x->s;
    s->exit_reason = reason;
    s->exit_faults = faults;
    x->p->events |= PIANO_CPS_EV_EXIT;
    if (s->start_sent || x->drv->switching) {
        set_state(x, PIANO_CPS_STOPPING);
        s->stop_ms = x->now;
        x->p->out.action = PIANO_CP_ACT_STOP;
        x->p->out.stop_reason = reason;
        return;
    }
    finish(x);
}

/* why a running session must end now (0: it may go on) */
static uint8_t stop_reason(Ctx *x, uint32_t *faults)
{
    const PianoCpSys *y = x->sys;
    const struct piano_cp_in *d = x->drv;
    const PianoCpState *s = x->s;
    *faults = 0;
    if (!y->attached) return PIANO_CPS_EXIT_DETACH;
    if (y->sleep_pending || !y->awake) return PIANO_CPS_EXIT_SLEEP;
    if (d->lost) return PIANO_CPS_EXIT_LOST;
    if (d->faults & PIANO_CP_FAULT_KICK_LATE) {*faults = d->faults; return PIANO_CPS_EXIT_WATCHDOG;}
    if (hard_faults(d)) {*faults = d->faults; return PIANO_CPS_EXIT_PUMP_FAULT;}
    if (!d->armed || (d->faults & PIANO_CP_FAULT_UNARMED)) return PIANO_CPS_EXIT_DISARM;
    if (!d->present) return PIANO_CPS_EXIT_LOST;
    if (y->health_hold) return PIANO_CPS_EXIT_HEALTH;
    if (!y->gauges_valid || !y->cells_valid) return PIANO_CPS_EXIT_GAUGE;
    if (dc_floor(y->t_cool_dc) < PIANO_CPS_TBAT_EXIT_MIN || dc_ceil(y->t_warm_dc) >= PIANO_CPS_TBAT_EXIT_MAX)
        return PIANO_CPS_EXIT_TBAT;
    if (!y->conn_valid || dc_ceil(y->t_conn_dc) >= PIANO_CPS_CONN_MAX_C) return PIANO_CPS_EXIT_CONN;
    if (iabs(y->ibat_a_ma) >= PIANO_CPS_CELL_OCP_MA || iabs(y->ibat_b_ma) >= PIANO_CPS_CELL_OCP_MA) return PIANO_CPS_EXIT_CELL_OCP;
    /* sim (fork B): the gauges' sign checked against the pump: the pump pushing >= 2 A into the cells while they read as a
     * discharge of more than half of it means the pack currents cannot be trusted (an inverted sign hides the per-pack share) */
    if (d->switching && adc_has(d, PIANO_CP_ADC_IBAT) && d->adc.ibat_ma >= 2000 && y->cells_valid &&
        y->ibat_a_ma + y->ibat_b_ma <= -(d->adc.ibat_ma / 2)) return PIANO_CPS_EXIT_GAUGE;
    if (s->state >= PIANO_CPS_OPENING) {
        if (!adc_has(d, PIANO_CP_ADC_TDIE) || !adc_has(d, PIANO_CP_ADC_IBUS) || !adc_has(d, PIANO_CP_ADC_VBUS))
            return PIANO_CPS_EXIT_GAUGE;   /* the pump's own readings are what the loop runs on */
        if (d->adc.vbus_mv < PIANO_CPS_INPUT_LOST_MV) return PIANO_CPS_EXIT_DETACH;   /* the input gone (this pass's reading) */
        if (!d->switching) {*faults = d->faults; return PIANO_CPS_EXIT_PUMP_FAULT;}   /* the pump stopped by itself */
    }
    if (adc_has(d, PIANO_CP_ADC_TDIE) && dc_ceil(d->adc.tdie_dc) >= PIANO_CPS_TDIE_EXIT) return PIANO_CPS_EXIT_TDIE;
    if (d->switching && adc_has(d, PIANO_CP_ADC_VBUS) && y->vbat_max_mv > 0 &&
        d->adc.vbus_mv > 2 * y->vbat_max_mv + VBUS_REQ_MARGIN_MV)
        return PIANO_CPS_EXIT_BUS_HIGH;
    /* the contract: once the start voltage was taken, a validated PPS contract all the time; a request in flight (and the UCSI
     * re-read after it) may hide it for at most START_TIMEOUT_MS, counted from the last call that saw it valid */
    if (s->state >= PIANO_CPS_ENABLING && y->src_kind == PIANO_CPS_SRC_PPS) {
        if (y->contract_valid && !y->contract_pps && !y->volt_pending) return PIANO_CPS_EXIT_CONTRACT;
        if (x->now >= s->contract_ok_ms + START_TIMEOUT_MS) return PIANO_CPS_EXIT_CONTRACT;
    }
    if (y->mipps_cleanup_pending) return PIANO_CPS_EXIT_CONTRACT;
    if (s->errors >= PIANO_CPS_ERRORS_MAX) return PIANO_CPS_EXIT_ERRORS;
    return PIANO_CPS_EXIT_NONE;
}

/* the start gates (IDLE -> BUCK_HOLD); 1 when all hold */
static int may_start(Ctx *x)
{
    const PianoCpConfig *c = x->c;
    const PianoCpSys *y = x->sys;
    const struct piano_cp_in *d = x->drv;
    PianoCpState *s = x->s;
    if (x->now < s->retry_ms) return 0;
    if (!d->armed || !d->present || d->lost || d->switching || hard_faults(d)) return 0;
    if (!y->attached || !y->awake || y->sleep_pending || x->now < s->awake_since_ms + WAKE_SETTLE_MS) return 0;
    if (y->health_hold) return 0;
    if (c->mipps && !d->mipps_done) return 0;      /* MiPPS runs before any pump step */
    if (y->ladder_hold) return 0;                  /* charge/mipps2: only from a confirmed 5 V after MiPPS or a confirmed 9 V */
    if (y->mipps_cleanup_pending || y->volt_pending) return 0;
    if (!y->gauges_valid || !y->cells_valid) return 0;
    const unsigned entry = umin(y->soc_entry_max ? y->soc_entry_max : (c->soc_entry_max ? c->soc_entry_max : PIANO_CPS_SOC_ENTRY),
                                PIANO_CPS_SOC_ENTRY);
    if (y->soc_pct < 0 || (unsigned)y->soc_pct >= entry) return 0;
    if (y->vbat_min_mv < PIANO_CPS_VBAT_MIN_MV || y->vbat_max_mv <= 0 ||
        y->vbat_max_mv >= PIANO_CPS_VBAT_START_MAX_MV - START_HYS_MV || y->vbat_max_mv >= cv_mv(c) - CV_ENTER_MV)
        return 0;
    if (dc_floor(y->t_cool_dc) < PIANO_CPS_TBAT_START_MIN || dc_ceil(y->t_warm_dc) > PIANO_CPS_TBAT_START_MAX) return 0;
    if (!y->conn_valid || dc_ceil(y->t_conn_dc) >= PIANO_CPS_CONN_MAX_C) return 0;
    if (adc_has(d, PIANO_CP_ADC_TDIE) && dc_ceil(d->adc.tdie_dc) >= PIANO_CPS_TDIE_START_MAX) return 0;
    if (iabs(y->ibat_a_ma) >= PIANO_CPS_CELL_OCP_MA || iabs(y->ibat_b_ma) >= PIANO_CPS_CELL_OCP_MA) return 0;
    if (!y->contract_valid) return 0;
    const uint32_t start = (2u * (uint32_t)y->vbat_max_mv + START_OFFSET_MV + 19u) / 20u * 20u;
    if (y->src_kind == PIANO_CPS_SRC_PPS) {
        if (y->apdo_ma < APDO_MIN_MA || y->apdo_min_mv > start || y->apdo_max_mv < start + APDO_HEADROOM_MV) return 0;
    } else if (qc_kind(y->src_kind)) {
        if (!c->qc3) return 0;
    } else {
        return 0;
    }
    if (start + APDO_HEADROOM_MV > PIANO_CPS_REQ_MAX_MV) return 0;
    const uint32_t target = piano_cp_ibat_target(c, y, d, s, 0, start);
    return target >= PIANO_CPS_EXIT_IBAT_MA + 500u;
}

/* the bus OCP for the bus target: target + 1000, within [1000, 7000], reprogrammed on a step's move */
static void busocp(Ctx *x, uint32_t ibus_target)
{
    uint32_t want = umin(umax(ibus_target + BUSOCP_MARGIN_MA, PIANO_CP_BUSOCP_MIN_MA), PIANO_CP_BUSOCP_MAX_MA);
    want = want / BUSOCP_STEP_MA * BUSOCP_STEP_MA;
    const uint32_t had = x->s->busocp_ma;
    if (x->p->out.action == PIANO_CP_ACT_NONE && (!x->s->busocp_set || (want > had ? want - had : had - want) >= BUSOCP_STEP_MA)) {
        x->p->out.action = PIANO_CP_ACT_SET_BUSOCP;
        x->p->out.busocp_ma = want;
        x->s->busocp_ma = want;
        x->s->busocp_set = 1;
    }
}

/* the die-temperature derate, once per regulation step */
static void derate(Ctx *x)
{
    PianoCpState *s = x->s;
    if (!adc_has(x->drv, PIANO_CP_ADC_TDIE)) return;
    const int32_t t = dc_ceil(x->drv->adc.tdie_dc);
    if (t >= PIANO_CPS_TDIE_DERATE) {
        const uint32_t from = umin(s->derate_ma, s->ibat_target_ma ? s->ibat_target_ma : s->derate_ma);
        s->derate_ma = from > DERATE_FLOOR_MA + DERATE_STEP_MA ? from - DERATE_STEP_MA : DERATE_FLOOR_MA;
        x->p->events |= PIANO_CPS_EV_DERATE;
    } else if (t < PIANO_CPS_TDIE_RELEASE && s->derate_ma < PIANO_CPS_IBAT_MAX_MA) {
        s->derate_ma = umin(s->derate_ma + DERATE_BACK_MA, PIANO_CPS_IBAT_MAX_MA);
    }
}

/* one regulation step (CC or CV): 1 when the session goes on */
static int regulate(Ctx *x, int cv)
{
    PianoCpState *s = x->s;
    const PianoCpSys *y = x->sys;
    const struct piano_cp_in *d = x->drv;
    derate(x);
    uint32_t target = piano_cp_ibat_target(x->c, y, d, s, 1, s->req_mv);
    if (cv) {
        /* the taper: the bus target comes down while the higher pack sits at the limit */
        const uint32_t was = s->ibat_target_ma ? s->ibat_target_ma : target;
        if (y->vbat_max_mv >= cv_mv(x->c) - CV_HOLD_MV) target = umin(target, was > CV_STEP_MA * 2 ? was - CV_STEP_MA * 2 : 0);
        else target = umin(target, was);
    }
    if (target <= PIANO_CPS_EXIT_IBAT_MA) {begin_exit(x, PIANO_CPS_EXIT_TAPER, 0); return 0;}
    const uint32_t ibus_target = umin(target / 2u, piano_cp_ibus_budget(y, d, s->req_mv));
    s->ibat_target_ma = target;
    s->ibus_target_ma = ibus_target;
    x->p->ibat_target_ma = target;
    x->p->ibus_target_ma = ibus_target;
    busocp(x, ibus_target);
    /* one pack well above its share, two polls in a row */
    const int32_t share = (int32_t)(target / 2u) + CELL_SOFT_MARGIN_MA;
    if (iabs(y->ibat_a_ma) > share || iabs(y->ibat_b_ma) > share) {   /* sim (fork B): the magnitude, whatever the gauges' sign */
        if (++s->ocp_strikes >= CELL_SOFT_STRIKES) {begin_exit(x, PIANO_CPS_EXIT_CELL_OCP, 0); return 0;}
    } else {
        s->ocp_strikes = 0;
    }
    if (y->volt_pending) {
        if (x->now >= s->request_ms + VOLT_PENDING_MS) {s->errors++; x->p->events |= PIANO_CPS_EV_ERROR; s->request_ms = x->now;}
        return 1;
    }
    const int32_t ibus = d->adc.ibus_ma, vbus = d->adc.vbus_mv;
    const int32_t ibat = y->ibat_a_ma + y->ibat_b_ma;
    const int32_t t = (int32_t)ibus_target;
    /* sim (fork B): above the stage window step down by the excess (at least one step); never step up within 100 mV of it */
    const int32_t window = 2 * y->vbat_max_mv + VBUS_STAGE_MARGIN_MV;
    if (vbus > window) {
        unsigned n = (unsigned)((vbus - window) / (int32_t)REG_STEP_MV) + 1u;
        n = n > REG_DOWN_MAX_MV / REG_STEP_MV ? REG_DOWN_MAX_MV / REG_STEP_MV : n;
        step(x, -1, n);
    } else if (ibus > t + REG_WELL_ABOVE_MA) {
        unsigned n = (unsigned)((ibus - t) / REG_IBUS_PER_STEP_MA);
        n = n < 1u ? 1u : n > REG_DOWN_MAX_MV / REG_STEP_MV ? REG_DOWN_MAX_MV / REG_STEP_MV : n;
        step(x, -1, n);
    } else if (ibus > t + REG_ABOVE_MA || ibat > (int32_t)target + REG_WELL_ABOVE_MA) step(x, -1, 1);
    else if (ibus < t - REG_BAND_BELOW_MA && vbus < window - 100 && !(cv && y->vbat_max_mv >= cv_mv(x->c) - CV_HOLD_MV)) step(x, +1, 1);
    return 1;
}

/* charge/mipps2 ladder: whether an idle session would start now (the start gates on a copy of the state, nothing planned): the
 * buck then holds its 9 V request, so the pump goes first from a confirmed 5 V, as the Ubuntu port's policy pass tries cp_try_start
 * before apply_policy (xiaomi_mca.c v1.4.0 L5280-5282) */
int piano_cp_may_start(const PianoCpConfig *c, const PianoCpSys *sys, const struct piano_cp_in *drv, const PianoCpState *st)
{
    if (!c || !sys || !drv || !st || st->state != PIANO_CPS_IDLE) return 0;
    PianoCpState copy = *st;
    PianoCpPlan plan;
    zero(&plan, sizeof plan);
    Ctx x = {c, sys, drv, &copy, &plan, sys->now_ms};
    return may_start(&x);
}

void piano_cp_decide(const PianoCpConfig *c, const PianoCpSys *sys, const struct piano_cp_in *drv, PianoCpState *s,
                     PianoCpPlan *p)
{
    zero(p, sizeof *p);
    Ctx x = {c, sys, drv, s, p, sys->now_ms};
    const uint64_t now = sys->now_ms;
    /* a new attach clears the latch and the fault count */
    if (!s->attach_known || sys->attach_seq != s->attach_seq) {
        const int was_latched = s->state == PIANO_CPS_LATCHED;
        s->attach_known = 1;
        s->attach_seq = sys->attach_seq;
        s->faults_this_attach = 0;
        s->retry_ms = 0;
        if (was_latched && !drv->lost) {set_state(&x, PIANO_CPS_IDLE); p->events |= PIANO_CPS_EV_UNLATCHED;}
    }
    if (sys->awake && !sys->sleep_pending) {
        if (!s->was_awake) {s->was_awake = 1; s->awake_since_ms = now;}
    } else {
        s->was_awake = 0;
    }

    if (sys->src_kind == PIANO_CPS_SRC_PPS && sys->contract_valid && sys->contract_pps) s->contract_ok_ms = now;
    uint32_t faults = 0;
    if (in_session(s->state)) {
        const uint8_t why = stop_reason(&x, &faults);
        if (why) begin_exit(&x, why, faults);
    }

    switch (s->state) {
    case PIANO_CPS_IDLE:
        if (drv->switching && drv->present && !drv->lost) {
            /* nothing of ours runs, yet the pump reads on: off, and hold the source (no sleep) until it reads clear */
            p->out.action = PIANO_CP_ACT_STOP;
            p->out.stop_reason = PIANO_CPS_EXIT_STUCK;
            p->events |= PIANO_CPS_EV_STUCK;
            break;
        }
        if (drv->lost) {set_state(&x, PIANO_CPS_LATCHED); p->events |= PIANO_CPS_EV_LATCHED; break;}
        if (may_start(&x)) {
            set_state(&x, PIANO_CPS_BUCK_HOLD);
            s->errors = 0; s->tries = 0; s->start_tries = 0; s->ocp_strikes = 0; s->start_sent = 0;
            s->req_word = 0; s->req_mv = 0; s->qc_pulses_want = 0; s->exit_reason = 0; s->exit_faults = 0;
            s->derate_ma = PIANO_CPS_IBAT_MAX_MA;
            s->sessions++;
            p->events |= PIANO_CPS_EV_START;
        }
        break;
    case PIANO_CPS_BUCK_HOLD:
        if (sys->buck_icl_set_ma && sys->buck_icl_set_ma <= 500u && sys->buck_fcc_set_ma && sys->buck_fcc_set_ma <= 500u &&
            !sys->buck_pending) {
            set_state(&x, PIANO_CPS_SET_START);
            s->request_ms = 0;
        } else if (now >= s->state_ms + BUCK_HOLD_MS) {
            begin_exit(&x, PIANO_CPS_EXIT_BUCK, 0);
        }
        break;
    case PIANO_CPS_SET_START: {
        const uint32_t start = (2u * (uint32_t)sys->vbat_max_mv + START_OFFSET_MV + 19u) / 20u * 20u;
        if (sys->src_kind == PIANO_CPS_SRC_PPS) {
            if (!s->request_ms) {
                if (!ask_pps(&x, umax(start, sys->apdo_min_mv))) {
                    if (volt_free(&x)) begin_exit(&x, PIANO_CPS_EXIT_CONTRACT, 0);   /* the start is outside what may be asked */
                }
                break;
            }
            const int taken = sys->pps_taken_word == s->req_word && !sys->volt_pending;
            const int contract = sys->contract_valid && sys->contract_pps &&
                                 iabs((int32_t)sys->contract_mv - (int32_t)s->req_mv) <= START_CONTRACT_MV;
            const int bus = sys->bus_valid && sys->vbus_mv >= (int32_t)s->req_mv - START_BUS_BELOW_MV &&
                            sys->vbus_mv <= (int32_t)s->req_mv + START_BUS_ABOVE_MV;
            if (taken && contract && bus) {set_state(&x, PIANO_CPS_ENABLING); break;}
            if (now >= s->request_ms + START_TIMEOUT_MS) {
                s->errors++;
                p->events |= PIANO_CPS_EV_ERROR;
                if (++s->start_tries >= START_TRIES) {begin_exit(&x, PIANO_CPS_EXIT_START_FAIL, 0); break;}
                if (volt_free(&x)) s->request_ms = 0;   /* ask once more */
                else s->request_ms = now;              /* still not answered: one more wait, then the end */
            }
        } else {
            /* QC3-B / 3.5: pulses from 5 V up to the start voltage, one per call, each answered before the next */
            if (!s->req_mv) {s->req_mv = 5000u; s->request_ms = now;}
            const int32_t want = (int32_t)((start > 5000u ? start - 5000u : 0) + qc_step_mv(sys->src_kind) - 1u) /
                                 (int32_t)qc_step_mv(sys->src_kind);
            if (sys->qc_pulses_taken < want && sys->qc_pulses_taken >= s->qc_pulses_want) (void)ask_pulse(&x, 1);
            const int bus = sys->bus_valid && sys->vbus_mv >= (int32_t)start - START_BUS_BELOW_MV &&
                            sys->vbus_mv <= (int32_t)start + (int32_t)qc_step_mv(sys->src_kind) + START_BUS_ABOVE_MV;
            if (sys->qc_pulses_taken >= want && bus && !sys->volt_pending) {set_state(&x, PIANO_CPS_ENABLING); break;}
            if (now >= s->state_ms + START_TIMEOUT_MS + (uint64_t)want * 200u) begin_exit(&x, PIANO_CPS_EXIT_START_FAIL, 0);
        }
        break;
    }
    case PIANO_CPS_ENABLING:
        if (!s->start_sent) {
            p->out.action = PIANO_CP_ACT_START;
            p->out.busocp_ma = PIANO_CP_BUSOCP_START_MA;
            s->busocp_ma = PIANO_CP_BUSOCP_START_MA;
            s->busocp_set = 1;
            s->start_sent = 1;
            s->state_ms = now;
            break;
        }
        if (drv->switching) {
            set_state(&x, PIANO_CPS_OPENING);
            s->next_ms = now + OPEN_FIRST_MS;
            s->tries = 0;
            p->events |= PIANO_CPS_EV_ENABLED;
        } else if (now >= s->state_ms + ENABLE_TIMEOUT_MS) {
            s->errors++;
            begin_exit(&x, PIANO_CPS_EXIT_ENABLE_FAIL, drv->faults);
        }
        break;
    case PIANO_CPS_OPENING:
        if (now < s->next_ms) break;
        if (drv->adc.ibus_ma > OPEN_IBUS_MA) {
            set_state(&x, PIANO_CPS_CC);
            s->next_ms = now + CC_MS;
            p->events |= PIANO_CPS_EV_OPENED;
            break;
        }
        if (s->tries >= OPEN_TRIES) {begin_exit(&x, PIANO_CPS_EXIT_OPEN_FAIL, 0); break;}
        if (sys->volt_pending) {
            if (now >= s->request_ms + VOLT_PENDING_MS) {s->errors++; s->request_ms = now;}
            break;
        }
        if (!up_confirmed(&x)) {
            /* the previous step is not confirmed: no further one; a step that stays unconfirmed counts as an error */
            p->events |= PIANO_CPS_EV_UNCONFIRMED;
            if (now >= s->request_ms + VOLT_PENDING_MS) {s->errors++; p->events |= PIANO_CPS_EV_ERROR; s->request_ms = now;}
            s->next_ms = now + OPEN_STEP_MS;
            break;
        }
        if (qc_kind(sys->src_kind)) {
            if (!ask_pulse(&x, 1)) {begin_exit(&x, PIANO_CPS_EXIT_OPEN_FAIL, 0); break;}
        } else if (!ask_pps(&x, s->req_mv + OPEN_STEP_MV)) {
            if (volt_free(&x)) {begin_exit(&x, PIANO_CPS_EXIT_OPEN_FAIL, 0); break;}
        }
        s->tries++;
        s->next_ms = now + OPEN_STEP_MS;
        break;
    case PIANO_CPS_CC:
        if (now < s->next_ms) break;
        s->next_ms = now + CC_MS;
        if (!regulate(&x, 0)) break;
        if (sys->vbat_max_mv >= cv_mv(c) - CV_ENTER_MV) {set_state(&x, PIANO_CPS_CV); s->next_ms = now + CV_MS; p->events |= PIANO_CPS_EV_CV;}
        break;
    case PIANO_CPS_CV:
        if (now < s->next_ms) break;
        s->next_ms = now + CV_MS;
        (void)regulate(&x, 1);
        break;
    case PIANO_CPS_STOPPING:
        if (!(drv->faults & PIANO_CP_FAULT_STOP_STUCK) && !drv->switching && now >= s->stop_ms + STOP_GUARD_MS) {finish(&x); break;}
        if ((drv->faults & PIANO_CP_FAULT_STOP_STUCK) || now >= s->stop_ms + STOP_STUCK_MS) {
            /* the driver's guard stops it again; the source is handed back all the same, the pump latched off this attach */
            p->events |= PIANO_CPS_EV_STUCK;
            s->exit_reason = PIANO_CPS_EXIT_STUCK;
            p->out.action = PIANO_CP_ACT_STOP;
            p->out.stop_reason = PIANO_CPS_EXIT_STUCK;
            finish(&x);
            break;
        }
        if (p->out.action == PIANO_CP_ACT_NONE && now >= s->state_ms + STOP_RESEND_MS) {
            p->out.action = PIANO_CP_ACT_STOP;
            p->out.stop_reason = s->exit_reason;
            s->state_ms = now;
        }
        break;
    case PIANO_CPS_LATCHED:
        if (drv->switching && drv->present) {p->out.action = PIANO_CP_ACT_STOP; p->out.stop_reason = PIANO_CPS_EXIT_STUCK;}
        break;
    default:
        set_state(&x, PIANO_CPS_IDLE);
        break;
    }

    p->state = s->state;
    p->owns_volt = (uint8_t)(in_session(s->state) || s->state == PIANO_CPS_STOPPING);
    if (p->owns_volt) {
        p->out.req_valid |= PIANO_CP_REQ_BUCK;
        p->out.buck_icl_ma = 500u;
        p->out.buck_fcc_ma = 500u;
    }
    if (!p->ibat_target_ma) {p->ibat_target_ma = s->ibat_target_ma; p->ibus_target_ma = s->ibus_target_ma;}
    p->exit_reason = s->exit_reason;
    p->exit_faults = s->exit_faults;
    p->latched = s->state == PIANO_CPS_LATCHED;
    p->sleep_ready = (uint8_t)(!p->owns_volt && !p->handback && !(drv->present && drv->switching) &&
                               p->out.action != PIANO_CP_ACT_START);
}
