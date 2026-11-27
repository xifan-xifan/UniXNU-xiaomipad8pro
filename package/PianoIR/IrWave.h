// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoIR's portable part (C and C++; the kext, the host tests and tooling/native/piano-ir share it): checks of a pulse/space
// list, the bit-stream waveform the SE15 MOSI line sends to the IR emitter, the NEC encoder, the ir-ctl style parser and the
// clock pick. See UPSTREAM for the Linux piano ir-spi reference. No I/O here.
//   Waveform: one bit per sample at the engine's sample rate (about 1.92 MHz), most significant bit of each byte first; a mark
//   is the carrier (period = round(rate / carrier) samples, the first floor(period * duty / 100) of them high, the phase
//   restarting at every mark), a space is all zero; segment ends come from the cumulative time, so there is no drift; at least
//   one all-zero 32-bit word follows the last sample, so the line is low when the engine stops.
#ifndef PIANO_IR_WAVE_H
#define PIANO_IR_WAVE_H
#include <stddef.h>
#include <stdint.h>

#define IR_MAX_DURATIONS 1024u          // entries of one send (marks and spaces), as LIRC's piano transmitter
#define IR_MAX_TOTAL_US 500000u         // one send's whole length (rc-core's IR_MAX_DURATION)
#define IR_MAX_WAVE_BYTES 32768u        // one engine transfer: no split (a split inserts an unmeasured gap)
#define IR_MIN_CARRIER 30000u
#define IR_MAX_CARRIER 56000u
#define IR_DEFAULT_CARRIER 38000u
#define IR_MIN_DUTY 10u
#define IR_MAX_DUTY 70u                 // tighter than LIRC's 1..99: the LED's average current stays bounded
#define IR_DEFAULT_DUTY 40u
#define IR_MIN_RATE 1000000u            // sample rates the encoder accepts (the clock pick aims at IR_TARGET_RATE)
#define IR_MAX_RATE 4000000u
#define IR_TARGET_RATE 1920000u

static inline int ir_carrier_valid(uint64_t carrier) { return carrier >= IR_MIN_CARRIER && carrier <= IR_MAX_CARRIER; }
static inline int ir_duty_valid(uint64_t duty) { return duty >= IR_MIN_DUTY && duty <= IR_MAX_DUTY; }

// Whole milliseconds that fit in one transfer, including the final zero word.
static inline uint32_t ir_burst_part_ms(uint32_t left, uint32_t rate)
{
    if (rate < IR_MIN_RATE || rate > IR_MAX_RATE) return 0;
    const uint32_t fit = (IR_MAX_WAVE_BYTES - 4u) * 8000u / rate;
    const uint32_t limit = fit < 120 ? fit : 120;
    return left < limit ? left : limit;
}

enum ir_error {
    IR_OK = 0,
    IR_ERR_COUNT = -1,      // no entries, more than IR_MAX_DURATIONS, or an even count (a send ends on a mark)
    IR_ERR_ZERO = -2,       // a zero duration
    IR_ERR_LONG = -3,       // longer than IR_MAX_TOTAL_US in all
    IR_ERR_CARRIER = -4,    // carrier outside 30..56 kHz
    IR_ERR_DUTY = -5,       // duty outside 10..70 %, or no high sample at this rate
    IR_ERR_RATE = -6,       // sample rate outside 1..4 MHz
    IR_ERR_SIZE = -7,       // the waveform does not fit IR_MAX_WAVE_BYTES (or the caller's buffer)
};

static inline const char *ir_error_name(int e)
{
    switch (e) {
    case IR_OK: return "ok";
    case IR_ERR_COUNT: return "count (1..1024 entries, odd: mark first and last)";
    case IR_ERR_ZERO: return "a zero duration";
    case IR_ERR_LONG: return "longer than 500 ms";
    case IR_ERR_CARRIER: return "carrier outside 30000..56000 Hz";
    case IR_ERR_DUTY: return "duty outside 10..70 %";
    case IR_ERR_RATE: return "sample rate outside 1..4 MHz";
    case IR_ERR_SIZE: return "waveform larger than 32 KiB (shorten the send)";
    default: return "unknown";
    }
}

struct ir_wave_plan {
    uint32_t rate, carrier, duty;
    uint32_t period, high;      // samples per carrier period, high samples in it
    uint32_t samples;           // samples of the whole list
    uint32_t bytes;             // the transfer: samples rounded up to a 32-bit word, plus one all-zero word
    uint32_t total_us;          // the list's length
    uint32_t marks;             // mark entries
    uint32_t high_samples;      // samples sent high (for the pad sampler's comparison)
};

static inline uint32_t ir_samples_at(uint64_t us, uint32_t rate) { return (uint32_t)((us * rate + 500000u) / 1000000u); }

// Checks a list and fills the plan (everything but high_samples, which only encoding counts). IR_OK or an ir_error.
static inline int ir_wave_check(const uint32_t *d, unsigned n, uint32_t carrier, uint32_t duty, uint32_t rate, struct ir_wave_plan *p)
{
    if (!d || !p || !n || n > IR_MAX_DURATIONS || !(n & 1u)) return IR_ERR_COUNT;
    if (rate < IR_MIN_RATE || rate > IR_MAX_RATE) return IR_ERR_RATE;
    if (!ir_carrier_valid(carrier)) return IR_ERR_CARRIER;
    if (!ir_duty_valid(duty)) return IR_ERR_DUTY;
    uint64_t total = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!d[i]) return IR_ERR_ZERO;
        total += d[i];
        if (total > IR_MAX_TOTAL_US) return IR_ERR_LONG;
    }
    const uint32_t period = (rate + carrier / 2) / carrier, high = period * duty / 100u;
    if (!high || high >= period) return IR_ERR_DUTY;
    const uint32_t samples = ir_samples_at(total, rate);
    const uint64_t bytes = ((uint64_t)samples + 31u) / 32u * 4u + 4u;
    if (bytes > IR_MAX_WAVE_BYTES) return IR_ERR_SIZE;
    p->rate = rate;
    p->carrier = carrier;
    p->duty = duty;
    p->period = period;
    p->high = high;
    p->samples = samples;
    p->bytes = (uint32_t)bytes;
    p->total_us = (uint32_t)total;
    p->marks = (n + 1u) / 2u;
    p->high_samples = 0;
    return IR_OK;
}

// Encodes into out (cap bytes, cleared here). Returns the byte count (plan->bytes) or an ir_error.
static inline int ir_wave_encode(const uint32_t *d, unsigned n, uint32_t carrier, uint32_t duty, uint32_t rate, uint8_t *out, size_t cap,
                                 struct ir_wave_plan *p)
{
    int r = ir_wave_check(d, n, carrier, duty, rate, p);
    if (r) return r;
    if (!out || cap < p->bytes) return IR_ERR_SIZE;
    for (uint32_t i = 0; i < p->bytes; i++) out[i] = 0;
    uint64_t end_us = 0;
    uint32_t sample = 0, highs = 0;
    for (unsigned i = 0; i < n; i++) {
        end_us += d[i];
        const uint32_t end = ir_samples_at(end_us, rate);
        if (i & 1u) {               // a space: nothing to set
            sample = end;
            continue;
        }
        for (uint32_t phase = 0; sample < end; sample++) {
            if (phase < p->high) {
                out[sample >> 3] |= (uint8_t)(0x80u >> (sample & 7u));
                highs++;
            }
            if (++phase == p->period) phase = 0;
        }
    }
    p->high_samples = highs;
    return (int)p->bytes;
}

// Reads sample i of an encoded waveform (tests, the GPIO path, the pad sampler's expectation).
static inline int ir_wave_bit(const uint8_t *w, uint32_t i) { return (w[i >> 3] >> (7u - (i & 7u))) & 1; }

// NEC: 9000 mark, 4500 space, 32 bits LSB first: address, ~address (or the high byte of a 16-bit extended address), command,
// ~command; bit = 562 mark + 562 (0) or 1687 (1) space; a final 562 mark. 67 entries. Returns the count or IR_ERR_COUNT (cap).
#define IR_NEC_ENTRIES 67u
#define IR_NEC_REPEAT_ENTRIES 3u
#define IR_NEC_PERIOD_US 108000u
static inline int ir_nec_frame(uint32_t address, uint8_t command, uint32_t *out, unsigned cap)
{
    if (!out || cap < IR_NEC_ENTRIES || address > 0xffffu) return IR_ERR_COUNT;
    const uint8_t a0 = (uint8_t)address, a1 = address > 0xffu ? (uint8_t)(address >> 8) : (uint8_t)~a0;
    const uint32_t word = (uint32_t)a0 | (uint32_t)a1 << 8 | (uint32_t)command << 16 | (uint32_t)(uint8_t)~command << 24;
    unsigned k = 0;
    out[k++] = 9000;
    out[k++] = 4500;
    for (unsigned b = 0; b < 32; b++) {
        out[k++] = 562;
        out[k++] = (word >> b) & 1u ? 1687 : 562;
    }
    out[k++] = 562;
    return (int)k;
}
static inline int ir_nec_repeat(uint32_t *out, unsigned cap)
{
    if (!out || cap < IR_NEC_REPEAT_ENTRIES) return IR_ERR_COUNT;
    out[0] = 9000;
    out[1] = 2250;
    out[2] = 562;
    return 3;
}

// One duration token as ir-ctl and LIRC mode2 text write them: "9000", "+9000" (pulse) or "-4500" (space), optionally after
// "pulse" / "space" words handled by the caller. Returns 1 with *us set, 0 when the token is not a duration.
static inline int ir_parse_duration(const char *s, uint32_t *us)
{
    if (!s || !*s) return 0;
    if (*s == '+' || *s == '-') s++;
    uint64_t v = 0;
    unsigned digits = 0;
    for (; *s >= '0' && *s <= '9'; s++, digits++) {
        v = v * 10u + (uint64_t)(*s - '0');
        if (v > IR_MAX_TOTAL_US) return 0;
    }
    if (!digits || *s) return 0;
    *us = (uint32_t)v;
    return 1;
}

// The clock: the engine's source rates (the RCG's DFS levels, or its one current rate as level 0; 0 = unknown) and an integer
// GENI divider (1..4095). Picks the exact divider of IR_TARGET_RATE at the lowest source rate; else the sample rate nearest to
// the target within IR_MIN_RATE..IR_MAX_RATE. 0 when nothing fits.
struct ir_clock_pick {
    unsigned level;
    uint32_t source_hz, divider, rate;
    int exact;
};
static inline int ir_pick_clock(const uint32_t *source_hz, unsigned levels, struct ir_clock_pick *out)
{
    int found = 0;
    uint32_t best_err = 0xffffffffu;
    for (unsigned l = 0; l < levels; l++) {
        const uint32_t hz = source_hz[l];
        if (!hz) continue;
        uint32_t div = (hz + IR_TARGET_RATE / 2) / IR_TARGET_RATE;
        if (!div) div = 1;
        for (uint32_t d = div > 1 ? div - 1 : 1; d <= div + 1; d++) {
            if (d > 0xfffu) break;
            const uint32_t rate = hz / d;
            if (rate < IR_MIN_RATE || rate > IR_MAX_RATE) continue;
            const int exact = (hz % d == 0) && rate == IR_TARGET_RATE;
            const uint32_t err = rate > IR_TARGET_RATE ? rate - IR_TARGET_RATE : IR_TARGET_RATE - rate;
            // exact beats inexact; among exact ones the lowest source rate; otherwise the smallest error
            const int better = !found || (exact && !out->exact) || (exact == out->exact && (exact ? hz < out->source_hz : err < best_err));
            if (better) {
                out->level = l;
                out->source_hz = hz;
                out->divider = d;
                out->rate = rate;
                out->exact = exact;
                best_err = err;
                found = 1;
            }
        }
    }
    return found;
}
#endif
