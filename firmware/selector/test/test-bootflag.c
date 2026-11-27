/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host tests of bootflag.h: the codec over all 256 byte values, every writer request, and the selector decision over every input. */
#include <stdio.h>
#include <string.h>
#include "bootflag.h"
#include "selector_fdt.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const unsigned ours_values[] = { 0x00, 0xA1, 0xA2, 0xA4, 0xA5, 0xA6 };

static int in_list(unsigned v)
{
	unsigned i;
	for (i = 0; i < sizeof(ours_values) / sizeof(ours_values[0]); i++)
		if (ours_values[i] == v)
			return 1;
	return 0;
}

static void test_ours(void)
{
	unsigned v;
	/* 0xA0 is a valid encoding of "nothing" but no writer stores it; treat it as ours (it never leaves the byte in that state). */
	for (v = 0; v < 256; v++) {
		int want = in_list(v) || v == 0xA0;
		CHECK(piano_bootflag_ours(v) == want, "ours(%#x) = %d, want %d", v, piano_bootflag_ours(v), want);
	}
	CHECK(!piano_bootflag_ours(0xA3) && !piano_bootflag_ours(0xA7), "both once bits are not ours");
	CHECK(!piano_bootflag_ours(0xA8) && !piano_bootflag_ours(0xFF) && !piano_bootflag_ours(0x01), "foreign values");
}

static void test_apply(void)
{
	unsigned v, out;
	int req;
	for (v = 0; v < 256; v++) {
		for (req = 0; req <= 8; req++) {
			int r = piano_bootflag_apply(v, req, &out);
			if (!piano_bootflag_ours(v) || req < 1 || req > 6) {
				CHECK(r == -1, "apply(%#x, %d) must refuse", v, req);
				continue;
			}
			CHECK(r == 0, "apply(%#x, %d) refused", v, req);
			/* every stored value is ours, never 0xA0, and never has both once bits */
			CHECK(piano_bootflag_ours(out) && out != 0xA0, "apply(%#x, %d) = %#x not a stored form", v, req, out);
			CHECK(in_list(out), "apply(%#x, %d) = %#x outside the stored set", v, req, out);
			switch (req) {
			case PIANO_REQ_ONCE_ANDROID:
				CHECK(out & PIANO_BOOTFLAG_ONCE_ANDROID && !(out & PIANO_BOOTFLAG_ONCE_MACOS), "once android %#x", out);
				break;
			case PIANO_REQ_ONCE_MACOS:
				CHECK(out & PIANO_BOOTFLAG_ONCE_MACOS && !(out & PIANO_BOOTFLAG_ONCE_ANDROID), "once macos %#x", out);
				break;
			case PIANO_REQ_ONCE_CLEAR:
				CHECK(!(out & PIANO_BOOTFLAG_ONCE_MASK), "clear %#x", out);
				break;
			case PIANO_REQ_DEFAULT_ANDROID:
				CHECK(piano_bootflag_default_android(out), "default android %#x", out);
				break;
			case PIANO_REQ_DEFAULT_MACOS:
				CHECK(!piano_bootflag_default_android(out), "default macos %#x", out);
				break;
			case PIANO_REQ_RESET:
				CHECK(out == 0, "reset %#x", out);
				break;
			}
			/* the default survives a one-shot request, the one-shot survives a default request */
			if (req == PIANO_REQ_ONCE_ANDROID || req == PIANO_REQ_ONCE_MACOS || req == PIANO_REQ_ONCE_CLEAR)
				CHECK(piano_bootflag_default_android(out) == piano_bootflag_default_android(v), "default kept %#x->%#x", v, out);
			if ((req == PIANO_REQ_DEFAULT_ANDROID || req == PIANO_REQ_DEFAULT_MACOS) && v)
				CHECK((out & PIANO_BOOTFLAG_ONCE_MASK) == (v & PIANO_BOOTFLAG_ONCE_MASK), "once kept %#x->%#x", v, out);
		}
	}
}

static const char *tname(enum piano_target t)
{
	return t == PIANO_TARGET_STOCK ? "stock" : t == PIANO_TARGET_ANDROID ? "android" : t == PIANO_TARGET_MACOS ? "macos" : "fastboot";
}

/* The table of named cases (UEFI-SWITCH-DESIGN.md §3) */
static void test_named(void)
{
	struct { const char *name; struct piano_sel_in in; enum piano_target want; int write; unsigned nf; } c[] = {
		{ "factory: no key, 0x00 -> macOS", { 1, 0, 1, 0, 0, 1, 0x00 }, PIANO_TARGET_MACOS, 0, 0 },
		{ "vol-up, 0x00 -> Android", { 1, 0, 1, 0, 1, 1, 0x00 }, PIANO_TARGET_ANDROID, 0, 0 },
		{ "vol-down -> fastboot", { 1, 0, 1, 1, 0, 1, 0x00 }, PIANO_TARGET_FASTBOOT, 0, 0 },
		{ "both keys -> fastboot", { 1, 0, 1, 1, 1, 1, 0xA1 }, PIANO_TARGET_FASTBOOT, 0, 0 },
		{ "recovery bootmode -> stock even with a key", { 1, 1, 1, 1, 1, 1, 0xA1 }, PIANO_TARGET_STOCK, 0, 0 },
		{ "poweroff_cg -> stock", { 1, 1, 1, 0, 0, 1, 0x00 }, PIANO_TARGET_STOCK, 0, 0 },
		{ "MMU on -> stock", { 0, 0, 1, 0, 0, 1, 0x00 }, PIANO_TARGET_STOCK, 0, 0 },
		{ "no UEFI payload -> stock", { 1, 0, 0, 0, 0, 1, 0xA1 }, PIANO_TARGET_STOCK, 0, 0 },
		{ "once android -> Android, consumed to 0x00", { 1, 0, 1, 0, 0, 1, 0xA1 }, PIANO_TARGET_ANDROID, 1, 0x00 },
		{ "once android + vol-up -> macOS, consumed", { 1, 0, 1, 0, 1, 1, 0xA1 }, PIANO_TARGET_MACOS, 1, 0x00 },
		{ "default android -> Android, nothing written", { 1, 0, 1, 0, 0, 1, 0xA4 }, PIANO_TARGET_ANDROID, 0, 0 },
		{ "default android + vol-up -> macOS", { 1, 0, 1, 0, 1, 1, 0xA4 }, PIANO_TARGET_MACOS, 0, 0 },
		{ "default android + once macos -> macOS, back to 0xA4", { 1, 0, 1, 0, 0, 1, 0xA6 }, PIANO_TARGET_MACOS, 1, 0xA4 },
		{ "default android + once android -> Android, 0xA4", { 1, 0, 1, 0, 0, 1, 0xA5 }, PIANO_TARGET_ANDROID, 1, 0xA4 },
		{ "once macos on default macos -> macOS, 0x00", { 1, 0, 1, 0, 0, 1, 0xA2 }, PIANO_TARGET_MACOS, 1, 0x00 },
		{ "foreign value -> macOS, untouched", { 1, 0, 1, 0, 0, 1, 0x5C }, PIANO_TARGET_MACOS, 0, 0 },
		{ "foreign value + vol-up -> Android, untouched", { 1, 0, 1, 0, 1, 1, 0x5C }, PIANO_TARGET_ANDROID, 0, 0 },
		{ "unreadable flag -> macOS", { 1, 0, 1, 0, 0, 0, 0xA4 }, PIANO_TARGET_MACOS, 0, 0 },
		{ "both once bits (not ours) -> macOS, untouched", { 1, 0, 1, 0, 0, 1, 0xA3 }, PIANO_TARGET_MACOS, 0, 0 },
	};
	unsigned i;
	for (i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		struct piano_sel_out o = piano_select(&c[i].in);
		CHECK(o.target == c[i].want, "%s: target %s", c[i].name, tname(o.target));
		CHECK(o.write_flag == c[i].write, "%s: write %d", c[i].name, o.write_flag);
		if (c[i].write)
			CHECK(o.new_flag == c[i].nf, "%s: new %#x", c[i].name, o.new_flag);
		CHECK(o.if_write_fails == PIANO_TARGET_MACOS, "%s: fail-safe", c[i].name);
	}
	printf("named cases: %u\n", i);
}

/* Invariants over every input combination and byte value. */
static void test_exhaustive(void)
{
	unsigned v, bits;
	for (bits = 0; bits < 64; bits++) {
		for (v = 0; v < 256; v++) {
			struct piano_sel_in in = { bits & 1, !!(bits & 2), !!(bits & 4), !!(bits & 8), !!(bits & 16), !!(bits & 32), v };
			struct piano_sel_out o = piano_select(&in);
			int pre = !in.can_run || in.bootmode_stock || !in.have_uefi;
			if (pre)
				CHECK(o.target == PIANO_TARGET_STOCK && !o.write_flag, "pre %u %#x", bits, v);
			else if (in.key_down)
				CHECK(o.target == PIANO_TARGET_FASTBOOT && !o.write_flag, "down %u %#x", bits, v);
			/* never a write over a foreign or unread byte, never a write of a non-stored form */
			if (o.write_flag) {
				CHECK(in.flag_ok && piano_bootflag_ours(v) && v, "write over %#x", v);
				CHECK(in_list(o.new_flag) && !(o.new_flag & PIANO_BOOTFLAG_ONCE_MASK), "new %#x", o.new_flag);
				CHECK(piano_bootflag_default_android(o.new_flag) == piano_bootflag_default_android(v), "default kept");
			}
			/* a pending one-shot is always consumed when the selector picks a system */
			if (!pre && !in.key_down && in.flag_ok && piano_bootflag_ours(v) && (v & PIANO_BOOTFLAG_ONCE_MASK))
				CHECK(o.write_flag, "once not consumed %u %#x", bits, v);
			/* the stock path is only reached from the guards; ANDROID and MACOS are the only picks after them */
			if (!pre && !in.key_down)
				CHECK(o.target == PIANO_TARGET_ANDROID || o.target == PIANO_TARGET_MACOS, "pick %u %#x", bits, v);
			/* without a key and without any ours-and-set bit, macOS */
			if (!pre && !in.key_down && !in.key_up && (!in.flag_ok || !piano_bootflag_ours(v) || v == 0))
				CHECK(o.target == PIANO_TARGET_MACOS, "default macos %u %#x", bits, v);
		}
	}
}

static void fdt_word(unsigned char *p, unsigned value)
{
	p[0] = (unsigned char)(value >> 24);
	p[1] = (unsigned char)(value >> 16);
	p[2] = (unsigned char)(value >> 8);
	p[3] = (unsigned char)value;
}

static unsigned fdt_fixture(unsigned char *fdt, const char *node, const char *args)
{
	const unsigned structure = 56, prop = structure + 20;
	unsigned length = (unsigned)strlen(args) + 1;
	unsigned end = prop + 12 + ((length + 3) & ~3u);
	unsigned strings = end + 12;
	unsigned total = strings + 9;

	memset(fdt, 0, total);
	fdt_word(fdt, 0xd00dfeedu);
	fdt_word(fdt + 4, total);
	fdt_word(fdt + 8, structure);
	fdt_word(fdt + 12, strings);
	fdt_word(fdt + 16, 40);
	fdt_word(fdt + 20, 17);
	fdt_word(fdt + 24, 16);
	fdt_word(fdt + 32, 9);
	fdt_word(fdt + 36, strings - structure);
	fdt_word(fdt + structure, 1);	/* root */
	fdt_word(fdt + structure + 8, 1);
	memcpy(fdt + structure + 12, node, strlen(node));
	fdt_word(fdt + prop, 3);
	fdt_word(fdt + prop + 4, length);
	memcpy(fdt + prop + 12, args, length);
	fdt_word(fdt + end, 2);
	fdt_word(fdt + end + 4, 2);
	fdt_word(fdt + end + 8, 9);
	memcpy(fdt + strings, "bootargs", 9);
	return total;
}

static void test_fdt(void)
{
	static const char recovery[] = "bootmonitor.bootmode=recovery";
	static const char charging[] = "bootmonitor.bootmode=poweroff_cg";
	unsigned char fdt[256];
	unsigned size = fdt_fixture(fdt, "chosen", "quiet\tbootmonitor.bootmode=recovery\tdebug");
	CHECK(piano_fdt_has_bootarg(fdt, size, recovery, sizeof(recovery) - 1) == 1, "tab-delimited recovery");
	for (unsigned i = 0; i < size; i++)
		CHECK(piano_fdt_has_bootarg(fdt, i, recovery, sizeof(recovery) - 1) == -1, "truncated FDT at %u", i);
	size = fdt_fixture(fdt, "chosen", charging);
	CHECK(piano_fdt_has_bootarg(fdt, size, charging, sizeof(charging) - 1) == 1, "off-mode charging");
	size = fdt_fixture(fdt, "chosen", "alias=bootmonitor.bootmode=recovery bootmonitor.bootmode=recovery-extra");
	CHECK(piano_fdt_has_bootarg(fdt, size, recovery, sizeof(recovery) - 1) == 0, "only exact recovery tokens");
	fdt_word(fdt + size - 13, 0);
	CHECK(piano_fdt_has_bootarg(fdt, size, recovery, sizeof(recovery) - 1) == -1, "bad structure after bootargs takes stock");
	size = fdt_fixture(fdt, "chosen", "quiet");
	{
		const unsigned end = size - 21;
		memmove(fdt + end + 16, fdt + end, size - end);
		memcpy(fdt + end, fdt + 76, 16);
		fdt_word(fdt + end + 4, 4);
		memcpy(fdt + end + 12, "a=b", 4);
		fdt_word(fdt + 4, size + 16);
		fdt_word(fdt + 12, size - 9 + 16);
		fdt_word(fdt + 36, size - 9 + 16 - 56);
		CHECK(piano_fdt_has_bootarg(fdt, size + 16, recovery, sizeof(recovery) - 1) == -1, "duplicate bootargs takes stock");
	}
	size = fdt_fixture(fdt, "other", recovery);
	CHECK(piano_fdt_has_bootarg(fdt, size, recovery, sizeof(recovery) - 1) == 0, "recovery token outside /chosen");
	size = fdt_fixture(fdt, "chosen", recovery);
	fdt_word(fdt + 80, ~0u);
	CHECK(piano_fdt_has_bootarg(fdt, size, recovery, sizeof(recovery) - 1) == -1, "oversized bootargs property");
	size = fdt_fixture(fdt, "chosen", recovery);
	fdt_word(fdt + 84, 9);
	CHECK(piano_fdt_has_bootarg(fdt, size, recovery, sizeof(recovery) - 1) == -1, "out-of-range property name");
	size = fdt_fixture(fdt, "chosen", recovery);
	fdt[88 + strlen(recovery)] = 'X';
	CHECK(piano_fdt_has_bootarg(fdt, size, recovery, sizeof(recovery) - 1) == -1, "unterminated bootargs");
	fdt_word(fdt + 56, 9);
	CHECK(piano_fdt_has_bootarg(fdt, size, recovery, sizeof(recovery) - 1) == -1, "missing FDT root");
	CHECK(piano_fdt_has_bootarg(NULL, size, recovery, sizeof(recovery) - 1) == -1, "missing FDT");
}

int main(void)
{
	test_ours();
	test_apply();
	test_named();
	test_exhaustive();
	test_fdt();
	printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
	return fails != 0;
}
