// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano: the SC8541 2:1 charge pump (CP) on piano, the shared interface of the driver (this file's C++ part, ChargePump.cpp
// tests in tests/cp_driver_host.cpp), the CP session (the C part's piano_cp_in / piano_cp_out, ChargePumpSession.c) and the
// simulator (tests/cp_sim*). Specification and provenance: private/host-checks/20261123-charging/CP-SPEC.md (register map, the
// values Ubuntu v1.4.0's sc8541_charger.c / xiaomi_mca.c and the stock Android DT use, the safer pick where they differ).
//
// Hardware: SC8541 at I2C 0x6a on i2c@a94000 (qup1_se5, gpio52/gpio53), the bus pack A's bq27z561 (0x55) shares. Both are driven by
// PianoPower's one worker through the software I2C of Gauge.h (open drain, ~80 kHz); nothing else touches the pins.
//
// Safety model (every rule below is enforced in the driver, not left to the session):
//   - Arming: no SC8541 register is ever written unless the boot argument piano-power-cp=1 AND the per-boot root arm
//     (sysctl machdep.piano_power_cp_arm=1) are both set. Unarmed, the driver only reads (device id, status, CTRL2, ADC if on).
//   - Allowlist: a write must match one row of kCpAllowed (register, mask, value range); anything else is refused before the bus.
//   - Read-back: every write is followed by a read of the same register; a masked mismatch is a fault: CHG_EN off, CP lost for this
//     boot. The one exception is the stop itself (CHG_EN = 0 is written whatever the register reads), followed by a guard read
//     kCpGuardMs later that stops it again while it still reads set (the CHG_EN read-back latency).
//   - Watchdog: the SC8541's own watchdog (CTRL3 WD field) is enabled before CHG_EN and kicked every kCpKickMs from the worker; the
//     kick runs ahead of any gauge transaction on the shared bus when it is due. A host stall stops switching in hardware.
//   - Bus: a stuck bus (SDA held low) gets the 9-clock recovery of Gauge.h; if the bus does not come back, or kCpMaxBusErrors
//     transactions fail in a row, the CP is lost for this boot (no more writes; the session falls back to the buck).
#ifndef PIANO_POWER_CHARGE_PUMP_H
#define PIANO_POWER_CHARGE_PUMP_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- registers and fields (CP-SPEC.md section 2) ---- */
#define PIANO_CP_ADDRESS        0x6au
#define PIANO_CP_REG_BATOVP     0x00u   /* bit7 disable, [6:0] 3840 + 10 mV */
#define PIANO_CP_REG_BATOVP_ALM 0x01u
#define PIANO_CP_REG_BATOCP     0x02u   /* bit7 disable, [6:0] 100 mA steps */
#define PIANO_CP_REG_REVERSE    0x05u   /* never written */
#define PIANO_CP_REG_BUSOVP     0x06u   /* [7:0] 7000 + 50 mV */
#define PIANO_CP_REG_BUSOVP_ALM 0x07u
#define PIANO_CP_REG_BUSOCP     0x08u   /* bit7 disable, [4:0] 1000 + 250 mA */
#define PIANO_CP_REG_TEMP_CTRL  0x0au   /* bit7 TSHUT disable (we keep 0: thermal shutdown on) */
#define PIANO_CP_REG_USBOVP     0x0eu   /* VAC OVP, [7:5]; read and checked, never written */
#define PIANO_CP_REG_CTRL2      0x0fu   /* bit4 CHG_EN, bit3 EN_BYPASS */
#define PIANO_CP_REG_CTRL3      0x10u   /* [4:2] watchdog */
#define PIANO_CP_REG_PROT_CTRL  0x12u   /* never written */
#define PIANO_CP_REG_STAT_FIRST 0x13u   /* status 0x13..0x17, flags 0x18..0x1c (latched; read once per falling CHG_EN) */
#define PIANO_CP_STAT_BYTES     10u
#define PIANO_CP_REG_DEVICE     0x22u
#define PIANO_CP_DEVICE_ID      0x41u
#define PIANO_CP_REG_ADC_CTRL1  0x23u   /* bit7 ADC_EN; mask 0xc3 written as 0x80 (continuous) */
#define PIANO_CP_REG_ADC_CTRL2  0x24u   /* per-channel disable bits: 0 = all channels on */
#define PIANO_CP_REG_ADC_BASE   0x25u   /* channel n: MSB at base + 2n, LSB at base + 2n + 1 */

#define PIANO_CP_CHG_EN         0x10u
#define PIANO_CP_EN_BYPASS      0x08u
#define PIANO_CP_WD_MASK        0x1cu
#define PIANO_CP_WD_ACTIVE      0x10u   /* the watchdog value Ubuntu v1.4.0 programs (WD_TIMEOUT=2, WD_TIMEOUT_DIS=0) */
#define PIANO_CP_TSHUT_DISABLE  0x80u
#define PIANO_CP_ADC_EN         0x80u
#define PIANO_CP_USBOVP_MASK    0xe0u
#define PIANO_CP_USBOVP_STOCK   0x60u   /* the stock mode table's VAC OVP field (stock DT usb-ovp 14000 mV) */

/* ---- thresholds: the picked (safer) values of CP-SPEC.md section 3; never raised at run time ---- */
#define PIANO_CP_BATOVP_MV       4800u
#define PIANO_CP_BATOVP_ALM_MV   4600u
#define PIANO_CP_BATOCP_MA       8000u   /* the SC8541's own pack-side trip; the session's cell OCP (7000) acts first */
#define PIANO_CP_BUSOVP_MV      11000u   /* Ubuntu 11000 vs stock DT 13000: 11000 */
#define PIANO_CP_BUSOVP_ALM_MV  10500u
#define PIANO_CP_BUSOCP_MIN_MA   1000u
#define PIANO_CP_BUSOCP_MAX_MA   7000u
#define PIANO_CP_BUSOCP_START_MA 3500u   /* programmed at arm; the session then sets target + 1000, clamped */
#define PIANO_CP_KICK_MS         1000u   /* our kick period (Ubuntu's software watch: 3000) */
#define PIANO_CP_KICK_LATE_MS    2500u   /* a kick later than this is a fault (CHG_EN off) */
#define PIANO_CP_GUARD_MS         500u   /* CHG_EN read-back latency after a stop */
#define PIANO_CP_MAX_BUS_ERRORS     3u

/* encoders (the register value of a threshold) */
#define PIANO_CP_ENC_BATOVP(mv)  ((uint8_t)(((mv) - 3840u) / 10u))
#define PIANO_CP_ENC_BUSOVP(mv)  ((uint8_t)(((mv) - 7000u) / 50u))
#define PIANO_CP_ENC_BUSOCP(ma)  ((uint8_t)(((ma) - 1000u) / 250u))
#define PIANO_CP_ENC_BATOCP(ma)  ((uint8_t)((ma) / 100u))

/* ---- the ADC (stock scales: CP-SPEC.md section 2.3) ---- */
enum piano_cp_adc_channel {
    PIANO_CP_ADC_IBUS = 0, PIANO_CP_ADC_VBUS, PIANO_CP_ADC_VAC1, PIANO_CP_ADC_VAC2, PIANO_CP_ADC_VOUT, PIANO_CP_ADC_VBAT,
    PIANO_CP_ADC_IBAT, PIANO_CP_ADC_TSBUS, PIANO_CP_ADC_TSBAT, PIANO_CP_ADC_TDIE, PIANO_CP_ADC_CHANNELS
};
struct piano_cp_adc {
    uint32_t valid;          /* bit n: channel n read this cycle */
    int32_t ibus_ma, vbus_mv, vac1_mv, vac2_mv, vout_mv, vbat_mv, ibat_ma;
    int32_t tsbus_raw, tsbat_raw;
    int32_t tdie_dc;         /* 0.1 degC */
};

/* ---- faults the driver reports (bit mask) ---- */
enum piano_cp_fault {
    PIANO_CP_FAULT_NONE        = 0,
    PIANO_CP_FAULT_BUS         = 1u << 0,   /* NAK / timeout / stuck bus beyond recovery */
    PIANO_CP_FAULT_READBACK    = 1u << 1,   /* a written register read back different */
    PIANO_CP_FAULT_REFUSED     = 1u << 2,   /* a write outside kCpAllowed was asked for (a bug: CP lost) */
    PIANO_CP_FAULT_KICK_LATE   = 1u << 3,   /* the watchdog was not kicked within PIANO_CP_KICK_LATE_MS while switching */
    PIANO_CP_FAULT_ID          = 1u << 4,   /* the device id is not 0x41 */
    PIANO_CP_FAULT_STOP_STUCK  = 1u << 5,   /* CHG_EN still reads set after stop + guard (stopped again; reported) */
    PIANO_CP_FAULT_HW          = 1u << 6,   /* the SC8541's own status/flags latched a protection (decoded in stat[]) */
    PIANO_CP_FAULT_VACOVP      = 1u << 7,   /* VAC OVP field not the stock value: never enabled */
    PIANO_CP_FAULT_UNARMED     = 1u << 8,   /* an enable without both arms (refused; not a hardware fault) */
};

/* ---- one cycle, driver/system -> session ---- */
struct piano_cp_in {
    uint64_t now_ms;
    uint8_t  armed;              /* piano-power-cp=1 and machdep.piano_power_cp_arm=1 */
    uint8_t  present;            /* probe read the device id 0x41 */
    uint8_t  lost;               /* the driver gave up on the CP for this boot (no more writes) */
    uint8_t  switching;          /* CHG_EN reads set */
    uint32_t faults;             /* piano_cp_fault bits since the last cycle */
    uint8_t  stat[PIANO_CP_STAT_BYTES];   /* 0x13..0x1c as last read (valid when stat_valid) */
    uint8_t  stat_valid;
    struct piano_cp_adc adc;
    uint32_t busocp_ma;          /* as programmed */
    uint64_t last_kick_ms;
    /* MiPPS (fork D, xiaomi_mca's pd_auth): the source budget the session may use for the pump */
    uint8_t  mipps_done;         /* authentication finished (verified or not) */
    uint8_t  mipps_verified;
    uint32_t source_budget_ma;   /* 6000 when verified, 5000 otherwise (PIANO_CP_BUDGET_*); min'd with the APDO by the session */
};
#define PIANO_CP_BUDGET_VERIFIED_MA 6000u
#define PIANO_CP_BUDGET_STANDARD_MA 5000u

/* ---- one cycle, session -> driver and the buck/PPS requests (the session's own; ChargePolicy applies the buck/PPS parts) ---- */
enum piano_cp_action {
    PIANO_CP_ACT_NONE = 0,       /* leave the CP as it is (keep kicking while switching) */
    PIANO_CP_ACT_START,          /* arm the protections and set CHG_EN (driver refuses unless armed, present, not lost) */
    PIANO_CP_ACT_STOP,           /* CHG_EN off (and the guard), the session's exit */
    PIANO_CP_ACT_SET_BUSOCP,     /* reprogram BUSOCP to busocp_ma (clamped by the driver) */
};
struct piano_cp_out {
    enum piano_cp_action action;
    uint32_t busocp_ma;
    uint32_t stop_reason;        /* the session's reason code, logged with the stop */
    /* requests the session asks ChargePolicy to make (valid bits) */
    uint32_t req_valid;          /* PIANO_CP_REQ_* */
    uint32_t pps_mv, pps_ma;     /* PPS request word (0x21017) */
    uint32_t buck_icl_ma, buck_fcc_ma;   /* the buck's share while the pump runs (held at 500 in the opening) */
};
#define PIANO_CP_REQ_PPS  1u
#define PIANO_CP_REQ_BUCK 2u

#ifdef __cplusplus
}  /* extern "C" */

#include "Gauge.h"

namespace pianopower {

// One allowed write: (value & mask) must lie in [lo, hi] (equal for fixed values), the bits outside mask keep what the register reads.
struct CpAllowedWrite { uint8_t reg, mask, lo, hi; };
constexpr CpAllowedWrite kCpAllowed[] = {
  {PIANO_CP_REG_BATOVP,     0xff, PIANO_CP_ENC_BATOVP(PIANO_CP_BATOVP_MV),     PIANO_CP_ENC_BATOVP(PIANO_CP_BATOVP_MV)},
  {PIANO_CP_REG_BATOVP_ALM, 0xff, PIANO_CP_ENC_BATOVP(PIANO_CP_BATOVP_ALM_MV), PIANO_CP_ENC_BATOVP(PIANO_CP_BATOVP_ALM_MV)},
  {PIANO_CP_REG_BATOCP,     0xff, PIANO_CP_ENC_BATOCP(PIANO_CP_BATOCP_MA),     PIANO_CP_ENC_BATOCP(PIANO_CP_BATOCP_MA)},
  {PIANO_CP_REG_BUSOVP,     0xff, PIANO_CP_ENC_BUSOVP(PIANO_CP_BUSOVP_MV),     PIANO_CP_ENC_BUSOVP(PIANO_CP_BUSOVP_MV)},
  {PIANO_CP_REG_BUSOVP_ALM, 0xff, PIANO_CP_ENC_BUSOVP(PIANO_CP_BUSOVP_ALM_MV), PIANO_CP_ENC_BUSOVP(PIANO_CP_BUSOVP_ALM_MV)},
  {PIANO_CP_REG_BUSOCP,     0x9f, PIANO_CP_ENC_BUSOCP(PIANO_CP_BUSOCP_MIN_MA), PIANO_CP_ENC_BUSOCP(PIANO_CP_BUSOCP_MAX_MA)},
  {PIANO_CP_REG_TEMP_CTRL,  PIANO_CP_TSHUT_DISABLE, 0, 0},
  {PIANO_CP_REG_CTRL2,      PIANO_CP_EN_BYPASS, 0, 0},
  {PIANO_CP_REG_CTRL2,      PIANO_CP_CHG_EN, 0, PIANO_CP_CHG_EN},
  {PIANO_CP_REG_CTRL3,      PIANO_CP_WD_MASK, PIANO_CP_WD_ACTIVE, PIANO_CP_WD_ACTIVE},   // the release to the saved value: see Sc8541
  {PIANO_CP_REG_ADC_CTRL2,  0xff, 0, 0},
  {PIANO_CP_REG_ADC_CTRL1,  0xc3, PIANO_CP_ADC_EN, PIANO_CP_ADC_EN},
};
static_assert(PIANO_CP_ENC_BATOVP(4800) == 0x60 && PIANO_CP_ENC_BATOVP(4600) == 0x4c, "BATOVP encoding (stock table 0x60/0x4c)");
static_assert(PIANO_CP_ENC_BUSOVP(11000) == 80 && PIANO_CP_ENC_BUSOVP(13000) == 0x78, "BUSOVP encoding (stock table 0x78 = 13000)");
static_assert(PIANO_CP_ENC_BUSOCP(7000) == 0x18 && PIANO_CP_ENC_BATOCP(8000) == 0x50, "BUSOCP/BATOCP encoding (stock table)");

// Whether a write of value under mask to reg is in kCpAllowed (savedWd: the CTRL3 field read before the driver took the watchdog,
// the only other CTRL3 value it may write back, after CHG_EN reads clear).
bool cpWriteAllowed(uint8_t reg, uint8_t mask, uint8_t value, int savedWd /* -1: none */);

// The arming state (both needed for any write).
struct CpArm {
  bool bootArg = false;    // piano-power-cp=1
  bool sysctl = false;     // machdep.piano_power_cp_arm=1 (root, this boot)
  bool armed() const { return bootArg && sysctl; }
};

// The driver. Not thread safe by itself: PianoPower's worker is its only caller and owns the bus (Gauge.h I2cBus) while it runs.
class Sc8541 {
 public:
  struct Clock { void *ctx; uint64_t (*nowMs)(void *ctx); };
  explicit Sc8541(Clock clock) : clock_(clock) {}

  CpArm arm;                                     // set by PianoPower from the boot argument and the sysctl

  // Read only: the device id (0x22 == 0x41) and the VAC OVP field (must be the stock value before any enable).
  bool probe(I2cBus &bus);
  // Read only: CTRL2 (switching), status/flags 0x13..0x1c, the ADC channels (when ADC_EN reads set).
  bool readCtrl2(I2cBus &bus, uint8_t *ctrl2);
  bool readStatus(I2cBus &bus, uint8_t stat[PIANO_CP_STAT_BYTES]);
  bool readAdc(I2cBus &bus, piano_cp_adc *adc);

  // Writes (armed, present, not lost; every one allowlisted and read back):
  bool start(I2cBus &bus, uint32_t busocpMa);    // the watchdog, the protections, the ADC, then CHG_EN
  bool setBusOcp(I2cBus &bus, uint32_t ma);      // clamped to [1000, 7000]
  bool kick(I2cBus &bus);                        // the CTRL3 watchdog write (forced even if unchanged), read back
  void stop(I2cBus &bus, uint32_t reason);       // CHG_EN = 0 whatever it reads; the guard follows
  bool guard(I2cBus &bus);                       // after PIANO_CP_GUARD_MS: still set -> stop again; clear -> release the watchdog

  // Scheduling (the worker's deadlines): the kick runs before any other transaction on the shared bus when due.
  bool kickDue(uint64_t now) const { return switching_ && now >= lastKickMs_ + PIANO_CP_KICK_MS; }
  bool guardDue(uint64_t now) const { return guardAtMs_ && now >= guardAtMs_; }
  uint64_t nextDeadline() const;                 // the earliest of the kick and the guard (0: none)
  // Called by the shared-bus hook before every gauge transaction on bus A (I2cBus::setHook).
  void beforeBusTransaction(I2cBus &bus);

  // State for the session's piano_cp_in.
  bool present() const { return present_; }
  bool lost() const { return lost_; }
  bool switching() const { return switching_; }
  uint32_t takeFaults() { const uint32_t f = faults_; faults_ = 0; return f; }
  uint32_t busOcpMa() const { return busocpMa_; }
  uint64_t lastKickMs() const { return lastKickMs_; }
  void fill(piano_cp_in *in) const;

 private:
  Clock clock_;
  bool present_ = false, lost_ = false, switching_ = false, wdOwned_ = false, inHook_ = false;
  int wdSaved_ = -1;
  uint32_t faults_ = 0, busErrors_ = 0, busocpMa_ = 0, stopReason_ = 0;
  uint64_t lastKickMs_ = 0, guardAtMs_ = 0;
  uint8_t ctrl2Shadow_ = 0;          // sim (fork B): CTRL2 verified at the start, without CHG_EN / EN_BYPASS
  bool ctrl2ShadowValid_ = false;
  uint8_t stat_[PIANO_CP_STAT_BYTES] = {};   // 0x13..0x1c as read at the last falling CHG_EN
  bool statValid_ = false;

  bool writeChecked(I2cBus &bus, uint8_t reg, uint8_t mask, uint8_t value);
  bool readReg(I2cBus &bus, uint8_t reg, uint8_t *value);
  void busError();
  void fail(uint32_t fault);
};

}  // namespace pianopower
#endif  /* __cplusplus */
#endif  /* PIANO_POWER_CHARGE_PUMP_H */
