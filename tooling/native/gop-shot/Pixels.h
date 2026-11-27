// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "../../../package/NativeGopDisplay/GopShot.h"

typedef enum {
    GOP_SHOT_LAYOUT_AUTO,
    GOP_SHOT_LAYOUT_BGRA8,
    GOP_SHOT_LAYOUT_PACKED10,
} GopShotLayout;

static inline uint8_t gop_shot_srgb8(uint32_t sample)
{
    return (uint8_t)((sample * 255u + 511u) / 1023u);
}

// Input is little-endian B:G:R:A in memory (8:8:8:8 or 10:10:10:2).
// Output is RGBX. Scanout is opaque; the stored alpha bits do not affect the still.
static inline void gop_shot_rgbx(uint8_t pixel[4], int packed10)
{
    const uint8_t blue = pixel[0], green = pixel[1], red = pixel[2];
    if (packed10) {
        const uint32_t word = blue | ((uint32_t)green << 8) | ((uint32_t)red << 16) | ((uint32_t)pixel[3] << 24);
        pixel[0] = gop_shot_srgb8((word >> 20) & 1023u);
        pixel[1] = gop_shot_srgb8((word >> 10) & 1023u);
        pixel[2] = gop_shot_srgb8(word & 1023u);
    } else {
        pixel[0] = red;
        pixel[2] = blue;
    }
    pixel[3] = 255;
}

static inline int gop_shot_decode_reply(GopShotHeader *frame, size_t size, GopShotLayout override_layout,
                                       GopShotLayout *layout)
{
    if (size != GOP_SHOT_BYTES)
        return EPROTO;
    if (!frame || !layout || (override_layout != GOP_SHOT_LAYOUT_AUTO &&
        override_layout != GOP_SHOT_LAYOUT_BGRA8 && override_layout != GOP_SHOT_LAYOUT_PACKED10))
        return EINVAL;
    if (frame->version != GOP_SHOT_VERSION ||
        frame->width != GOP_SHOT_WIDTH || frame->height != GOP_SHOT_HEIGHT ||
        (frame->source != GOP_SHOT_FLIP && (frame->source != GOP_SHOT_BOOT || frame->format != GOP_SHOT_BGRA8)))
        return EPROTO;

    GopShotLayout declared_layout;
    switch (frame->format) {
    case GOP_SHOT_BGRA8:
    case GOP_SHOT_BGA:
        declared_layout = GOP_SHOT_LAYOUT_BGRA8;
        break;
    case GOP_SHOT_ARGB2101010:
        declared_layout = GOP_SHOT_LAYOUT_PACKED10;
        break;
    default:
        return EPROTO;
    }

    *layout = override_layout == GOP_SHOT_LAYOUT_AUTO ? declared_layout : override_layout;
    const int packed10 = *layout == GOP_SHOT_LAYOUT_PACKED10;
    uint8_t *pixels = (uint8_t *)(frame + 1);
    for (size_t position = 0; position < size - sizeof *frame; position += 4)
        gop_shot_rgbx(pixels + position, packed10);
    return 0;
}
