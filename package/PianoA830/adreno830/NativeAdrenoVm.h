/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_VM_H
#define NATIVE_ADRENO_VM_H
#include "NativeAdrenoSmmuContext.h"
#include "NativeAdrenoHandleIndex.h"
#include "ArmSmmu4KPaged.h"   /* UniXNU-piano: GPU-PAGED P1 */
struct NativeAdrenoVmRegion {
    NativeAdrenoMemory *tables;
    ArmSmmu4KLiveArena *arena;
    ArmSmmu4KLiveState *state;
};
struct NativeAdrenoVmResult {
    ArmSmmu4KStatus operation,quarantine;
    size_t completed_spans;
    bool complete;
};
/* Kernel-owned BO mapping transactions of one context bank, not a userspace ABI or GPU scheduler.
 * Attach under the actual bank/ASID/streams of a programmed, routed context (NativeAdrenoSmmuContext::binding) with empty
 * native4K arenas. Attach proves the whole of both regions once (the kit's table audit) and the routes and context through the
 * board's programmer (A830Smmu::verifyRoutes); after it the table transactions read no SMMU register: a map writes leaves and
 * publishes their pages (a clean to PoC, or only dsb ishst once the coherent table walk was proved, ARCHITECTURE 4.6.1/4.14.2),
 * an unmap clears leaves and invalidates their pages (<= 512 by TLBIVAL, else TLBIASID) with one TLBSYNC (4.6.4). A programmer
 * with sync_after_map (A830Smmu's annibale profile) has every map end with one TLBIASID of the bank and its TLBSYNC too. No fault
 * status, sGFSR or route is polled per operation: a client's page fault never poisons the shared VM (ARCHITECTURE 4.12.3).
 * A table transaction that cannot be proved poisons the VM, keeps every old/new BO reference and FAULTs the owned streams.
 * The real table allocations, context/streams and BO objects remain retained by the owning driver. No implicit destructor
 * cleanup; detach must succeed. Every method serializes on the VM's own lock (ARCHITECTURE 5.4, the M10 lock split); callers
 * may hold their own locks around it but never call back into the VM from a hook. A buffer is unmapped only once the fence of
 * its last use completed (noteUse/setFenceSource) and no job holds it: the GPU is not idled globally. */
/* One table for every client's mappings: 1024 filled up once the render server primed its shaders (int-237: BoMap refused with
 * kIOReturnNoResources at objects=1024, Turnip wrote through a NULL command stream and backboardd crashed), and 4096 after a few app
 * switches (i567: both backboardd crashes came at objects=4096, after it held about 4000 for minutes). 32768 since every app may be a
 * Metal client (2026-10-30): 2.5 MiB of records and a 1 MiB index, allocated once at attach. */
enum : size_t {NativeAdrenoVmMaxObjects=32768,NativeAdrenoVmMaxWindows=4};
/* What a mapping of any client but the render server must leave free in the shared tables (mapClient's keep arguments, 2026-10-30):
 * a refusal there costs one app a VK_ERROR_OUT_OF_DEVICE_MEMORY, the render server's own refusal the whole interface (int-237).
 * backboardd held about 4000 objects for minutes (i567); all clients together held 52609 table ranges once (i568). */
enum : size_t {NativeAdrenoVmRenderReserveObjects=8192,NativeAdrenoVmRenderReserveSpans=32768};
#ifdef ADRENO830_MACOS
/* UniXNU-piano (2026-11-12): the table's range capacity, twice upstream's 262144: 2 GiB of BOs even when every page is its own span. The
 * first macOS desktop (run attend4: 24 Metal clients, 701 MiB of own BOs past a 1024 MiB pool, the render server's surfaces imported)
 * ran out of ranges: WindowServer's BoMap answered NoResources, WindowServer aborted. 20 MiB of records, allocated once at attach.
 * 2026-11-22 (gpu/table1, GPU-DAILY.md "gpu-daily3 result"): 1048576, 4 GiB of scattered pages: a 45 min heavy session filled the
 * 524288 (3.0 GB mapped, 1.8 GB of it outside the 1024 MiB pool at one 4 KiB range per page), every map was refused and WindowServer
 * died at a display wake. 40 MiB of records (40-byte ArmSmmu4KRange), one IONewData at attach; NativeAdrenoGpuContext falls back to
 * NativeAdrenoVmFallbackSpans when that allocation fails, so a short kernel data range never costs the GPU. */
enum : size_t {NativeAdrenoVmMaxSpans=1048576,NativeAdrenoVmFallbackSpans=524288};
/* Why a map was refused (NativeAdrenoVm::lastRefusal, NativeAdrenoUserClient::lastMapRefusal): the code and up to three figures, named
 * per code where it is set; memory is the object the refusal was about (a caller checks it is its own). */
enum NativeAdrenoVmWhy : uint8_t {
    NativeAdrenoVmWhyNone,
    NativeAdrenoVmWhyRequest,       /* permissions/writeback/shareability (a, b, c), no memory, or an unbacked/quarantined/empty object */
    NativeAdrenoVmWhyAperture,      /* iova a, bytes b outside the region (its end c) or not page aligned */
    NativeAdrenoVmWhyReserved,      /* iova a, bytes b touch a reserved window (the null guard, GMEM) */
    NativeAdrenoVmWhyReadOnly,      /* a writable mapping of memory the GPU may only read, or the scanout write-back */
    NativeAdrenoVmWhyObjects,       /* mapping records: a in use of b */
    NativeAdrenoVmWhySpans,         /* table ranges: a needed, b free of c */
    NativeAdrenoVmWhyOverlap,       /* iova a overlaps the mapped range at b */
    NativeAdrenoVmWhyPages,         /* the object's own spans (a: the bad span's index) or a span on the page tables themselves */
    NativeAdrenoVmWhyAlias,         /* a page already mapped with another cache/shareability class */
    NativeAdrenoVmWhyReserveObjects,/* the render server's reserve: records a in use, b kept */
    NativeAdrenoVmWhyReserveSpans,  /* the render server's reserve: ranges a needed, b free, c kept */
    NativeAdrenoVmWhyHold,          /* the object refused a device reference (status a) */
    NativeAdrenoVmWhyNotReady,      /* the VM is not attached or poisoned */
    /* UniXNU-piano (2026-11-23, GPU-PAGED P1, paged mode only) */
    NativeAdrenoVmWhyTables,        /* page-table pages: a needed, b free, c the cap of table pages */
    NativeAdrenoVmWhyCap,           /* mapped bytes: a mapped, b this mapping, c the cap (piano-gpu-mem-cap-mib) */
    NativeAdrenoVmWhyReserveBytes,  /* the render server's reserve in bytes: a mapped, b this mapping, c kept */
};
struct NativeAdrenoVmRefusal {
    uint8_t why;uint64_t a,b,c;const NativeAdrenoMemory *memory;
    /* mapClient's per-call snapshot, copied while the shared VM lock is held. */
    uint64_t objects=0,spans=0,capacity=0,bytes=0,keep_objects=0,keep_spans=0;
};
/* UniXNU-piano (2026-11-21, a CB0 fault's detail): a live mapping as locate() found it: its map handle (0: none; handles count up, so
 * nextHandle() - handle is its age in maps), GPU start and bytes, and what backs it: an own BO (or a kernel buffer), a pooled BO, a
 * task's pages (import) or the hand-off scanout. */
enum NativeAdrenoVmKind : uint8_t {NativeAdrenoVmKindNone,NativeAdrenoVmKindOwn,NativeAdrenoVmKindPool,NativeAdrenoVmKindImport,
                                   NativeAdrenoVmKindScanout};
struct NativeAdrenoVmPlace {uint64_t handle,iova,bytes;uint8_t kind;};
#endif
#ifndef ADRENO830_MACOS
struct NativeAdrenoVmRefusal;   /* macOS builds only; the paged map path takes a pointer to it */
#endif
/* Mappings refused for the render server's reserve: [0] for objects, [1] for table ranges (paged mode: bytes). */
extern "C" volatile uint32_t native_adreno_vm_reserve_refused[2];
/* UniXNU-piano (2026-11-23, GPU-PAGED P1, private host-checks/20261122-gpu-paged/P1-GATE.md): paged mode of the GPU's VM, chosen at
 * boot (piano-gpu-mem=paged; the board sets these before S3): no BO pool, page tables built on demand from kernel chunks
 * (ArmSmmu4KPaged), one record per mapping instead of one table range per physical span, a mapped-bytes cap with the render server's
 * reserve, emptied tables trimmed on the publish tick after one TLBIASID. Off (0), the VM is exactly the live-kit VM. */
extern "C" volatile uint32_t native_adreno_gpu_mem_paged;          /* 1: paged */
extern "C" volatile uint32_t native_adreno_gpu_mem_cap_mib;        /* the mapped-bytes cap (0: the default 7168) */
extern "C" volatile uint32_t native_adreno_gpu_mem_reserve_mib;    /* the render server's reserve of it (0: the default 1536) */
extern "C" volatile uint32_t native_adreno_gpu_pt_audit_s;         /* the tick audits the tables every N s (0: only on request) */
enum : size_t {
    NativeAdrenoVmPagedMaxChunks=256,     /* table chunks: 2 MiB (else 512 KiB, else 128 KiB) each */
    NativeAdrenoVmPagedSpare=64,          /* free table pages a map leaves beyond its worst case when it grows the list */
    NativeAdrenoVmPagedTrimRanges=512,    /* 2 MiB-block ranges with emptied tables the tick trims (more: a resumable sweep) */
    NativeAdrenoVmPagedTrimSteps=64,      /* steps of one tick without a clock (host) */
    NativeAdrenoVmPagedTrimBatch=256,     /* tables one trim unlinks at most (the rest at the next tick) */
    NativeAdrenoVmPagedTableCap=32833,    /* table pages at most: today's preallocated 64 GiB table (REPLAY.md worst case) */
};
enum : uint64_t {NativeAdrenoVmPagedCapDefault=UINT64_C(7168)<<20,NativeAdrenoVmPagedReserveDefault=UINT64_C(1536)<<20,
    /* UniXNU-piano (GPU-PAGED P1b): one tick's trim work under the VM lock, and the bytes of one step (64 level-3 tables) */
    NativeAdrenoVmPagedTrimBudgetUs=500,NativeAdrenoVmPagedTrimSlice=UINT64_C(128)<<20};
struct NativeAdrenoVmPagedConfig {
    unsigned input_bits=36,output_bits=40;   /* the context's: 36 bits = the 64 GiB aperture */
    uint64_t aperture=0;                     /* the space is [0, aperture) */
    size_t table_cap_pages=NativeAdrenoVmPagedTableCap;
    uint64_t cap_bytes=NativeAdrenoVmPagedCapDefault,reserve_bytes=NativeAdrenoVmPagedReserveDefault;
    uint32_t *alias_scratch=nullptr;size_t alias_scratch_entries=0;   /* the cross-class alias index of imports */
};
/* machdep.piano_gpu_pt and the `gpu pt:` line (a copy under the VM lock). */
struct NativeAdrenoVmPagedStats {
    bool paged,poisoned;
    uint64_t table_pages,table_peak,table_cap,chunks,chunk_pages,free_pages,leaves;
    uint64_t maps,unmaps,unmap_val,unmap_val_pages,unmap_asid,kept_empty,trims,trim_full,trim_retired,trim_us_max,trim_pending;
    uint64_t mapped,mapped_peak,cap,reserve,records;
    uint64_t refused_tables,refused_cap,refused_reserve,grow_failures,grow_retries;
    uint64_t audits,audit_failures,audit_why,audit_iova,audit_tables,audit_leaves,audit_us;
};
/* Coherence level (ARCHITECTURE 4.14.1, 3.6 pagani-gpu-coherent): 0 every GPU mapping non-cacheable; 1 GPU-read-only imports
 * write-back inner-shareable (needs V9a); 2 also GPU-written imports and own BOs a client maps write-back (CachedCoherent, needs
 * V9b and V9c). The effective level is native_adreno_io_coherent; the board sets it once S8 decided (NativeAdrenoVm::setCoherence). */
enum NativeAdrenoVmCoherence : unsigned {NativeAdrenoCoherenceOff=0,NativeAdrenoCoherenceReadImports=1,NativeAdrenoCoherenceCached=2};
extern "C" volatile uint32_t native_adreno_io_coherent;
/* Board hook for sampleWord(): reads one aligned word of physical memory through a fresh uncached mapping; 0 on success. */
extern "C" int (*native_adreno_dram_read32)(uint64_t physical,uint32_t *value);
/* The board's reader through the kernel's cacheable physical aperture (null on the host: the BO's own mapping is read). */
extern "C" int (*native_adreno_cached_read32)(uint64_t physical,uint32_t *value);
/* Board hook for auditRange(): [physical, physical+bytes), whole 64-byte lines inside one page, as the cacheable physical aperture and
 * DRAM (a fresh uncached mapping) read it. A line that differs is cleaned and invalidated and DRAM read again: when DRAM then holds the
 * cached copy the line was dirty (*dirty), when DRAM is unchanged it was a stale clean copy (*stale). 0 on success. */
extern "C" int (*native_adreno_line_audit)(uint64_t physical,size_t bytes,uint32_t *dirty,uint32_t *stale);
class NativeAdrenoVm final {
    struct Mapping {
        NativeAdrenoMemory *memory=nullptr;
        uint64_t handle=0,iova=0,last_use=0;
        size_t spans=0,mapped_spans=0;
        unsigned permissions=0,writeback=0,shareability=0,jobs=0;
        uint32_t prev=UINT32_MAX,next=UINT32_MAX;   /* the maintained list (GPU-read-only imports mapped non-cacheable) */
        bool maintained=false;
    };
    Mapping *mappings_=nullptr;
    NativeAdrenoHandleIndex<NativeAdrenoVmMaxObjects> index_;   /* live handle -> its mappings_ slot */
    size_t free_hint_=0;   /* no free mappings_ slot below it */
    size_t high_=0;   /* no live mappings_ slot at or above it: the walks over every mapping stop there */
    size_t class_spans_[16]{};   /* ranges_ per alias class, so a map counts the other classes without a walk */
    size_t import_spans_[16]{};   /* the imported mappings' ranges per alias class */
    uint32_t maintained_head_=UINT32_MAX;
    size_t maintained_count_=0;
    NativeAdrenoVmRegion regions_[2]{};
    ArmSmmu4KLiveVisibility visibility_[2]{};
    ArmSmmu4KIo io_{};
    ArmSmmu4KContextConfig *context_=nullptr;
    /* The routes and the programmer: a board programmer (verify, fault), or the kit's stream state of the legacy attach. */
    NativeAdrenoSmmuProgrammer programmer_{};
    bool has_programmer_=false;
    bool sync_after_map_=false;   /* the programmer's sync_after_map: each map ends with the bank's ASID invalidation */
    ArmSmmu4KStreamState *kit_streams_=nullptr;
    ArmSmmu4KStream streams_[NativeAdrenoSmmuContext::MaxStreams]{};
    size_t stream_count_=0;
    unsigned bank_=0;
    uint64_t context_offset_=0;
    struct Window {uint64_t iova,bytes;unsigned upper;} windows_[NativeAdrenoVmMaxWindows]{};
    size_t window_count_=0;
    const volatile uint32_t *fence_=nullptr;
    void *lock_=nullptr;
    ArmSmmu4KRange *ranges_=nullptr;
    size_t capacity_=0,lower_count_=0,upper_count_=0,objects_=0;
    uint64_t bytes_=0;   /* bytes of every live mapping (own, pooled, imported, scanout): the MEM row's vm */
    unsigned budget_=0;
    uint64_t next_handle_=1;
    bool attached_=false,poisoned_=false,distrust_write_combined_=false,coherent_walk_=false;
    NativeAdrenoVmResult result_{};
#ifdef ADRENO830_MACOS
    /* UniXNU-piano: why the last map was refused (written under the VM's lock, read right after the call by its caller). */
    mutable NativeAdrenoVmRefusal refusal_{};
    IOReturn refuse(uint8_t why,IOReturn status,uint64_t a=0,uint64_t b=0,uint64_t c=0,const NativeAdrenoMemory *memory=nullptr) const
    {refusal_={why,a,b,c,memory};return status;}
#endif
    ArmSmmu4KLiveRegion region(unsigned) const;
    IOReturn fail(ArmSmmu4KStatus);
    Mapping *find(uint64_t);
    const Mapping *find(uint64_t) const;
    size_t rangeAt(uint64_t iova) const;
    void insert(size_t,unsigned);
    void erase(size_t,size_t,unsigned);
    IOReturn preflight(const NativeAdrenoMemory *,unsigned,uint64_t,unsigned,unsigned,unsigned,bool exclusive) const;
    bool exclusivePages(const NativeAdrenoMemory *,unsigned cls) const;
    bool forbidden(uint64_t iova,uint64_t bytes) const;
    bool completed(uint64_t fence) const;
    void linkMaintained(Mapping &);
    void unlinkMaintained(Mapping &);
    IOReturn attachCommon(const ArmSmmu4KIo *,ArmSmmu4KContextConfig *,const NativeAdrenoVmRegion *,const NativeAdrenoVmRegion *,
        size_t,unsigned);
    IOReturn verifyAndSeed();
    IOReturn mapLocked(NativeAdrenoMemory *,unsigned,uint64_t,unsigned,unsigned,unsigned,uint64_t *);
    void lock() const;
    void unlock() const;
    /* UniXNU-piano (2026-11-23, GPU-PAGED P1, private P1-GATE.md): paged mode. The table pages come from chunks_ (contiguous, zeroed,
     * kernel-mapped, held for the device for the VM's life) through a free list linked by each free page's first word; every list and
     * table change happens under the VM lock, a chunk is allocated only by growTables outside it (its own grow lock). ranges_ holds
     * one record per mapping: {iova, the mapping's slot in mappings_, the BO's bytes, attributes}. */
    bool paged_=false;
    struct TrimRange {uint64_t iova,bytes;};
    /* Everything paged mode keeps, allocated by preparePaged (the kexts' __DATA of the collection has little room: no paged state in
     * the VM object itself, which every VM carries). */
    struct PagedState {
        NativeAdrenoVmPagedConfig config{};
        ArmSmmu4KPagedSpace space{};
        NativeAdrenoMemory chunks[NativeAdrenoVmPagedMaxChunks];
        uint16_t order[NativeAdrenoVmPagedMaxChunks]{};   /* chunk indices sorted by physical address */
        size_t chunk_count=0,chunk_pages=0;
        uint64_t free_head=0;
        size_t free_pages=0;                               /* read without the lock as a hint (growTables) */
        TrimRange trim[NativeAdrenoVmPagedTrimRanges]{};
        size_t trim_count=0;
        bool sweep=false;                                  /* every pending slot was taken: the tick sweeps the space */
        uint64_t sweep_cursor=0;
        uint64_t retired[NativeAdrenoVmPagedTrimBatch]{};
        NativeAdrenoVmPagedStats stats{};
        size_t short_pages=0;                              /* table pages the last refused map lacked (0: another reason) */
    };
    PagedState *pg_=nullptr;
    void *grow_lock_=nullptr;
    static int tableAllocate(void *,uint64_t *,volatile uint64_t **);
    static void tableRelease(void *,uint64_t,volatile uint64_t *);
    static volatile uint64_t *tableCpu(void *,uint64_t);
    static int tablePublish(void *,volatile uint64_t *,size_t);
    int chunkAt(uint64_t physical) const;
    bool onTables(uint64_t physical,uint64_t bytes) const;
    IOReturn addChunk(size_t bytes);
    IOReturn preflightPaged(const NativeAdrenoMemory *,uint64_t,unsigned,unsigned,unsigned) const;
    IOReturn mapPagedLocked(NativeAdrenoMemory *,uint64_t,unsigned,unsigned,unsigned,uint64_t *,bool client,bool keep_reserve);
    IOReturn mapPaged(NativeAdrenoMemory *,uint64_t,unsigned,unsigned,unsigned,uint64_t *,bool client,size_t keep_objects,bool keep_reserve,
        NativeAdrenoVmRefusal *refusal);
    IOReturn unmapPagedTables(const Mapping &,size_t at);
    void noteTrim(uint64_t iova,uint64_t bytes);
public:
    NativeAdrenoVm()=default;
    ~NativeAdrenoVm()=default;
    NativeAdrenoVm(const NativeAdrenoVm &)=delete;
    NativeAdrenoVm &operator=(const NativeAdrenoVm &)=delete;
    /* The board's path: the binding of a programmed, routed NativeAdrenoSmmuContext. Proves both regions (the kit's table audit)
     * and the routes and context (the programmer's verify), then records each region as proved for the kit's later transactions. */
    IOReturn attach(const NativeAdrenoSmmuBinding &smmu,const NativeAdrenoVmRegion *lower,const NativeAdrenoVmRegion *upper,
        size_t max_spans,unsigned poll_budget);
    /* The kit's own route state (contexts the kit programmed through ArmSmmu4KAttachStreams, as the software peers of the older
     * tests): the routes and context are proved once with ArmSmmu4KObserveStreams, a failure FAULTs them with ArmSmmu4KFaultStreams. */
    IOReturn attach(const ArmSmmu4KIo *,ArmSmmu4KContextConfig *,ArmSmmu4KStreamState *,
        const NativeAdrenoVmRegion *lower,const NativeAdrenoVmRegion *upper,
        size_t max_spans,unsigned poll_budget);
    /* Frees CPU metadata only, with no BOs remaining. Never releases roots,
     * disables a bank or returns the platform lease; board lifecycle owns it. */
    IOReturn detach();
    /* A GPU address window no mapping may touch, before any map (the null guard 0..0x100000 and the 12 MiB GMEM window at
     * 0x4000000 of ARCHITECTURE 4.6.2). findAddress skips them. */
    IOReturn reserveWindow(uint64_t iova,uint64_t bytes);
    IOReturn findAddress(unsigned upper,size_t bytes,uint64_t alignment,uint64_t *iova) const;
    /* A mapping with exactly these attributes (kernel buffers: privileged ring/memptrs, firmware, GMU memory). */
    IOReturn map(NativeAdrenoMemory *,unsigned upper,uint64_t iova,
        unsigned permissions,unsigned writeback,unsigned shareability,uint64_t *handle);
    /* A client mapping (BoMap): the attributes of ARCHITECTURE 4.14.1 for this memory at the current coherence level replace the
     * requested ones (attributes()); a GPU-read-only import mapped non-cacheable joins the maintained list. keep_objects and keep_spans
     * (0 for the render server) are mapping records and table ranges this mapping must leave free, checked under the VM's lock: else
     * kIOReturnNoResources and native_adreno_vm_reserve_refused counts it. */
    IOReturn mapClient(NativeAdrenoMemory *,uint64_t iova,unsigned permissions,unsigned writeback,unsigned shareability,
        uint64_t *handle,size_t keep_objects=0,size_t keep_spans=0
#ifdef ADRENO830_MACOS
        ,NativeAdrenoVmRefusal *refusal=nullptr
#endif
        );
    /* Live mapping records and table ranges this VM can still take (no lock: a figure for the status rows and tests). */
    size_t freeObjects() const {return NativeAdrenoVmMaxObjects>objects_?NativeAdrenoVmMaxObjects-objects_:0;}
    size_t freeSpans() const {return capacity_>spanCount()?capacity_-spanCount():0;}
    /* ARCHITECTURE 4.14.1: own BOs (pool or scattered) non-cacheable, unless the level is 2 and the client asked write-back;
     * GPU-read-only imports write-back inner-shareable from level 1; GPU-written imports write-back from level 2; the scanout
     * import always non-cacheable. Non-cacheable mappings are outer shareable. */
    static void attributes(const NativeAdrenoMemory &memory,unsigned permissions,unsigned requested_writeback,
        unsigned requested_shareability,unsigned *writeback,unsigned *shareability);
    /* The effective coherence level (min of pagani-gpu-coherent or the S8 default and the proved ceiling), for new mappings. */
    static void setCoherence(unsigned level);
    static unsigned coherence();
    /* Refused (kIOReturnBusy) while a job holds the mapping or the fence of its last use has not completed. */
    IOReturn unmap(uint64_t handle);
    IOReturn holdJob(uint64_t handle);
    /* Only a scheduler's actual successful GPU completion may call this.
     * A timeout, interrupt bit alone or CPU-written timestamp is insufficient. */
    IOReturn retireJobAfterCompletion(uint64_t handle);
    /* The fence the CP writes when a submission completed (memptrs + 4, the BR fence; 32 bits wrap) for unmap and idle. Null:
     * a mapping that was ever used counts as busy. */
    void setFenceSource(const volatile uint32_t *completed);
    /* The submission with this fence reads or writes the mapping (Submit, per buffer, before it is queued). */
    IOReturn noteUse(uint64_t handle,uint64_t fence);
    /* The fence of the last use (0: never used) and whether it completed. */
    uint64_t lastUse(uint64_t handle) const;
    bool idle(uint64_t handle) const;
    /* Before a submission: cleans the CPU caches over the maintained list only (GPU-read-only imports mapped non-cacheable; empty
     * from coherence level 1). Own write-combined BOs and GPU-written imports take no maintenance per submission (4.14.1). */
    IOReturn cleanForSubmission();
    size_t maintainedCount() const;
    /* The previous names of the submission and completion maintenance: publishMapped() is cleanForSubmission(); acquireImported()
     * does nothing (GPU-written imports were cleaned and invalidated once when mapped, ARCHITECTURE 4.14.1). */
    IOReturn publishMapped();
    IOReturn acquireImported();
    /* M10 (ARCHITECTURE 4.6.1): once V4 proved the table walk coherent, a changed table page takes only dsb ishst (true); false
     * goes back to cleaning it to PoC. Applies to the following transactions. */
    IOReturn setTableWalkCoherent(bool coherent);
    bool tableWalkCoherent() const {return coherent_walk_;}
    /* TLBIALL for the context bank plus TLBSYNC: drops every cached translation of the bank whatever its ASID (recovery). */
    IOReturn invalidateTlb();
    /* The submit path saw a CPU-cached word that DRAM did not hold yet in a client's BO: from now on write-combined BOs are maintained
     * like any other (sticky; the safe behaviour of int-070..097). */
    void distrustWriteCombined() {distrust_write_combined_=true;}
    bool writeCombinedTrusted() const {return !distrust_write_combined_;}
    /* Diagnostic, never a proof of coherence: the 32-bit word at a mapped GPU address as the kernel's cacheable alias reads it
     * and, when the board installed native_adreno_dram_read32, as DRAM reads it through an uncached mapping (else ~0). */
    IOReturn sampleWord(uint64_t iova,uint32_t *cached,uint32_t *dram) const;
    /* Before a submission while write-combined BOs are trusted: every page of every mapped write-combined BO that XNU set back to the
     * default attribute is write-combined again (and its lines written back); *reset counts the pages. */
    IOReturn reassertWriteCombined(uint32_t *reset) const;
    /* Every line of [iova, iova+bytes) that lies in a write-combined BO, through native_adreno_line_audit: *dirty counts lines a CPU
     * cache held newer than DRAM, *stale clean copies, *lines the lines compared. kIOReturnUnsupported without a board hook. */
    IOReturn auditRange(uint64_t iova,size_t bytes,uint32_t *lines,uint32_t *dirty,uint32_t *stale) const;
    /* Read-only description of a live mapping; never pins or extends it. */
    IOReturn describe(uint64_t handle,uint64_t *iova,size_t *bytes,unsigned *permissions) const;
    /* Up to `count` dwords of a mapped kernel-owned BO at a GPU address (through the kernel's own mapping), for the board log. */
    IOReturn peekWords(uint64_t iova,uint32_t *out,size_t count) const;
    /* The same after cleaning and invalidating the CPU cache lines over the words. */
    IOReturn peekWordsUncached(uint64_t iova,uint32_t *out,size_t count) const;
    /* The context bank's fault record for the board log: FSR, FAR (lo, hi) and FSYNR0. Reads only; never clears a fault. */
    IOReturn faultState(uint32_t out[4]) const;
    /* Map handles count up and are never reused, so they order mappings in time: the next one, and the one over a GPU address (0: none). */
    uint64_t nextHandle() const {return next_handle_;}
    uint64_t handleAt(uint64_t iova) const;
    bool poisoned() const {return poisoned_;}
    bool attached() const {return attached_;}
    size_t objectCount() const {return objects_;}
    uint64_t mappedBytes() const {return __atomic_load_n(&bytes_,__ATOMIC_RELAXED);}   /* read without the VM lock (status row) */
    size_t spanCount() const {return lower_count_+upper_count_;}
    NativeAdrenoVmResult lastResult() const {return result_;}
    unsigned bank() const {return bank_;}
    /* UniXNU-piano (2026-11-23, GPU-PAGED P1, private P1-GATE.md): the paged VM.
     * preparePaged, before the bank is programmed: the records, the first table chunk and the space's root page (*root: TTBR0 for
     * NativeAdrenoSmmuContext::preparePaged). attachPaged then binds the programmed, routed context as attach does (the programmer's
     * verify), with no table pool: an empty space proves itself by its audit. map and mapClient take their table pages from the free
     * list (reserve, then commit; growTables first, outside the lock) and refuse a short list, the mapped-bytes cap and, for clients
     * with keep_spans set, the render server's reserve with kIOReturnNoResources before any write. unmap never unlinks a table; the
     * tables it emptied are trimmed by trimTables (the publish tick): one TLBIASID + TLBSYNC, then the pages go back to the list. */
    IOReturn preparePaged(const NativeAdrenoVmPagedConfig &config,uint64_t *root);
    IOReturn attachPaged(const NativeAdrenoSmmuBinding &smmu,unsigned poll_budget);
    bool paged() const {return paged_;}
    /* Up to `pages` free table pages (chunks allocated outside the VM lock; never blocks a submission). */
    IOReturn growTables(size_t pages);
    /* The tick: the ranges unmaps left empty tables in are trimmed, one TLBIASID + TLBSYNC, the pages freed. *retired may be null. */
    IOReturn trimTables(size_t *retired=nullptr);
    /* The whole tree against the records and each mapped BO's spans (ArmSmmu4KPagedAudit, read-only, under the lock). */
    IOReturn auditTables(ArmSmmu4KPagedAuditResult *out=nullptr);
    void pagedStats(NativeAdrenoVmPagedStats *out) const;
    /* After detach (no mapping left) and once no walker can reach the space (the bank disabled): every table and chunk back. */
    IOReturn releasePaged();
    /* The leaf the tables hold for iova (0: none), for tests and the fault detail. */
    uint64_t pagedLeaf(uint64_t iova) const;
#ifdef ADRENO830_MACOS
    /* Largest unmapped extent of a published client window. The client windows exclude the VM's reserved regions. */
    uint64_t largestClientHole(uint64_t start,uint64_t bytes) const;
    /* UniXNU-piano: the last refused map's reason (BoMap's console detail), the table's range capacity. */
    NativeAdrenoVmRefusal lastRefusal() const {return refusal_;}
    size_t spanCapacity() const {return capacity_;}
    /* UniXNU-piano (2026-11-21): the mapping over iova (*over) and the one with the highest end at or below iova (*below), either null;
     * handle 0 where there is none. One walk over the mappings under the VM's lock (a fault's detail, never a hot path). */
    IOReturn locate(uint64_t iova,NativeAdrenoVmPlace *over,NativeAdrenoVmPlace *below) const;
#endif
};
#endif
