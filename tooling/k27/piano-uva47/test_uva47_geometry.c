// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of k27 0075's geometry decisions (pmap_uva47_policy.h): the 47-bit user attribute (root level 0, the T0SZ_HW 17 mask,
// a 2048-byte root that the root allocator rounds to a page), the table indices of the addresses the user space depends on, the
// width and the map end, the kernel's attribute left native, and piano-uva47=0 giving 0010's 39-bit attribute, shim and map end.
// The expected values are the ARMv8-A 4 KiB granule's (Arm ARM D8.2: 9 index bits per level, L0 at VA[47:39]), written out here,
// not derived from the header.
#include <stdio.h>
#include <stdlib.h>
#include "pmap_uva47_policy.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* The kernel tree's constants for the 4 KiB BCM2837 build (proc_reg.h, shared_region.h, cpu_capabilities.h); test-uva47.sh checks
 * that the pinned tree still defines these values. */
#define K_ARM_4K_TT_L0_SHIFT            39u
#define K_ARM_4K_TT_L1_SHIFT            30u
#define K_ARM_4K_TT_L2_SHIFT            21u
#define K_ARM_4K_TT_L3_SHIFT            12u
#define K_PMAP_ROOT_ALLOC_SIZE          4096ULL   /* (PMAP_L1_MAX_ENTRY + 1) * 8 with T1SZ_BOOT 25 */
#define K_T1SZ_BOOT                     25u       /* the kernel's TTBR1 width stays 39 bits */
#define K_SHARED_REGION_BASE_ARM64      0x180000000ULL
#define K_SHARED_REGION_SIZE_ARM64      0x200000000ULL
#define K_COMM_PAGE64_BASE_ADDRESS      0x0000000FFFFFC000ULL
#define K_COMM_PAGE64_NESTING_START     0x0000000FC0000000ULL
#define K_COMM_PAGE64_NESTING_SIZE      0x40000000ULL
#define K_VMGO_START_SHIFT              16u

struct idx { uint64_t va; unsigned l0, l1, l2, l3; };

static void
test_attr(void)
{
	/* The new user attribute. */
	const uint64_t m47 = piano_uva_va_mask(PIANO_UVA_T0SZ_HW);
	CHECK(PIANO_UVA_T0SZ_HW == 17u);
	CHECK(piano_uva_root_level_4k(PIANO_UVA_T0SZ_HW) == 0u);
	CHECK(m47 == 0x00007FFFFFFFFFFFULL);
	CHECK(piano_uva_bits_from_mask(m47) == 47u);
	CHECK((1ULL << piano_uva_bits_from_mask(m47)) == 0x0000800000000000ULL);       /* pmap->max = pmap_user_va_size() */
	CHECK(piano_uva_root_alloc_size(m47, piano_uva_4k_shift(0)) == 2048u);          /* 256 L0 entries */
	CHECK(piano_uva_tt1_alloc_bytes(2048u, PIANO_UVA_4K_PAGE, K_PMAP_ROOT_ALLOC_SIZE) == 4096u);
	CHECK(!piano_uva_needs_shim(piano_uva_root_level_4k(PIANO_UVA_T0SZ_HW)));

	/* The level shifts are the kernel's ARM_4K_TT_Ln_SHIFT. */
	CHECK(piano_uva_4k_shift(0) == K_ARM_4K_TT_L0_SHIFT);
	CHECK(piano_uva_4k_shift(1) == K_ARM_4K_TT_L1_SHIFT);
	CHECK(piano_uva_4k_shift(2) == K_ARM_4K_TT_L2_SHIFT);
	CHECK(piano_uva_4k_shift(3) == K_ARM_4K_TT_L3_SHIFT);

	/* The legacy (native) attribute: what piano-uva47=0 and the kernel pmap keep. */
	const uint64_t m39 = piano_uva_va_mask(PIANO_UVA_T0SZ_LEGACY);
	CHECK(piano_uva_root_level_4k(PIANO_UVA_T0SZ_LEGACY) == 1u);
	CHECK(m39 == 0x0000007FFFFFFFFFULL);
	CHECK(piano_uva_bits_from_mask(m39) == 39u);
	CHECK(piano_uva_root_alloc_size(m39, piano_uva_4k_shift(1)) == 4096u);          /* 512 L1 entries, a page */
	CHECK(piano_uva_tt1_alloc_bytes(4096u, PIANO_UVA_4K_PAGE, K_PMAP_ROOT_ALLOC_SIZE) == 4096u);
	CHECK(piano_uva_needs_shim(piano_uva_root_level_4k(PIANO_UVA_T0SZ_LEGACY)));
	/* The kernel's TTBR1 geometry: T1SZ_BOOT 25 = the native attribute's width, untouched by 0075. */
	CHECK(piano_uva_bits_from_mask(piano_uva_va_mask(K_T1SZ_BOOT)) == 39u);
	CHECK(piano_uva_root_level_4k(K_T1SZ_BOOT) == 1u);
}

static void
test_indices(void)
{
	static const struct idx v[] = {
		{ 0x0000000000001000ULL, 0, 0, 0, 1 },            /* the first page after page zero */
		{ 0x0000000180000000ULL, 0, 6, 0, 0 },            /* SHARED_REGION_BASE_ARM64 */
		{ 0x0000000FFFFFC000ULL, 0, 63, 511, 508 },       /* _COMM_PAGE64_BASE_ADDRESS: L0 0 / L1 63 */
		{ 0x0000007D80000000ULL, 0, 502, 0, 0 },          /* libmalloc's fixed range (above 454 GiB, under 512 GiB) */
		{ 0x0000010000000000ULL, 2, 0, 0, 0 },            /* 1 TiB */
		{ 0x0000640000000000ULL, 200, 0, 0, 0 },          /* 100 TiB */
		{ 0x00007F0000000000ULL, 254, 0, 0, 0 },          /* 127 TiB */
		{ 0x00007FFFFDFFC000ULL, 255, 511, 495, 508 },    /* the last page under MACH_VM_MAX_ADDRESS */
	};
	const uint64_t m47 = piano_uva_va_mask(PIANO_UVA_T0SZ_HW);
	for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		CHECK(piano_uva_ttn_index(m47, v[i].va, 0) == v[i].l0);
		CHECK(piano_uva_ttn_index(m47, v[i].va, 1) == v[i].l1);
		CHECK(piano_uva_ttn_index(m47, v[i].va, 2) == v[i].l2);
		CHECK(piano_uva_ttn_index(m47, v[i].va, 3) == v[i].l3);
		CHECK(v[i].va < (1ULL << 47));
	}
	/* The L0 index never leaves the 256 entries the root holds (the valid mask cuts VA[47]). */
	CHECK(piano_uva_ttn_index(m47, ~0ULL, 0) == 255u);
	CHECK(piano_uva_ttn_index(m47, 0x0000800000000000ULL, 0) == 0u);
	/* Under 512 GiB the 39-bit attribute's L1 index equals the 47-bit one's (L0 0): libmalloc's fixed ranges keep their slots. */
	const uint64_t m39 = piano_uva_va_mask(PIANO_UVA_T0SZ_LEGACY);
	for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		if (v[i].va < (1ULL << 39)) {
			CHECK(piano_uva_ttn_index(m39, v[i].va, 1) == piano_uva_ttn_index(m47, v[i].va, 1));
			CHECK(piano_uva_ttn_index(m39, v[i].va, 2) == piano_uva_ttn_index(m47, v[i].va, 2));
		}
	}
	/* pmap_deallocate_all_leaf_tts() walks the root up to ttn_index(~0): 255 for L0 (47 bits), 511 for the L1 root (39 bits). */
	CHECK(piano_uva_ttn_index(m39, ~0ULL, 1) == 511u);
}

static void
test_switch_and_map_end(void)
{
	/* piano-uva47: absent = on, =1 on, =0 off, any other nonzero on. */
	CHECK(piano_uva47_on(false, 0));
	CHECK(piano_uva47_on(true, 1));
	CHECK(!piano_uva47_on(true, 0));
	CHECK(piano_uva47_on(true, 2));

	const uint64_t on = piano_uva_max_address(true), off = piano_uva_max_address(false);
	CHECK(on == 0x00007FFFFE000000ULL);                    /* macOS's MACH_VM_MAX_ADDRESS (vm_param.h, XNU_PLATFORM_MacOSX) */
	CHECK(off == 0x0000007FFE000000ULL);                   /* 0010 / y12k */
	CHECK(on < (1ULL << 47) && (1ULL << 47) - on == 32ULL << 20);
	CHECK(off < (1ULL << 39) && (1ULL << 39) - off == 32ULL << 20);
	/* Both ends lie inside their pmap (pmap->max) and above the shared region, the commpage nesting region and the GPU carveout. */
	CHECK(on <= (1ULL << piano_uva_bits_from_mask(piano_uva_va_mask(PIANO_UVA_T0SZ_HW))));
	CHECK(off <= (1ULL << piano_uva_bits_from_mask(piano_uva_va_mask(PIANO_UVA_T0SZ_LEGACY))));
	CHECK(K_SHARED_REGION_BASE_ARM64 + K_SHARED_REGION_SIZE_ARM64 + 0x20000000ULL <= off);   /* ARM64_MIN_MAX_ADDRESS */
	CHECK(K_COMM_PAGE64_NESTING_START + K_COMM_PAGE64_NESTING_SIZE <= off);
	CHECK(K_COMM_PAGE64_BASE_ADDRESS < off);
	CHECK(0x0000004000000000ULL <= off);                   /* 0046's GPU carveout end (256 GiB) */
	/* vm_map_store.c: MACH_VM_MAX_ADDRESS <= INT32_MAX << VMGO_START_SHIFT; vm_map_store_internal.h: <= 1 << 47. */
	CHECK(on <= ((uint64_t)INT32_MAX << K_VMGO_START_SHIFT));
	CHECK(on <= (1ULL << 47));
	/* V8's full sandbox (1 TiB + 32 GiB guards each side, 1344 GiB) fits only the 47-bit map. */
	CHECK(1344ULL << 30 < on);
	CHECK(!(1344ULL << 30 < off));

	/* machdep.virtual_address_size: 64 - T0SZ_HW in both modes (the hardware's PAC/strip width). */
	CHECK(piano_uva_virtual_address_size(PIANO_UVA_T0SZ_HW) == 47);
	CHECK(piano_uva_virtual_address_size(PIANO_UVA_T0SZ_HW) == 0x2f);
}

int
main(void)
{
	test_attr();
	test_indices();
	test_switch_and_map_end();
	if (failures) {
		fprintf(stderr, "test-uva47-geometry: %d of %d checks FAILED\n", failures, checks);
		return 1;
	}
	printf("test-uva47-geometry: %d checks passed\n", checks);
	return 0;
}
