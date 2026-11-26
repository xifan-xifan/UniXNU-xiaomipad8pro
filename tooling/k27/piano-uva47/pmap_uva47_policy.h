/*
 * piano (k27 0075): the user address width decisions of the 4 KiB k27 pmap (osfmk/arm/pmap/pmap.c), as plain inline C so that they
 * are host-tested (UniXNU-piano tooling/k27/piano-uva47: this file byte for byte, test_uva47_geometry.c, test_uva47_walk.c).
 * No kernel headers.
 *
 * k27 0010 runs the hardware's TTBR0 at T0SZ 17 (47 bits, a level-0 walk) through a one-entry level-0 shim per pmap, while the pmap
 * itself stays the 39-bit full-page level-1 root (T0SZ_BOOT 25) and the VM map ends at 0x7FFE000000. 0075 gives every user pmap
 * (task, shared region, commpage) its own 47-bit geometry instead: 4 KiB granule, root level 0, the valid mask from T0SZ_HW 17, TTBR0
 * = the root itself (no shim); the kernel pmap keeps the native 39-bit level-1 attribute. piano-uva47=0 restores 0010's course (the
 * native attribute, the shim, 0x7FFE000000) for an A/B on one image.
 */
#ifndef PMAP_UVA47_POLICY_H
#define PMAP_UVA47_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* TCR_EL1.T0SZ the hardware runs TTBR0 with since k27 0010 (proc_reg.h T0SZ_HW), and the 4 KiB build's pmap T0SZ_BOOT. */
#define PIANO_UVA_T0SZ_HW               17u
#define PIANO_UVA_T0SZ_LEGACY           25u

/* The 64-bit user VM map's end: macOS's MACH_VM_MAX_ADDRESS (2^47 less 32 MiB), and 0010's 39-bit one (2^39 less 32 MiB). */
#define PIANO_UVA47_MAX_ADDRESS         0x00007FFFFE000000ULL
#define PIANO_UVA39_MAX_ADDRESS         0x0000007FFE000000ULL

#define PIANO_UVA_4K_PAGE               4096ULL
#define PIANO_UVA_TTE_BYTES             8ULL

/* The 4 KiB granule's root level for a T0SZ: (T0SZ - 16) / 9, pmap.c's pmap_pt_attr_4k formula (17 -> 0, 25 -> 1). */
static inline unsigned int
piano_uva_root_level_4k(unsigned int t0sz)
{
	return (t0sz - 16u) / 9u;
}

/* The valid VA mask for a T0SZ: proc_reg.h's ARM_PTE_REGION_MASK(SZ) = 2^(64 - SZ) - 1. */
static inline uint64_t
piano_uva_va_mask(unsigned int t0sz)
{
	return (1ULL << (64u - t0sz)) - 1ULL;
}

/* pmap_user_va_bits() from an attribute's valid mask (a nonzero 2^n - 1): n. */
static inline unsigned int
piano_uva_bits_from_mask(uint64_t va_valid_mask)
{
	return 64u - (unsigned int)__builtin_clzll(va_valid_mask);
}

/* The 4 KiB granule's index shift at a level: L0 39, L1 30, L2 21, L3 12. */
static inline unsigned int
piano_uva_4k_shift(unsigned int level)
{
	return 39u - 9u * level;
}

/* pmap_root_alloc_size(): the root table's bytes, ((mask >> root shift) + 1) entries (T0SZ 17 at L0: 256 entries, 2048 bytes). */
static inline uint64_t
piano_uva_root_alloc_size(uint64_t va_valid_mask, unsigned int root_shift)
{
	return ((va_valid_mask >> root_shift) + 1ULL) * PIANO_UVA_TTE_BYTES;
}

/* pmap_tt1_allocate()/pmap_tt1_deallocate()'s rounding: a root smaller than a page and not PMAP_ROOT_ALLOC_SIZE takes a whole page. */
static inline uint64_t
piano_uva_tt1_alloc_bytes(uint64_t size, uint64_t page_bytes, uint64_t pmap_root_alloc_size_const)
{
	if ((size < page_bytes) && (size != pmap_root_alloc_size_const)) {
		size = page_bytes;
	}
	return size;
}

/* ttn_index() for the 4 KiB granule: the address under the valid mask, 9 index bits at the level's shift. */
static inline unsigned int
piano_uva_ttn_index(uint64_t va_valid_mask, uint64_t addr, unsigned int level)
{
	const unsigned int shift = piano_uva_4k_shift(level);
	return (unsigned int)(((addr & va_valid_mask) & (0x1FFULL << shift)) >> shift);
}

/* piano-uva47: absent or nonzero = the 47-bit user geometry (the default), =0 = 0010's 39-bit pmap behind the shim. */
static inline bool
piano_uva47_on(bool arg_present, uint32_t arg_value)
{
	return !arg_present || arg_value != 0;
}

/* arm64_pmap_max_offset_default (the exec map's end, pmap_max_offset(DEVICE / JUMBO / MAX)) for the switch. */
static inline uint64_t
piano_uva_max_address(bool uva47)
{
	return uva47 ? PIANO_UVA47_MAX_ADDRESS : PIANO_UVA39_MAX_ADDRESS;
}

/* A pmap whose root is not level 0 needs 0010's level-0 shim under TTBR0 (the hardware always starts at level 0, T0SZ 17). */
static inline bool
piano_uva_needs_shim(unsigned int root_level)
{
	return root_level != 0u;
}

/* machdep.virtual_address_size: the width the hardware strips and walks TTBR0 addresses with, 64 - T0SZ_HW (47 in both modes). */
static inline int
piano_uva_virtual_address_size(unsigned int t0sz_hw)
{
	return (int)(64u - t0sz_hw);
}

#endif /* PMAP_UVA47_POLICY_H */
