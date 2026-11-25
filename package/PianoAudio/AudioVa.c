/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The VA macro's DMIC path: see AudioVa.h. */
#include "AudioVa.h"

static uint32_t rd(const AudioVaIo *io, uint32_t off) {return io->read(io->ctx, off);}
static void wr(const AudioVaIo *io, uint32_t off, uint32_t v) {io->write(io->ctx, off, v);}
static void field(const AudioVaIo *io, uint32_t off, uint32_t mask, uint32_t value) {wr(io, off, (rd(io, off) & ~mask) | (value & mask));}
static void sleep_ms(const AudioVaIo *io, uint32_t ms) {if (io->sleep_us) io->sleep_us(io->ctx, ms * 1000u);}

uint8_t audio_va_gain_register(unsigned gain)
{
    if (gain > AUDIO_VA_GAIN_MAX) gain = AUDIO_VA_GAIN_MAX;
    return (uint8_t)((int)gain - 84);
}

/* A decimator powered up: its clock, the high-pass zero gate on, the 150 Hz cut-off asked for (a change request around it), the
 * gate off again, then its gain written once more (the gain takes effect only on a running decimator). */
static void decimator_up(const AudioVaIo *io, unsigned d, uint8_t gain)
{
    field(io, VA_TX_CTL(d), 1u << 5, 1u << 5);
    field(io, VA_TX_GATE(d), 1u << 0, 0);
    sleep_ms(io, 1);
    if ((rd(io, VA_TX_CFG0(d)) >> 5 & 3u) != VA_HPF_150HZ) {
        field(io, VA_TX_CFG0(d), 3u << 5, VA_HPF_150HZ << 5);
        field(io, VA_TX_GATE(d), 1u << 1, 1u << 1);
        sleep_ms(io, 1);
        field(io, VA_TX_GATE(d), 1u << 1, 0);
    }
    sleep_ms(io, 1);
    field(io, VA_TX_GATE(d), 1u << 0, 1u << 0);
    sleep_ms(io, 6);
    wr(io, VA_TX_VOL(d), gain);
}

static void clock_off(const AudioVaIo *io)
{
    field(io, VA_MCLK_CONTROL, 1u, 0);
    field(io, VA_FS_CNT_CONTROL, 1u, 0);
    field(io, VA_TOP_CFG0, 1u << 1, 0);
}

static const uint32_t kPairCtl[] = {VA_DMIC01_CTL, VA_DMIC23_CTL, VA_DMIC45_CTL, VA_DMIC67_CTL};

unsigned audio_va_pair_mask(int route)
{
    return audio_mic_route_valid(route) ? (1u << 1) | (1u << (audio_mic_dec0(route) / 2)) : 0;
}

static void readback(const AudioVaIo *io, AudioVaReadback *rb)
{
    rb->mclk = rd(io, VA_MCLK_CONTROL);
    rb->fs_count = rd(io, VA_FS_CNT_CONTROL);
    rb->fs_broadcast = rd(io, VA_TOP_CFG0);
    rb->mux[0] = rd(io, VA_ADC_MUX0_CFG0);
    rb->mux[1] = rd(io, VA_ADC_MUX1_CFG0);
    rb->src[0] = rd(io, VA_ADC_MUX0_CFG1);
    rb->src[1] = rd(io, VA_ADC_MUX1_CFG1);
    for (unsigned d = 0; d < 2; ++d) {
        rb->tx[d] = rd(io, VA_TX_CTL(d));
        rb->cfg[d] = rd(io, VA_TX_CFG0(d));
    }
    for (unsigned p = 0; p < 4; ++p) rb->ctl[p] = rd(io, kPairCtl[p]);
}

int audio_va_on(const AudioVaIo *io, unsigned gain, int route, AudioVaReadback *rb)
{
    const unsigned pairs = audio_va_pair_mask(route);
    if (!pairs) return -1;
    const uint32_t mux0 = (audio_mic_dec0(route) + 1u) << 4;
    const uint8_t g = audio_va_gain_register(gain);
    /* MCLK, FS counter (enabled and cleared, then enabled), FS broadcast. */
    field(io, VA_MCLK_CONTROL, 1u, 1u);
    if (!(rd(io, VA_MCLK_CONTROL) & 1u)) {clock_off(io); return -1;}
    field(io, VA_FS_CNT_CONTROL, 3u, 3u);
    field(io, VA_FS_CNT_CONTROL, 3u, 1u);
    field(io, VA_TOP_CFG0, 1u << 1, 1u << 1);
    for (unsigned d = 0; d < 2; ++d) field(io, VA_TX_CTL(d), 0xfu, VA_RATE_48K);
    field(io, VA_ADC_MUX0_CFG1, 1u, 0);
    field(io, VA_ADC_MUX1_CFG1, 1u, 0);
    field(io, VA_ADC_MUX0_CFG0, 0xf0u, mux0);
    field(io, VA_ADC_MUX1_CFG0, 0xf0u, 0x30);
    for (unsigned d = 0; d < 2; ++d) {
        field(io, VA_TX_CFG0(d), 1u << 7, 1u << 7);
        wr(io, VA_TX_VOL(d), g);
        field(io, VA_TX_CFG0(d), 3u << 1, 0);
    }
    for (unsigned d = 0; d < 2; ++d) decimator_up(io, d, g);
    field(io, VA_DMIC_CFG, 1u << 7, 0);
    const uint32_t ctl = VA_DMIC_DIV4 << 1 | 1u;
    for (unsigned p = 0; p < 4; ++p)
        if (pairs & (1u << p)) field(io, kPairCtl[p], 0xfu, ctl);
    for (unsigned d = 0; d < 2; ++d) field(io, VA_TX_CTL(d), 1u << 4, 0);

    readback(io, rb);
    int ok = (rb->mux[0] & 0xf0u) == mux0 && (rb->mux[1] & 0xf0u) == 0x30;
    for (unsigned d = 0; d < 2; ++d)
        ok &= !(rb->src[d] & 1u) && (rb->tx[d] & 0x3fu) == (0x20 | VA_RATE_48K) &&
              (rb->cfg[d] & 0x80u);
    for (unsigned p = 0; p < 4; ++p)
        if (pairs & (1u << p)) ok &= (rb->ctl[p] & 0xfu) == ctl;
    if (ok) return 0;
    AudioVaReadback stopped;
    return audio_va_off(io, route, &stopped) ? -3 : -2;
}

int audio_va_off(const AudioVaIo *io, int route, AudioVaReadback *rb)
{
    const unsigned pairs = audio_va_pair_mask(route);
    if (!pairs) return -1;
    for (unsigned d = 0; d < 2; ++d) field(io, VA_TX_CTL(d), 1u << 4, 1u << 4);
    for (unsigned p = 0; p < 4; ++p) {
        if (!(pairs & (1u << p))) continue;
        field(io, kPairCtl[p], 1u, 0);
        field(io, kPairCtl[p], 0xeu, 0);
    }
    for (unsigned d = 0; d < 2; ++d) field(io, VA_TX_CTL(d), 1u << 5, 0);
    field(io, VA_ADC_MUX0_CFG0, 0xf0u, 0);
    field(io, VA_ADC_MUX1_CFG0, 0xf0u, 0);
    readback(io, rb);
    int ok = !((rb->mux[0] | rb->mux[1]) & 0xf0u);
    for (unsigned d = 0; d < 2; ++d) ok &= (rb->tx[d] & 0x30u) == 0x10u;
    for (unsigned p = 0; p < 4; ++p)
        if (pairs & (1u << p)) ok &= !(rb->ctl[p] & 0xfu);
    clock_off(io);
    return ok ? 0 : -1;
}

unsigned audio_va_pins(int route, AudioVaPin pins[AUDIO_VA_PINS])
{
    static const unsigned clock_gpio[] = {6, 8, 12, 17};
    const unsigned mask = audio_va_pair_mask(route);
    unsigned count = 0;
    for (unsigned p = 0; p < 4; ++p) {
        if (!(mask & (1u << p))) continue;
        /* Function 1, 4 mA, no pull, with output enable on the clock. */
        pins[count++] = (AudioVaPin){clock_gpio[p], 0x244, 1};
        pins[count++] = (AudioVaPin){clock_gpio[p] + 1, 0x044, 0};
    }
    return count;
}

unsigned audio_va_pin_apply(const AudioVaPinIo *io, const AudioVaPin *pins, AudioVaPinState *state, unsigned count)
{
    unsigned errors = 0;
    for (unsigned i = 0; i < count; ++i) {
        state[i].old_cfg = io->read(io->ctx, i, 0);
        state[i].old_value = io->read(io->ctx, i, 4);
        if (pins[i].drive) io->write(io->ctx, i, 4, 2);
        io->write(io->ctx, i, 0, pins[i].cfg);
        state[i].cfg = io->read(io->ctx, i, 0);
        state[i].value = io->read(io->ctx, i, 4);
        errors += (state[i].cfg & 0x3ffu) != pins[i].cfg || (pins[i].drive && !(state[i].value & 2u));
    }
    return errors;
}

unsigned audio_va_pin_restore(const AudioVaPinIo *io, AudioVaPinState *state, unsigned count)
{
    unsigned errors = 0;
    for (unsigned i = 0; i < count; ++i) {
        io->write(io->ctx, i, 4, state[i].old_value & 2u);
        io->write(io->ctx, i, 0, state[i].old_cfg);
        state[i].cfg = io->read(io->ctx, i, 0);
        state[i].value = io->read(io->ctx, i, 4);
        errors += state[i].cfg != state[i].old_cfg || ((state[i].value ^ state[i].old_value) & 2u) != 0;
    }
    return errors;
}
