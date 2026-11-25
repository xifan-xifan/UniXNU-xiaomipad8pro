/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
/* Bounded diagnostic text; never feeds a hardware/protocol decision. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#ifndef KERNEL
#include <stdio.h>
#endif
namespace pianoobserve {
struct Text {
    char *data; size_t capacity, used = 0; bool truncated = false;
    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (used >= capacity) {truncated = true; return;}
        va_list ap; va_start(ap, fmt);
        const int n = vsnprintf(data + used, capacity - used, fmt, ap); va_end(ap);
        if (n < 0 || size_t(n) >= capacity - used) {truncated = true; used = capacity - 1;}
        else used += size_t(n);
    }
    void hex(const uint8_t *p, unsigned n) {for (unsigned i = 0; i < n; ++i) add("%02x", p[i]);}
};
}
