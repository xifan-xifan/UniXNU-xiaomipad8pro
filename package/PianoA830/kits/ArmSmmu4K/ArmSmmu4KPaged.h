/* SPDX-License-Identifier: MIT */
#ifndef ARM_SMMU_4K_PAGED_H
#define ARM_SMMU_4K_PAGED_H
#include "ArmSmmu4K.h"
#ifdef __cplusplus
extern "C" {
#endif
/* On-demand native4K stage-1 tables for one TTBR0 address space: the same format and leaf bits as ArmSmmu4KBuild and the live
 * kit (LPAE, 4 KiB granule, 4 KiB level-3 leaves only, table descriptors PA|3, MAIR index 0 NC / 1 WB), but no fixed topology:
 * a table page is taken from an injected page allocator when a map first needs it and handed back once it is empty, so a space
 * costs table pages for what it maps, not for its aperture. The tables are the only per-page record (no copy of the physical
 * spans); a mapping's own record (iova, bytes, attributes) is the caller's.
 *
 * Reserve, then commit: ArmSmmu4KPagedReserve takes table pages from the allocator into a reservation (off the tree; an
 * allocator failure gives back what this call took and writes no table). ArmSmmu4KPagedMap first proves, reading only, that
 * every target leaf is invalid and that the reservation holds every table page the map needs; only then does it write, bottom
 * up: a new table is filled and published before the descriptor that links it is written. The physical runs are read once, in
 * order, straight into the leaves; a run that is malformed (unaligned, empty, past the output size, on a table page) is a caller
 * bug: the leaves already written are cleared again (new tables stay linked and empty) and the plan asks for an ASID
 * invalidation. A map of invalid leaves needs no TLB invalidation (the plan says None; a profile may still sync).
 *
 * Unmap clears an exactly mapped range (every leaf valid, optionally with the expected attribute bits) and returns the TLB plan:
 * TLBIVAL of each page up to ArmSmmu4KPagedTlbPageLimit pages, else the ASID. A table left empty is unlinked into the caller's
 * array (none or full: it stays linked and empty for a later Trim); TLBIVAL does not reach the walk caches, so any unlinked
 * table makes the plan the ASID with walk set. Unlinked pages are not written again and go back to the allocator only through
 * ArmSmmu4KPagedRelease, after the caller's TLBSYNC completed. The order of an unmap is: leaves cleared and published, empty
 * tables unlinked and their parents published, caller TLBI + TLBSYNC, Release; the object's own pages may be unwired only after
 * the same TLBSYNC.
 *
 * Shared subtree: ArmSmmu4KPagedShare pins one root slot of an owner space (normally the kernel buffers' 0..1 GiB) and returns
 * the table it points at; ArmSmmu4KPagedLinkShared writes that table into the same root slot of another space, which never maps,
 * unmaps, retires or frees anything below a linked slot. The owner keeps mapping there; every root sees it at once (one ASID:
 * the owner's TLB plan covers all roots). Teardown of the owner frees the shared table: unlink it everywhere first.
 *
 * Every function serializes on the caller's lock for the space (and the owner's for a shared subtree). No MMIO, TLB operation,
 * cache maintenance (only the publish hook), allocation besides the hooks, or libc. A publish failure poisons the space (every
 * later change refuses; the caller retains every page). The host tests are not hardware qualification. */
enum {ArmSmmu4KPagedTlbPageLimit=512};   /* = ArmSmmu4KTlbPageLimit (ArmSmmu4KProgram.h) */

typedef struct {
    void *cookie;
    /* One 4 KiB aligned table page with a writable 4 KiB aligned CPU view (any content: the kit zeroes it before use). 0 on success. */
    int (*allocate)(void *cookie,uint64_t *physical,volatile uint64_t **cpu);
    /* A page allocate() gave, back (only from Cancel, Release and Teardown). */
    void (*release)(void *cookie,uint64_t physical,volatile uint64_t *cpu);
    /* The CPU view of a page allocate() gave; null for any other address (a descriptor naming one is corruption). */
    volatile uint64_t *(*cpu)(void *cookie,uint64_t physical);
    /* Clean/order one changed table page for the device's walks (dsb ishst once the walk is coherent). 0 only once visible. */
    int (*publish)(void *cookie,volatile uint64_t *cpu,size_t bytes);
    /* Optional: nonzero when [physical, physical + bytes) touches a page allocate() gave to any space (a leaf never names a table). */
    int (*owns)(void *cookie,uint64_t physical,uint64_t bytes);
} ArmSmmu4KPagedPages;
typedef struct {
    unsigned input_bits;            /* 32..48: the root is level 1 up to 39 bits, level 0 above */
    unsigned output_bits;           /* 32, 36, 40, 42, 44, 48 */
    uint64_t iova_start,iova_end;   /* this space's own mappings stay inside [start, end); 0, 0: the whole input range */
} ArmSmmu4KPagedConfig;
typedef struct {unsigned permissions,writeback,shareability;} ArmSmmu4KPagedAttrs;   /* as ArmSmmu4KRange's */
typedef struct {
    void *cookie;
    /* The physically contiguous run at byte offset `offset` of the object, called with ascending offsets: its start and length (4 KiB
     * multiples, length > 0; it may reach past what is left, the kit takes what it needs). 0 on success. */
    int (*run)(void *cookie,uint64_t offset,uint64_t *physical,uint64_t *bytes);
} ArmSmmu4KPagedRuns;
/* Table pages held off the tree, first in first out, linked through their first word. Zero-initialize; Cancel gives back the rest. */
typedef struct {uint64_t head,tail;size_t pages;} ArmSmmu4KPagedReservation;
typedef struct {
    uint64_t table_pages,table_pages_peak,level_pages[4];   /* owned tables linked (root included, shared slots not) */
    uint64_t leaves,leaves_peak;                            /* valid leaves this space wrote */
    uint64_t retired_pending;                               /* unlinked, waiting for Release */
    uint64_t maps,unmaps,trims,refusals,rollbacks;
    uint64_t reserved,allocations,allocation_failures,cancelled,released;
    uint64_t descriptor_reads,descriptor_writes,published_pages;
    uint64_t plans_pages,plans_asid,plans_walk;
} ArmSmmu4KPagedCounters;
typedef struct {
    ArmSmmu4KPagedConfig config;
    ArmSmmu4KPagedPages pages;
    uint64_t root_physical;          /* TTBR0's table address */
    volatile uint64_t *root;
    unsigned start_level,root_entries;
    uint64_t pinned[8],foreign[8];   /* root slots: shared out by this space (never retired) / linked from another space */
    unsigned prepared,poisoned;
    uint64_t generation;             /* +1 per change */
    ArmSmmu4KPagedCounters counters;
} ArmSmmu4KPagedSpace;
typedef enum {ArmSmmu4KPagedTlbNone=0,ArmSmmu4KPagedTlbPages,ArmSmmu4KPagedTlbAsid} ArmSmmu4KPagedTlbKind;
typedef struct {
    ArmSmmu4KPagedTlbKind kind;
    unsigned walk;              /* a table was unlinked: walk caches must go too, so kind is Asid */
    unsigned rolled_back;       /* a map's run was refused after leaves were written: they were cleared again, kind is Asid */
    uint64_t iova,pages;        /* kind Pages: TLBIVAL of every page of [iova, iova + pages * 4096) */
    size_t leaves;              /* leaves written (map) or cleared (unmap) */
    size_t tables_new;          /* table pages the map linked */
    size_t retired;             /* table pages unlinked into the caller's array */
    size_t kept_empty;          /* tables left empty and linked (no room in the array) */
} ArmSmmu4KPagedPlan;
typedef struct {uint64_t iova,bytes;ArmSmmu4KPagedAttrs attrs;const ArmSmmu4KPagedRuns *runs;} ArmSmmu4KPagedRecord;
typedef enum {
    ArmSmmu4KPagedAuditOk=0,ArmSmmu4KPagedAuditStrayLeaf,ArmSmmu4KPagedAuditMissingLeaf,ArmSmmu4KPagedAuditWrongLeaf,
    ArmSmmu4KPagedAuditBadTable,ArmSmmu4KPagedAuditTableCount,ArmSmmu4KPagedAuditLeafCount,ArmSmmu4KPagedAuditRuns
} ArmSmmu4KPagedAuditWhy;
typedef struct {
    size_t tables,empty_tables,leaves,descriptor_reads;
    ArmSmmu4KPagedAuditWhy why;
    uint64_t iova;              /* where the first finding is (the table's first address for a table finding) */
} ArmSmmu4KPagedAuditResult;

/* The leaf descriptor of ArmSmmu4KBuild/the live kit for a page at physical (AF, nG, PXN/UXN unless Execute, AP[1] unless
 * Privileged, AP[2] unless Write, SH, AttrIndx 1 for write-back). Attributes are not validated here. */
uint64_t ArmSmmu4KPagedLeaf(const ArmSmmu4KPagedAttrs *,uint64_t physical);
/* An empty space: one zeroed, published root page. The space must be zero-initialized. Capacity when the allocator refuses. */
ArmSmmu4KStatus ArmSmmu4KPagedInit(ArmSmmu4KPagedSpace *,const ArmSmmu4KPagedConfig *,const ArmSmmu4KPagedPages *);
/* Read-only: the table pages a map of [iova, iova + bytes) needs now; Overlap when a leaf there is valid. */
ArmSmmu4KStatus ArmSmmu4KPagedNeed(const ArmSmmu4KPagedSpace *,uint64_t iova,uint64_t bytes,size_t *pages);
/* The table pages such a map can need at most whatever is mapped (its whole subtree missing). */
size_t ArmSmmu4KPagedWorstCase(const ArmSmmu4KPagedSpace *,uint64_t iova,uint64_t bytes);
/* pages more table pages into the reservation; on an allocator failure every page this call took goes back (Capacity). */
ArmSmmu4KStatus ArmSmmu4KPagedReserve(ArmSmmu4KPagedSpace *,size_t pages,ArmSmmu4KPagedReservation *);
void ArmSmmu4KPagedCancel(ArmSmmu4KPagedSpace *,ArmSmmu4KPagedReservation *);
/* Map [iova, iova + bytes) to the object's runs with attrs, taking new tables from the reservation (what it does not need stays
 * there). Refusals before any write: Argument/BadRange (inputs, the space's range, a shared slot: Overlap), Overlap (a valid leaf),
 * Capacity (the reservation is short). After writes started: BadRange with plan->rolled_back, IoError (poisoned). */
ArmSmmu4KStatus ArmSmmu4KPagedMap(ArmSmmu4KPagedSpace *,ArmSmmu4KPagedReservation *,uint64_t iova,uint64_t bytes,
    const ArmSmmu4KPagedRuns *,const ArmSmmu4KPagedAttrs *,ArmSmmu4KPagedPlan *);
/* Unmap an exactly mapped range (StateMismatch before any write when a leaf is invalid or, with expect, has other attribute bits).
 * Up to capacity emptied tables are unlinked into retired (physical addresses). */
ArmSmmu4KStatus ArmSmmu4KPagedUnmap(ArmSmmu4KPagedSpace *,uint64_t iova,uint64_t bytes,const ArmSmmu4KPagedAttrs *expect,
    uint64_t *retired,size_t capacity,ArmSmmu4KPagedPlan *);
/* Unlink up to capacity empty tables anywhere in the space (never the root, a pinned or a linked shared slot). Plan Asid + walk. */
ArmSmmu4KStatus ArmSmmu4KPagedTrim(ArmSmmu4KPagedSpace *,uint64_t *retired,size_t capacity,ArmSmmu4KPagedPlan *);
/* Trim only the tables covering [iova, iova + bytes) (the range of an unmap that left tables empty): the same rules and plan as Trim,
 * without a walk of the whole tree. A table is unlinked when it is empty as a whole, whatever part of it the range covers. */
ArmSmmu4KStatus ArmSmmu4KPagedTrimRange(ArmSmmu4KPagedSpace *,uint64_t iova,uint64_t bytes,uint64_t *retired,size_t capacity,
    ArmSmmu4KPagedPlan *);
/* Give unlinked tables back to the allocator: only after the TLBSYNC of their plan completed. */
void ArmSmmu4KPagedRelease(ArmSmmu4KPagedSpace *,const uint64_t *retired,size_t count);
/* Pin root slot index of owner (its table allocated when missing) and return the table's physical address. */
ArmSmmu4KStatus ArmSmmu4KPagedShare(ArmSmmu4KPagedSpace *owner,unsigned index,uint64_t *table_physical);
/* Link another space's shared table into root slot index of this space (empty, not pinned here). */
ArmSmmu4KStatus ArmSmmu4KPagedLinkShared(ArmSmmu4KPagedSpace *,unsigned index,uint64_t table_physical);
/* The leaf for iova (0: none) as the walker finds it, through shared slots too. */
ArmSmmu4KStatus ArmSmmu4KPagedLookup(const ArmSmmu4KPagedSpace *,uint64_t iova,uint64_t *descriptor);
/* Read-only proof of the whole tree against the caller's records (sorted by iova, disjoint, outside shared slots, each with its
 * runs): every table descriptor PA|3 of an allocator page, every owned table reached once (the table count), every leaf inside a
 * record and equal to its descriptor, every record page present. Success with why Ok; StateMismatch with the first finding. */
ArmSmmu4KStatus ArmSmmu4KPagedAudit(const ArmSmmu4KPagedSpace *,const ArmSmmu4KPagedRecord *,size_t count,ArmSmmu4KPagedAuditResult *);
/* Every owned table back to the allocator (linked shared slots are only cleared). No walker may reach the space any more. */
ArmSmmu4KStatus ArmSmmu4KPagedTeardown(ArmSmmu4KPagedSpace *);
#ifdef __cplusplus
}
#endif
#endif
