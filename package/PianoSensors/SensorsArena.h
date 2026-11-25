/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The only accesses to the file service's arena (SensorsRpc.h): the memory the DSP reads and writes, below 4 GiB, mapped uncached.
 * On the tablet that mapping takes an alignment fault for an unaligned access, as device memory does (s1b, 2026-11-14: the DSP's
 * first call, remotectl open "apps_std", has its out-buffer lengths at blob offset 0x21; clang had merged a byte-wise get32 into one
 * ldr w10, [x8, x11] there; esr 0x96000021). Whether the mapping or the hypervisor's stage 2 makes it so is not known; these accesses
 * are safe either way: every one is a single byte or a naturally aligned 4- or 8-byte word, through volatile pointers, so the compiler
 * can neither merge nor widen them, nor turn a loop into memcpy or memset (whose block forms, dc zva or unaligned ldp/stp, fault there
 * too). Callers copy what they need between the arena and normal memory and work on the copy. Little-endian (AArch64, and the hosts the
 * tests run on). Portable C, no C library; host-tested with every access checked for alignment and for the arena (tests/bringup.c:
 * SAR_CHECK, the arena unreadable outside these functions). */
#ifndef SENSORS_ARENA_H
#define SENSORS_ARENA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void sar_read(uint8_t *to, const uint8_t *arena, uint32_t n);     /* to: normal memory */
void sar_write(uint8_t *arena, const uint8_t *from, uint32_t n);  /* from: normal memory */
void sar_zero(uint8_t *arena, uint32_t n);
uint32_t sar_get32(const uint8_t *arena);
uint64_t sar_get64(const uint8_t *arena);
void sar_put32(uint8_t *arena, uint32_t value);
void sar_put64(uint8_t *arena, uint64_t value);

#ifdef SAR_CHECK
/* Host tests only: each function brackets its accesses with enter/leave and reports each one (address, bytes) before it is made. */
void sar_enter(void);
void sar_leave(void);
void sar_access(const volatile void *address, unsigned bytes);
#endif

#ifdef __cplusplus
}
#endif
#endif
