/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * piano's pre-UEFI OS selector (T-16 UEFI OS switch). It sits after the stock kernel's memory footprint in the combined boot_a;
 * pack-selector.py redirects the kernel's second instruction (its branch to primary_entry) here. ABL's state arrives untouched:
 * x0 = FDT, x1..x3 = 0, MMU and D-cache off, EL1. The selector saves x0..x3, sp and DAIF, decides (bootflag.h piano_select), and
 * restores everything before it branches to the stock kernel's primary_entry or to the UEFI payload (BootShim + FD, started like
 * a Linux Image). Nothing in the kernel image is written at run time.
 *
 * The entry/metadata contract and the packer layout follow piano-uefi's selector (BSD-2-Clause-Patent, NOTICE.md); the key,
 * SDAM and arbiter code is ours, from package/PianoInput/Buttons.h, package/PianoPower/SpmiObserver.h, package/include/PianoSpmi.h
 * and PianoDwc3's setRestartReason.
 *
 * One translation unit with no relocations: built with -mcmodel=tiny, every object static and in .text, so the assembler resolves
 * every reference PC-relative (the Makefile refuses an object with a relocation). No linker is needed.
 */
#include "bootflag.h"
#include "selector_fdt.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

/* ---- entry ------------------------------------------------------------------------------------------------------------- */
__asm__(
	".section .text,\"ax\",%progbits\n"
	"_start:\n"
	"	mov	x19, x0\n"		/* FDT */
	"	mov	x20, x1\n"
	"	mov	x21, x2\n"
	"	mov	x22, x3\n"
	"	mov	x23, sp\n"
	"	mrs	x24, daif\n"
	"	msr	daifset, #15\n"
	"	adr	x25, _start\n"
	"	adr	x26, piano_sel_meta\n"
	"	ldr	x9, [x26, #16]\n"	/* selector_offset */
	"	sub	x25, x25, x9\n"		/* kernel base */
	"	adr	x9, piano_sel_stack_end\n"
	"	mov	sp, x9\n"
	"	mov	x0, x19\n"
	"	mov	x1, x26\n"
	"	mov	x2, x25\n"
	"	bl	piano_sel_main\n"	/* 0 = stock kernel, 1 = UEFI payload */
	"	cbz	w0, 1f\n"
	"	ldr	x9, [x26, #40]\n"	/* uefi_offset from the kernel base */
	"	add	x9, x25, x9\n"
	"	b	2f\n"
	"1:	ldr	w8, [x26, #24]\n"	/* the original code1: b primary_entry */
	"	sbfx	x8, x8, #0, #26\n"
	"	add	x9, x25, #4\n"
	"	add	x9, x9, x8, lsl #2\n"
	"2:	mov	sp, x23\n"
	"	mov	x0, x19\n"
	"	mov	x1, x20\n"
	"	mov	x2, x21\n"
	"	mov	x3, x22\n"
	"	msr	daif, x24\n"
	"	isb\n"
	"	br	x9\n"
	/* Filled in by pack-selector.py (layout checked there). */
	"	.balign 16\n"
	"piano_sel_meta:\n"
	"	.ascii	\"PIANO-UEFI-SEL1\\0\"\n"	/* 0: magic */
	"	.quad	0\n"			/* 16: selector_offset */
	"	.word	0\n"			/* 24: original code1 */
	"	.word	0\n"			/* 28: original image_size (low) */
	"	.quad	0\n"			/* 32: original image_size */
	"	.quad	0\n"			/* 40: uefi_offset, 0 if none */
	"	.quad	0\n"			/* 48: uefi_size */
	"	.quad	0\n"			/* 56: original kernel file size */
	"	.quad	0\n"			/* 64: esp_offset (unused, 0) */
	"	.quad	0\n"			/* 72: esp_size (unused, 0) */
	"	.ascii	\"PIANO-SW-SEL-V1\\0\"\n"	/* 80: our version tag (pack-selector.py checks it) */
	"want_rec:	.asciz	\"bootmonitor.bootmode=recovery\"\n"
	"want_cg:	.asciz	\"bootmonitor.bootmode=poweroff_cg\"\n"
	"	.balign 16\n"
	"piano_sel_stack:\n"
	"	.zero	8192\n"
	"piano_sel_stack_end:\n"
	".previous\n");

struct piano_meta {
	char magic[16];
	u64 selector_offset;
	u32 code1, image_size_lo;
	u64 image_size, uefi_offset, uefi_size;
	u64 kernel_size, esp_offset, esp_size;
};

/* ---- MMIO, timer ------------------------------------------------------------------------------------------------------- */
static inline u32 rd32(u64 a) { return *(volatile u32 *)a; }
static inline void wr32(u64 a, u32 v) { *(volatile u32 *)a = v; }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static u64 ticks(void)
{
	u64 t;
	__asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void delay_us(u32 us)
{
	u64 f, end;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	end = ticks() + (f / 1000000u) * us + 1;
	while (ticks() < end)
		;
}

/* ---- SPMI arbiter v7, EE 0 (SpmiObserver.h / PianoSpmi.h facts) -------------------------------------------------------- */
#define SPMI_CORE	0x0C400000UL
#define SPMI_CNFG	0x0C42D000UL
#define SPMI_OBSV	0x0C440000UL
#define SPMI_CHNL	0x0C500000UL
#define CH_CMD		0x00
#define CH_STATUS	0x08
#define CH_WDATA0	0x10
#define CH_RDATA0	0x18
#define ST_DONE		0x1u
#define ST_ERRORS	0xEu
#define OP_EXT_WRITEL	0u
#define OP_EXT_READL	1u

struct apid { int apid; u32 owner; };

static int spmi_ok(void)
{
	u32 v = rd32(SPMI_CORE), n = rd32(SPMI_CORE + 4) & 0x7FFu;
	return (v >> 28) == 7 && n && n <= 1024;
}

/* The APID of a PPID; an EE-0-owned entry wins. */
static struct apid spmi_find(u16 ppid)
{
	struct apid r = { -1, 0 };
	u32 n = rd32(SPMI_CORE + 4) & 0x7FFu, i;

	for (i = 0; i < n; i++) {
		u32 map = rd32(SPMI_CORE + 0x2000 + 4 * i), owner;
		if (!map || ((map >> 8) & 0xFFFu) != ppid)
			continue;
		owner = rd32(SPMI_CNFG + 4 * i) & 7u;
		if (r.apid < 0 || (owner == 0 && r.owner != 0)) {
			r.apid = (int)i;
			r.owner = owner;
		}
	}
	return r;
}

static int ch_poll(u64 ch)
{
	u32 i, st = 0;
	for (i = 0; i < 1000; i++) {
		st = rd32(ch + CH_STATUS);
		if (st & ST_DONE)
			return (st & ST_ERRORS) ? -1 : 0;
		delay_us(1);
	}
	for (i = 0; i < 5000 && !(rd32(ch + CH_STATUS) & ST_DONE); i++)
		delay_us(1);
	return -2;
}

/* One-byte observer read, up to 5 tries 500 us apart. */
static int spmi_read8(int apid, u8 reg, u8 *val)
{
	u64 ch = SPMI_OBSV + 0x20UL * (u32)apid;
	int t, r = -1;

	if (apid < 0)
		return -1;
	for (t = 0; t < 5; t++) {
		wr32(ch + CH_CMD, OP_EXT_READL << 27 | (u32)reg << 4);
		dsb();
		r = ch_poll(ch);
		if (!r) {
			*val = (u8)rd32(ch + CH_RDATA0);
			return 0;
		}
		delay_us(500);
	}
	return r;
}

/* One-byte owner-channel write; the APID must be EE 0's. Not retried. */
static int spmi_write8(struct apid a, u8 reg, u8 val)
{
	u64 ch = SPMI_CHNL + 0x1000UL * (u32)a.apid;

	if (a.apid < 0 || a.owner != 0)
		return -1;
	wr32(ch + CH_WDATA0, val);
	wr32(ch + CH_CMD, OP_EXT_WRITEL << 27 | (u32)reg << 4);
	dsb();
	return ch_poll(ch);
}

/* ---- keys (Buttons.h): PON 0x013 INT_RT_STS bit 7 power, bit 6 vol-down (high); GPIO6 0x18D bit 0 vol-up (low) --------- */
#define KEY_UP		1u
#define KEY_DOWN	2u

static u32 keys_sample(int pon, int gpio)
{
	u8 p = 0, g = 1;
	u32 k = 0;

	if (pon >= 0 && !spmi_read8(pon, 0x10, &p) && (p & 0x40))
		k |= KEY_DOWN;
	if (gpio >= 0 && !spmi_read8(gpio, 0x10, &g) && !(g & 0x01))
		k |= KEY_UP;
	return k;
}

/* A key counts when every one of 4 samples 10 ms apart reads it held (a failed read counts as up). */
static u32 keys_held(int pon, int gpio)
{
	u32 k = KEY_UP | KEY_DOWN, i;
	for (i = 0; i < 4; i++) {
		if (i)
			delay_us(10000);
		k &= keys_sample(pon, gpio);
	}
	return k;
}

/* In the entry block above (local labels in .text, so the references need no relocation). */
extern const char want_rec[30], want_cg[33];

/* ---- guards, reset, record ------------------------------------------------------------------------------------------- */
static int can_run(void)
{
	u64 el, sctlr;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
	if (el != 4)
		return 0;
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	return !(sctlr & 1);
}

#define REASON_BOOTLOADER	0x02u
#define PSCI_SYSTEM_RESET	0x84000009UL

static void restart_bootloader(struct apid sdam)
{
	u8 old = 0;
	if (!spmi_read8(sdam.apid, 0x48, &old))
		(void)spmi_write8(sdam, 0x48, (u8)((old & 1u) | (REASON_BOOTLOADER << 1)));
	register u64 x0 __asm__("x0") = PSCI_SYSTEM_RESET;
	__asm__ volatile("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
	for (;;)
		__asm__ volatile("wfe");
}

/* ABL's splash framebuffer (cont_splash): clean its dirty lines before UEFI takes the display (piano-uefi's finding). */
#define FB_BASE		0xFC800000UL
#define FB_BYTES	0x02B00000UL
static void fb_clean(void)
{
	u64 ctr, line, a;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	line = 4UL << ((ctr >> 16) & 0xF);
	for (a = FB_BASE; a < FB_BASE + FB_BYTES; a += line)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	dsb();
}

/*
 * The decision record, for the RAM test: the last page of the FD's Piano_Log reservation (0xD7F00000 + 1 MiB), read with
 * `fastboot oem dump D7F00000 100000` from the FD. Written only on the UEFI path (the stock kernel owns that memory otherwise).
 */
#define SEL_RECORD	0xD7FFF000UL
#define SEL_MAGIC	0x4C45535753504950UL	/* "PIPSWSEL" */
static void record(const struct piano_sel_in *in, const struct piano_sel_out *o, int wrote, u32 flag_after)
{
	volatile u64 *r = (volatile u64 *)SEL_RECORD;
	r[1] = 1;	/* version */
	r[2] = (u64)in->flag_ok << 8 | in->flag;
	r[3] = (u64)(u32)o->target;
	r[4] = (u64)in->key_up | (u64)in->key_down << 1;
	r[5] = (u64)wrote << 32 | flag_after;
	r[0] = SEL_MAGIC;
	dsb();
}

/* ---- main -------------------------------------------------------------------------------------------------------------- */
/* static + used: a local symbol in .text, so the entry's bl to it needs no relocation. */
static __attribute__((used)) int piano_sel_main(void *fdt, const struct piano_meta *meta, u64 base)
{
	struct piano_sel_in in = { 0, 0, 0, 0, 0, 0, 0 };
	struct piano_sel_out o;
	struct apid sdam = { -1, 0 }, pon = { -1, 0 }, gpio = { -1, 0 };
	u8 v = 0, back = 0xFF;
	int wrote = 0;

	(void)base;
	in.can_run = can_run();
	if (in.can_run) {
		in.bootmode_stock = piano_fdt_has_bootarg(fdt, PIANO_FDT_MAX, want_rec, sizeof(want_rec) - 1) ||
			piano_fdt_has_bootarg(fdt, PIANO_FDT_MAX, want_cg, sizeof(want_cg) - 1);
		in.have_uefi = meta->uefi_offset != 0 && meta->uefi_size != 0;
	}
	if (in.can_run && !in.bootmode_stock && in.have_uefi && spmi_ok()) {
		pon = spmi_find(0x013);
		gpio = spmi_find(0x18D);
		sdam = spmi_find(PIANO_BOOTFLAG_PPID);
		u32 k = keys_held(pon.apid, gpio.apid);
		in.key_up = !!(k & KEY_UP);
		in.key_down = !!(k & KEY_DOWN);
		in.flag_ok = !spmi_read8(sdam.apid, PIANO_BOOTFLAG_OFFSET, &v);
		in.flag = v;
	}
	o = piano_select(&in);
	if (o.target == PIANO_TARGET_FASTBOOT)
		restart_bootloader(sdam);
	if (o.write_flag) {
		wrote = !spmi_write8(sdam, PIANO_BOOTFLAG_OFFSET, (u8)o.new_flag) &&
			!spmi_read8(sdam.apid, PIANO_BOOTFLAG_OFFSET, &back) && back == (u8)o.new_flag;
		if (!wrote)
			o.target = o.if_write_fails;
	}
	if (o.target == PIANO_TARGET_MACOS) {
		fb_clean();
		record(&in, &o, wrote, back);
		return 1;
	}
	return 0;
}
