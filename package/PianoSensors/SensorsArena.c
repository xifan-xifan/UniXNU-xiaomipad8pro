/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The arena's accesses: see SensorsArena.h. */
#include "SensorsArena.h"

#ifdef SAR_CHECK
#define ENTER() sar_enter()
#define LEAVE() sar_leave()
#define ACCESS(p, n) sar_access((p), (n))
#else
#define ENTER() do {} while (0)
#define LEAVE() do {} while (0)
#define ACCESS(p, n) do {} while (0)
#endif

static uint8_t load8(const uint8_t *p)
{
    const volatile uint8_t *v = (const volatile uint8_t *)p;
    ACCESS(v, 1);
    return *v;
}
static void store8(uint8_t *p, uint8_t value)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    ACCESS(v, 1);
    *v = value;
}
/* p is 4 (8) aligned */
static uint32_t load32(const uint8_t *p)
{
    const volatile uint32_t *v = (const volatile uint32_t *)(const volatile void *)p;
    ACCESS(v, 4);
    return *v;
}
static void store32(uint8_t *p, uint32_t value)
{
    volatile uint32_t *v = (volatile uint32_t *)(volatile void *)p;
    ACCESS(v, 4);
    *v = value;
}
static uint64_t load64(const uint8_t *p)
{
    const volatile uint64_t *v = (const volatile uint64_t *)(const volatile void *)p;
    ACCESS(v, 8);
    return *v;
}
static void store64(uint8_t *p, uint64_t value)
{
    volatile uint64_t *v = (volatile uint64_t *)(volatile void *)p;
    ACCESS(v, 8);
    *v = value;
}

static uint64_t word_of(const uint8_t *from)   /* normal memory, any alignment, little-endian */
{
    uint64_t w = 0;
    for (unsigned i = 0; i < 8; ++i) w |= (uint64_t)from[i] << (8 * i);
    return w;
}

void sar_read(uint8_t *to, const uint8_t *arena, uint32_t n)
{
    uint32_t i = 0;
    ENTER();
    for (; i < n && ((uintptr_t)(arena + i) & 7); ++i) to[i] = load8(arena + i);
    for (; n - i >= 8; i += 8) {
        const uint64_t w = load64(arena + i);
        for (unsigned b = 0; b < 8; ++b) to[i + b] = (uint8_t)(w >> (8 * b));
    }
    for (; i < n; ++i) to[i] = load8(arena + i);
    LEAVE();
}

void sar_write(uint8_t *arena, const uint8_t *from, uint32_t n)
{
    uint32_t i = 0;
    ENTER();
    for (; i < n && ((uintptr_t)(arena + i) & 7); ++i) store8(arena + i, from[i]);
    for (; n - i >= 8; i += 8) store64(arena + i, word_of(from + i));
    for (; i < n; ++i) store8(arena + i, from[i]);
    LEAVE();
}

void sar_zero(uint8_t *arena, uint32_t n)
{
    uint32_t i = 0;
    ENTER();
    for (; i < n && ((uintptr_t)(arena + i) & 7); ++i) store8(arena + i, 0);
    for (; n - i >= 8; i += 8) store64(arena + i, 0);
    for (; i < n; ++i) store8(arena + i, 0);
    LEAVE();
}

uint32_t sar_get32(const uint8_t *arena)
{
    uint32_t v = 0;
    ENTER();
    if (!((uintptr_t)arena & 3)) v = load32(arena);
    else for (unsigned i = 0; i < 4; ++i) v |= (uint32_t)load8(arena + i) << (8 * i);
    LEAVE();
    return v;
}

uint64_t sar_get64(const uint8_t *arena)
{
    uint64_t v = 0;
    ENTER();
    if (!((uintptr_t)arena & 7)) v = load64(arena);
    else for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)load8(arena + i) << (8 * i);
    LEAVE();
    return v;
}

void sar_put32(uint8_t *arena, uint32_t value)
{
    ENTER();
    if (!((uintptr_t)arena & 3)) store32(arena, value);
    else for (unsigned i = 0; i < 4; ++i) store8(arena + i, (uint8_t)(value >> (8 * i)));
    LEAVE();
}

void sar_put64(uint8_t *arena, uint64_t value)
{
    ENTER();
    if (!((uintptr_t)arena & 7)) store64(arena, value);
    else for (unsigned i = 0; i < 8; ++i) store8(arena + i, (uint8_t)(value >> (8 * i)));
    LEAVE();
}
