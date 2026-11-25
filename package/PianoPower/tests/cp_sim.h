// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// T-CP fork B (charge/cp-sim): the host plant for the SC8541 2:1 charge pump session. Models: the source (PPS APDOs with current-limit
// mode, fixed PD, QC3 / QC3-B / QC3.5 with 0x201d pulses, a MiPPS adapter answering the VDM commands), the cable and connector, the
// SC8541 register file (protections, status/flags 0x13..0x1c, ADC, watchdog, CHG_EN latency, the never-written registers), two packs
// (OCV curve, internal resistance, heating), the buck share, the charger firmware's MCA properties, and the shared software I2C bus of
// pack A's gauge (0x55) and the pump (0x6a) at two levels: transactions (Bus, the reference controller) and lines (I2cIo, the real
// driver of fork A through Gauge.h's I2cBus). Faults are injected on both. An independent checker reads the plant's ground truth only.
// Thresholds come from ChargePump.h (fork A's picks) and ChargeMipps.h (fork D's protocol); the bit layout of status/flags 0x13..0x1c
// is this model's (CP-SPEC.md leaves it to the datasheet): flags byte 0x18 bit 0 BUSOVP, 1 BUSOCP, 2 BATOVP, 3 BATOCP, 4 TSHUT, 5 WD,
// 6 VBUS low, 7 VBUS high (read clears).
#pragma once
#include "ChargePump.h"
#include "ChargeMipps.h"
#include "Gauge.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace cpsim {

// ---------------------------------------------------------------- SHA-256 and HMAC-SHA256 (FIPS 180-4 / RFC 2104), the model's own
struct Sha256 {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf[64] = {};
  uint64_t bits = 0;
  unsigned n = 0;
  static uint32_t rotr(uint32_t x, unsigned r) {return x >> r | x << (32 - r);}
  void block(const uint8_t *p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (unsigned i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (unsigned i = 0; i < 64; ++i) {
      const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const uint8_t *p, size_t len) {
    for (size_t i = 0; i < len; ++i) {buf[n++] = p[i]; bits += 8; if (n == 64) {block(buf); n = 0;}}
  }
  void final(uint8_t out[32]) {
    const uint64_t total = bits;
    const uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (n != 56) update(&zero, 1);
    uint8_t l[8];
    for (unsigned i = 0; i < 8; ++i) l[i] = uint8_t(total >> (56 - 8 * i));
    update(l, 8);
    for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
  }
};
inline void hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]) {
  uint8_t k[64] = {}, ipad[64], opad[64], inner[32];
  if (klen > 64) {Sha256 s; s.update(key, klen); s.final(k);} else std::memcpy(k, key, klen);
  for (unsigned i = 0; i < 64; ++i) {ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c;}
  Sha256 a; a.update(ipad, 64); a.update(msg, mlen); a.final(inner);
  Sha256 b; b.update(opad, 64); b.update(inner, 32); b.final(out);
}
inline void mipps_answer(const uint8_t key[32], const uint8_t challenge[16], uint32_t id, uint8_t out[16]) {
  uint8_t msg[20], mac[32];   // challenge || the adapter ID as a big-endian u32 (fork D's reading of the protocol)
  std::memcpy(msg, challenge, 16);
  msg[16] = uint8_t(id >> 24); msg[17] = uint8_t(id >> 16); msg[18] = uint8_t(id >> 8); msg[19] = uint8_t(id);
  hmac_sha256(key, 32, msg, sizeof msg, mac);
  std::memcpy(out, mac, 16);
}

// ---------------------------------------------------------------- the chip's registers and this model's flag layout
namespace reg {
constexpr uint8_t BATOVP = PIANO_CP_REG_BATOVP, BATOVP_ALM = PIANO_CP_REG_BATOVP_ALM, BATOCP = PIANO_CP_REG_BATOCP,
                  REVERSE = PIANO_CP_REG_REVERSE, BUSOVP = PIANO_CP_REG_BUSOVP, BUSOVP_ALM = PIANO_CP_REG_BUSOVP_ALM,
                  BUSOCP = PIANO_CP_REG_BUSOCP, TEMP_CTRL = PIANO_CP_REG_TEMP_CTRL, USBOVP = PIANO_CP_REG_USBOVP, CTRL2 = PIANO_CP_REG_CTRL2,
                  CTRL3 = PIANO_CP_REG_CTRL3, PROT = PIANO_CP_REG_PROT_CTRL, STAT = PIANO_CP_REG_STAT_FIRST, FLAG = 0x18,
                  DEVICE = PIANO_CP_REG_DEVICE, ADC1 = PIANO_CP_REG_ADC_CTRL1, ADC2 = PIANO_CP_REG_ADC_CTRL2, ADC = PIANO_CP_REG_ADC_BASE;
constexpr uint8_t F_BUSOVP = 1, F_BUSOCP = 2, F_BATOVP = 4, F_BATOCP = 8, F_TSHUT = 16, F_WD = 32, F_VBUS_LO = 64, F_VBUS_HI = 128;
constexpr uint32_t adc_lsb[PIANO_CP_ADC_CHANNELS] = {2500, 3750, 5000, 5000, 1250, 1250, 3125, 1, 1, 5};   // uA, uV x5, uA, raw x2, 0.1 C
inline uint32_t batovp_mv(uint8_t v) {return 3840 + 10u * (v & 0x7f);}
inline uint32_t busovp_mv(uint8_t v) {return 7000 + 50u * v;}
inline uint32_t busocp_ma(uint8_t v) {return 1000 + 250u * (v & 0x1f);}
inline uint32_t batocp_ma(uint8_t v) {return 100u * (v & 0x7f);}
}
constexpr uint32_t kWdMs = 5000;        // the model's hardware watchdog behind CTRL3 0x10 (CP-SPEC.md: the real period is not known)
constexpr uint32_t kEnLatencyMs = 200;  // CHG_EN takes effect, and reads back set, this long after it is written
constexpr int32_t kTshutC10 = 1500;

// The checker's limits: fork A's picks (ChargePump.h) and the session rules of CP-SPEC.md / STACK.md.
struct Limits {
  uint32_t cell_ocp_ma = 7000, battery_ma = 12000, busovp_mv = PIANO_CP_BUSOVP_MV, busocp_max_ma = PIANO_CP_BUSOCP_MAX_MA,
           start_vbat_max_mv = 4473, start_vbat_min_mv = 3450, stage_window_mv = 1200, tdie_exit_c10 = 1250, std_budget_ma = 5000,
           verified_budget_ma = 6000, power_mw = 67000, soc_max = 91, conn_max_c10 = 500, cell_min_c10 = 80, cell_max_c10 = 550;
};

// ---------------------------------------------------------------- the MCA properties the session and MiPPS use
namespace prop {
constexpr uint32_t VBUS_UV = PIANO_MIPPS_PROP_VBUS, REAL_TYPE = 0x2000c, ICL = 0x2008, FCC = 0x2005, FV = 0x2007, CHG_STOP = 0x2003,
                   QC_MV = 0x2011, QC_PULSE = 0x201d, VERIFY = PIANO_MIPPS_PROP_VERIFY_PROCESS, VDM = PIANO_MIPPS_PROP_VDM_BASE,
                   AUTH = PIANO_MIPPS_PROP_AUTH, ADAPTER_ID = PIANO_MIPPS_PROP_ADAPTER_ID, PD_VERIFIED = PIANO_MIPPS_PROP_PD_VERIFIED,
                   UVDM = PIANO_MIPPS_PROP_UVDM_STATE, PPS_WORD = 0x21017, FIXED_MV = 0x21018;
constexpr int32_t T_QC2 = 5, T_QC3 = 6, T_QC3B = 7, T_QC35 = 8, T_PD = 10, T_PPS = 11, T_PD_VERIFIED = 12;
inline uint32_t pps_word(uint32_t mv, uint32_t ma) {return (mv / 20) << 16 | (ma / 50);}
}

// ---------------------------------------------------------------- injected faults
enum class BusFault { none, nak, timeout, stuck, bitflip, latency };
struct BusFaultWindow {BusFault kind = BusFault::none; uint64_t from_ms = 0, to_ms = 0; double prob = 1.0; uint32_t extra_us = 0; uint8_t addr = 0;};
enum : int { kOk = 0, kNak = -5, kTimeout = -60, kStuck = -16 };

enum class SrcKind { none, pps, fixed_pd, qc2, qc3, qc3b, qc35 };
enum class Mipps { none, ok, wrong_response, timeout_cmd, stall, id_zero };
struct SourceSpec {
  SrcKind kind = SrcKind::pps;
  uint32_t apdo_min_mv = 3300, apdo_max_mv = 11000, apdo_max_ma = 5000;   // the original adapter: 6100 (read as such)
  uint32_t fixed9_ma = 3000, qc_max_ma = 3000, qc_min_mv = 3600, qc_max_mv = 12000;
  double cable_mohm = 150;
  uint32_t cable_rating_ma = 6000;
  bool pdo_above_5v = true;
  Mipps mipps = Mipps::none;
  unsigned mipps_fault_cmd = 5;
  uint32_t mipps_latency_ms = 120;
  uint16_t adapter_id = 0x2b41;
  uint8_t seed[16] = {}, key[32] = {};        // the adapter's own seed/key pair (a TEST key)
};

struct Pack {
  double soc = 0.4, cap_mah = 5025, r_ohm = 0.025, temp_c = 28;
  static double ocv_mv(double soc) {
    static const double s[] = {0, .05, .10, .20, .40, .60, .80, .90, 1.0};
    static const double v[] = {3300, 3550, 3650, 3700, 3780, 3900, 4050, 4150, 4400};
    soc = std::clamp(soc, 0.0, 1.0);
    for (unsigned i = 1; i < 9; ++i) if (soc <= s[i]) return v[i - 1] + (v[i] - v[i - 1]) * (soc - s[i - 1]) / (s[i] - s[i - 1]);
    return v[8];
  }
};

// ---------------------------------------------------------------- what a controller sees and calls
struct GaugeView {bool valid = false; int32_t mv = 0, ma = 0, soc = 0, temp_c10 = 0;};
struct Sensors {
  uint64_t now_ms = 0;
  bool attached = false;
  int32_t real_type = 0;
  uint32_t apdo_min_mv = 0, apdo_max_mv = 0, apdo_max_ma = 0;
  bool pdo_above_5v = false;
  GaugeView g[2];
  int32_t conn_c10 = 250, conn2_c10 = 250, charger_c10 = 300;
  bool sleep_request = false;
  bool health_hold = false;     // the existing 80/75 policy's decision (unchanged)
  uint32_t attach_seq = 0;
};
struct Bus {   // transaction level (the reference controller)
  virtual ~Bus() = default;
  virtual int read(uint8_t addr, uint8_t r, uint8_t *b, unsigned n) = 0;
  virtual int write(uint8_t addr, uint8_t r, uint8_t v) = 0;
  virtual int recover() = 0;
};
struct Firmware {   // the MCA owner's properties: 4 bytes (one u32 LE) or 16 bytes (four u32 LE)
  virtual ~Firmware() = default;
  virtual int write(uint32_t p, const uint8_t *b, unsigned n) = 0;
  virtual int read(uint32_t p, uint8_t *b, unsigned n) = 0;
  int set(uint32_t p, uint32_t v) {uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; return write(p, b, 4);}
  int get(uint32_t p, uint32_t *v) {uint8_t b[4]; const int rc = read(p, b, 4); if (!rc) *v = b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24; return rc;}
};
struct Controller {
  virtual ~Controller() = default;
  virtual void tick(const Sensors &s, Bus &bus, Firmware &fw) = 0;
  virtual bool sleep_ready() = 0;
  virtual uint32_t period_ms() const {return 100;}
  virtual const char *name() const = 0;
};

struct Config {
  uint32_t dt_ms = 10;
  double sys_mw = 4000, ambient_c = 25;
  bool health_policy = true;      // 80/75 (owner rule); off only in the SOC-gate scenario
  uint32_t power_cap_mw = 67000;
  uint8_t host_seed[16] = {}, host_key[32] = {};   // the TEST key file the host loaded (count 1); all zero = no key
  bool host_has_key = false;
};

// ================================================================ the plant
struct Plant final : Bus, Firmware {
  Config cfg;
  SourceSpec src;
  Limits lim;
  Pack pk[2];
  uint64_t now = 0;
  uint64_t bus_us = 0;            // time the bus took (line level); the harness turns it into a late tick
  std::mt19937_64 rng{7};
  // source
  bool attached = false;
  uint32_t attach_seq = 0;
  uint64_t reset_until = 0;
  enum class Mode { off, fixed5, fixed9, pps, qc } mode = Mode::off;
  uint32_t set_mv = 0, lim_ma = 0, pend_mv = 0, pend_ma = 0;
  Mode pend_mode = Mode::off;
  uint64_t pend_at = 0;
  bool pend = false;
  double vbus_mv = 0, ibus_ma = 0, cp_ibus_ma = 0, iout_ma = 0, vnode_mv = 0, buck_out_ma = 0, buck_in_ma = 0, ipk_ma[2] = {0, 0};
  bool cl_mode = false;
  uint64_t ocp_since = 0;
  double overshoot_mv = 0, sag_mv = 0;
  uint32_t apdo_override_ma = 0;
  unsigned source_resets = 0;
  // buck as the firmware holds it
  uint32_t icl_ma = 1500, fcc_ma = 3000, fv_mv = 4400;
  bool chg_stop = false;
  // the chip
  uint8_t r[0x42] = {};
  bool active = false, en_req = false;
  uint64_t en_at = 0, wd_kick = 0, en_written_at = 0;
  unsigned unstepped_starts = 0;   // the pump started (or resumed) while a source outside PPS/QC stepping powered the bus
  uint32_t wd_ms = kWdMs, en_latency_ms = kEnLatencyMs;   // sensitivity: the unknown hardware watchdog period and CHG_EN delay
  bool auto_inc = true;                                       // sensitivity: block reads without auto-increment
  uint8_t flags = 0;
  double tdie_c = 30, conn_c = 25, conn2_c = 25, charger_c = 30, extra_die_w = 0, extra_conn_w = 0, r_path = 0.004;
  bool last_write_corrupt = false, keep_en = false;
  std::array<uint8_t, 0x42> ignore_writes{};
  std::vector<std::string> write_violations;
  unsigned cp_writes = 0;
  // bus
  std::vector<BusFaultWindow> faults;
  bool stuck = false;
  unsigned cp_txn = 0, cp_err = 0;
  // gauges
  GaugeView gv[2];
  unsigned gauge_fail_run[2] = {0, 0};
  bool gauge_forced_invalid[2] = {false, false};
  bool hold = false;
  // firmware state
  int32_t real_type = 0;
  uint32_t verify = 0, pd_verified = 0, uvdm_answered = 0, adapter_sel = 0;
  bool adapter_has_key = false, mipps_legit = false, cmd8_seen = false, stalled = false;
  uint8_t challenge[16] = {}, answer[16] = {};
  uint64_t uvdm_due = 0;
  unsigned uvdm_pending = 0;
  uint64_t verify_since = 0;
  unsigned verify_sets = 0, voltage_while_verify = 0;
  uint32_t qc_mv_req = 0;
  // statistics
  uint64_t cp_on_ms = 0;
  double max_ibus = 0, max_ibat = 0, max_pack = 0, max_tdie = 0, max_power = 0, mah_in = 0;

  Plant() {
    r[reg::DEVICE] = PIANO_CP_DEVICE_ID;
    r[reg::BATOVP] = 0x80; r[reg::BUSOCP] = 0x80; r[reg::BATOCP] = 0x80;   // reset state: protections off
    r[reg::BUSOVP] = 0xff; r[reg::TEMP_CTRL] = PIANO_CP_TSHUT_DISABLE; r[reg::CTRL3] = 0;
    r[reg::USBOVP] = PIANO_CP_USBOVP_STOCK;                                   // the stock VAC OVP field, never written
  }
  bool qc_kind() const {return src.kind == SrcKind::qc2 || src.kind == SrcKind::qc3 || src.kind == SrcKind::qc3b || src.kind == SrcKind::qc35;}
  bool qc_steppable() const {return src.kind == SrcKind::qc3 || src.kind == SrcKind::qc3b || src.kind == SrcKind::qc35;}
  double qc_step_mv() const {return src.kind == SrcKind::qc3 ? 200 : 20;}
  uint32_t apdo_ma() const {return apdo_override_ma ? std::min(apdo_override_ma, src.apdo_max_ma) : src.apdo_max_ma;}
  int32_t type_of() const {
    switch (src.kind) {
      case SrcKind::pps: return prop::T_PPS; case SrcKind::fixed_pd: return prop::T_PD; case SrcKind::qc2: return prop::T_QC2;
      case SrcKind::qc3: return prop::T_QC3; case SrcKind::qc3b: return prop::T_QC3B; case SrcKind::qc35: return prop::T_QC35;
      default: return 0;
    }
  }
  void attach() {
    attached = true; ++attach_seq; mode = Mode::fixed5; set_mv = 5000; lim_ma = 3000; pend = false; real_type = type_of();
    verify = pd_verified = uvdm_answered = 0; uvdm_pending = 0; stalled = false; mipps_legit = false; qc_mv_req = 0;
    adapter_has_key = false;
  }
  void detach() {attached = false; mode = Mode::off; set_mv = 0; real_type = 0; pend = false; uvdm_pending = 0;}
  void hard_reset(uint64_t ms) {reset_until = now + ms; mode = Mode::off; pend = false; ++source_resets; uvdm_pending = 0;}
  bool powered() const {return attached && !reset_until && mode != Mode::off;}
  // ------------------------------------------------------------ Firmware
  void request(Mode m, uint32_t mv, uint32_t ma, uint32_t lat) {pend = true; pend_mode = m; pend_mv = mv; pend_ma = ma; pend_at = now + lat;}
  bool voltage_prop(uint32_t p) const {return p == prop::PPS_WORD || p == prop::FIXED_MV || p == prop::QC_MV || p == prop::QC_PULSE;}
  int write(uint32_t p, const uint8_t *b, unsigned n) override {
    const uint32_t v = n >= 4 ? (b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24) : 0;
    if (stalled && p >= prop::VDM && p < prop::VDM + 8) return -16;
    if (voltage_prop(p) && verify) {++voltage_while_verify; return -16;}   // the firmware refuses a voltage step during MiPPS
    if (voltage_prop(p) && !powered()) return -19;
    switch (p) {
    case prop::ICL: icl_ma = std::min<uint32_t>(v, 3000); return 0;
    case prop::FCC: fcc_ma = std::min<uint32_t>(v, 8000); return 0;
    case prop::FV: if (v != 4400 && v != 4060) return -22; fv_mv = v; return 0;
    case prop::CHG_STOP: chg_stop = v == 0; return 0;
    case prop::FIXED_MV:
      if (v == 5000) {request(Mode::fixed5, 5000, 3000, 80); return 0;}
      if (v == 9000 && (src.kind == SrcKind::pps || src.kind == SrcKind::fixed_pd)) {request(Mode::fixed9, 9000, src.fixed9_ma, 120); return 0;}
      return -22;
    case prop::PPS_WORD: {
      if (src.kind != SrcKind::pps) return -22;
      const uint32_t mv = (v >> 16) * 20, ma = (v & 0xffff) * 50;
      if (mv < src.apdo_min_mv || mv > src.apdo_max_mv || !ma || ma > apdo_ma()) return -22;
      request(Mode::pps, mv, ma, mode == Mode::pps ? 60 : 150);
      return 0;
    }
    case prop::QC_MV:
      if (!qc_kind()) return -22;
      if (v == 5000) {qc_mv_req = 5000; request(Mode::fixed5, 5000, src.qc_max_ma, 100); return 0;}
      if (v == 9000) {qc_mv_req = 9000; request(Mode::qc, 9000, src.qc_max_ma, 200); return 0;}
      return -22;
    case prop::QC_PULSE: {
      if (!qc_steppable() || mode != Mode::qc) return -22;
      const int32_t k = int32_t(v);
      if (!k || k < -50 || k > 50) return -22;
      const double base = pend && pend_mode == Mode::qc ? double(pend_mv) : double(set_mv);   // pulses add to a step in flight
      const double mv = std::clamp(base + k * qc_step_mv(), double(src.qc_min_mv), double(src.qc_max_mv));
      const uint64_t keep = pend && pend_mode == Mode::qc ? pend_at : 0;
      request(Mode::qc, uint32_t(mv), src.qc_max_ma, 40 + 10u * unsigned(std::abs(k)));
      if (keep) pend_at = std::min(pend_at, keep);
      return 0;
    }
    case prop::VERIFY:
      if (wlog) std::printf("      VERIFY <- %u at %llu (attached %d)\n", v, (unsigned long long)now, attached);
      if (v && !verify) {++verify_sets; verify_since = now;}
      verify = v ? 1 : 0;
      return 0;
    case prop::PD_VERIFIED: pd_verified = v ? 1 : 0; return 0;
    default:
      if (p >= prop::VDM && p < prop::VDM + 8) return vdm(p - prop::VDM + 1, b, n, v);
      return -22;
    }
  }
  int vdm(unsigned cmd, const uint8_t *b, unsigned n, uint32_t v) {
    if (!verify || !powered()) return -16;
    if (cmd == 8) {cmd8_seen = true; stalled = true; return 0;}
    uvdm_pending = cmd;
    const bool never = src.mipps == Mipps::none || (src.mipps == Mipps::timeout_cmd && cmd == src.mipps_fault_cmd) ||
                       (src.mipps == Mipps::stall && cmd >= 3);
    uvdm_due = never ? UINT64_MAX : now + src.mipps_latency_ms;
    if (cmd == 4) adapter_has_key = n == 16 && std::memcmp(b, src.seed, 16) == 0;
    if (cmd == 5) {
      if (n != 16) {uvdm_due = UINT64_MAX; return 0;}
      std::memcpy(challenge, b, 16);
      if (adapter_has_key) mipps_answer(src.key, challenge, src.adapter_id, answer);
      else for (auto &x : answer) x = uint8_t(rng());
      if (src.mipps == Mipps::wrong_response) answer[5] ^= 0xa5;
    }
    if (cmd == 6 && v == 1 && src.mipps == Mipps::ok && adapter_has_key) mipps_legit = true;
    return 0;
  }
  int read(uint32_t p, uint8_t *b, unsigned n) override {
    auto put = [&](uint32_t v) {if (n != 4) return -22; b[0] = uint8_t(v); b[1] = uint8_t(v >> 8); b[2] = uint8_t(v >> 16); b[3] = uint8_t(v >> 24); return 0;};
    switch (p) {
    case prop::VBUS_UV: return put(uint32_t(std::max(0.0, vbus_mv) * 1000));
    case prop::REAL_TYPE: return put(uint32_t(powered() ? real_type : 0));
    case prop::UVDM: return put(uvdm_answered);
    case prop::VDM: return put(uvdm_answered >= 1 && src.mipps != Mipps::none ? 0x0103u : 0u);   // the version after command 1
    case prop::AUTH: if (n != 16 || uvdm_answered < 5) return -11; std::memcpy(b, answer, 16); return 0;
    case prop::ADAPTER_ID: return put(src.mipps == Mipps::id_zero ? 0u : src.adapter_id);
    case prop::VERIFY: return put(verify);
    case prop::PD_VERIFIED: return put(pd_verified);
    case prop::ICL: return put(icl_ma);
    case prop::FCC: return put(fcc_ma);
    default: return -22;
    }
  }
  // ------------------------------------------------------------ the chip (shared by both bus levels)
  bool wd_on() const {return (r[reg::CTRL3] & PIANO_CP_WD_MASK) == PIANO_CP_WD_ACTIVE;}
  bool armed() const {
    return !(r[reg::BATOVP] & 0x80) && reg::batovp_mv(r[reg::BATOVP]) <= PIANO_CP_BATOVP_MV && reg::busovp_mv(r[reg::BUSOVP]) <= lim.busovp_mv &&
           !(r[reg::BUSOCP] & 0x80) && reg::busocp_ma(r[reg::BUSOCP]) <= lim.busocp_max_ma && !(r[reg::BATOCP] & 0x80) &&
           reg::batocp_ma(r[reg::BATOCP]) <= PIANO_CP_BATOCP_MA && !(r[reg::TEMP_CTRL] & PIANO_CP_TSHUT_DISABLE) && wd_on() &&
           !(r[reg::CTRL2] & PIANO_CP_EN_BYPASS) && (r[reg::USBOVP] & PIANO_CP_USBOVP_MASK) == PIANO_CP_USBOVP_STOCK;
  }
  std::string armed_why() const {
    char b[160]; std::snprintf(b, sizeof b, "BATOVP %02x BUSOVP %02x BUSOCP %02x BATOCP %02x TEMP %02x CTRL3 %02x CTRL2 %02x USBOVP %02x", r[reg::BATOVP], r[reg::BUSOVP], r[reg::BUSOCP], r[reg::BATOCP], r[reg::TEMP_CTRL], r[reg::CTRL3], r[reg::CTRL2], r[reg::USBOVP]);
    return b;
  }
  void trip(uint8_t f) {flags |= f; active = false; en_req = false; r[reg::CTRL2] &= uint8_t(~PIANO_CP_CHG_EN);}
  uint8_t chip_read(uint8_t a) {
    if (a >= sizeof r) return 0;
    if (a == reg::CTRL2) return uint8_t((r[a] & ~PIANO_CP_CHG_EN) | ((r[a] & PIANO_CP_CHG_EN) && now >= en_written_at + en_latency_ms ? PIANO_CP_CHG_EN : 0));   // the bit as written, after its latency
    if (a == reg::FLAG) {const uint8_t v = flags; flags = 0; return v;}
    if (a == reg::STAT) return active ? 1 : 0;
    return r[a];
  }
  bool wlog = false;
  void chip_write(uint8_t a, uint8_t v) {
    ++cp_writes;
    if (wlog) std::printf("      w %llu 0x%02x <- 0x%02x (active %d en_req %d)\n", (unsigned long long)now, a, v, active, en_req);
    if (a >= sizeof r) return;
    // every write must be one of fork A's allowed rows (the checker's own copy of the rule)
    bool ok = false;
    uint8_t masks = 0;
    for (const auto &w : pianopower::kCpAllowed) if (w.reg == a) masks |= w.mask;
    for (const auto &w : pianopower::kCpAllowed)
      if (w.reg == a) {
        const uint8_t changed = uint8_t(v ^ r[a]);
        // bits outside this row keep what the register holds, or go back to 0 (a stop restoring a field a bus fault set)
        const uint8_t out = uint8_t(changed & ~w.mask);
        if ((out & ~masks) == (out & ~masks & ~v) && (out & masks & ~w.mask) == (out & masks & ~w.mask & ~v) &&
            (v & w.mask) >= w.lo && (v & w.mask) <= w.hi) ok = true;
      }
    if (a == reg::CTRL3 && !active && (((v ^ r[a]) & ~PIANO_CP_WD_MASK) & v) == 0) ok = true;   // the release of the watchdog field to what it held before (A's savedWd), never while switching
    if (!ok && !last_write_corrupt) {char t[64]; std::snprintf(t, sizeof t, "write 0x%02x <- 0x%02x outside kCpAllowed", a, v); write_violations.push_back(t);}
    if (a == reg::DEVICE || a == reg::FLAG || a == reg::STAT || (a >= reg::ADC && a < reg::ADC + 2 * PIANO_CP_ADC_CHANNELS)) return;
    if (ignore_writes[a]) return;
    r[a] = v;
    if (a == reg::CTRL3) wd_kick = now;
    if (a == reg::CTRL2) {
      if (v & PIANO_CP_CHG_EN) {if (!en_req && !active) {en_req = true; en_at = now + en_latency_ms; en_written_at = now;}}
      else {en_req = false; active = false; keep_en = false;}
    }
  }
  // ------------------------------------------------------------ the bus faults
  BusFault fault_now(uint8_t addr) {
    BusFault got = BusFault::none;
    for (auto &f : faults) {
      if (now < f.from_ms || now >= f.to_ms || (f.addr && f.addr != addr)) continue;
      if (f.kind == BusFault::stuck) return BusFault::stuck;
      if (std::uniform_real_distribution<double>(0, 1)(rng) < f.prob) {if (f.kind == BusFault::latency) bus_us += f.extra_us; got = f.kind;}
    }
    return got;
  }
  bool bus_fault_recent() const {   // a bus fault window now or within the watchdog period: only the watchdog may be able to stop
    for (auto &f : faults) if (f.kind != BusFault::latency && now >= f.from_ms && now < f.to_ms + wd_ms) return true;
    return false;
  }
  bool stuck_now() {
    bool s = false;
    for (auto &f : faults) if (f.kind == BusFault::stuck && now >= f.from_ms && now < f.to_ms) s = true;
    if (s) stuck = true;
    return stuck;
  }
  // ------------------------------------------------------------ Bus (transaction level)
  int read(uint8_t addr, uint8_t a, uint8_t *b, unsigned n) override {
    if (addr == PIANO_CP_ADDRESS) ++cp_txn;
    bus_us += 100 + 230 * (n + 3);
    if (stuck_now()) {if (addr == PIANO_CP_ADDRESS) ++cp_err; return kStuck;}
    const BusFault f = fault_now(addr);
    if (f == BusFault::nak) {if (addr == PIANO_CP_ADDRESS) ++cp_err; return kNak;}
    if (f == BusFault::timeout) {bus_us += 50000; if (addr == PIANO_CP_ADDRESS) ++cp_err; return kTimeout;}
    if (addr != PIANO_CP_ADDRESS) return kNak;
    for (unsigned i = 0; i < n; ++i) b[i] = chip_read(uint8_t(auto_inc ? a + i : a));
    if (f == BusFault::bitflip && n) b[std::uniform_int_distribution<unsigned>(0, n - 1)(rng)] ^= uint8_t(1u << (rng() % 8));
    return kOk;
  }
  int write(uint8_t addr, uint8_t a, uint8_t v) override {
    if (addr == PIANO_CP_ADDRESS) ++cp_txn;
    bus_us += 100 + 230 * 3;
    if (stuck_now()) {if (addr == PIANO_CP_ADDRESS) ++cp_err; return kStuck;}
    const BusFault f = fault_now(addr);
    if (f == BusFault::nak) {if (addr == PIANO_CP_ADDRESS) ++cp_err; return kNak;}
    if (f == BusFault::timeout) {bus_us += 50000; if (addr == PIANO_CP_ADDRESS) ++cp_err; return kTimeout;}
    if (addr != PIANO_CP_ADDRESS) return kNak;
    last_write_corrupt = f == BusFault::bitflip;
    if (last_write_corrupt) v ^= uint8_t(1u << (rng() % 8));
    chip_write(a, v);
    last_write_corrupt = false;
    return kOk;
  }
  int recover() override {bus_us += 120; stuck = false; return stuck_now() ? kStuck : kOk;}
  // ------------------------------------------------------------ one physics step
  void step() {
    const double dt = cfg.dt_ms / 1000.0;
    now += cfg.dt_ms;
    if (reset_until && now >= reset_until) {reset_until = 0; if (attached) {mode = Mode::fixed5; set_mv = 5000; lim_ma = 3000;}}
    if (pend && now >= pend_at && powered()) {pend = false; set_mv = pend_mv; lim_ma = pend_ma; mode = pend_mode;}
    const bool on = powered();
    const double vs = on ? std::max(0.0, set_mv + overshoot_mv - sag_mv) : 0;
    const double o0 = Pack::ocv_mv(pk[0].soc), o1 = Pack::ocv_mv(pk[1].soc);
    const double g0 = 1 / pk[0].r_ohm, g1 = 1 / pk[1].r_ohm, rth = 1 / (g0 + g1), vth = (o0 * g0 + o1 * g1) * rth;
    const double isys = cfg.sys_mw / std::max(vth, 3000.0) * 1000;
    const double rcab = src.cable_mohm / 1000, rcp = 0.012, eta = 0.97;
    // the buck
    buck_out_ma = buck_in_ma = 0;
    const double vb_est = vbus_mv > 0 ? vbus_mv : vs;
    if (on && vb_est >= 4400) {
      double out = std::min<double>(fcc_ma, icl_ma * vb_est * 0.9 / std::max(vth, 3000.0));
      if (chg_stop) out = std::min(out, isys);
      const double cv = (fv_mv - vth) / rth + isys - iout_ma;
      out = std::max(0.0, std::min(out, cv));
      buck_out_ma = out; buck_in_ma = out * std::max(vth, 3000.0) / (vb_est * 0.9);
    }
    // CHG_EN latency and the start window (VBUS against 2 x VOUT)
    if (en_req && !active && now >= en_at) {
      const double vout = vnode_mv > 0 ? vnode_mv : vth;
      en_req = false;
      if (!on || vs < 2 * vout + 100) {flags |= reg::F_VBUS_LO; if (!keep_en) r[reg::CTRL2] &= uint8_t(~PIANO_CP_CHG_EN);}
      else if (vs > 2 * vout + 2500) {flags |= reg::F_VBUS_HI; if (!keep_en) r[reg::CTRL2] &= uint8_t(~PIANO_CP_CHG_EN);}
      else {
        active = true; keep_en = false;
        if (powered() && mode != Mode::pps && mode != Mode::qc) ++unstepped_starts;   // a start on a source with no current limit
      }
    }
    if (active && !on) {active = false; keep_en = true; flags |= reg::F_VBUS_LO;}   // input gone: switching stops, CHG_EN stays written
    if (!active && !en_req && on && (r[reg::CTRL2] & PIANO_CP_CHG_EN)) {en_req = true; en_at = now + en_latency_ms;}   // and resumes
    if (active && vs < 2 * (vnode_mv > 0 ? vnode_mv : vth) - 200) {active = false; keep_en = true; flags |= reg::F_VBUS_LO;}
    iout_ma = cp_ibus_ma = 0; cl_mode = false;
    if (active) {
      const double num = vs / 2 - vth - rth * (buck_out_ma - isys) / 1000;
      const double den = rcab / (4 * eta) + rcp + r_path + rth;
      iout_ma = std::max(0.0, num / den);
      cp_ibus_ma = iout_ma / (2 * eta);
      const double cap = mode == Mode::pps ? double(std::min<uint32_t>(lim_ma, apdo_ma())) :
                         mode == Mode::qc ? double(src.qc_max_ma) * 1.08 : double(lim_ma) * 1.1;
      if (cp_ibus_ma + buck_in_ma > cap) {
        if (mode == Mode::pps) {cl_mode = true; cp_ibus_ma = std::max(0.0, cap - buck_in_ma); iout_ma = cp_ibus_ma * 2 * eta;}
        else {if (!ocp_since) ocp_since = now; if (now - ocp_since > 50) {hard_reset(1000); active = false; iout_ma = cp_ibus_ma = 0; ocp_since = 0;}}
      } else ocp_since = 0;
    } else ocp_since = 0;
    ibus_ma = cp_ibus_ma + buck_in_ma;
    vnode_mv = vth + rth * (iout_ma + buck_out_ma - isys);
    if (active) {
      const double vout = vnode_mv + (rcp + r_path) * iout_ma;
      vbus_mv = cl_mode ? 2 * vout : vs - rcab * ibus_ma;
      if (cl_mode && vbus_mv + rcab * ibus_ma < src.apdo_min_mv) {hard_reset(1000); active = false;}
    } else vbus_mv = on ? vs - rcab * ibus_ma : 0;
    ipk_ma[0] = (vnode_mv - o0) / pk[0].r_ohm;
    ipk_ma[1] = (vnode_mv - o1) / pk[1].r_ohm;
    for (int k = 0; k < 2; ++k) {
      pk[k].soc = std::clamp(pk[k].soc + ipk_ma[k] * dt / 3600 / pk[k].cap_mah, 0.0, 1.0);
      pk[k].temp_c += (ipk_ma[k] * ipk_ma[k] / 1e6 * pk[k].r_ohm * 8 + cfg.ambient_c + 3 - pk[k].temp_c) * dt / 120;
    }
    const double ploss = active ? std::max(0.0, (vbus_mv * cp_ibus_ma - (vnode_mv + rcp * iout_ma) * iout_ma) / 1e6) + iout_ma * iout_ma / 1e6 * rcp : 0;
    tdie_c += ((ploss + (active ? extra_die_w * std::min(1.0, iout_ma / 6000) : 0)) * 38 + cfg.ambient_c + 5 - tdie_c) * dt / 15;
    conn_c += ((ibus_ma * ibus_ma / 1e6 * 0.010 + extra_conn_w) * 55 + cfg.ambient_c - conn_c) * dt / 40;
    conn2_c += (ibus_ma * ibus_ma / 1e6 * 0.006 * 55 + cfg.ambient_c - conn2_c) * dt / 40;
    charger_c += (buck_in_ma * std::max(0.0, vbus_mv) / 1e6 * 0.1 * 30 + cfg.ambient_c + 5 - charger_c) * dt / 60;
    if (active) {
      if (vbus_mv > reg::busovp_mv(r[reg::BUSOVP])) trip(reg::F_BUSOVP);
      else if (!(r[reg::BUSOCP] & 0x80) && cp_ibus_ma > reg::busocp_ma(r[reg::BUSOCP])) trip(reg::F_BUSOCP);
      else if (!(r[reg::BATOVP] & 0x80) && vnode_mv > reg::batovp_mv(r[reg::BATOVP])) trip(reg::F_BATOVP);
      else if (!(r[reg::BATOCP] & 0x80) && iout_ma > reg::batocp_ma(r[reg::BATOCP])) trip(reg::F_BATOCP);
      else if (!(r[reg::TEMP_CTRL] & PIANO_CP_TSHUT_DISABLE) && tdie_c * 10 > kTshutC10) trip(reg::F_TSHUT);
      else if (wd_on() && now - wd_kick > wd_ms) trip(reg::F_WD);
    }
    if (active) cp_on_ms += cfg.dt_ms;
    if (r[reg::ADC1] & PIANO_CP_ADC_EN) {
      const double ch[PIANO_CP_ADC_CHANNELS] = {cp_ibus_ma * 1000, std::max(0.0, vbus_mv) * 1000, vs * 1000, 0,
                                                (vnode_mv + rcp * iout_ma) * 1000, vnode_mv * 1000, iout_ma * 1000, 0, 0, tdie_c * 10};
      for (unsigned c = 0; c < PIANO_CP_ADC_CHANNELS; ++c) {
        const uint32_t raw = uint32_t(std::clamp(ch[c] / reg::adc_lsb[c], 0.0, 65535.0));
        r[reg::ADC + 2 * c] = uint8_t(raw >> 8); r[reg::ADC + 2 * c + 1] = uint8_t(raw);
      }
    } else for (unsigned c = 0; c < 2 * PIANO_CP_ADC_CHANNELS; ++c) r[reg::ADC + c] = 0;
    if (uvdm_pending && now >= uvdm_due && !stalled) {uvdm_answered = uvdm_pending; uvdm_pending = 0;}
    const double soc_hi = std::max(pk[0].soc, pk[1].soc) * 100;
    hold = cfg.health_policy ? (hold ? soc_hi >= 75 : soc_hi >= 80) : false;
    max_ibus = std::max(max_ibus, ibus_ma); max_ibat = std::max(max_ibat, iout_ma + buck_out_ma);
    max_pack = std::max({max_pack, ipk_ma[0], ipk_ma[1]}); max_tdie = std::max(max_tdie, tdie_c);
    max_power = std::max(max_power, std::max(0.0, vbus_mv) * ibus_ma / 1000);
    mah_in += (ipk_ma[0] + ipk_ma[1]) * dt / 3600;
  }
  // the gauge worker (every 1 s on the shared bus for pack A)
  void gauge_worker() {
    for (int k = 0; k < 2; ++k) {
      bool ok = !gauge_forced_invalid[k];
      if (k == 0 && ok) {
        bus_us += 3000;
        if (stuck_now()) ok = false;
        else {const BusFault f = fault_now(0x55); if (f == BusFault::nak || f == BusFault::timeout) ok = false;}
      }
      if (ok) {gauge_fail_run[k] = 0; gv[k] = {true, int32_t(vnode_mv), int32_t(ipk_ma[k]), int32_t(pk[k].soc * 100 + 0.5), int32_t(pk[k].temp_c * 10)};}
      else if (++gauge_fail_run[k] >= 3) gv[k].valid = false;
    }
  }
  Sensors sensors() const {
    Sensors s;
    s.now_ms = now; s.attached = powered() || (attached && !reset_until); s.real_type = s.attached ? real_type : 0;
    if (s.attached && src.kind == SrcKind::pps) {s.apdo_min_mv = src.apdo_min_mv; s.apdo_max_mv = src.apdo_max_mv; s.apdo_max_ma = apdo_ma();}
    s.pdo_above_5v = s.attached && src.pdo_above_5v && (src.kind == SrcKind::pps || src.kind == SrcKind::fixed_pd);
    s.g[0] = gv[0]; s.g[1] = gv[1];
    s.conn_c10 = int32_t(conn_c * 10); s.conn2_c10 = int32_t(conn2_c * 10); s.charger_c10 = int32_t(charger_c * 10);
    s.health_hold = hold; s.attach_seq = attach_seq;
    return s;
  }
};

// ================================================================ the line-level bus: an I2C slave for Gauge.h's I2cBus (fork A's driver)
struct LineBus {
  Plant &p;
  bool m_sda = true, m_scl = true;    // the master's releases
  bool s_sda_low = false;             // the slave drives SDA low (ACK / a 0 bit)
  enum class St { idle, addr, reg, wdata, rdata, nacked } st = St::idle;
  uint8_t shift = 0, addr = 0, rgp = 0, out = 0;
  unsigned nbit = 0;
  bool acking = false, reading_ack = false, sel_cp = false, sel_gauge = false, rd = false;
  uint16_t gword = 0;
  unsigned gbyte = 0;
  BusFault txn_fault = BusFault::none;
  explicit LineBus(Plant &plant) : p(plant) {}
  bool sda_line() const {return m_sda && !s_sda_low && !p.stuck;}
  uint64_t stretch_until = 0;
  bool scl_line() {if (txn_fault == BusFault::timeout && st != St::idle) {if (p.bus_us < stretch_until) return false; txn_fault = BusFault::none; st = St::idle;} return m_scl;}
  uint16_t gauge_word(uint8_t cmd) const {
    switch (cmd) {
      case pianopower::kGaugeVoltage: return uint16_t(p.vnode_mv);
      case pianopower::kGaugeCurrent: return uint16_t(int16_t(p.ipk_ma[0]));
      case pianopower::kGaugeAverageCurrent: return uint16_t(int16_t(p.ipk_ma[0]));
      case pianopower::kGaugeSoc: return uint16_t(p.pk[0].soc * 100 + 0.5);
      case pianopower::kGaugeTemperature: return uint16_t(p.pk[0].temp_c * 10 + 2731.5);
      case pianopower::kGaugeFull: return uint16_t(p.pk[0].cap_mah);
      case pianopower::kGaugeRemaining: return uint16_t(p.pk[0].cap_mah * p.pk[0].soc);
      default: return 0;
    }
  }
  void start_cond() {st = St::addr; nbit = 0; shift = 0; acking = false; reading_ack = false; s_sda_low = false;
                     p.stuck_now(); txn_fault = p.fault_now(0); if (txn_fault == BusFault::timeout) stretch_until = p.bus_us + 60000;}
  void stop_cond() {st = St::idle; s_sda_low = false; acking = false; reading_ack = false;}
  void scl_rise() {   // sample SDA
    const bool b = sda_line();
    if (st == St::idle || st == St::nacked) return;
    if (acking) return;   // the master samples our ACK
    if (reading_ack) {    // the master's ACK after a byte we sent
      reading_ack = false;
      if (b) {st = St::nacked; return;}
      if (p.auto_inc) ++rgp;
      prepare_byte();
      return;
    }
    if (st == St::rdata) return;   // we drive the bits
    shift = uint8_t(shift << 1 | (b ? 1 : 0));
    if (++nbit < 8) return;
    nbit = 0;
    if (st == St::addr) {
      addr = uint8_t(shift >> 1); rd = shift & 1;
      sel_cp = addr == PIANO_CP_ADDRESS; sel_gauge = addr == pianopower::kGaugeAddress;
      const bool ack = (sel_cp || sel_gauge) && txn_fault != BusFault::nak;
      if (sel_cp) ++p.cp_txn;
      if (!ack) {if (sel_cp) ++p.cp_err; st = St::nacked; return;}
      acking = true;
      st = rd ? St::rdata : St::reg;
      if (rd) {gbyte = 0; prepare_byte_after_ack = true;}
    } else if (st == St::reg) {rgp = shift; acking = true; st = St::wdata; if (sel_gauge) {gword = gauge_word(rgp); gbyte = 0;}}
    else if (st == St::wdata) {
      uint8_t v = shift;
      p.last_write_corrupt = txn_fault == BusFault::bitflip;
      if (p.last_write_corrupt) v ^= uint8_t(1u << (p.rng() % 8));
      if (sel_cp) p.chip_write(rgp, v);
      p.last_write_corrupt = false;
      ++rgp; acking = true;
    }
  }
  bool prepare_byte_after_ack = false;
  void prepare_byte() {
    if (sel_cp) out = p.chip_read(rgp);
    else {out = gbyte == 0 ? uint8_t(gword) : uint8_t(gword >> 8); ++gbyte;}
    if (txn_fault == BusFault::bitflip) out ^= uint8_t(1u << (p.rng() % 8));
    nbit = 0;
  }
  void scl_fall() {   // set SDA for the next bit
    if (acking) {   // the ACK bit ends now? the falling edge after the ACK's high phase
      if (ack_driven) {ack_driven = false; acking = false; s_sda_low = false;
        if (st == St::rdata && prepare_byte_after_ack) {prepare_byte_after_ack = false; prepare_byte(); drive_bit();}
        return;}
      s_sda_low = true; ack_driven = true;   // drive the ACK for the coming high phase
      return;
    }
    if (st == St::rdata && !reading_ack) {
      if (nbit < 8) {drive_bit(); return;}
      s_sda_low = false; reading_ack = true; nbit = 0;   // release for the master's ACK
    }
  }
  bool ack_driven = false;
  void drive_bit() {s_sda_low = !((out >> (7 - nbit)) & 1); ++nbit;}
  // I2cIo callbacks
  static void sda(void *c, bool release) {
    auto &b = *static_cast<LineBus *>(c);
    const bool before = b.sda_line();
    b.m_sda = release;
    const bool after = b.sda_line();
    if (b.scl_line() && before && !after) b.start_cond();
    else if (b.scl_line() && !before && after && b.st != St::idle) b.stop_cond();
  }
  static void scl(void *c, bool release) {
    auto &b = *static_cast<LineBus *>(c);
    const bool before = b.scl_line();
    b.m_scl = release;
    const bool after = b.scl_line();
    if (!before && after) b.scl_rise();
    else if (before && !after) b.scl_fall();
  }
  static bool rsda(void *c) {return static_cast<LineBus *>(c)->sda_line();}
  static bool rscl(void *c) {return static_cast<LineBus *>(c)->scl_line();}
  static void delay(void *c, unsigned us) {static_cast<LineBus *>(c)->p.bus_us += us;}
  pianopower::I2cIo io() {return {this, sda, scl, rsda, rscl, delay};}
};

// ================================================================ the checker
struct Checker {
  struct Grace {uint32_t attach = 300, mode = 400, hold = 1500, soc = 1500, vbat = 1500, cell_t = 1500, conn = 1500, gauge = 2500,
                 tdie = 1000, bus = kWdMs + 1500, sleep = 2500;};
  Grace grace;
  std::vector<std::string> v;
  std::array<uint64_t, 20> since{}, over{};
  uint64_t armed_bad_since = 0;
  bool sleep_req = false;
  uint64_t sleep_at = 0;
  size_t seen_write_violations = 0;
  unsigned count = 0;
  void bad(const Plant &p, const std::string &what) {
    ++count;
    if (v.size() < 12) {char b[40]; std::snprintf(b, sizeof b, "t=%llu ms: ", (unsigned long long)p.now); v.push_back(b + what);}
  }
  bool unable = false;
  void cond(const Plant &p, unsigned k, bool violated, uint32_t g, const char *what) {
    if (unable && k != 9) {since[k] = violated && p.active ? p.now : 0; return;}
    if (violated && p.active) {if (!since[k]) since[k] = p.now; if (p.now - since[k] >= g) {bad(p, std::string("pump on while ") + what); since[k] = p.now + 1000000;}}
    else since[k] = 0;
  }
  void hold_for(const Plant &p, unsigned k, bool o, uint32_t ms, const std::string &what) {
    if (o) {if (!over[k]) over[k] = p.now; if (p.now - over[k] > ms) {bad(p, what); over[k] = p.now + 1000000;}}
    else over[k] = 0;
  }
  void check(const Plant &p, bool sleep_request, bool stalled, bool asleep = false) {
    const Limits &L = p.lim;
    const double vbat = p.vnode_mv;
    const bool verified_ok = p.mipps_legit && p.pd_verified;
    const double src_ma = p.mode == Plant::Mode::pps ? p.apdo_ma() : p.mode == Plant::Mode::qc ? p.src.qc_max_ma :
                          p.mode == Plant::Mode::fixed9 ? p.src.fixed9_ma : 3000.0;
    const double budget = std::min<double>(verified_ok ? L.verified_budget_ma : L.std_budget_ma, src_ma);
    hold_for(p, 8, p.ipk_ma[0] > L.cell_ocp_ma * 1.06 || p.ipk_ma[1] > L.cell_ocp_ma * 1.06, 15, "pack current above the cell OCP + 6 % for two steps");
    hold_for(p, 9, p.iout_ma + p.buck_out_ma > L.battery_ma * 1.04, 15, "battery current above 12 A + 4 % for two steps");
    if (p.vbus_mv > L.busovp_mv && p.active) bad(p, "VBUS above 11 V while switching");
    if (vbat > 4550) bad(p, "VBAT above 4550 mV");
    if (p.tdie_c * 10 > L.tdie_exit_c10 + 10 && p.active && !(p.stuck || stalled)) bad(p, "die above 126 C while switching");
    if (p.tdie_c * 10 > kTshutC10 + 20) bad(p, "die above the 150 C thermal shutdown");
    hold_for(p, 0, p.ipk_ma[0] > L.cell_ocp_ma || p.ipk_ma[1] > L.cell_ocp_ma, 150, "pack current above 7 A for 150 ms");
    hold_for(p, 1, p.ibus_ma > budget * 1.04, 300, "IBUS above the source budget (" + std::to_string(int(budget)) + " mA) for 300 ms");
    hold_for(p, 2, p.ibus_ma > p.src.cable_rating_ma * 1.04, 300, "IBUS above the cable rating for 300 ms");
    hold_for(p, 3, std::max(0.0, p.vbus_mv) * p.ibus_ma / 1000 > p.cfg.power_cap_mw * 1.04, 500, "input power above the cap for 500 ms");
    hold_for(p, 4, p.active && p.vbus_mv > 2 * vbat + L.stage_window_mv, 400, "stage VBUS above 2 x VBAT + 1200 for 400 ms");
    hold_for(p, 5, vbat > p.fv_mv + 30 && vbat > 4400, 1500, "VBAT above FV + 30 mV for 1.5 s");
    hold_for(p, 6, p.iout_ma + p.buck_out_ma > L.battery_ma, 300, "battery current above 12 A for 300 ms");
    hold_for(p, 7, p.iout_ma + p.buck_out_ma > 2.05 * budget + 700, 300, "battery current above 2 x the source budget");
    const bool stepping = p.powered() && (p.mode == Plant::Mode::pps || p.mode == Plant::Mode::qc);
    const double soc_hi = std::max(p.pk[0].soc, p.pk[1].soc) * 100;
    const double tmin = std::min(p.pk[0].temp_c, p.pk[1].temp_c) * 10, tmax = std::max(p.pk[0].temp_c, p.pk[1].temp_c) * 10;
    cond(p, 0, !p.attached || p.reset_until, grace.attach, "the source is gone");
    cond(p, 1, !stepping, grace.mode, "the source is not in PPS/QC stepping mode");
    cond(p, 2, p.hold, grace.hold, "the 80 % health hold is on");
    cond(p, 3, soc_hi >= L.soc_max, grace.soc, "SOC >= 91 %");
    cond(p, 4, vbat < L.start_vbat_min_mv - 50, grace.vbat, "VBAT below 3400 mV");
    cond(p, 5, tmin < L.cell_min_c10 || tmax > L.cell_max_c10, grace.cell_t, "a cell outside 8-55 C");
    cond(p, 6, p.conn_c * 10 >= L.conn_max_c10 || p.conn2_c * 10 >= L.conn_max_c10, grace.conn, "the connector at 50 C or above");
    const bool bus_faulty = p.stuck || p.bus_fault_recent();
    unable = p.stuck || (stalled && !asleep);
    cond(p, 7, (!p.gv[0].valid || !p.gv[1].valid) && !bus_faulty, grace.gauge, "a gauge invalid");
    cond(p, 8, p.tdie_c * 10 >= L.tdie_exit_c10, grace.tdie, "the die at 125 C");
    if ((p.stuck || stalled) && p.active && p.now - p.wd_kick > p.wd_ms + 500) bad(p, "pump on while the bus is stuck / the host stalled, past the watchdog");
    // a register a bus fault corrupted is caught by the read-back and stopped within one pass: 200 ms
    if (!p.armed() && p.active) {if (!armed_bad_since) armed_bad_since = p.now; if (p.now - armed_bad_since > 200 && !since[10]) {since[10] = 1; bad(p, "the chip's protections / watchdog / VAC OVP not as intended while switching for 200 ms: " + p.armed_why());}}
    else {armed_bad_since = 0; since[10] = 0;}
    cond(p, 11, p.verify != 0, 0, "MiPPS (0x21002) in progress");
    if (p.cmd8_seen) {bad(p, "VDM command 8 sent (it stalls this firmware)"); const_cast<Plant &>(p).cmd8_seen = false;}
    if (p.verify && stalled) const_cast<Plant &>(p).verify_since = std::min(p.now, p.verify_since + p.cfg.dt_ms);   // a stalled or sleeping host cannot clear it
    if (p.verify && p.now - p.verify_since > 15000) {bad(p, "0x21002 still set 15 s (awake) after it was set"); const_cast<Plant &>(p).verify_since = p.now;}
    while (seen_write_violations < p.write_violations.size()) bad(p, p.write_violations[seen_write_violations++]);
    cond(p, 12, asleep, 300, "the system asleep");
    if (sleep_request && !sleep_req) {sleep_req = true; sleep_at = p.now;}
    if (!sleep_request) sleep_req = false;
    const uint32_t sleep_grace = (p.stuck || p.bus_fault_recent() || stalled) ? p.wd_ms + 1500 : grace.sleep;
    if (sleep_req && p.active && p.now - sleep_at > sleep_grace) {bad(p, "pump still on after the sleep request"); sleep_at = p.now + 1000000;}
  }
};

}  // namespace cpsim
