/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * The Android/macOS boot flag of piano's combined boot_a (T-16 UEFI OS switch). Plain C99, no library calls: the selector (before
 * UEFI, MMU off), the FD (UniXNULib), a kext (C++) and the host tests include it.
 *
 * Storage: one byte, SID 0 SDAM 0x7100 (PPID 0x071) offset 0x49, next to ABL's restart reason at 0x48. The byte is a CANDIDATE
 * until the read-only survey shows it reads 0x00 across boots (private/host-checks/20261123-switcher/UEFI-SWITCH-DESIGN.md §2).
 *
 *   0x00              never written (the survey baseline): default macOS, no one-shot
 *   0xA0 | bits       ours: bit 0 ONCE_ANDROID, bit 1 ONCE_MACOS, bit 2 DEFAULT_ANDROID, bit 3 must be 0, and at most one ONCE bit
 *   anything else     not ours: never written by any of our writers; the selector boots macOS
 *
 * A writer only writes over 0x00 or one of our values. The selector consumes a ONCE bit (clears it, reads it back) before it jumps;
 * a value that is ours with no bit left is stored as 0x00, so the byte is back at its baseline whenever nothing is pending.
 */
#ifndef PIANO_BOOTFLAG_H
#define PIANO_BOOTFLAG_H

#define PIANO_BOOTFLAG_PPID		0x071u	/* SID 0, SDAM 0x7100 */
#define PIANO_BOOTFLAG_OFFSET		0x49u
#define PIANO_BOOTFLAG_MAGIC		0xA0u
#define PIANO_BOOTFLAG_MAGIC_MASK	0xF8u	/* magic nibble + reserved bit 3 */
#define PIANO_BOOTFLAG_ONCE_ANDROID	0x01u
#define PIANO_BOOTFLAG_ONCE_MACOS	0x02u
#define PIANO_BOOTFLAG_DEFAULT_ANDROID	0x04u
#define PIANO_BOOTFLAG_ONCE_MASK	0x03u

/* What starts. */
enum piano_target {
	PIANO_TARGET_STOCK = 0,		/* the stock kernel, ABL's own path (recovery, off-mode charging, no UEFI payload, guards) */
	PIANO_TARGET_ANDROID = 1,	/* the stock kernel because the flag or the key asked for Android */
	PIANO_TARGET_MACOS = 2,		/* the UEFI payload (our FD), which boots macOS from the ESP */
	PIANO_TARGET_FASTBOOT = 3	/* restart reason bootloader + PSCI reset: ABL fastboot */
};

/* 1 when the byte is ours (or the 0x00 baseline), 0 when another writer owns it. */
static inline int piano_bootflag_ours(unsigned v)
{
	if (v == 0)
		return 1;
	if ((v & PIANO_BOOTFLAG_MAGIC_MASK) != PIANO_BOOTFLAG_MAGIC)
		return 0;
	return (v & PIANO_BOOTFLAG_ONCE_MASK) != PIANO_BOOTFLAG_ONCE_MASK;
}

/* The stored form of a set of bits: 0x00 when nothing is set. */
static inline unsigned piano_bootflag_encode(unsigned bits)
{
	bits &= PIANO_BOOTFLAG_ONCE_MASK | PIANO_BOOTFLAG_DEFAULT_ANDROID;
	return bits ? (PIANO_BOOTFLAG_MAGIC | bits) : 0u;
}

static inline int piano_bootflag_default_android(unsigned v)
{
	return piano_bootflag_ours(v) && (v & PIANO_BOOTFLAG_DEFAULT_ANDROID);
}

/* Requests of the writers (FD menu, macOS sysctl, Android module). */
enum piano_bootflag_req {
	PIANO_REQ_ONCE_ANDROID = 1,	/* next boot Android (a pending ONCE_MACOS is replaced) */
	PIANO_REQ_ONCE_MACOS = 2,	/* next boot macOS */
	PIANO_REQ_ONCE_CLEAR = 3,	/* drop a pending one-shot */
	PIANO_REQ_DEFAULT_ANDROID = 4,	/* persistent default Android */
	PIANO_REQ_DEFAULT_MACOS = 5,	/* persistent default macOS (the factory state) */
	PIANO_REQ_RESET = 6		/* everything back to 0x00 */
};

/*
 * The new byte for a request on the byte read now. Returns 0 and sets *out, or -1 when the byte is not ours (the writer must not
 * write) or the request is unknown. A writer then writes *out (only when it differs) and reads it back.
 */
static inline int piano_bootflag_apply(unsigned old, int req, unsigned *out)
{
	unsigned bits;

	if (!piano_bootflag_ours(old))
		return -1;
	bits = old ? (old & (PIANO_BOOTFLAG_ONCE_MASK | PIANO_BOOTFLAG_DEFAULT_ANDROID)) : 0u;
	switch (req) {
	case PIANO_REQ_ONCE_ANDROID:
		bits = (bits & ~PIANO_BOOTFLAG_ONCE_MASK) | PIANO_BOOTFLAG_ONCE_ANDROID;
		break;
	case PIANO_REQ_ONCE_MACOS:
		bits = (bits & ~PIANO_BOOTFLAG_ONCE_MASK) | PIANO_BOOTFLAG_ONCE_MACOS;
		break;
	case PIANO_REQ_ONCE_CLEAR:
		bits &= ~PIANO_BOOTFLAG_ONCE_MASK;
		break;
	case PIANO_REQ_DEFAULT_ANDROID:
		bits |= PIANO_BOOTFLAG_DEFAULT_ANDROID;
		break;
	case PIANO_REQ_DEFAULT_MACOS:
		bits &= ~PIANO_BOOTFLAG_DEFAULT_ANDROID;
		break;
	case PIANO_REQ_RESET:
		bits = 0;
		break;
	default:
		return -1;
	}
	*out = piano_bootflag_encode(bits);
	return 0;
}

/* The selector's inputs, all read before any decision. */
struct piano_sel_in {
	int can_run;		/* EL1 with the MMU off */
	int bootmode_stock;	/* ABL's bootargs carry bootmonitor.bootmode=recovery or =poweroff_cg */
	int have_uefi;		/* the image carries the UEFI payload */
	int key_down;		/* vol-down held (debounced) */
	int key_up;		/* vol-up held (debounced) */
	int flag_ok;		/* the flag byte was read */
	unsigned flag;		/* its value */
};

/* The decision. *write_flag = 1 with *new_flag when the selector must consume a one-shot before it jumps. */
struct piano_sel_out {
	enum piano_target target;
	int write_flag;
	unsigned new_flag;
	enum piano_target if_write_fails;	/* the target when the write or its read-back fails */
};

/*
 * Order (UEFI-SWITCH-DESIGN.md §3):
 *   1. guards (not EL1 / MMU on) -> stock; ABL's recovery or off-mode-charging boot -> stock; no UEFI payload -> stock.
 *   2. vol-down -> ABL fastboot (a second road; ABL's own vol-down at power-on is before us and unchanged).
 *   3. the flag unreadable or not ours -> macOS, nothing written.
 *   4. a ONCE bit picks the system, else the DEFAULT bit; vol-up -> the other system than that.
 *   5. a ONCE bit is consumed (written back without it); if that write fails -> macOS.
 */
static inline struct piano_sel_out piano_select(const struct piano_sel_in *in)
{
	struct piano_sel_out o;
	unsigned v = in->flag;
	int android;

	o.target = PIANO_TARGET_MACOS;
	o.write_flag = 0;
	o.new_flag = 0;
	o.if_write_fails = PIANO_TARGET_MACOS;
	if (!in->can_run || in->bootmode_stock || !in->have_uefi) {
		o.target = PIANO_TARGET_STOCK;
		return o;
	}
	if (in->key_down) {
		o.target = PIANO_TARGET_FASTBOOT;
		return o;
	}
	if (!in->flag_ok || !piano_bootflag_ours(v)) {
		/* fail-safe; vol-up still reaches Android */
		o.target = in->key_up ? PIANO_TARGET_ANDROID : PIANO_TARGET_MACOS;
		return o;
	}
	if (v & PIANO_BOOTFLAG_ONCE_ANDROID)
		android = 1;
	else if (v & PIANO_BOOTFLAG_ONCE_MACOS)
		android = 0;
	else
		android = (v & PIANO_BOOTFLAG_DEFAULT_ANDROID) != 0;
	if (in->key_up)
		android = !android;
	o.target = android ? PIANO_TARGET_ANDROID : PIANO_TARGET_MACOS;
	if (v && (v & PIANO_BOOTFLAG_ONCE_MASK)) {
		o.write_flag = 1;
		o.new_flag = piano_bootflag_encode(v & PIANO_BOOTFLAG_DEFAULT_ANDROID);
	}
	return o;
}

#endif
