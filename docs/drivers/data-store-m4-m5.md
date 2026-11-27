# Data store concurrency and durability (M4/M5)

**Owner decision (2026-11-27, `OWNER-0748`):** M5 write-back disk caching (`data-store-write-back=1`) is APPROVED
for daily use. Mandatory pre-condition: take a fresh backup of the owner's Data store with SHA-256 recorded and
read-back verified first, then enable `data-store-write-back=1` in the next install package. M5-queue remains
disabled (`data-store-queue=0`).

## Installed baseline

M1 measured the installed 27.3 Data volume in single-gate mode at 15,580 4 KiB random-read IOPS at QD1 and 15,696 at
QD8. Android on the same device measured 14,967 and 88,663 respectively. The single reader's p99 rose from 85 µs to
2,325 µs alongside a disjoint 1 MiB sequential writer. QD1 random writes reached 6,494 IOPS versus 36,693 on Android.
WriteBooster was already enabled by firmware and qualification made zero flag changes. Full sync and barrier calls
took 1.67 and 1.78 ms in M1. Those are application timings and do not identify which commands reached UFS.

The measured read tail matches the scope of the disk gate. A 1 MiB write can keep it through eight 128 KiB data
commands plus first-touch bitmap updates. A disjoint read waits for the whole request, even if another UFS slot is
available. Root uses a separate provider and bypasses the Data gate. M4 exercises concurrent write-through I/O before
introducing a different completion or durability policy.

## M4 readiness

The installed Data disk combines a packed RAM seed with a bound overlay on LUN 0 in `linux_data`. A clear presence bit
selects the seed or zero. A set bit selects the corresponding 4 KiB store page. The device presents 512-byte logical
sectors, so a partial-page write reads and merges the existing 4 KiB page. See [Data store format](../data-store.md).

| Path | Single gate | `data-store-concurrent=1` |
| --- | --- | --- |
| Admission | One instance lock and one 128 KiB stage for the whole request | Up to eight private 128 KiB stages. Shared read ranges, exclusive whole-page write ranges, with earlier overlapping waiters protected from overtaking |
| Data | One read or FUA write per run through NativeUfs | Disjoint caller threads can have commands outstanding at the same time, subject to transport slots and stage allocation |
| First touch | Data FUA, then bitmap FUA before success | Data FUA, then a serialized publisher can merge already-durable runs in the same bitmap block. It publishes the in-memory bits after bitmap FUA and never waits to form a batch |
| Overwrite | Data FUA only | Data FUA only |
| Completion | Callback in the caller after the lock is released | Callback still in the caller, after releasing its range and stage |
| Sync and power | Completed writes are write-through. Normal sync returns the online status. The opt-in UFS sleep path holds I/O and syncs the store | Sync fences admission and drains admitted I/O. Sleep additionally syncs the LUN and holds admission through wake when UFS suspend is enabled. Stop closes admission and drains it |

The initial v13 integration linked the driver with the concurrency argument absent from both installer profiles.
After the installed v13.1y12o A/B, the daily v13.1y12q profile uses `data-store-concurrent=2`. Data mounts in
single-gate mode, then the boot daemon enables concurrency after it verifies the mounted store has live state 2.
[Installed A/B results](../results-2026-11-26.md#data-storage) · [Concurrent mode at boot](../data-store.md#concurrent-mode-at-boot-piano-data-concurrent-2026-11-26)

The concurrent harness exercises depth and exhaustion, shared/exclusive ranges and 512-byte RMW, disjoint read
progress behind a held write, merge and crash cuts at a bitmap-block boundary, short copies, failure latching, DMA
quarantine, sync, switch, sleep/wake and stop drains, and a concurrent stress run. The M4 startup test starts serial
with two stages armed, enters concurrent mode, proves two disjoint admissions and returns to serial before checking
the resumed store. The serial transcript checks command order, returned bytes and persisted contents against the
earlier driver. The UFS command and power models exercise CDB FUA, response/slot/error handling and failure to sync on
sleep. KIOXIA power-loss behavior, actual slot occupancy and APFS recovery require device evidence. [NativeRamBlock
tests](../../package/NativeRamBlock/Test/run-tests.sh) · [NativeUfs tests](../../package/NativeUfs/tests/run-tests.sh)

The store can be degraded even when its header and window still match. A store-write failure or failed store read
during RMW latches the experimental path against further writes. An unsuccessful command can have committed earlier
runs, and a failed bitmap commit can leave durable data hidden behind a clear bit after reboot. In the serial control
the legacy publisher mutates its in-memory bitmap before the bitmap I/O returns, so a failed publication can read
differently after reboot. Live DMA quarantines the affected buffer, and a transport timeout can poison the shared UFS
host and affect root reads. A write latch or quarantined stage blocks the live mode switch. A pure read error can
record state 5 without latching writes, so the test treats either signal as a stop. APFS can receive short I/O or EIO
and can fail to mount or recover. A foreign or wrong-window store can be refused or kept, and a blank or damaged
header in a writable window can trigger initialization. Require the recorded bound window, pinned seed, resumed state
2 and unchanged generation before testing, and keep a verified recovery set.

### First A/B on the bound store

This is a lead-session diagnostic RAM boot using the installed-equivalent 27.3 profile, its pinned seed, trust, root
layout and explicit bound Data window. M1 is the installed-from-disk reference. Use the qualified M1 collection as the
integration base so the WB and store-command diagnostics are available. Use a RAM reset/return path and a preflight of
the effective command line including ADT spillover. Keep the daily installer and its verifier in force and retain the
current store and root settings.

1. Quiesce the installed system, shut it down cleanly, enter ABL and hold the tablet exclusively through backup and
   qualification. Keep the installed ESP and its matching build record as well as the exact seed. Confirm the
   maintenance environment has the store unmounted. With the existing `out/persistent/installed.json` pointing to that
   ESP, run a fresh read-only store backup using a new session directory and the configured external backup disk:

   ```sh
   python3 tooling/persist/maint-session.py --session "$M4_SESSION" --new-backup \
     --esp "$INSTALLED_ESP" maint-boot map k273-store-backup maint-exit
   ```

   Require `map` to identify the same bound store the installed ESP resumes. The backup takes block 0 and the bitmap,
   then streams the present pages using the saved runs list. Verify the manifest and SHA256SUMS, including the device
   hash, received hash, and uncached backup copy hash for both data sets. Record the session, seed, window,
   generation, flags and page count. Keep the set through qualification. The dry run checks inputs, and the
   fake-tablet tests in `tooling/persist/test-k273-only.py` cover sparse backup and restore. On corruption, capture
   the boot and stop writes. Bring up maintenance and run `rollback k273-store-backup` with the same `--session` and
   `--esp` and without `--new-backup`. The rollback verifies restored pages first and writes and verifies the header
   and bitmap last. Boot the saved matching ESP and check APFS and owner data before resuming use.

2. Use the installed entry's merged args with the diagnostic RAM builder, e.g. `PIANO_EXACT_ARGS` from the installed
   record and `data-store-concurrent=2` in `PIANO_EXTRA_ARGS`, along with the reset controls. The new value arms the
   existing switch while starting with serial admission. Existing `=1` starts concurrent before Data mounts, then
   supports the same root sysctl. It cannot supply a serial mount control. With the argument absent or `=0`, writes to
   the sysctl return `ENOTSUP`. Preflight expects `data-persist=2`, the installed seed and explicit store window, the
   intended argument, the diagnostic KC and the reset path. After boot require the known-positive NativeUfs line, Data
   state 2 and the recorded generation, seed and window, the armed stage count, `mode=single-gate depth=1`, and the
   expected Data and root providers from `diskutil` and `ioreg`. Read `kern.bootsessionuuid` and the effective boot
   args. A missing armed line, depth below two, or a store identity mismatch voids the A/B.

3. Use the host-smoked, hash-verified M1 `dskbench`. While serial, fill a new file in a test-owned directory on Data
   with `dd if=/dev/urandom of="$DATA_FIXTURE" bs=1m count=1024` and sync it to create a nonsparse, store-backed
   fixture. Keep its SHA-256 and check it before and after the read phases. For each phase capture
   `machdep.piano_data_io` and the boot UUID before and after. Compare these read rows against the same file and
   against a separately identified, read-only raw root window:

   ```sh
   (
     set -e
     rows=randr:4k:1:10,randr:4k:8:10,seqr:1m:1,seqr:1m:8
     for mode in 0 1 0; do
       sudo sysctl -w "machdep.piano_data_concurrent=$mode"
       sysctl kern.bootsessionuuid machdep.piano_data_concurrent machdep.piano_data_io
       "$BENCH" --ro "$DATA_FIXTURE" --size 1g --rows "$rows"
       sysctl kern.bootsessionuuid machdep.piano_data_concurrent machdep.piano_data_io
     done
   )
   ```

   Capture counters around each row when attributing depth. For the raw root control, use the same `--ro` rows
   against the unambiguously identified `NativeUfsBlock` whole-window character device. The benchmark uses
   `F_NOCACHE` and disables read-ahead for random rows. Require positive store read deltas for the Data fixture to
   confirm UFS activity. Capture UFS slots/peak, completed operation counts, throughput and p50/p95/p99. The Data
   stage peak is separate from both benchmark QD and UFS slot occupancy. Data peak, admission waits and bitmap
   publications reset on a real mode change. The M1 collection counts and times store accessor calls since boot,
   including any wait for a UFS slot. Use deltas. Switching drains active writes, which have already used FUA.

4. After the read comparison, repeat a serial–concurrent–serial cycle using a new benchmark-owned directory per pass
   on Data. Use the same `--size 1g --rows
   randw:4k:1:10,randw:4k:8:10,mixed:10,sync:fsync:300,sync:full:100,sync:barrier:300`. The mixed case uses separate
   halves of the file and a QD1 4 KiB reader with a QD1 1 MiB sequential writer. Their filesystem I/O can still
   contend for shared metadata. Require verification markers and a passing exit per phase. Keep the link, power mode,
   WB flags, background load, fixture size and row durations comparable. Record the log from each phase's start,
   including positive request/boot markers, UFS errors/timeouts, APFS and store state, and positive root reads. While
   concurrent, create and sync a separate test file and save its hash. Verify it after sleep/wake where that path is
   enabled, then reboot cleanly to the installed serial system, reconfirm the same state 2/generation and verify the
   test file again.

5. The proposed acceptance bands are 25% higher QD8 random-read throughput in B than both A runs, a 50% reduction in
   mixed-reader p99, and no QD1 or root control regression over 10%. Expect Data peak 1 in A and greater than 1 up to
   the allocated depth in B for QD8. A missing throughput or latency gain with demonstrated overlap is a valid
   negative result. Any short I/O, data mismatch, state 5, new APFS checksum/mount error, timeout/quarantine,
   unexpected reset or failed fence/switch stops the test. Preserve evidence and return to the saved serial boot or
   restore the verified backup if the store changed unexpectedly. A reboot into serial alone cannot repair a damaged
   store.

## M5 durability contract

The decoded 27.3 user-space cache exposes `pread`, `pwrite`, `fsync` and `fcntl` in `libsystem_kernel.dylib`. XNU's
VFS sends `fsync` to the vnode and routes `F_FULLFSYNC` and `F_BARRIERFSYNC` through the filesystem's vnode ioctl. XNU
retries the barrier as a full sync on specific unsupported errors. Public IOStorageFamily shows how the media
synchronization ioctl forwards range and options through IOMedia and IOBlockStorageDriver to the provider's
`doSynchronize`. IOBlockStorageDriver submits data with an `IOStorageAttributes` and a completion. The exact APFS 27.3
checkpoint call sites, feature negotiation and issued flags are still unknown. The shared cache is user space, and the
APFS format reference describes the on-disk checkpoints. Before enabling write-back, obtain the APFS-side evidence
with read-only symbols and a trace of flags and commands on a disposable store, with owner approval for disassembly if
required. [Apple fcntl
interface](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html)
·
[IOMediaBSDClient](https://github.com/apple-oss-distributions/IOStorageFamily/blob/main/IOMediaBSDClient.cpp#L1684-L1724)
·
[IOBlockStorageDriver](https://github.com/apple-oss-distributions/IOStorageFamily/blob/main/IOBlockStorageDriver.cpp#L1601-L1639)
· [APFS format reference](https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf)

| Request or declaration | Provider obligation for a cached Data disk | Direct Data with write-back off |
| --- | --- | --- |
| `getWriteCacheState` / `WriteCacheState` | Report enabled before publication when ordinary completion may leave volatile data. A fixed boot mode can refuse `setWriteCacheState` | Reports disabled, setter unsupported |
| Ordinary `fsync` | Waits for the filesystem work it requests. The provider must honor any sync and ordered writes APFS actually submits. A file-level `fsync` call alone is insufficient evidence of a LUN flush | Completed direct writes use FUA |
| `F_FULLFSYNC` / media sync | For a provider sync, close admission, drain pre-fence writes and bitmap operations, then wait for successful SYNCHRONIZE CACHE before reopening. Return failure on device error. Both `doSynchronize` and legacy `doSynchronizeCache` need the same path | The unarmed path returns online status immediately. The armed path fences, drains admitted I/O and reports a latched write failure. Neither sends a routine LUN flush |
| Barrier | Order pre-barrier writes before post-barrier writes. `kIOStorageFeatureBarrier` advertises support, and `kIOStorageSynchronizeOptionBarrier` requests it. A drained whole-LUN flush is a safe stronger implementation. Without a proven cheaper ordering primitive omit the feature and tolerate promotion to full sync | No Barrier feature, options go through the same synchronization entry point |
| Forced write | `kIOStorageFeatureForceUnitAccess` advertises honoring `kIOStorageOptionForceUnitAccess` in write attributes. Make every piece of the requested write durable, including any newly needed bitmap blocks. SCSI FUA applies to that CDB's blocks. It does not drain older unrelated cached writes | No FUA feature in direct mode and attributes ignored. All store writes use FUA internally |

`WriteCacheState` is the state exposed by the virtual device, independent of WriteBooster status. Public
IOStorageFamily exposes its Barrier and Force Unit Access properties as feature bits in `DKIOCGETFEATURES`. APFS must
be able to establish an ordered durable checkpoint using the operations the provider offers. Acknowledging a full sync
early can leave a valid-looking checkpoint that references unavailable blocks. Raw LUN 0 also serves other windows, so
synchronize the whole LUN with IMMED clear and propagate completion status. Measure the cost and incidental flushes of
other windows. [IOBlockStorageDevice property
implementation](https://github.com/apple-oss-distributions/IOStorageFamily/blob/main/IOBlockStorageDevice.cpp#L41-L63)
·
[DKIOCGETFEATURES](https://github.com/apple-oss-distributions/IOStorageFamily/blob/main/IOMediaBSDClient.cpp#L2050-L2080)
· [IOStorage feature and attribute
definitions](https://github.com/apple-oss-distributions/IOStorageFamily/blob/main/IOStorage.h) · [Apple barrier
semantics](https://developer.apple.com/videos/play/wwdc2019/419/)

### Linux and Android on this part

The Ubuntu v1.4.0 release pins kernel `83f0f89bed14`. The checks below compare that source with the M4 baseline
from before write-back. The `a43f79c` manifest's kernel pin `1c21cb9895d6` has the same sd, block flush and UFS core sources.
The Android capture on the KIOXIA THGJFLT2G46BATPB shows a write-back block queue, FUA capability 0, WriteBooster
enabled with both buffer-flush flags set, and configured MCQ depth 64. The cause of the Android FUA setting and
effective occupancy during the benchmark are unknown. Linux reads WCE and DPOFUA from MODE SENSE and can mask FUA for
broken devices or six-byte read/write CDBs. The model-specific UFS quirk in the Ubuntu source masks the
critical-health exception, and the generic Toshiba entry delays low-power entry. Native macOS's successful FUA
commands show command acceptance. Survival after sudden power loss requires separate qualification.

| Ubuntu v1.4.0 reference | Native path | Comparison |
| --- | --- | --- |
| `drivers/scsi/sd.c:3113–3247, 206–216` reads cache mode and publishes WCE/FUA | `NativeRamBlock.cpp:530–534, 842` | Different. NativeRamBlock presents write-through storage. Physical cache and FUA are unprobed on this Data path |
| `drivers/scsi/sd.c:1475` maps FUA to bit 3. `block/blk-flush.c:390–419` substitutes postflush for requested FUA on a write-back device without FUA | `RootCommands.h:668–705`, `NativeUfs.cpp:1813–1847` | Same WRITE(10) bit. Different selection, Data always uses it. A cached accessor exists without a live-DMA result and has no Data callers |
| `drivers/scsi/sd.c:1254–1273, 1557–1558` turns a flush into SYNCHRONIZE CACHE(10/16) | `RootCommands.h:720–741`, `NativeUfs.cpp:1849–1856` | Same whole-LUN command in the (10) case. Different call sites, chiefly setup and power on native |
| `drivers/ufs/core/ufshcd.c:2878, 3119–3186, 5799–6018` copies the SCSI CDB and runs submission/completion | `RootCommands.h:284–346, 419–459` | Same UPIU/CDB mechanism, different execution. Native Data uses UTRL slots and polls on this profile. Android reports MCQ and poll support |
| `drivers/ufs/core/ufshcd.c:312–315, 399–409, 9609` has the model quirk and configures WriteBooster on probe | M1 collection's `RootDevice.h:49–71` and `NativeUfs.cpp:3533–3541` at `8daa9b47` | Same enabled flags on the measured part. Native qualification saw flags already set. WB buffer migration and SCSI cache flush are distinct |
| Linux block devices have no seed or overlay | `DirectDisk.h:23–84`, `PersistOverlay.h:230–246`, `ConcurrentDirect.h:125–194` | Piano adds bitmap ordering for the seed overlay. The sheng RamBlock reference `525abe5` likewise keeps caller-thread servicing and separate durable publication |

[Ubuntu kernel pin](https://github.com/yzddmr6/xiaomipad-8pro-mainline/blob/v1.4.0/kernel/source.json) · [Pinned SCSI
disk
driver](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14a0cb7d582a26e72bced0387870f4/drivers/scsi/sd.c)
· [Pinned block flush
sequencing](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14a0cb7d582a26e72bced0387870f4/block/blk-flush.c)
· [Pinned UFS
core](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14a0cb7d582a26e72bced0387870f4/drivers/ufs/core/ufshcd.c)

### Bitmap publication and cache policy

For a first touch, writing the data and the bitmap through a volatile cache and flushing only at a later filesystem
sync breaks the store format's recovery invariant. The device can destage the bitmap before the data. A reset then
redirects a previously clear bit to stale bytes from the store's data area, even if APFS never received a successful
full sync. A bitmap FUA after an ordinary data write has the same hole. A single cache sync after both writes narrows
the exposure window and still leaves that window open. The required order is durable data, then durable presence bits.
The current write-through path enforces this with two FUA commands for a new run, subject to the device honoring those
commands.

Already-present pages need no bitmap update. Their old durable contents can remain visible after a power cut until a
new overwrite is fenced. A successful cached overwrite must still be visible to reads during that boot, including the
read side of subsequent 512-byte RMW. FUA and a flush do not make multiple pages an atomic transaction. Power loss can
leave part of an in-place write, and a physical tear can invalidate a data block or corrupt the bitmap. The current
format has no bitmap checksum or repair journal. APFS checkpoint recovery and the backup cover different failure
scopes.

| Rank after M4 | Option | Trade-off |
| --- | --- | --- |
| 1 | Cache only ordinary writes whose entire page range is already present. Keep data and bitmap FUA for first touches and a real flush at filesystem fences | Removes per-write FUA from the overwrite case while keeping the presence invariant independent of APFS's sync frequency. Benefits depend on overwrite fraction and flush frequency |
| 2 | Keep write-through and improve scheduling and opportunistic bitmap merging | Preserves today's durability contract and directly targets the measured read tail. Each data run still incurs FUA |
| 3 | Batch first-touch data into a fenced generation, flush it, then publish the bitmap durably | Could amortize much more of the cost. Requires distinct live and durable visibility, read coherence, a bounded dirty-page generation and two ordered phases. Defer until the simpler case is measured |
| 4 | Cache data and bitmap together with a single eventual flush | Reject. Arbitrary destage can expose a bitmap bit with missing or stale data |

**M5's one write-policy candidate** is rank 1, behind a separate default-off boot opt-in on a disposable store.
Acquire exclusive range ownership before deciding that all pages are present. If even one bit is clear, use the
current FUA path for the whole request. Keep open, reset and flatten writes on the current protocol. Extend the
tracked UFS store accessor so cached writes can quarantine live DMA. Expose the static write-cache state before APFS
probes. Treat both explicit media sync APIs and the barrier option as a fenced whole-LUN flush. Make explicit FUA
requests conservative: drain older cached writes and use verified FUA, or emulate the forced write with a synchronous
postflush under the fence. Keep native barrier advertisement off until a cheaper ordering primitive has evidence.
Validate the 27.3 behavior for advertised and unadvertised FUA, and use an explicit trace to account for ordinary
`fsync`, full sync, barriers and checkpoint writes from APFS.

A successful flush retires the completed dirty epoch. It cannot retire a concurrent new epoch, so close admission
while issuing the flush. On I/O, timeout or flush failure, retain dirty state, latch failure, report the error and
prevent a successful mode change or sleep. Quiesce and flush on clean shutdown using a path that can report failure.
Stop and failed suspend must preserve quarantine. Qualify read-after-cached-write coherence, MODE SENSE or equivalent
capability evidence, synchronous cache completion and cold-power-loss behavior on a sacrificial store before testing
the owner store. M1's read and write latency difference includes APFS and UFS effects, so estimate the FUA
contribution with CDB timing and the actual ratio of first touches, overwrites and syncs.

### Crash model for M5

Extend the existing `Test/concurrent.cpp` medium rather than simulating only a filesystem. Use the production
admission and publication policies and a separate durable map and volatile cache. Ordinary WRITE completion updates
the live view, READ forwards from cache, FUA durably commits its own LBA range, and SYNCHRONIZE CACHE durably commits
the fenced preflush epoch. Let the model destage individual cached blocks in any order, independently of command
completion. On a cut discard the volatile cache, create a new in-memory disk on the durable bytes, reopen through
`Overlay::open` and read through `DirectDisk.h`.

- Enumerate cuts before and after data/bitmap submission, command completion, arbitrary destages, fence start, flush
  completion and synchronous acknowledgment. Include two writers to different bits in one bitmap block, a run spanning
  bitmap blocks, a 512-byte RMW on a cached page, and a later write waiting at a fence. Start the store's data area
  with plausible stale bytes so the oracle checks exact page contents, not just whether a block exists.
- For each durable set bit, require the page to have been durably initialized before publication. Old and new data
  with defined pre-fence persistence are permitted for overwrites. Every acknowledged full sync must recover all
  writes covered by that fence, unless a later write validly superseded them. A synthetic checkpoint block referring
  to earlier pages must survive with its dependencies if the trace reports a successful preceding barrier. No
  post-fence request may issue early.
- Inject short transfer, bitmap error, timed-out DMA, partial flush, sync veto and later recovery attempts. Failed
  operations must never claim durable success. Test 4 KiB atomic durable writes first, then allow torn old/new sectors
  to expose the limits of in-place data and bitmap writes. Require a negative test with cached data and FUA bitmap and
  another with both cached. Allow the early bitmap to destage in each case and require the stale-page assertion to
  fail.
- Record CDB FUA and flush count per workload in the fake transport. A pure overwrite workload should collapse data
  FUA between fences. First touches keep the current data-before-bitmap trace. Stage reclamation, generation and
  header bytes, and the argument-off serial transcript are regression controls. Run the real APFS trace and a
  clean-remount and power-cut recovery campaign only after the host oracle passes.

## Async completion and the single-reader tail

M4 can benefit a QD1 reader when another caller is writing, because disjoint requests can enter together. A
synchronous `pread` stream still has one request outstanding, and the M4 provider still executes and completes in the
submitter's thread. Moving only the callback after inline I/O does not let the next request start sooner. One FIFO
worker moves the same head-of-line block to a different thread. Increasing write batch size can make that block
longer. Device slots, APFS dependency waits and overlapping page ranges can also limit progress after M4.

If M4 leaves significant disjoint mixed-read latency, the follow-on design is a bounded Data submission queue with a
reserved read lane and completions out of the submitter. Reuse the root worker pattern in `NativeUfs.cpp` for copied
completions and retained descriptors/device references, and the DirectDisk policies for staging and range exclusion.
Keep it behind a dedicated default-off boot opt-in. Use preallocated request records and separate read/write dispatch
capacity. Each accepted request enters the ordered range wait list before returning success. Dispatch eligible
requests to workers, with at least one Data stage and one runnable worker reserved for reads. Preserve the preference
of older overlapping writers over later reads. Never occupy a reserved read worker with a blocked overlapping request.
Limit Data's outstanding writer commands so at least one transport data slot can serve a disjoint read when
root/background traffic permits. If only one transport slot is available, prioritize eligible reads at 128 KiB command
boundaries and measure the remaining device-command tail before promising a gain.

Follow the IOKit accepted/rejected contract. Copy the completion and the needed attributes before returning. Retain
the memory descriptor and device while work is outstanding. Preserve the byte count and status and invoke exactly one
callback for accepted requests, outside driver locks and from a completion context that can reenter submission or
sync. A rejected submission returns an error with no callback. Saturation applies backpressure while workers continue
making progress. It must never silently fall back to a caller-thread write or drop an accepted write. Release a
request's stage and ordering ownership when the I/O is done, before calling out, but keep the references and
completion record through the callback. Sync, mode change, sleep and stop fence accepted queued as well as active I/O.
Sleep flushes before holding the link and resumes admission on wake or failure. Stop rejects new work, resolves
accepted work exactly once and keeps callbacks and late DMA from accessing released state. Write coalescing can merge
already durable bitmap runs as today, with no wait to batch. Preserve 128 KiB dispatch boundaries and FUA in this
follow-on so the latency result isolates scheduling.

The host test design uses controlled command completion instead of wall-clock thresholds. Hold a writer between its
first data command and bitmap publication. Assert that a disjoint 4 KiB reader is submitted and completes on a worker
before the writer is released, and that an older overlapping writer still blocks a reader of its page. Saturate write
submissions and prove reserved read progress, then flood reads and prove write progress. Measure queue-full and
one-slot behavior. Test 512-byte overlap and boundary splitting, sync fencing of accepted queued writes, backpressure
and reentrant callbacks during sync, mode change, sleep veto, wake and stop. Inject buffer errors, transport poison
and late DMA. Assert exact callbacks, bytes and retain/release counts, no callbacks on rejected requests, no callback
under either lock, and no reuse of quarantined stages. Compare the observable disabled path against the existing
serial transcript. A device test then repeats M1's D5 mixed rows with queue/dispatch/UFS/completion timestamps to
distinguish software delay from the physical command tail.

## M5 switches and instrumentation

On `main`, M5 is opt-in. The defaults are `data-store-write-back=0` and `data-store-queue=0`.

- `data-store-write-back=1` opts a read-write direct Data disk into write-back. The advertised cache state is fixed at
  publication. Only an ordinary write whose entire provider request is already present uses WRITE(10) without FUA.
  First touches, mixed present/clear requests, the bitmap and store setup use FUA. A forced write drains older I/O
  and flushes any dirty epoch before using FUA. The provider accepts both media sync entry points and the barrier
  option as a whole-LUN SYNCHRONIZE CACHE(10) with IMMED clear. Barrier and FUA advertisement stay off while APFS
  negotiation and physical durability are qualified. IOKit file-level sync behavior depends on the calls APFS
  actually issues.
- `data-store-queue=1` arms the worker queue when `data-store-concurrent=1` or `=2` supplies at least two stages. It
  is active only in concurrent mode. The workers can overlap disjoint IOKit requests from the same workload,
  including several outstanding reads or writes where the UFS data slot count permits. Each individual request
  retains its 128 KiB command boundaries and page-range ownership. A QD1 synchronous syscall stream submits one
  request at a time. Each direction has 64 preallocated records and its own workers and stages. The writer count
  leaves a UFS slot for reads when two or more transport data slots exist and other traffic allows. On a single
  slot, a waiting Data read can pass the head of the transport wait list once per head at a command boundary.
  Pool exhaustion returns `kIOReturnNoResources` without a provider callback. The completion thread can reenter
  submission or sync while workers continue draining I/O.
- An explicit sync drains accepted requests, including pending queue entries, before the flush. Mode changes
  also flush a dirty epoch. Sleep drains and flushes before the UFS link hold and vetoes suspend on failure.
  IOBlockStorageDriver's public shutdown path calls the provider sync on writable media. The provider returns
  flush errors and logs transport failure. The public shutdown caller waits up to five seconds and ignores the
  return value, so qualification requires a captured successful flush. Stop drains
  I/O, finishes callbacks via retained references and logs any dirty epoch it cannot confirm. Check shutdown
  dispatch and remount on 27.3 during qualification.
  [IOBlockStorageDriver shutdown](https://github.com/apple-oss-distributions/IOStorageFamily/blob/main/IOBlockStorageDriver.cpp#L226-L287)

`machdep.piano_data_io` shows `queue`, worker counts, `pending`, `accepted`, `finished`, callback starts and queue-full
returns. Its three queue timing sums cover acceptance to dispatch, dispatch to I/O finish and I/O finish to callback.
It also shows `write_back`, `dirty`, write-back flush attempts, forced requests and barrier options received.
`fua` and `cached` count tracked runtime write accessors and their timing sums include slot wait and transfer.
`store_rd` and `store_sync` include setup and power calls. They report cumulative counts and microseconds as
`OPS/USus`, so use per-row deltas. Convert raw tick fields using the host timebase. Data `inflight_max` is the
stage peak. UFS `ufs_peak` is the shared slot peak since boot. Use the
`native-ufs-perf=1` per-window transport samples in a matching trace to attribute physical overlap.

### Scratch-window power-cut results, 2026-11-26

All three modes passed on a disposable window in `linux_rootfs`. Each run used the direct PSCI `SYSTEM_RESET`
path and verified every record acknowledged after `F_FULLFSYNC` on recovery.

| Mode | Acknowledged records recovered | Recovery errors |
| --- | ---: | ---: |
| M4 concurrent write-through | 32/32 | 0 |
| M5 write-back | 32/32 | 0 |
| M5 queue | 32/32 | 0 |

[November 26 results](../results-2026-11-26.md#data-storage)

### Scratch-window throughput and latency A/B (2026-11-27)

Three RAM boots ran the pinned `dskbench` on the confirmed `linux_rootfs` scratch window at LBA 64973631
(6,291,456 pages), using the same frozen M5 images that passed 32/32 power-cut durability on 2026-11-26.
The owner's Data store was untouched across all three boots. Store and UFS kernel errors were zero.

| Run | Phases | Verdict |
| --- | --- | --- |
| `m5bench-m4cc` | A1 serial → B M4 concurrent → A2 serial | `VERDICT GAIN`, `rc=0`, 0 errors |
| `m5bench-m5wb` | P1/P2 concurrent, `data-store-write-back=1`, `data-store-queue=0` | `VERDICT PASS`, `rc=0`, 0 errors |
| `m5bench-m5q` | P1/P2 concurrent, `data-store-write-back=0`, `data-store-queue=1`, four read workers and four write workers | `VERDICT FAIL`, 401 queue-full refusals between P1 and P2, 0 during timed read/write phases |

Serial is the A1/A2 mean, M4-conc is B, and M5-wb and M5-queue are the P1/P2 means from their respective boots.
Ratios use M4-conc as the baseline.

| Workload and metric | Serial | M4-conc | M5-wb | M5-queue |
| --- | ---: | ---: | ---: | ---: |
| Data 4 KiB random read QD1, IOPS | 11,804 | 11,922 | 10,626 (0.89×) | 6,124 (0.51×) |
| Data 4 KiB random read QD1, p99 µs | 172.4 | 154.4 | 173.9 (1.13×) | 235.2 (1.52×) |
| Data 4 KiB random read QD8, IOPS | 13,814 | 61,988 | 57,736 (0.93×) | 35,407 (0.57×) |
| Data 4 KiB random read QD8, p99 µs | 8,294.3 | 202.5 | 227.8 (1.13×) | 451.9 (2.23×) |
| Data 4 KiB random read QD32, IOPS | 13,762 | 75,660 | 68,208 (0.90×) | 41,561 (0.55×) |
| Data sequential read 128 KiB QD1, MB/s | 801.5 | 867.3 | 822.1 (0.95×) | 450.6 (0.52×) |
| Data sequential read 1 MiB QD1, MB/s | 900.6 | 953.2 | 922.2 (0.97×) | 678.1 (0.71×) |
| Data sequential read 1 MiB QD8, MB/s | 690.5 | 3,207.6 | 2,775.1 (0.87×) | 1,997.7 (0.62×) |
| Data fill 4 MiB QD1, MB/s | 397.6 | 452.3 | 822.1 (1.82×) | 406.6 (0.90×) |
| Data sequential write 1 MiB QD1, MB/s | 447.1 | 455.0 | 841.1 (1.85×) | 386.1 (0.85×) |
| Data sequential write 1 MiB QD8, MB/s | 494.0 | 750.7 | 3,089.8 (4.12×) | 768.8 (1.02×) |
| Data 4 KiB random write QD1, IOPS and available p99 | 6,159 | 6,617 | 11,969 (1.81×), 756.7 µs | 12,767 (1.93×), 675.2 µs |
| Data 4 KiB random write QD8, IOPS and available p99 | 6,006 | 6,158, 5,518.1 µs | 11,066 (1.80×), 3,836.7 µs | 12,676 (2.06×), 4,287.1 µs |
| Data 128 KiB write QD1, MB/s | 413.2 | 412.6 | 706.5 (1.71×) | 234.8 (0.57×) |
| Mixed reader alone, p99 µs | 119.7 | 133.3 | 134.5 (1.01×) | 189.4 (1.42×) |
| Mixed reader with 1 MiB writer, p99 µs | 2,394.9 | 262.9 | 243.7 (0.93×) | 426.5 (1.62×) |
| Mixed writer, MB/s | 456.9 | 434.9 | 816.8 (1.88×) | 434.6 (1.00×) |
| `sync=none` 4 KiB, p50 µs | 127.5 | 121.1 | 51.0 (0.42×) | 44.9 (0.37×) |
| `fsync` 4 KiB, p50 µs | 133.2 | 135.7 | 58.2 (0.43×) | 188.9 (1.39×) |
| `F_FULLFSYNC` 4 KiB, p50 µs | 1,510.6 | 1,470.7 | 853.7 (0.58×) | 1,276.6 (0.87×) |
| `F_BARRIERFSYNC` 4 KiB, p50 µs | 1,504.9 | 1,546.7 | 791.0 (0.51×) | 1,283.5 (0.83×) |

Raw root controls stayed flat across the three boots, at approximately 4,645–5,071 IOPS for 4 KiB QD1,
37,981–38,801 IOPS for 4 KiB QD8, and 1,663–1,857 MB/s for 1 MiB QD8.

**Provider counter deltas.** M5-wb averaged 351,194 `cached` writes out of 351,496 writes (99.9%) and
807 flushes. Cached writes averaged 53.7 µs versus 191.2 µs for `fua` writes on M4-conc. M5-queue
averaged 381,550 queued write operations, with `q_w_us` at 256–289 µs.

**Queue-full diagnosis.** The inter-phase burst between P1 and P2 accounted for `full=401` and the M5-queue
failure verdict. The timed read and write phases recorded zero queue-full refusals, and the kernel logged zero
store or UFS errors. Worker-thread dispatch overhead accompanied the read regression even in the timed phases.

- **M5-wb** delivered 1.71×–4.12× M4-conc write throughput, reaching 3,089.8 MB/s for 1 MiB QD8 sequential
  writes and about 11.1k–12.0k IOPS for 4 KiB random writes. Sync p50 latency fell 42%–58%, and reader p99
  under mixed load fell 7%. With zero errors and the prior 32/32 power-cut pass, it is a strong daily candidate.
  Daily promotion requires an owner decision and a fresh owner-store backup. `data-store-write-back=0` remains
  the default on `main`.
- **M5-queue** is unsuitable for daily use. Across QD1/QD8/QD32, 4 KiB random-read IOPS fell to 0.51×–0.57×
  M4-conc, and 1 MiB QD8 read throughput fell to 0.62×. The 128 KiB write rate fell to 0.57×, mixed-reader p99
  rose to 1.62×, and `fsync` p50 rose to 1.39×. The 401 inter-phase refusals also failed the run.
  `data-store-queue=0` remains the default on `main`.

[November 27 scratch-window results](../results-2026-11-27.md#disk-m5-scratch-window-ab)

### Expected gain from M1

The [APFS analysis](m5-apfs.md) found cached traffic in scratch sync and mixed intervals. The estimates below predate
the measured scratch comparison. Write-back remains scratch-qualified, and installed workload eligibility is undetermined.

- **Write-back.** QD1 sustained 6,494 writes/s, or about 154 µs per write. The 4 KiB write/read p50 gap was 33 µs
  (103 vs 70 µs). If removing FUA saves 33 µs per eligible overwrite, the estimate is about 8.3k IOPS (+27%)
  with all writes eligible, or 7.3k (+12%) with half eligible, before extra flush time. This is a sensitivity
  estimate. The p50 gap includes filesystem and device work, and write-back can introduce new flushes. At an
  illustrative 1.7 ms per added flush, each flush consumes the savings from about 52 such overwrites. Measure the
  actual eligible share and flush rate. Android's 36.7k QD1 IOPS is a 5.65× comparison from a different stack.
- **Queue depth.** M1's 15,696 QD8 4 KiB reads/s is within 1% of QD1. Two fully utilized commands at M1's per-command
  rate would yield about 31k IOPS, four about 62k, before sharing device service time, CPU and APFS dependencies.
  Android's QD8 88.7k is 5.65× M1. Use M4 concurrent write-through as the queue's control and measure the increment
  over it. The M1 mixed p99 penalty was 2,240 µs above the 85 µs idle reader. Eight 128 KiB runs from a 1 MiB writer
  can account for an entire request wait. A 128 KiB run at M1's 478 MB/s averages about 274 µs. Reserved read dispatch
  targets queueing behind the whole writer, with a remaining device tail measured separately. A 50% reduction from
  M1 would put mixed p99 near 1.16 ms. Report the measured difference from both M1 and the M4 control.

### Installed tablet A/B read list

1. Qualify M4 first. Run write-back feature and command tracing, clean remount, and controlled cold-power recovery
   on a disposable store before touching the owner store. Show APFS's ordinary fsync, full sync, barrier and
   checkpoint requests and identify which reached the provider and completed SYNCHRONIZE CACHE. Require cache
   coherence, device cache/FUA capability evidence, and verified recovery. Keep the daily installer exclusion.
2. For installed testing, take a fresh, verified sparse backup with the maintenance sequence above. Use the
   installed-from-disk identity and pinned seed and store window. Save the ESP and matching build record.
   Use an authorized diagnostic installed entry with a verified rollback. Compare A–B–A for each switch on the same
   KC, changing only that switch's boot argument. Keep `data-store-concurrent=1` in all phases so both controls
   use concurrent write-through. Set only `data-store-write-back=1` in the write-policy B and only
   `data-store-queue=1` in the queue B. Run the combined configuration only after both standalone comparisons.
   Use this document as the image spec's `ANALYSIS`. Confirm effective args including ADT spillover, boot UUID,
   the intended provider chain, state 2, unchanged
   generation, exact seed and bound window. Verify `write_back` and `queue` against the intended phase before reads.
3. Run the same hash-verified M1 `dskbench`, 1 GiB span and seed for every phase. For each phase capture the read list
   below under `--ro "$DATA_FIXTURE"`, then under `--ro "$ROOT_RAW"` on the independently identified root window.
   Hash the nonsparse Data fixture outside timed windows and require positive `store_rd` deltas on Data.

   ```text
   randr:4k:1:10,randr:4k:8:10,randr:4k:32:10,seqr:128k:1,seqr:1m:1,seqr:1m:8
   ```

4. Use a new benchmark-owned `--dir` for each pass. Compare fill separately from its pre-filled overwrite rows.
   Capture the following rows with the same settings. `mixed:10` emits the QD1 reader-alone and reader-with-1 MiB
   writer p99 as well as writer throughput. Preserve the dskbench BEGIN/END and verification output.

   | Comparison | Rows |
   | --- | --- |
   | Write-back | `randw:4k:1:10,randw:4k:8:10,randw:128k:1:8,sync:none:500,sync:fsync:300,sync:full:100,sync:barrier:300,mixed:10` |
   | Queue depth | `randw:4k:1:10,randw:4k:8:10,seqw:1m:8,mixed:10,sync:full:100,sync:barrier:300` |

5. Record p50/p95/p99, operations, bytes, IOPS, throughput and `inflight_max` before and after each switch.
   Capture the Data sysctl and transport window around each row, link and WriteBooster evidence, queue acceptance,
   completions, full returns, accessors and bitmap publications. Count physical overlap separately from userspace
   QD and Data stages. For write-back require cached traffic on overwrites, data and bitmap FUA on new pages,
   a positive completed sync on full/barrier or a captured APFS equivalent, and checksum survival across clean
   remount. For queue depth require accepted off-caller work, physical overlap at QD8 where slots permit, a lower
   mixed reader tail, and stable QD1 and raw root controls. Record a performance miss as a result when the path
   and evidence are valid. Stop on any short I/O, queue refusal under the benchmark, degraded store, quarantine,
   unexpected reset, checksum or APFS error. Preserve the console and use the saved boot or verified rollback
   procedure from the M4 preflight.

## Fix-image gate

Path: decoded 27.3 libsystem_kernel pread/pwrite/fsync/fcntl → XNU VFS/UBC → APFS Data → IOMedia/IOBlockStorageDriver → NativeRamBlock doAsyncReadWrite / doSynchronize / doSynchronizeCache → ordered admission and DirectDisk / PersistOverlay → tracked NativeUfs store I/O → UTRL slots, WRITE(10), SYNCHRONIZE CACHE(10), validated completion → KIOXIA UFS. Sheng RamBlock 525abe5 supplies the caller-thread reference. The Ubuntu v1.4.0 sd/UFS comparison is above.

Checks: ours-OK in the host models for the bound store format, stage and range ownership, data FUA before bitmap FUA, cached overwrites and fenced sync, failed flush handling, queued dispatch and completion, the read reserve, one-slot fairness, and tracked DMA quarantine. Both provider sync entry points and the barrier option compile for arm64e. The scratch-window direct-reset and performance results are above. Open checks include APFS's exact fsync and checkpoint calls including shutdown, device cache/FUA capability, cold-power durability beyond the direct-reset tests, UFS occupancy under load, and installed APFS recovery. An installed write-back comparison requires the owner decision, fresh verified backup, effective-arg and boot checks, and recovery plan above.

Root cause: M1 measured a whole-request Data gate, 15,696 QD8 read IOPS against 15,580 at QD1 and an 85 → 2,325 µs mixed-reader p99. The gate covers all writer stages and bitmap publications. It can hide free transport slots. M1 Data also incurs FUA on every overwrite, which is a candidate for part of the 6,494 vs 36,693 QD1 write gap. M1 did not measure the share of time spent in FUA writes. WriteBooster was already enabled.

ONE change: M5 caches only ordinary overwrites of fully present request ranges behind `data-store-write-back=1`, with synchronous whole-LUN fences and the existing first-touch protocol. The independent `data-store-queue=1` scratch run used bounded off-caller dispatch and completion with write-through. Both switches default to `0` on `main`.
