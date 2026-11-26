// SPDX-License-Identifier: BSD-2-Clause-Patent
// Version 1 of machdep.native_gop_display_shot: header followed by tightly packed little-endian pixels.
// format is the IOSurface FourCC for a flip, or BGRA for the boot framebuffer.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/errno.h>

#define GOP_SHOT_SYSCTL "machdep.native_gop_display_shot"
#define GOP_SHOT_VERSION 1u
#define GOP_SHOT_WIDTH 3200u
#define GOP_SHOT_HEIGHT 2136u
#define GOP_SHOT_ROW_BYTES (GOP_SHOT_WIDTH * 4u)
#define GOP_SHOT_CHUNK_BYTES 65536u
#define GOP_SHOT_BGRA8 0x42475241u        // 'BGRA', 8 bits per channel
#define GOP_SHOT_BGA 0x26424741u          // '&BGA', linear plane 0 with BGRA8 pixels
#define GOP_SHOT_ARGB2101010 0x6c313072u  // 'l10r', full-range little-endian ARGB2101010

enum { GOP_SHOT_BOOT, GOP_SHOT_FLIP };

typedef struct {
    uint32_t version, width, height, format, source;
} GopShotHeader;

#define GOP_SHOT_BYTES (sizeof(GopShotHeader) + GOP_SHOT_ROW_BYTES * GOP_SHOT_HEIGHT)

static inline int gop_shot_access(int enabled, int root, int writing)
{
    if (!enabled)
        return ENOENT;
    return root && !writing ? 0 : EPERM;
}

static inline int gop_shot_bounds(int mapped, uint64_t length, uint64_t offset, uint64_t stride)
{
    if (!mapped)
        return ENXIO;
    if (stride < GOP_SHOT_ROW_BYTES || stride > UINT16_MAX || offset > length)
        return EINVAL;
    const uint64_t bytes = (GOP_SHOT_HEIGHT - 1) * stride + GOP_SHOT_ROW_BYTES;
    return bytes <= length - offset ? 0 : EINVAL;
}

// Walk the visible rows. A padded source never exposes padding, even on the last row.
static inline size_t gop_shot_chunk(size_t position, uint64_t stride, uint64_t *source_offset)
{
    const size_t bytes = GOP_SHOT_ROW_BYTES * GOP_SHOT_HEIGHT;
    if (position >= bytes)
        return 0;
    const size_t column = position % GOP_SHOT_ROW_BYTES;
    *source_offset = (position / GOP_SHOT_ROW_BYTES) * stride + column;
    const size_t contiguous = stride == GOP_SHOT_ROW_BYTES ? bytes - position : GOP_SHOT_ROW_BYTES - column;
    return contiguous < GOP_SHOT_CHUNK_BYTES ? contiguous : GOP_SHOT_CHUNK_BYTES;
}
