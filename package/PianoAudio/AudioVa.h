/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Level 10: the LPASS VA macro and LPI DMIC pins. Access only while the PRM macro and
 * dcodec votes and TX core MCLK hold. DEC1 keeps the side-bottom DMIC2 reference.
 * Register and pin facts from Linux lpass-va-macro and sm8650-lpass-lpi, piano DT,
 * Ubuntu UCM (legacy side mics), and ODM piano audio routing (main DMIC4). */
#ifndef PIANO_AUDIO_VA_H
#define PIANO_AUDIO_VA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The VA macro's registers (offsets from 0x07660000, all in its first page). */
#define VA_MCLK_CONTROL 0x000u            /* bit 0: MCLK enable */
#define VA_FS_CNT_CONTROL 0x004u          /* bit 0: FS counter enable, bit 1: clear */
#define VA_TOP_CFG0 0x080u                /* bit 1: FS broadcast enable */
#define VA_DMIC01_CTL 0x084u              /* the DMIC0/1 pair: bit 0 clock enable, bits 3:1 the divider code (2: / 4) */
#define VA_DMIC23_CTL 0x088u              /* the DMIC2/3 pair */
#define VA_DMIC45_CTL 0x08cu              /* the DMIC4/5 pair */
#define VA_DMIC67_CTL 0x090u              /* the DMIC6/7 pair */
#define VA_DMIC_CFG 0x094u                /* bit 7: all DMICs held in reset (its reset value 0x80) */
#define VA_ADC_MUX0_CFG0 0x100u           /* DEC0's DMIC: bits 7:4 = n + 1 for DMIC n, 0 none */
#define VA_ADC_MUX0_CFG1 0x104u           /* DEC0's source: bit 0 = 0 the DMICs (1: SoundWire) */
#define VA_ADC_MUX1_CFG0 0x108u
#define VA_ADC_MUX1_CFG1 0x10cu
#define VA_TX_CTL(d) (0x400u + 0x80u * (d))      /* bits 3:0 the rate (4: 48 kHz), bit 4 mute, bit 5 the decimator's clock */
#define VA_TX_CFG0(d) (0x404u + 0x80u * (d))     /* bit 7 DMIC select, bits 6:5 the high-pass cut-off (2: 150 Hz), bits 2:1 ADC mode */
#define VA_TX_VOL(d) (0x40cu + 0x80u * (d))      /* the decimator's gain: signed dB (0 = 0 dB) */
#define VA_TX_GATE(d) (0x418u + 0x80u * (d))     /* bit 0 the high-pass zero gate off, bit 1 a cut-off change request */
#define VA_DMIC_DIV4 2u                   /* 9.6 MHz / 4 = 2.4 MHz */
#define VA_RATE_48K 4u
#define VA_HPF_150HZ 2u

/* The decimators' gain as Ubuntu's UCM gives it: a control of 0..124, 84 = 0 dB, 1 dB a step (124: +40 dB). */
#define AUDIO_VA_GAIN_DEFAULT 124u
#define AUDIO_VA_GAIN_MAX 124u
uint8_t audio_va_gain_register(unsigned gain);

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, uint32_t offset);
    void (*write)(void *ctx, uint32_t offset, uint32_t value);
    void (*sleep_us)(void *ctx, uint32_t us);
} AudioVaIo;
#define AUDIO_MIC_ROUTE_LEGACY (-1)
static inline int audio_mic_route_valid(int route)
{
    return route == AUDIO_MIC_ROUTE_LEGACY || route == 0 || route == 2 || route == 4 || route == 6;
}
/* The accepted boot spelling is exactly one of the signed route values. */
static inline int audio_mic_route_parse(const char *text, int *route)
{
    int value;
    if (text[0] == '-' && text[1] == '1' && !text[2]) value = -1;
    else if (text[0] >= '0' && text[0] <= '9' && !text[1]) value = text[0] - '0';
    else return 0;
    if (!audio_mic_route_valid(value)) return 0;
    *route = value;
    return 1;
}
static inline unsigned audio_mic_dec0(int route) {return route == AUDIO_MIC_ROUTE_LEGACY ? 0u : (unsigned)route;}
static inline const char *audio_mic_mono_name(int route) {return route == AUDIO_MIC_ROUTE_LEGACY ? "mean" : "DEC0";}

typedef struct {
    uint32_t mux[2], src[2], tx[2], cfg[2], ctl[4];
    uint32_t mclk, fs_count, fs_broadcast;
} AudioVaReadback;
unsigned audio_va_pair_mask(int route);
/* Returns -1 for invalid route or failed MCLK, -2 for failed route readback
 * after cleanup, -3 if cleanup readback also failed. */
int audio_va_on(const AudioVaIo *io, unsigned gain, int route, AudioVaReadback *rb);
int audio_va_off(const AudioVaIo *io, int route, AudioVaReadback *rb);

/* At most two distinct clock/data pin pairs. The caller maps every selected
 * page before changing pins and retains all mappings through restore. */
#define AUDIO_VA_PINS 4u
typedef struct {
    unsigned gpio;
    uint32_t cfg;
    int drive;
} AudioVaPin;
typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, unsigned pin, unsigned offset);
    void (*write)(void *ctx, unsigned pin, unsigned offset, uint32_t value);
} AudioVaPinIo;
typedef struct {
    uint32_t old_cfg, old_value, cfg, value;
} AudioVaPinState;

unsigned audio_va_pins(int route, AudioVaPin pins[AUDIO_VA_PINS]);
unsigned audio_va_pin_apply(const AudioVaPinIo *io, const AudioVaPin *pins, AudioVaPinState *state, unsigned count);
unsigned audio_va_pin_restore(const AudioVaPinIo *io, AudioVaPinState *state, unsigned count);

#ifdef __cplusplus
}
#endif
#endif
