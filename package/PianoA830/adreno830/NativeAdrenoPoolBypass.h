/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_POOL_BYPASS_H
#define NATIVE_ADRENO_POOL_BYPASS_H
#include <stddef.h>
#include <stdint.h>

/* UniXNU-piano (2026-11-22, GPU-PAGED P0b): the run-time pool bypass and the figures its A/B reads.
 *
 * The paged design (one pageable named entry per BO, 4 KiB pages from XNU, no 1 GiB contiguous pool) inherits the scattered path's
 * per-page cost: prepare() wiring, one getPhysicalSegment per page, one GPU leaf per 4 KiB page and the GPU's TLB misses on them. No run
 * has measured it against the pool. native_adreno_pool_bypass (boot argument piano-gpu-pool-bypass, sysctl machdep.piano_gpu_pool_bypass)
 * sends every NEW BoAlloc past the pool to the existing path outside it (with piano-gpu-user-vm=1: allocateUser, the named entry); 0, the
 * default, is today's behaviour exactly. BOs already allocated keep their backing, so one boot can run the same workload pooled and
 * scattered. The figures below count every BoAlloc by the path that backed it, the wiring and physical lookups of the scattered path, the
 * GPU map time by backing, the TLB invalidations of maps and unmaps and the per-submission maintenance walk; machdep.piano_gpu_paged
 * prints them (NativeAdrenoPoolBypassLine), the publish tick logs a summary line while they move.
 *
 * Every figure is a relaxed atomic add (a figure may land a little late, none is lost); the clock is the publish shell's microsecond
 * clock (null before publication or on the host: counts without times). Defined in NativeAdrenoMemory.cpp, which every writer links. */
enum : unsigned {
    NativeAdrenoPathPool=0,         /* the BO pool (allocatePooled, zeroed by finishPooled) */
    NativeAdrenoPathContiguous=1,   /* outside the pool: one physically contiguous run (small BOs, mapped-control mode only) */
    NativeAdrenoPathUser=2,         /* outside the pool: the pageable named entry (allocateUser), wired by prepare(), one span per run */
    NativeAdrenoPathMapped=3,       /* outside the pool: kernel-mapped pages (mapped-control mode, or an explicit contiguous request) */
    NativeAdrenoPaths=4,
    NativeAdrenoP0bBuckets=8,       /* latency histogram: < 16 us, < 64, < 256, < 1 ms, < 4 ms, < 16 ms, < 64 ms, longer */
    NativeAdrenoP0bTimeWords=3+8,   /* a latency row: count, total us, longest us, then the buckets */
    NativeAdrenoP0bMapKinds=3,      /* a GPU map of: [0] a pooled BO, [1] an own BO outside the pool, [2] an import or the scanout */
    NativeAdrenoP0bLineBytes=200,   /* a machdep.piano_gpu_paged row, NUL included */
};
extern "C" {
extern volatile uint32_t native_adreno_pool_bypass;        /* 0: the pool as today; 1: new BOs skip it */
extern volatile uint32_t native_adreno_pool_bypass_flips;  /* how often the switch changed at run time */
extern uint64_t (*native_adreno_p0b_now_us)(void);         /* the publish shell's clock; null: counts only */
/* Per path: [0] BOs backed, [1] their bytes, [2] failed attempts (the path that gave the answer), [3] their bytes, [4] allocations that
 * skipped the pool because of the bypass (path pool row only). */
extern volatile uint64_t native_adreno_p0b_alloc[NativeAdrenoPaths][5];
extern volatile uint64_t native_adreno_p0b_alloc_us[NativeAdrenoPaths][NativeAdrenoP0bTimeWords];   /* BoAlloc to the reply, by path */
/* allocateBacking: [0] the named entry (user path), [1] a kernel allocation. The wiring: descriptor create + prepare(). */
extern volatile uint64_t native_adreno_p0b_wire_us[2][NativeAdrenoP0bTimeWords];
/* allocateBacking's physical walk: [0] us, [1] getPhysicalSegment calls (one per 4 KiB page), [2] spans it made. */
extern volatile uint64_t native_adreno_p0b_lookup[2][3];
extern volatile uint64_t native_adreno_p0b_map_us[NativeAdrenoP0bMapKinds][NativeAdrenoP0bTimeWords];   /* mapLocked, by backing */
extern volatile uint64_t native_adreno_p0b_map_pages[NativeAdrenoP0bMapKinds];                         /* 4 KiB GPU leaves written */
/* TLB invalidations: [0] TLBIASID + SYNC after a map (sync_after_map), [1] unmaps invalidating the ASID, [2] unmaps by page (TLBIVAL),
 * [3] the pages those invalidated. */
extern volatile uint64_t native_adreno_p0b_tlbi[4];
/* cleanForSubmission: [0] submissions, [1] maintained objects walked, [2] their spans (each a walk of physical pages: the per-submission
 * physical lookups; 0 for write-combined own BOs, which need none). */
extern volatile uint64_t native_adreno_p0b_submit[3];
}
static inline bool NativeAdrenoPoolBypassed(void) {return __atomic_load_n(&native_adreno_pool_bypass,__ATOMIC_ACQUIRE)!=0;}
static inline uint64_t NativeAdrenoP0bNow(void) {return native_adreno_p0b_now_us?native_adreno_p0b_now_us():0;}
static inline void NativeAdrenoP0bAdd(volatile uint64_t *word,uint64_t value) {__atomic_fetch_add(word,value,__ATOMIC_RELAXED);}
/* The bucket of a latency: edges 16 us times powers of 4. */
static inline unsigned NativeAdrenoP0bBucket(uint64_t us)
{
    unsigned bucket=0;
    for(uint64_t edge=16;bucket<NativeAdrenoP0bBuckets-1 && us>=edge;edge<<=2)++bucket;
    return bucket;
}
/* One latency into a row (count, total, longest, bucket). */
static inline void NativeAdrenoP0bTime(volatile uint64_t *row,uint64_t us)
{
    NativeAdrenoP0bAdd(&row[0],1);
    NativeAdrenoP0bAdd(&row[1],us);
    uint64_t seen=__atomic_load_n(&row[2],__ATOMIC_RELAXED);
    while(us>seen && !__atomic_compare_exchange_n(&row[2],&seen,us,true,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    NativeAdrenoP0bAdd(&row[3+NativeAdrenoP0bBucket(us)],1);
}
/* Row `row` of machdep.piano_gpu_paged into out (at most size bytes, NUL included, no newline): its length, 0 past the last row. */
int NativeAdrenoPoolBypassLine(unsigned row,char *out,size_t size);
/* The publish tick's summary line (NativeAdrenoMapLineBytes): its length; 0 when nothing moved since the last one. */
int NativeAdrenoPoolBypassSummary(char *out,size_t size);
#endif
