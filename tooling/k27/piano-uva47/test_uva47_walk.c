// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of k27 0075's tables: a model of the pmap's table builder (pmap_create_options_internal's root and 0010 shim,
// pmap_ttne, pmap_expand, pmap_enter's leaf, pmap_nest's twig copy, pmap_insert_commpage's L1 copy, pmap_unmap_commpage and
// pmap_deallocate_all_leaf_tts) driven by pmap_uva47_policy.h, checked against an independent ARMv8-A stage-1 walker written from
// the Arm ARM (D8: 4 KiB granule, TCR_EL1.T0SZ 17, start level from the input size, table/block/page descriptors, TTBR0 range,
// faults by level) that reads the simulated physical memory by address only. The hardware runs T0SZ 17 in both modes (0010); the
// model builds the 47-bit level-0 root (piano-uva47 on) or the 39-bit level-1 root behind the one-entry shim (piano-uva47=0).
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmap_uva47_policy.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---- simulated physical memory ---------------------------------------------------------------------------------------------- */
#define NPAGES          16384u
#define PHYS_BASE       0x0000000880000000ULL
static uint64_t phys[NPAGES][512];
static int page_owner[NPAGES];          /* 0 free, else the owning model pmap's id */

static uint64_t
page_alloc(int owner)
{
	for (unsigned i = 0; i < NPAGES; i++) {
		if (page_owner[i] == 0) {
			page_owner[i] = owner;
			memset(phys[i], 0, sizeof(phys[i]));
			return PHYS_BASE + (uint64_t)i * 4096u;
		}
	}
	fprintf(stderr, "test-uva47-walk: out of simulated pages\n");
	exit(2);
}

static unsigned
page_index(uint64_t pa)
{
	if (pa < PHYS_BASE || pa >= PHYS_BASE + (uint64_t)NPAGES * 4096u || (pa & 0xFFFu)) {
		fprintf(stderr, "test-uva47-walk: bad table PA 0x%" PRIx64 "\n", pa);
		exit(2);
	}
	return (unsigned)((pa - PHYS_BASE) >> 12);
}

static void
page_free(uint64_t pa)
{
	unsigned i = page_index(pa);
	CHECK(page_owner[i] != 0);
	page_owner[i] = 0;
	memset(phys[i], 0xA5, sizeof(phys[i]));     /* poison: a later walk through a freed table must not resolve */
}

static uint64_t *
tbl(uint64_t pa)
{
	return phys[page_index(pa)];
}

static unsigned
pages_owned(int owner)
{
	unsigned n = 0;
	for (unsigned i = 0; i < NPAGES; i++) {
		n += page_owner[i] == owner;
	}
	return n;
}

/* ---- the pmap model (the kernel's code paths, written against the policy) --------------------------------------------------- */
#define TTE_VALID       0x1ULL
#define TTE_TABLE       0x2ULL          /* ARM_TTE_TYPE_TABLE */
#define TTE_TABLE_MASK  0x0000FFFFFFFFF000ULL
#define PTE_PAGE        0x3ULL          /* valid | ARM_PTE_TYPE (L3 page) */
#define PTE_AF          (1ULL << 10)
#define TWIG_LEVEL      2u
#define LEAF_LEVEL      3u
#define COMMPAGE_LEVEL  1u              /* pta_commpage_level for 4 KiB, both attributes */

#define K_COMM_PAGE64_BASE_ADDRESS      0x0000000FFFFFC000ULL
#define K_COMM_PAGE64_RO_ADDRESS        0x0000000FFFFF4000ULL
#define K_SHARED_REGION_BASE_ARM64      0x180000000ULL

struct mpmap {
	int id;
	unsigned root_level;
	uint64_t mask, min, max;
	uint64_t root_pa, shim_pa, ttbr;
};

static int next_id = 1;

static struct mpmap
model_create(bool uva47)
{
	struct mpmap p = { .id = next_id++ };
	const unsigned t0sz = uva47 ? PIANO_UVA_T0SZ_HW : PIANO_UVA_T0SZ_LEGACY;
	p.root_level = piano_uva_root_level_4k(t0sz);
	p.mask = piano_uva_va_mask(t0sz);
	p.min = 0;
	p.max = 1ULL << piano_uva_bits_from_mask(p.mask);
	uint64_t bytes = piano_uva_tt1_alloc_bytes(piano_uva_root_alloc_size(p.mask, piano_uva_4k_shift(p.root_level)), 4096u, 4096u);
	CHECK(bytes == 4096u);
	p.root_pa = page_alloc(p.id);
	if (piano_uva_needs_shim(p.root_level)) {
		p.shim_pa = page_alloc(p.id);
		tbl(p.shim_pa)[0] = (p.root_pa & TTE_TABLE_MASK) | TTE_TABLE | TTE_VALID;
		p.ttbr = p.shim_pa;
	} else {
		p.shim_pa = 0;
		p.ttbr = p.root_pa;
	}
	return p;
}

static uint64_t *
model_ttne(const struct mpmap *p, unsigned target, uint64_t va)
{
	if (va < p->min || va >= p->max) {
		return NULL;
	}
	uint64_t *table = tbl(p->root_pa);
	for (unsigned lvl = p->root_level; lvl <= target; lvl++) {
		uint64_t *e = &table[piano_uva_ttn_index(p->mask, va, lvl)];
		if (lvl == target) {
			return e;
		}
		if ((*e & (TTE_TABLE | TTE_VALID)) != (TTE_TABLE | TTE_VALID)) {
			return NULL;
		}
		table = tbl(*e & TTE_TABLE_MASK);
	}
	return NULL;
}

/* 0 = KERN_SUCCESS, 4 = KERN_INVALID_ADDRESS (pmap_expand's answer above pmap->max). */
static int
model_expand(struct mpmap *p, uint64_t va, unsigned level)
{
	if (va < p->min || va >= p->max) {
		return 4;
	}
	for (unsigned lvl = p->root_level; lvl < level; lvl++) {
		if (model_ttne(p, lvl + 1, va) == NULL) {
			uint64_t pa = page_alloc(p->id);
			*model_ttne(p, lvl, va) = (pa & TTE_TABLE_MASK) | TTE_TABLE | TTE_VALID;
		}
	}
	return 0;
}

static int
model_enter(struct mpmap *p, uint64_t va, uint64_t pa)
{
	int kr = model_expand(p, va, LEAF_LEVEL);
	if (kr) {
		return kr;
	}
	*model_ttne(p, LEAF_LEVEL, va) = (pa & TTE_TABLE_MASK) | PTE_PAGE | PTE_AF;
	return 0;
}

static void
model_nest(struct mpmap *grand, struct mpmap *subord, uint64_t start, uint64_t size)
{
	const uint64_t twig = 1ULL << piano_uva_4k_shift(TWIG_LEVEL);
	for (uint64_t va = start; va < start + size; va += twig) {
		CHECK(model_expand(subord, va, LEAF_LEVEL) == 0);
		CHECK(model_expand(grand, va, TWIG_LEVEL) == 0);
		uint64_t *g = model_ttne(grand, TWIG_LEVEL, va), *s = model_ttne(subord, TWIG_LEVEL, va);
		CHECK(g && s && *g == 0);
		*g = *s;
	}
}

static void
model_unnest(struct mpmap *grand, uint64_t start, uint64_t size)
{
	const uint64_t twig = 1ULL << piano_uva_4k_shift(TWIG_LEVEL);
	for (uint64_t va = start; va < start + size; va += twig) {
		uint64_t *g = model_ttne(grand, TWIG_LEVEL, va);
		if (g) {
			*g = 0;
		}
	}
}

static void
model_insert_commpage(struct mpmap *p, const struct mpmap *cp)
{
	CHECK(model_expand(p, K_COMM_PAGE64_BASE_ADDRESS, COMMPAGE_LEVEL) == 0);
	uint64_t *e = model_ttne(p, COMMPAGE_LEVEL, K_COMM_PAGE64_BASE_ADDRESS);
	const uint64_t *src = model_ttne(cp, COMMPAGE_LEVEL, K_COMM_PAGE64_BASE_ADDRESS);
	CHECK(e && src && *e == 0 && *src != 0);
	*e = *src;
}

static void
model_unmap_commpage(struct mpmap *p, const struct mpmap *cp)
{
	uint64_t *e = model_ttne(p, COMMPAGE_LEVEL, K_COMM_PAGE64_BASE_ADDRESS);
	if (e) {
		CHECK(*e == 0 || *e == *model_ttne(cp, COMMPAGE_LEVEL, K_COMM_PAGE64_BASE_ADDRESS));
		*e = 0;
	}
}

/* pmap_deallocate_all_leaf_tts: entries 0..ttn_index(~0) of each table above the leaf; pmap_tte_deallocate panics on a table the
 * pmap does not own (the model counts that as a failure). */
static void
model_dealloc_all(struct mpmap *p, uint64_t table_pa, unsigned level)
{
	uint64_t *t = tbl(table_pa);
	const unsigned last = piano_uva_ttn_index(p->mask, ~0ULL, level);
	for (unsigned i = 0; i <= last; i++) {
		uint64_t e = t[i];
		if (!(e & TTE_VALID)) {
			continue;
		}
		CHECK((e & TTE_TABLE) != 0);                    /* no blocks in user pmaps */
		uint64_t child = e & TTE_TABLE_MASK;
		CHECK(page_owner[page_index(child)] == p->id);   /* pmap_tte_deallocate's "doesn't own the page table" panic */
		if (level < TWIG_LEVEL) {
			model_dealloc_all(p, child, level + 1);
		}
		t[i] = 0;
		page_free(child);
	}
}

static void
model_destroy(struct mpmap *p, const struct mpmap *cp)
{
	if (cp) {
		model_unmap_commpage(p, cp);
	}
	model_dealloc_all(p, p->root_pa, p->root_level);
	page_free(p->root_pa);
	if (p->shim_pa) {
		page_free(p->shim_pa);
	}
	CHECK(pages_owned(p->id) == 0);
}

/* ---- the independent walker (Arm ARM D8, stage 1, EL0/EL1 TTBR0, 4 KiB granule, no FEAT_LPA2) --------------------------------- */
enum { WALK_OK = 0, WALK_FAULT = 1 };

struct walk {
	int result;
	int fault_level;        /* translation fault level */
	uint64_t pa;
	unsigned index[4];      /* the index the walk used per level (for the cross-check) */
	int start_level;
};

static uint64_t
phys_read64(uint64_t pa)
{
	return tbl(pa & ~0xFFFULL)[(pa & 0xFFFu) >> 3];
}

static struct walk
hw_walk(uint64_t ttbr0, unsigned t0sz, uint64_t va)
{
	struct walk w = { .result = WALK_FAULT, .fault_level = -1 };
	const unsigned ias = 64u - t0sz;                       /* input address size */
	/* Start level for the 4 KiB granule: 4 - ceil((ias - 12) / 9). */
	const int start = 4 - (int)((ias - 12u + 8u) / 9u);
	w.start_level = start;
	const uint64_t tagless = va & 0x00FFFFFFFFFFFFFFULL;  /* TBI0: the top byte is ignored */
	if ((tagless >> 55) & 1u) {
		w.fault_level = start;                          /* a TTBR1 address: not this walker's (fault for the test) */
		return w;
	}
	if (tagless >> ias) {
		w.fault_level = start;                          /* outside the TTBR0 input range: translation fault, start level */
		return w;
	}
	uint64_t table = ttbr0 & 0x0000FFFFFFFFF000ULL;       /* BADDR; ASID in [63:48] ignored */
	for (int lvl = start; lvl <= 3; lvl++) {
		const unsigned shift = 12u + 9u * (unsigned)(3 - lvl);
		const unsigned nbits = (lvl == start) ? (ias - shift) : 9u;
		const unsigned index = (unsigned)((tagless >> shift) & ((1ULL << nbits) - 1ULL));
		w.index[lvl] = index;
		const uint64_t desc = phys_read64(table + (uint64_t)index * 8u);
		if (!(desc & 1u)) {
			w.fault_level = lvl;
			return w;
		}
		if (lvl == 3) {
			if ((desc & 3u) != 3u) {
				w.fault_level = 3;              /* reserved encoding at level 3 */
				return w;
			}
			w.pa = (desc & 0x0000FFFFFFFFF000ULL) | (tagless & 0xFFFu);
			w.result = WALK_OK;
			return w;
		}
		if (!(desc & 2u)) {
			if (lvl == 0) {
				w.fault_level = 0;              /* no level-0 block with a 4 KiB granule */
				return w;
			}
			const uint64_t size = 1ULL << shift;
			w.pa = (desc & 0x0000FFFFFFFFF000ULL & ~(size - 1u)) | (tagless & (size - 1u));
			w.result = WALK_OK;
			return w;
		}
		table = desc & 0x0000FFFFFFFFF000ULL;
	}
	return w;
}

/* ---- the tests ------------------------------------------------------------------------------------------------------------- */
static const uint64_t probe_va[] = {
	0x0000000000001000ULL, 0x0000000180000000ULL, 0x0000007D80000000ULL, 0x0000007FFDFFF000ULL,
	0x0000010000000000ULL, 0x0000014FFFFFF000ULL, 0x0000640000000000ULL, 0x00007F0000000000ULL, 0x00007FFFFDFFC000ULL,
};
#define NPROBE (sizeof(probe_va) / sizeof(probe_va[0]))

static uint64_t
fake_pa(uint64_t va)
{
	/* A distinct, page-aligned "data page" PA outside the table pool (PA space is 40 bits on the pad). */
	return 0x0000004000000000ULL | ((va >> 12) * 0x9E37ULL & 0x0000003FFFFFF000ULL);
}

static void
check_resolves(const struct mpmap *p, uint64_t va, uint64_t want_pa, const char *what)
{
	struct walk w = hw_walk(p->ttbr, PIANO_UVA_T0SZ_HW, va + 0x123u);
	const uint64_t *leaf = model_ttne(p, LEAF_LEVEL, va);
	if (w.result != WALK_OK || w.pa != ((want_pa & ~0xFFFULL) | 0x123u) || !leaf || ((*leaf & TTE_TABLE_MASK) != (want_pa & ~0xFFFULL))) {
		fprintf(stderr, "FAIL %s: va 0x%" PRIx64 " walk %d (fault L%d) pa 0x%" PRIx64 " want 0x%" PRIx64 "\n", what, va, w.result,
		    w.fault_level, w.pa, want_pa);
		failures++;
	}
	checks++;
}

static void
check_faults(const struct mpmap *p, uint64_t va, int level, const char *what)
{
	struct walk w = hw_walk(p->ttbr, PIANO_UVA_T0SZ_HW, va);
	if (w.result != WALK_FAULT || (level >= 0 && w.fault_level != level)) {
		fprintf(stderr, "FAIL %s: va 0x%" PRIx64 " walk %d fault L%d, want a fault at L%d\n", what, va, w.result, w.fault_level, level);
		failures++;
	}
	checks++;
}

static void
test_uva47_user(void)
{
	struct mpmap p = model_create(true);
	CHECK(p.root_level == 0 && p.shim_pa == 0 && p.ttbr == p.root_pa);
	CHECK(p.max == (1ULL << 47));
	for (size_t i = 0; i < NPROBE; i++) {
		CHECK(model_enter(&p, probe_va[i], fake_pa(probe_va[i])) == 0);
	}
	for (size_t i = 0; i < NPROBE; i++) {
		check_resolves(&p, probe_va[i], fake_pa(probe_va[i]), "uva47 probe");
		/* the hardware's per-level indices are the policy's ttn_index */
		struct walk w = hw_walk(p.ttbr, PIANO_UVA_T0SZ_HW, probe_va[i]);
		CHECK(w.start_level == 0);
		for (unsigned l = 0; l < 4; l++) {
			CHECK(w.index[l] == piano_uva_ttn_index(p.mask, probe_va[i], l));
		}
	}
	/* 2^47 and above: outside TTBR0's range for the hardware, refused by pmap_expand. */
	check_faults(&p, 0x0000800000000000ULL, 0, "uva47 2^47");
	CHECK(model_expand(&p, 0x0000800000000000ULL, LEAF_LEVEL) == 4);
	CHECK(model_ttne(&p, 0, 0x0000800000000000ULL) == NULL);
	/* An empty L0 entry (3 TiB: L0 6) faults at level 0; an empty L1 entry under a used L0 one (1 TiB + 1 GiB) at level 1; an
	 * empty leaf next to a mapped one at level 3. */
	check_faults(&p, 0x0000030000000000ULL, 0, "uva47 empty L0");
	check_faults(&p, 0x0000010040000000ULL, 1, "uva47 empty L1");
	check_faults(&p, 0x0000010000001000ULL, 3, "uva47 empty L3");
	/* The top byte is ignored (TBI0): a tagged pointer reaches the same page. */
	struct walk w = hw_walk(p.ttbr, PIANO_UVA_T0SZ_HW, 0x5A00640000000000ULL);
	CHECK(w.result == WALK_OK && w.pa == fake_pa(0x0000640000000000ULL));
	/* Random pages anywhere under 2^47 (page zero excluded), then random holes. */
	srand(47);
	uint64_t vas[600];
	unsigned n = 0;
	while (n < 600) {
		uint64_t va = ((((uint64_t)rand() << 31) ^ (uint64_t)rand()) << 12) & 0x00007FFFFFFFF000ULL;
		if (va < 0x4000u || va >= PIANO_UVA47_MAX_ADDRESS) {
			continue;
		}
		vas[n++] = va;
		CHECK(model_enter(&p, va, fake_pa(va)) == 0);
	}
	for (unsigned i = 0; i < n; i++) {
		check_resolves(&p, vas[i], fake_pa(vas[i]), "uva47 random");
	}
	unsigned holes = 0;
	for (unsigned i = 0; i < 2000; i++) {
		uint64_t va = ((((uint64_t)rand() << 31) ^ (uint64_t)rand()) << 12) & 0x00007FFFFFFFF000ULL;
		const uint64_t *leaf = model_ttne(&p, LEAF_LEVEL, va);
		if (leaf && *leaf) {
			continue;
		}
		holes++;
		check_faults(&p, va, -1, "uva47 hole");
	}
	CHECK(holes > 1000);
	model_destroy(&p, NULL);
}

static void
test_shim_control(void)
{
	/* piano-uva47=0: the 39-bit level-1 root behind 0010's shim, the hardware still at T0SZ 17. */
	struct mpmap p = model_create(false);
	CHECK(p.root_level == 1 && p.shim_pa != 0 && p.ttbr == p.shim_pa);
	CHECK(p.max == (1ULL << 39));
	unsigned below = 0;
	for (size_t i = 0; i < NPROBE; i++) {
		if (probe_va[i] < p.max) {
			below++;
			CHECK(model_enter(&p, probe_va[i], fake_pa(probe_va[i])) == 0);
			check_resolves(&p, probe_va[i], fake_pa(probe_va[i]), "shim probe");
			struct walk w = hw_walk(p.ttbr, PIANO_UVA_T0SZ_HW, probe_va[i]);
			CHECK(w.index[0] == 0);
			for (unsigned l = 1; l < 4; l++) {
				CHECK(w.index[l] == piano_uva_ttn_index(p.mask, probe_va[i], l));
			}
		} else {
			/* y12k's 512 GiB end: pmap_expand refuses, the hardware faults at the shim (L0 entry != 0 empty). */
			CHECK(model_enter(&p, probe_va[i], fake_pa(probe_va[i])) == 4);
			check_faults(&p, probe_va[i], 0, "shim above 512 GiB");
		}
	}
	CHECK(below == 4);
	model_destroy(&p, NULL);
}

static void
test_nesting(bool uva47)
{
	const char *mode = uva47 ? "uva47" : "shim";
	/* The commpage pmap and the shared region's nested pmap take the same attribute as the tasks (pmap_create_options). */
	struct mpmap cp = model_create(uva47);
	CHECK(model_enter(&cp, K_COMM_PAGE64_BASE_ADDRESS, 0x0000004100000000ULL) == 0);
	CHECK(model_enter(&cp, K_COMM_PAGE64_RO_ADDRESS, 0x0000004100001000ULL) == 0);
	struct mpmap sr = model_create(uva47);
	const uint64_t sr_size = 64ULL << 20;                  /* 32 twigs of the 8 GiB region at 0x180000000 */
	for (uint64_t off = 0; off < sr_size; off += 0x00211000ULL) {
		CHECK(model_enter(&sr, K_SHARED_REGION_BASE_ARM64 + off, fake_pa(K_SHARED_REGION_BASE_ARM64 + off)) == 0);
	}
	/* pmap_nest expands the nested pmap to its leaf tables over the whole range (the first task's nest does it in the kernel). */
	for (uint64_t va = K_SHARED_REGION_BASE_ARM64; va < K_SHARED_REGION_BASE_ARM64 + sr_size; va += 1ULL << piano_uva_4k_shift(TWIG_LEVEL)) {
		CHECK(model_expand(&sr, va, LEAF_LEVEL) == 0);
	}
	const unsigned cp_pages = pages_owned(cp.id), sr_pages = pages_owned(sr.id);

	for (int task = 0; task < 3; task++) {
		struct mpmap g = model_create(uva47);
		model_insert_commpage(&g, &cp);
		model_nest(&g, &sr, K_SHARED_REGION_BASE_ARM64, sr_size);
		CHECK(model_enter(&g, 0x0000000100000000ULL, 0x0000004200000000ULL) == 0);   /* the task's own image */
		check_resolves(&g, K_COMM_PAGE64_BASE_ADDRESS, 0x0000004100000000ULL, mode);
		check_resolves(&g, K_COMM_PAGE64_RO_ADDRESS, 0x0000004100001000ULL, mode);
		for (uint64_t off = 0; off < sr_size; off += 0x00211000ULL) {
			check_resolves(&g, K_SHARED_REGION_BASE_ARM64 + off, fake_pa(K_SHARED_REGION_BASE_ARM64 + off), mode);
		}
		check_resolves(&g, 0x0000000100000000ULL, 0x0000004200000000ULL, mode);
		/* The commpage's L1 slot is the only cross-pmap entry above the twig level: L0 0 -> grand's own L1, entry 63 -> the
		 * commpage pmap's L2 table. */
		const uint64_t *l1e = model_ttne(&g, 1, K_COMM_PAGE64_BASE_ADDRESS);
		CHECK(l1e && page_owner[page_index(*l1e & TTE_TABLE_MASK)] == cp.id);
		if (uva47) {
			CHECK(page_owner[page_index(tbl(g.root_pa)[0] & TTE_TABLE_MASK)] == g.id);
		}
		/* Per task: the uva47 root + its L1 table, or the L1 root + the shim: the same page count (analysis item 31). */
		const unsigned task_pages = pages_owned(g.id);
		/* root + L1 (or L1 root + shim) + L2 for 4 GiB (image) + L3 there + the shared region's L2 (its twigs point into sr) */
		CHECK(task_pages == 5u);
		model_unnest(&g, K_SHARED_REGION_BASE_ARM64, sr_size);
		model_destroy(&g, &cp);
		/* The shared tables survive the task. */
		CHECK(pages_owned(cp.id) == cp_pages && pages_owned(sr.id) == sr_pages);
		check_resolves(&cp, K_COMM_PAGE64_BASE_ADDRESS, 0x0000004100000000ULL, "commpage pmap after a task");
	}
	model_destroy(&sr, NULL);
	model_destroy(&cp, NULL);
}

static void
test_walker_known_answers(void)
{
	/* The walker itself, on hand-built tables: T0SZ 17 starts at L0 with 256 entries, T0SZ 25 at L1 with 512. */
	struct walk w;
	uint64_t l0 = page_alloc(999), l1 = page_alloc(999), l2 = page_alloc(999), l3 = page_alloc(999);
	tbl(l0)[255] = l1 | 3u;
	tbl(l1)[511] = l2 | 3u;
	tbl(l2)[495] = l3 | 3u;
	tbl(l3)[508] = 0x0000004000123000ULL | 0x403u;
	w = hw_walk(l0, 17, 0x00007FFFFDFFC000ULL + 8u);
	CHECK(w.start_level == 0 && w.result == WALK_OK && w.pa == 0x0000004000123008ULL);
	w = hw_walk(l1, 25, 0x0000007FFDFFC000ULL);
	CHECK(w.start_level == 1 && w.result == WALK_OK && w.pa == 0x0000004000123000ULL);
	w = hw_walk(l1, 25, 0x0000008000000000ULL);
	CHECK(w.result == WALK_FAULT && w.fault_level == 1);                  /* outside a 39-bit TTBR0 */
	tbl(l1)[3] = 0x00000000C0000000ULL | 0x401u;                          /* a 1 GiB block at L1 */
	w = hw_walk(l0, 17, 0x00007F80C0012345ULL & ~0x00007F8000000000ULL);
	CHECK(w.result == WALK_FAULT && w.fault_level == 0);                  /* L0 entry 0 empty */
	tbl(l0)[0] = l1 | 3u;
	w = hw_walk(l0, 17, 0x00000000C0012345ULL);
	CHECK(w.result == WALK_OK && w.pa == 0x00000000C0012345ULL);
	tbl(l0)[1] = 0x0000008000000000ULL | 0x401u;                          /* an L0 block: invalid with 4 KiB */
	w = hw_walk(l0, 17, 0x0000008000000000ULL);
	CHECK(w.result == WALK_FAULT && w.fault_level == 0);
	page_free(l0); page_free(l1); page_free(l2); page_free(l3);
}

int
main(void)
{
	test_walker_known_answers();
	test_uva47_user();
	test_shim_control();
	test_nesting(true);
	test_nesting(false);
	for (unsigned i = 0; i < NPAGES; i++) {
		if (page_owner[i]) {
			fprintf(stderr, "FAIL: page %u still owned by %d\n", i, page_owner[i]);
			failures++;
			break;
		}
	}
	if (failures) {
		fprintf(stderr, "test-uva47-walk: %d of %d checks FAILED\n", failures, checks);
		return 1;
	}
	printf("test-uva47-walk: %d checks passed\n", checks);
	return 0;
}
