// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the VA macro's DMIC path (AudioVa.c) against a register model of the macro's first page with its reset values:
// the order (the macro's clock first; each decimator powered up before the DMIC clocks start; the decimators unmuted last; off: muted
// first, the macro's clock last), the values (48 kHz, DEC0 <- DMIC0, DEC1 <- DMIC2, the DMIC select, 150 Hz, the gain, 2.4 MHz,
// the DMIC reset released), the waits, a macro that does not take its clock, and the LPI pin values.
//   cc -std=c11 -Wall -Wextra -Werror va_host.c ../AudioVa.c && ./a.out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioVa.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

struct Model {
    uint32_t reg[0x1000 / 4];
    uint32_t log_off[512], log_val[512];
    uint64_t log_at[512];
    unsigned writes;
    uint64_t now_us;
    int bad_mux;       // DEC0 selector write is refused
    int dead_clock;    // MCLK enable does not stick
    int bad_offset;
};
static struct Model m;

static uint32_t rd(void *ctx, uint32_t off)
{
    (void)ctx;
    if (off >= 0x1000 || off % 4) {m.bad_offset = 1; return 0;}
    return m.reg[off / 4];
}
static void wr(void *ctx, uint32_t off, uint32_t v)
{
    (void)ctx;
    if (off >= 0x1000 || off % 4) {m.bad_offset = 1; return;}
    if (off == VA_MCLK_CONTROL && m.dead_clock) v &= ~1u;
    if (off == VA_ADC_MUX0_CFG0 && m.bad_mux) v &= ~0xf0u;
    m.reg[off / 4] = v;
    if (m.writes < 512) {m.log_off[m.writes] = off; m.log_val[m.writes] = v; m.log_at[m.writes] = m.now_us;}
    m.writes++;
}
static void sl(void *ctx, uint32_t us) {(void)ctx; m.now_us += us;}
static const AudioVaIo io = {0, rd, wr, sl};
static AudioVaReadback rb;

static void reset_model(void)
{
    memset(&m, 0, sizeof m);
    m.reg[VA_DMIC_CFG / 4] = 0x80;
    for (unsigned d = 0; d < 2; ++d) {
        m.reg[VA_TX_CTL(d) / 4] = 0x04;
        m.reg[VA_TX_CFG0(d) / 4] = 0x10;
        m.reg[VA_TX_GATE(d) / 4] = 0x01;
    }
}

// The index of the first write to off whose value satisfies (v & mask) == want, from `from`; -1 if none.
static int find(unsigned from, uint32_t off, uint32_t mask, uint32_t want)
{
    for (unsigned i = from; i < m.writes && i < 512; ++i)
        if (m.log_off[i] == off && (m.log_val[i] & mask) == want) return (int)i;
    return -1;
}
static int last_write(uint32_t off)
{
    int at = -1;
    for (unsigned i = 0; i < m.writes && i < 512; ++i) if (m.log_off[i] == off) at = (int)i;
    return at;
}

static void testOn(void)
{
    reset_model();
    CHECK(audio_va_on(&io, AUDIO_VA_GAIN_DEFAULT, -1, &rb) == 0 && !m.bad_offset);
    // the final state
    CHECK((m.reg[VA_MCLK_CONTROL / 4] & 1) && (m.reg[VA_FS_CNT_CONTROL / 4] & 3) == 1 && (m.reg[VA_TOP_CFG0 / 4] & 2));
    CHECK(m.reg[VA_DMIC01_CTL / 4] == 0x05 && m.reg[VA_DMIC23_CTL / 4] == 0x05 && !(m.reg[VA_DMIC_CFG / 4] & 0x80));
    CHECK(m.reg[VA_ADC_MUX0_CFG0 / 4] == 0x10 && m.reg[VA_ADC_MUX1_CFG0 / 4] == 0x30);
    CHECK(!(m.reg[VA_ADC_MUX0_CFG1 / 4] & 1) && !(m.reg[VA_ADC_MUX1_CFG1 / 4] & 1));
    for (unsigned d = 0; d < 2; ++d) {
        CHECK(m.reg[VA_TX_CTL(d) / 4] == (0x20 | 0x04));   // clock on, unmuted, 48 kHz
        CHECK(m.reg[VA_TX_CFG0(d) / 4] == 0xd0);           // DMIC select, 150 Hz, the reset's other bits
        CHECK(m.reg[VA_TX_VOL(d) / 4] == 0x28);            // +40 dB
        CHECK(m.reg[VA_TX_GATE(d) / 4] == 0x01);           // the zero gate off again, no change request left
    }
    // the order: the clock first, the FS counter cleared then enabled
    CHECK(m.log_off[0] == VA_MCLK_CONTROL && (m.log_val[0] & 1));
    const int clr = find(0, VA_FS_CNT_CONTROL, 3, 3), en = find(0, VA_FS_CNT_CONTROL, 3, 1);
    CHECK(clr > 0 && en > clr && find(0, VA_TOP_CFG0, 2, 2) > en);
    // every decimator's clock on and its gate cycled before the DMIC clocks; the gain written after the gate came back on
    const int dmic = find(0, VA_DMIC01_CTL, 1, 1);
    CHECK(dmic > 0 && find(0, VA_DMIC23_CTL, 1, 1) > dmic - 1 && find(0, VA_DMIC_CFG, 0x80, 0) < dmic);
    for (unsigned d = 0; d < 2; ++d) {
        const int clk = find(0, VA_TX_CTL(d), 0x20, 0x20), gate_on = find(0, VA_TX_GATE(d), 1, 0), gate_off = find((unsigned)gate_on + 1, VA_TX_GATE(d), 1, 1);
        const int req = find(0, VA_TX_GATE(d), 2, 2), req_end = find((unsigned)req + 1, VA_TX_GATE(d), 2, 0);
        CHECK(clk >= 0 && gate_on > clk && req > gate_on && req_end > req && gate_off > req_end && gate_off < dmic);
        CHECK(m.log_at[req_end] - m.log_at[req] >= 1000 && m.log_at[gate_off] - m.log_at[gate_on] >= 3000);
        const int vol = last_write(VA_TX_VOL(d));
        CHECK(vol > gate_off && m.log_at[vol] - m.log_at[gate_off] >= 6000 && vol < dmic);
        CHECK(find(0, VA_TX_CFG0(d), 0x80, 0x80) < clk);   // the DMIC select before the decimator runs
    }
    // unmuted last of all
    for (unsigned d = 0; d < 2; ++d) {
        const int un = last_write(VA_TX_CTL(d));
        CHECK(un > dmic && !(m.log_val[un] & 0x10));
    }
    CHECK(m.now_us >= 2 * 9000);
}

static void testOff(void)
{
    reset_model();
    CHECK(audio_va_on(&io, AUDIO_VA_GAIN_DEFAULT, -1, &rb) == 0);
    const unsigned from = m.writes;
    CHECK(audio_va_off(&io, -1, &rb) == 0);
    CHECK(!m.bad_offset);
    // muted first, the DMIC clocks stopped next, the macro's clock last
    CHECK(m.log_off[from] == VA_TX_CTL(0) && (m.log_val[from] & 0x10) && m.log_off[from + 1] == VA_TX_CTL(1) && (m.log_val[from + 1] & 0x10));
    CHECK(m.reg[VA_DMIC01_CTL / 4] == 0 && m.reg[VA_DMIC23_CTL / 4] == 0);
    CHECK(!(m.reg[VA_MCLK_CONTROL / 4] & 1) && !(m.reg[VA_FS_CNT_CONTROL / 4] & 1) && !(m.reg[VA_TOP_CFG0 / 4] & 2));
    CHECK(last_write(VA_MCLK_CONTROL) > last_write(VA_DMIC23_CTL) && last_write(VA_MCLK_CONTROL) > last_write(VA_TX_CTL(1)));
    for (unsigned d = 0; d < 2; ++d) CHECK(!(m.reg[VA_TX_CTL(d) / 4] & 0x20) && (m.reg[VA_TX_CTL(d) / 4] & 0x10));
    CHECK((m.reg[VA_ADC_MUX0_CFG0 / 4] & 0xf0) == 0 && (m.reg[VA_ADC_MUX1_CFG0 / 4] & 0xf0) == 0);
}

static void testDeadClock(void)
{
    reset_model();
    m.dead_clock = 1;
    CHECK(audio_va_on(&io, AUDIO_VA_GAIN_DEFAULT, -1, &rb) == -1);
    // nothing but the clock's enable and its undo
    for (unsigned i = 0; i < m.writes; ++i)
        CHECK(m.log_off[i] == VA_MCLK_CONTROL || m.log_off[i] == VA_FS_CNT_CONTROL || m.log_off[i] == VA_TOP_CFG0);
    CHECK(m.reg[VA_DMIC01_CTL / 4] == 0 && m.reg[VA_DMIC_CFG / 4] == 0x80);
}

static void testGainAndPins(void)
{
    CHECK(audio_va_gain_register(124) == 0x28 && audio_va_gain_register(84) == 0x00 && audio_va_gain_register(0) == 0xac);
    CHECK(audio_va_gain_register(200) == 0x28 && audio_va_gain_register(100) == 0x10);
    reset_model();
    CHECK(audio_va_on(&io, 100, -1, &rb) == 0 && m.reg[VA_TX_VOL(0) / 4] == 0x10 && m.reg[VA_TX_VOL(1) / 4] == 0x10);
}

static void testRoutes(void)
{
    const char *valid[] = {"-1", "0", "2", "4", "6"};
    const int wanted[] = {-1, 0, 2, 4, 6};
    for (unsigned i = 0; i < sizeof valid / sizeof *valid; ++i) {
        int route = -99;
        CHECK(audio_mic_route_parse(valid[i], &route) && route == wanted[i]);
    }
    const char *invalid[] = {"", "-", "1", "-0", "-2", "3", "4294967300", "4x", "04", "6 "};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        int route = -99;
        CHECK(!audio_mic_route_parse(invalid[i], &route) && route == -99);
    }
    static const struct {
        int route;
        unsigned mux, mask, count, gpio[4];
    } cases[] = {
        {-1, 0x10, 3, 4, {6, 7, 8, 9}}, {0, 0x10, 3, 4, {6, 7, 8, 9}},
        {2, 0x30, 2, 2, {8, 9}}, {4, 0x50, 6, 4, {8, 9, 12, 13}}, {6, 0x70, 10, 4, {8, 9, 17, 18}},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        reset_model();
        AudioVaPin pins[AUDIO_VA_PINS];
        CHECK(audio_va_pair_mask(cases[i].route) == cases[i].mask);
        CHECK(audio_va_pins(cases[i].route, pins) == cases[i].count);
        for (unsigned j = 0; j < cases[i].count; ++j) {
            CHECK(pins[j].gpio == cases[i].gpio[j] && pins[j].drive == !(j % 2));
            CHECK(pins[j].cfg == (j % 2 ? 0x044u : 0x244u));
        }
        for (unsigned pair = 0; pair < 4; ++pair)
            m.reg[(VA_DMIC01_CTL + 4 * pair) / 4] = cases[i].mask & (1u << pair) ? 0x100 : 0x109;
        CHECK(audio_va_on(&io, 124, cases[i].route, &rb) == 0);
        CHECK(rb.mux[0] == cases[i].mux && rb.mux[1] == 0x30);
        CHECK(audio_va_off(&io, cases[i].route, &rb) == 0);
        CHECK(rb.mux[0] == 0 && rb.mux[1] == 0);
        for (unsigned pair = 0; pair < 4; ++pair) {
            const unsigned ctl = VA_DMIC01_CTL + 4 * pair;
            const int owned = (cases[i].mask >> pair) & 1;
            const int enable = find(0, ctl, 0xf, 5);
            CHECK(rb.ctl[pair] == (owned ? 0x100 : 0x109));
            if (owned) {
                CHECK(enable >= 0 && find((unsigned)enable + 1, ctl, 0xf, 5) < 0);
            } else CHECK(last_write(ctl) < 0);
        }
    }
    reset_model();
    AudioVaPin pins[AUDIO_VA_PINS];
    CHECK(audio_va_pins(3, pins) == 0 && audio_va_on(&io, 124, 3, &rb) == -1);
    CHECK(audio_va_off(&io, 3, &rb) == -1 && m.writes == 0);
    m.bad_mux = 1;
    CHECK(audio_va_on(&io, 124, 4, &rb) == -2);
    CHECK(rb.mux[0] == 0 && rb.mux[1] == 0x30 && rb.ctl[2] == 5);
    CHECK(m.reg[VA_DMIC23_CTL / 4] == 0 && m.reg[VA_DMIC45_CTL / 4] == 0 && m.reg[VA_MCLK_CONTROL / 4] == 0);
}

static uint32_t pin_regs[4][2];
static uint32_t pin_read(void *ctx, unsigned pin, unsigned off)
{
    (void)ctx;
    return pin_regs[pin][off / 4];
}
static void pin_write(void *ctx, unsigned pin, unsigned off, uint32_t value)
{
    if (ctx && pin == 1 && !off && value == 0x044) return;
    pin_regs[pin][off / 4] = value;
}

static void testRestore(void)
{
    for (unsigned fail = 0; fail < 2; ++fail) {
        AudioVaPinIo pin_io = {fail ? &m : NULL, pin_read, pin_write};
        AudioVaPin pins[AUDIO_VA_PINS];
        AudioVaPinState state[AUDIO_VA_PINS];
        const unsigned count = audio_va_pins(4, pins);
        for (unsigned i = 0; i < count; ++i) {
            pin_regs[i][0] = 0x300 + i;
            pin_regs[i][1] = i % 2 ? 2 : 0;
        }
        CHECK(audio_va_pin_apply(&pin_io, pins, state, count) == fail);
        CHECK(state[0].value == 2 && state[2].value == 2 && state[3].cfg == 0x044);
        CHECK(audio_va_pin_restore(&pin_io, state, count) == 0);
        for (unsigned i = 0; i < count; ++i)
            CHECK(pin_regs[i][0] == 0x300 + i && pin_regs[i][1] == (i % 2 ? 2u : 0u));
    }
}

int main(void)
{
    testOn();
    testOff();
    testDeadClock();
    testGainAndPins();
    testRoutes();
    testRestore();
    printf("PASS va: %u checks\n", checks);
    return 0;
}
