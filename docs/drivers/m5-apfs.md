# APFS eligibility for M5 write-back

**Decision.** Drop M5 write-back from the daily APFS performance plan for now. Keep the current all-present rule
available on a disposable store with `data-store-write-back=1` (default off). The measurement below shows that
APFS exercised the rule, and a separate scratch run verified reset recovery. Installed write-back performance
remains open. A metadata-only rule would need a reliable way to identify metadata at the block provider and
the same checkpoint ordering proof. [Scratch-window reset results](data-store-m4-m5.md#scratch-window-power-cut-results-2026-11-26)

## What the 26 November captures show

At 15:12, `m5dur-m5wb-cut4` booted the intended 27.3 RAM scratch image. The console confirms the bound scratch
window, resumed store state 2 with 435,988 present pages, write-back enabled and concurrent depth 8. The verdict
confirms SSH, a root tool process, writable test workspace and queue off. The measurement transcript contains
the readback markers for all four 32-operation rows, positive
mixed reads and overlapping writes, `DUR END` and `HOST EXIT rc=0`. The failure came from the host parser after
the tablet program completed. The campaign has no write-back ACK/recovery pair for this run.

`cached` counts non-FUA data accessor calls after whole-request eligibility. `fua` counts tracked FUA accessor
calls, including bitmap I/O, first-touch or mixed data runs and any forced writes. Calls can span multiple 4 KiB
pages. Each successful runtime bitmap publication writes one 4 KiB bitmap block and sets one or more bits.
Publication can combine runs, and a data run can cross a bitmap-block boundary. The console's `present` is an
opening population count. The measurement logs provide neither the set of newly present pages per row nor a
present/clear tally per request. Even `fua - bitmap_publications` counts data calls with any clear pages in the
provider request, which can make all its runs ineligible. It cannot yield a count of fresh data pages.

| Window | Cached | FUA | Bitmap FUA | Flush attempts / store syncs |
| --- | ---: | ---: | ---: | ---: |
| At tool entry (since boot) | 2,116 | 604 | 239 | 52 / 52 |
| 128 KiB fill, including full sync | +15 | +4 | +2 | +2 / +2 |
| Plain (32 overwrites) | 0 | 0 | 0 | 0 / 0 |
| `fsync` (32 writes + syncs) | +32 | 0 | 0 | 0 / 0 |
| `F_FULLFSYNC` (32 writes + syncs) | +374 | +79 | +40 | +64 / +64 |
| `F_BARRIERFSYNC` (32 writes + syncs) | +356 | +64 | +32 | +64 / +64 |
| Mixed (after the separate prefill and full sync) | +5,397 | +4 | +2 | +4 / +4 |

The table sums each `before → write → after` interval, excluding gaps between operations and rows. Every
`before → write` interval had zero write calls, even with `F_NOCACHE`. All 762 cached calls in the small rows
arrived in the sync intervals. Each `fsync` had one cached call and no LUN flush. Each full and barrier sync had two
LUN sync calls, with zero provider Barrier or Force Unit Access requests recorded. This establishes the observed
whole-LUN sync behavior and leaves the exact APFS promotion or checkpoint call sites open. Counter samples and
successful syscalls show I/O and fence activity. Survival after loss of UFS power requires recovery evidence.

By data accessor count in the measured intervals, after subtracting bitmap FUA calls, the observed cache shares
are 32/32 for `fsync`, 374/413 for full sync (90.6%), 356/388 for barrier (91.8%) and 5,397/5,399 for mixed
(99.96%). Plain has a zero denominator. These are global intervals and include metadata and other Data traffic.
The mixed writer completed 671 × 1 MiB writes into a repeatedly overwritten, prefilled 32 MiB span. At eight
128 KiB staging calls per aligned contiguous write, that would be 5,368 data calls, close to the 5,397 cached
calls observed. This strongly suggests eligible file payload in that workload. The capture does not identify its
physical extents. The control measurement also recorded zero writes at the `pwrite` edges, with FUA at the sync
edges and 3,065 FUA calls in mixed. The host assertion that required cached traffic at a `pwrite` edge therefore
failed even though write-back ran.

## Eligibility by APFS write class

APFS and the store track different things. APFS allocates free physical addresses in a container and can reuse
addresses released by an older transaction. The overlay's bit means that the corresponding address has been
materialized in this store generation. Direct Data declines UNMAP, so APFS frees and reuse do not clear the
bitmap. An APFS copy-on-write allocation on a reused address can be M5-eligible. An APFS write to the original
address of an immutable seed page can be a first touch. One clear bit anywhere in a provider write makes the
entire request write-through. A force attribute also makes it write-through. [Apple APFS FAQ](https://developer.apple.com/library/archive/documentation/FileManagement/Conceptual/APFS_Guide/FAQ/FAQ.html)
· [Request eligibility](../../package/NativeRamBlock/ConcurrentDirect.h)
· [Direct UNMAP gate](../../package/NativeRamBlock/NativeRamBlock.cpp)

| Write class | APFS address behavior | Effect on the M5 rule |
| --- | --- | --- |
| Ordinary file overwrite, including a clone or snapshot-shared extent | Apple documents COW for file data. A shared old extent must be preserved. An allocation can recycle a physical address formerly used by another extent. The 27.3 fast paths and transaction-local rewrites are untraced. | Eligible on fully present recycled addresses. A logical file-offset overwrite or a filled fixture alone makes no guarantee. |
| New file, append, sparse-hole fill | A sparse hole has no allocated APFS block. Writing it requires an allocation. Rewriting an allocated extent in a sparse file follows the ordinary extent case. | The allocated address can be clear or present. File age and sparseness do not predict the overlay bit. |
| `F_NOCACHE` plus `F_PREALLOCATE` | XNU sets the file's cache flag and calls `VNOP_ALLOCATE` for the reservation. A reserved extent can still be unwritten, and these APIs expose no no-COW guarantee for later overwrites. No preallocated/no-COW special case was tested. | A first write to the reservation or later remap can land on either kind of page. A genuinely in-place rewrite only becomes eligible after the address is materialized. |
| Container checkpoint descriptor and data areas | The descriptor ring reuses physical slots for newer checkpoints. The data area also has bounded reusable locations for ephemeral objects. These writes can revisit device addresses after wrap. The advancing checkpoint preserves older recoverable state. | Once touched, reused slots can meet the bit test. A mixed or forced request still uses FUA. |
| Container block-zero superblock copy | A fixed address holds a copy used to locate the checkpoint area. The copy may lag the latest checkpoint after an unclean unmount. Refreshes can revisit that fixed address. | Eligible after first materialization if issued as an ordinary write. Its refresh frequency is unmeasured. |
| Container/volume object maps, volume superblock and filesystem trees | The APFS format specifies COW for on-disk objects. Checkpoints select versions, and object-map entries resolve object ID and transaction ID to a physical address. Old tree blocks can be recycled. | Potentially eligible through recycling. The logs cannot attribute the sync traffic to the omap, volume trees or fixed checkpoint areas. APFS uses checkpoints in place of a traditional filesystem journal. |

[Apple File System Reference, Container and Object Maps](https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf)
· [Apple sparse file and clone behavior](https://developer.apple.com/documentation/foundation/about-apple-file-system)
· [XNU fcntl implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_descrip.c)

A Catalina 10.15.2 VM study recovered only the latest file contents after some seek/write and Foundation
updates, while whole-file rewrites left multiple versions. The authors inferred that file data COW was conditional.
Recovered versions describe what survived in that setup. A per-write address trace is needed to distinguish an
in-place write from later reuse of old blocks and to establish the behavior on 27.3. [Catalina checkpoint experiments](https://rp.os3.nl/2019-2020/p08/report.pdf)

## Installed `dab-w1` workload

The installed 11:51–11:58 run compares serial, concurrent and serial in write-through mode. Its A1/B/A2
write phases recorded 261,771 / 265,700 / 258,259 store write calls and 5,952 / 8,192 / 2,208 bitmap
publications, respectively. For example, B had 257,508 data calls after subtracting the bitmap calls. These totals
span fill, all write rows, their verification and syncs, and background I/O. Publication proves first touches
occurred. A publication's share of accessor calls is neither a fresh-page share nor the fraction of requests
containing any clear bits.

| Concurrent B row | Application operations | What is known about eligibility |
| --- | ---: | --- |
| 1 GiB fill | 256 × 4 MiB | Makes a fixture. APFS may allocate previously used addresses. |
| 1 MiB sequential QD1 / QD8 | 1,024 each | Rewrites a prefilled logical span. Each provider request can cover several store runs. |
| 4 KiB random QD1 / QD8 | 82,848 / 75,493 | Rewrites a prefilled span. APFS extent placement and first-touch mix during each row are unrecorded. |
| 128 KiB random QD1 | 31,793 | Rewrites a prefilled span. Any clear page in an ordinary provider request disqualifies the whole request. |
| Mixed writer | 5,418 × 1 MiB | Repeats a file range. The scratch mixed workload's short, hot span and store history differ. |
| `fsync` / full / barrier | 300 / 100 / 300 × 4 KiB | Rewrites file slots and waits for the requested sync. The installed control has no write-back probe of the emitted requests. |

The eligible fraction of installed requests, payload bytes or application operations is **undetermined**, including
the 4 KiB QD1 row. Even the random writes' app-level denominator cannot be matched to the aggregate store
counters. The actual cached fraction in write-through mode is zero by configuration. An installed estimate needs
per-row counts of ordinary all-present, mixed, all-clear and forced provider requests and their bytes at
admission, page counts before publication, and cache sync counts/latencies. Physical address and timing
correlation with file extents and checkpoint areas would distinguish payload, relocation and background traffic.
An A/B can then measure the gain and cost without assuming that APFS reuses the same address for a file offset.

The [M1 +27% QD1 sensitivity](data-store-m4-m5.md#expected-gain-from-m1) assumed 100% eligibility and a 33 µs
saving per operation from the read/write median gap. The modeled service time is 154 µs minus
`33 µs × eligible fraction` plus added flush cost per write. The gap includes filesystem work. The advertised
cache policy can also change APFS's sync behavior, as the scratch control and write-back captures demonstrate.
The installed A/B supplies no value for either correction term.

## Crash contract and candidate rules

- Ordinary completion may leave data volatile when write cache is advertised. A later read must see the new value
  while the device is live. Ordinary `fsync` submits the file work to storage and may leave it in the device cache.
  A successful `F_FULLFSYNC` must make the requested data and its required metadata recoverable across loss of
  power. A successful `F_BARRIERFSYNC` establishes before/after order. A full flush is a valid stronger barrier.
  A newer durable checkpoint must have durable dependencies.
  [Apple sync API semantics](https://developer.apple.com/videos/play/wwdc2019/419/)
- The current rule writes first-touch data with FUA, then writes the matching bitmap with FUA before exposing the
  bit in memory and completing. A clear bit after a crash reads the seed or zero. A set bit must address initialized
  store data even after a cut **before** a filesystem fence. For already-present data a cached overwrite needs no
  new bitmap publication. The host model allows old or new data before the fence and requires the new data after
  it. The exact atomicity and durability of the physical UFS device still require qualification.
- Clearing FUA on first-touch data and batching all persistence into the eventual filesystem fence is unsafe. A
  first-touch data write can stay volatile while a FUA bitmap or a destaged cached bitmap becomes durable. A cut
  then redirects reads to stale store bytes. A single final flush after submitting both writes does not close the
  interval where the bitmap can reach the medium first. `writeBackCuts` reproduces both failures with stale data.
  A FUA checkpoint or bitmap CDB makes its own blocks durable and provides no ordering guarantee for unrelated
  cached writes. [Power-loss model](../../package/NativeRamBlock/Test/concurrent.cpp)
- A deferred first-touch protocol would need volatile mapping state for live reads and concurrent RMW, a
  successful data flush **before** bitmap publication, and durable bitmap completion before acknowledging a
  durability fence. It would also have to establish that any APFS commit write persisted during that process can
  find all its dependencies after a cut, including where data and metadata issued before the same fence can
  destage in either order. The provider sees ranges and options, and the observed trace does not identify the
  dependency boundary. Changing the first-touch or mixed-request rule is outside this conclusion.

## What a valid measurement proves

Keep file-level readbacks, the known-positive first-touch fill, the isolated store identity and write-through
control. Record the `pwrite` and sync edges separately. Require provider writes in the synced rows and positive
store sync and flush deltas in both full and barrier sync windows. For the write-back exercise count cached
traffic in the measured overwrite intervals, including their sync edges and the prefilled mixed writer interval.
Only counting the fill or boot traffic provides no overwrite-workload coverage. A complete capture with zero
cached activity leaves exercise of the path unproven. These intervals show that the path ran during the workload.
To prove exactly which requests used it, correlate provider admission ranges and their presence bits with the
data/bitmap CDBs and their FUA flags. The host power-loss model already drives a known-present range directly,
showing cached access, live readback, non-FUA loss before a fence and persistence after it. The separate file-level
campaign verified write-back recovery across a direct SoC reset. Cold loss of UFS power remains a separate test.

## Fix-image gate

Path: The decoded 27.3 `libsystem_kernel.dylib` exports `pread`, `pwrite`, `fsync` and `fcntl`. XNU
VFS/UBC dispatches writes and vnode sync/ioctl to APFS. Public IOStorageFamily routes synchronization and options
through IOMedia/IOBlockStorageDriver to NativeRamBlock, and sends write attributes and a completion on data
requests. NativeRamBlock owns the request's whole page range, selects CachedOverwrite or DirectDisk/ConcurrentDirect,
and completes after data and any required bitmap publication. Both sync entry points fence admission and drain
pre-fence work. NativeUfs maps to the bound LUN 0 window, issues tracked WRITE(10) or whole-LUN SYNCHRONIZE
CACHE(10) with IMMED clear, and waits for UTRL slot completion. The capture then goes from tablet sysctl
snapshots around the tool's syscalls to the host verdict. [Public IOMedia sync path](https://github.com/apple-oss-distributions/IOStorageFamily/blob/main/IOMediaBSDClient.cpp#L1684-L1724)
· [Block storage driver](https://github.com/apple-oss-distributions/IOStorageFamily/blob/main/IOBlockStorageDriver.cpp)

Checks: **ours-OK** for the recorded scratch identity, cached and FUA traffic, full/barrier sync counters,
and host-modeled admission, bitmap ordering, forced writes and failure propagation. The sheng RamBlock reference
`525abe5` supplies caller-thread stages and separate publication. Ubuntu v1.4.0
`sd.c:205–216, 3113–3247` probes WCE/DPOFUA, unlike our fixed virtual cache declaration. Its `sd.c:1475`
and `1254–1273` encode the same SCSI FUA bit and cache-sync operation as `RootCommands.h`. Linux
`blk-flush.c:390–419` provides ordered pre/postflush sequencing, and `ufshcd.c:2424–2463, 3119–3186`
dispatches each command by tag as the native UTRL path does, with Linux MCQ/interrupts also available.
**Ours-missing** are persistent discard and per-request eligibility telemetry in the saved captures.
**Unknown** are the APFS 27.3 VFS-to-checkpoint call sites and feature negotiation,
installed row eligibility, and UFS cache behavior across cold power loss. [Pinned reference comparison](data-store-m4-m5.md#linux-and-android-on-this-part)

Root cause: the host counted cached traffic only before each `pwrite` returned. The scratch workload deferred
all small provider writes beyond those snapshots. The same log shows 762 cached calls in the sync intervals
and 5,397 in mixed, so APFS COW did not prevent M5 eligibility.

ONE change: correct the host durability measurement to accept write-back exercised during observed overwrite
windows, including sync-triggered I/O, while preserving the full/barrier fence and crash-recovery checks.
The driver rule stays default off. No fix image is required for the host correction.
