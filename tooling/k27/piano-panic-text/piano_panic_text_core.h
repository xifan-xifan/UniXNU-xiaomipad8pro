/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Leo Zhang. All rights reserved.
 * Panic-only, bounded text selection and BGRA rendering. No kernel services.
 */
#ifndef PIANO_PANIC_TEXT_CORE_H
#define PIANO_PANIC_TEXT_CORE_H
#include <stdint.h>
#include <stddef.h>
#define PPT_BYTES 32768u
#define PPT_COLS 78u
#define PPT_ROWS 24u
struct ppt_capture { uint32_t used, active; char text[PPT_BYTES]; };
struct ppt_page { unsigned x, y; char cells[PPT_ROWS][PPT_COLS]; };
static inline void ppt_put(struct ppt_page *p, char c)
{
    if (p->y >= PPT_ROWS) return;
    if (c == '\r') return;
    if (c == '\n') { p->x = 0; ++p->y; return; }
    if (c == '\t') c = ' ';
    if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
    p->cells[p->y][p->x++] = c;
    if (p->x == PPT_COLS) { p->x = 0; ++p->y; }
}
static inline void ppt_string(struct ppt_page *p, const char *s)
{ while (*s) ppt_put(p, *s++); }
static inline int ppt_has(const char *s, unsigned n, const char *key)
{
    for (unsigned i = 0; i < n; ++i) {
        unsigned k = 0;
        while (key[k] && i + k < n && s[i + k] == key[k]) ++k;
        if (!key[k]) return 1;
    }
    return 0;
}
static inline void ppt_line(struct ppt_page *p, const char *s, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) ppt_put(p, s[i]);
    if (p->x) ppt_put(p, '\n');
}
static inline void ppt_select(struct ppt_page *p, const char *s, unsigned n)
{
    unsigned start = 0, frames = 0, panic_rows = 0;
    int in_panic = 0, found = 0, in_backtrace = 0;
    for (unsigned y = 0; y < PPT_ROWS; ++y)
        for (unsigned x = 0; x < PPT_COLS; ++x) p->cells[y][x] = ' ';
    p->x = p->y = 0;
    ppt_string(p, "PIANO PANIC TEXT v1 - 15 SECOND HOLD\n");
    for (unsigned end = 0; end <= n; ++end) {
        if (end < n && s[end] != '\n') continue;
        unsigned len = end - start;
        const char *line = s + start;
        if (ppt_has(line, len, "panic(cpu")) { in_panic = found = 1; }
        if (ppt_has(line, len, "Panicked thread:") && ppt_has(line, len, "backtrace:")) in_backtrace = 1;
        int frame = in_backtrace && ppt_has(line, len, "lr: 0x") && ppt_has(line, len, "fp: 0x");
        if (frame && frames < 12) {
            if (!frames) { if (p->y < 8) p->y = 8; p->x = 0; ppt_string(p, "FIRST 12 BACKTRACE FRAMES (LR / FP)\n"); }
            ppt_line(p, line, len); ++frames; in_panic = 0;
        } else if (in_panic && panic_rows < 5 && p->y < 6) {
            /* Stop at metadata, preserving wrapped panic/register detail. */
            if (ppt_has(line, len, "Debugger message:") || ppt_has(line, len, "Memory ID:")) in_panic = 0;
            else {
                unsigned room = (6 - p->y) * PPT_COLS - p->x;
                ppt_line(p, line, len < room ? len : room);
                ++panic_rows;
            }
        } else if (!frames && (ppt_has(line, len, "Kernel slide:") || ppt_has(line, len, "Kernel text base:") ||
                              ppt_has(line, len, "KernelCache slide:") || ppt_has(line, len, "KernelCache base:"))) {
            if (p->y < 8) ppt_line(p, line, len);
        }
        start = end + 1;
    }
    if (!found) { p->x = 0; p->y = 1; ppt_string(p, "PANIC STRING MISSING - CHANNEL NOT QUALIFIED\n"); }
    if (!frames) { p->x = 0; p->y = 9; ppt_string(p, "BACKTRACE MISSING - CHANNEL NOT QUALIFIED\n"); }
    p->x = 0; p->y = PPT_ROWS - 1;
    ppt_string(p, n == PPT_BYTES ? "CAPTURE FULL - TRUNCATED" : "END PIANO PANIC TEXT v1");
}
/* Deliberately accepts only this diagnostic line's non-flipping scanout. Font is
 * XNU's existing 256 x 16-byte iso_font, passed by its owning translation unit.
 */
static inline int ppt_render(volatile uint32_t *fb, unsigned width, unsigned height,
                             unsigned stride, unsigned depth, const unsigned char *font,
                             const struct ppt_page *p)
{
    const unsigned scale = 5, left = 40, top = 40;
    if (!fb || width != 3200 || height != 2136 || stride != 12800 || depth != 32) return 0;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) fb[y * (stride / 4) + x] = 0x00101010;
    for (unsigned row = 0; row < PPT_ROWS; ++row)
        for (unsigned col = 0; col < PPT_COLS; ++col) {
            unsigned c = (unsigned char)p->cells[row][col];
            for (unsigned gy = 0; gy < 16; ++gy)
                for (unsigned gx = 0; gx < 8; ++gx) {
                    uint32_t pixel = font[c * 16 + gy] & (1u << gx) ? 0x00ffffff : 0x00101010;
                    for (unsigned dy = 0; dy < scale; ++dy)
                        for (unsigned dx = 0; dx < scale; ++dx)
                            fb[(top + row * 80 + gy * scale + dy) * (stride / 4) +
                               left + col * 40 + gx * scale + dx] = pixel;
                }
        }
    return 1;
}
#endif
