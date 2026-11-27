# DPU M2: conditional scanout cache clean

Analysis and host implementation, 2026-11-26. On November 27, `v131y12u-m2opt2`
earned a RAM-qualified **`M2 PASS` (`rc=0`)** with automated C920 optical grading
and a reverse-order sanity check. **Owner decision (2026-11-27, `OWNER-0749`):**
promotion to the installed daily build is APPROVED (combinable with the `y12t`
Night Shift install package if safe, with backup, SHA-256, read-back verify, and
a ready rollback).

## Decision

Use a **write-combined-only** conditional clean, with a full scanout clean
as the fallback.
The kernel can read `IOSurface::getCacheMode()`, but must also verify the
backing pages' effective cache attributes. A retained, initialized backing
whose CPU writers remain write-combined can skip repeated pixel cleans.
CPU-write absence is neither required for that case nor established by M1.

The policy is implemented behind `piano-dpu-clean-skip=1`. The daily default is
**OFF** (`piano-dpu-clean-skip=0`) on installed **v13.1y12q**.

## Baseline and scope

The saved `20261125-runs/dpu-m1` observation passed 44/44 checks.
`flips.txt` records 1,719 flips, 1,719 cleans (mean 872 us), three maps
(mean 1,995 us), and no flips without a clean. The desktop swap summaries
report the lossless `&BGA` plane-zero flip path, one fallback copy, and no
cursor-on-panel copies. This establishes the existing path, not CPU coherency
of an alternative path. Snapshots and Night Shift logs are observations of
display state; they do not identify CPU writers.

The saved runner log has successful preflight/provenance, a responsive system,
a proven desktop session, nonempty captures, known-positive observer lines,
and the same boot at the end. All seven snapshots report 122 loads and zero
stores, and match 120 Hz on both links. The two asleep samples still have
the interfaces enabled and advancing frame counts. Night Shift enable/strength
changes reach CoreBrightness; the observer records matrix calls while DSPP
PCC remains disabled. These are M1 observations, not M2 coherency tests.

The broader baseline analysis reports approximately 1.35 ms wall and 5.4 ms
aggregate CPU for a four-thread clean of a 27.3 MB surface. M1's 872 us is a
different captured mean; neither value is a new measurement by this job.

The task is to trace macOS 27.3 build 26C5035r IOSurface, QuartzCore,
WindowServer/SkyLight and IOMobileFramebuffer, compare Linux page acquisition
and mapping policy, and identify an auditable condition for retaining or
skipping `DpuFlip.cpp`'s full-surface `DC CVAC`.

## Static findings

The trace used the pinned macOS 27.3 kernel collection and decoded shared
cache. QuartzCore's display allocator passes `0x400` (write-combined)
to `kIOSurfaceCacheMode`. A platform-dependent branch can pass `0x700`
(real-time cache). These modes have different maintenance requirements.

QuartzCore's software renderer can lock an IOSurface writable, render or
clear pixels through its base address, then unlock and flush processor caches.
M1 does not establish which observed scanout surfaces used that path or
whether CPU writes ended after initialization.

| Record or accessor | Observed behavior | Use at flip time |
| --- | --- | --- |
| `IOSurface::getCacheMode()` | Returns the allocation's CPU cache mode. | Require WC and validate effective page attributes. Default is unknown. |
| `getMapCacheAttribute()` | Reports separate mapping metadata. | Verify physical pages independently. |
| `getLockSeed()` / `getUnlockSeed()` | Writable CPU lock and unlock increment distinct counters. Device writes increment both. Read-only CPU locks leave both unchanged. | These counters do not identify the writer or exclude concurrent stores. |
| `IOSurfaceGetSeed` | Exposes the modification seed, which can also change during transaction publication. | A stable seed cannot exclude stores through an existing mapping. |
| Processor-cache dirty state | Tracks pending maintenance. Device lock or prepare can consume it. The clean can bypass maintenance in several cache modes or when IOSurface assumes coherence. | A clear flag does not prove visibility to the DPU. |
| Swap damage rectangle | Describes changed image content. | It cannot identify the writer or bound CPU-dirty cache lines. |
| GPU fence | Signals producer completion. | Preserve NativeGopDisplay's existing wait and handoff. |

A write-combined CPU unlock calls `OSSynchronizeIO`, which issues a system
DSB in the pinned kernel. This publishes the writer's stores. The framework's
explicit cache flush also takes a read-only lock during maintenance.

The existing read-only `kIOMapCopybackCache` alias can change the named
memory entry's cache mode. The allocation request alone cannot establish
that the backing remains WC. The conditional policy needs a maintenance
view that preserves the effective attributes.

## End-to-end path and the reference comparison

WindowServer launches SkyLight, which uses QuartzCore for composition.
QuartzCore publishes a surface through IOMobileFramebuffer's layer and swap
calls. The normal GPU path coexists with the software destination path.

The inherited kernel `surface_layer_map` would call `surface_map`, lock
the device surface and prepare it. NativeGopDisplay's success-only mapping
overrides bypass those operations for the normal plane path. Other clients
can consume shared pending maintenance independently.

NativeGopDisplay reads the swap's layer-zero IOSurface, retains the job's
surfaces, samples the native Adreno fence, and waits on the copy worker
before `draw()` can attempt `dpuFlip()` (`NativeGopDisplay.cpp:1937`,
`:1995`, `:2145`). Its existing no-fence/timeout behavior is not strengthened
by M2 and must remain separately observable. `dpuFlip()` normalizes eligible
lossless `&BGA` plane zero to the existing linear BGRA request and passes
the IOSurface plus its IOMemoryDescriptor (`:1732`). PianoDpu checks geometry,
format, backing bounds and slot limits, wires/lists pages, maps the DPU's
SMMU slot, cleans pixels, and programs/latches VIG0/VIG1 -> LM0/LM1 ->
DSPP0/1 -> DSC0 -> INTF1/2 -> bonded DSI -> panel. The clean occurs before
the same-surface early return as well as before a new latch
(the legacy path in `Flipper::flipSurface`).

The native GPU's imported-memory path is separate from IOSurface's device
cache protocol: `NativeAdrenoMemory.cpp:732` imports a user address range;
`:903` and `:914` perform the configured device-publication/acquisition
maintenance via physical pages. It is not a reason to reinterpret an
IOSurface seed as a GPU fence or to remove GPU maintenance in this task.

| Reference operation | Our operation | Comparison and implication |
| --- | --- | --- |
| Linux `msm_gem.c:240-243`: new WC pages are synchronized for device access once during page acquisition. | `Flipper::cleanForFlip` and `CleanEpoch::prepare` initialize a qualified whole backing after the SMMU mapping succeeds. | Same acquisition-time cache maintenance for a qualified WC backing. |
| Linux `msm_gem.c:357-361`: WC BOs use `pgprot_writecombine`. | `KernelHw::surfaceMap` in M2 preserves existing CPU mappings. `surfaceAttributes` checks the wired pages and `surfaceCleanPhysical` uses the physical aperture. | Different: IOSurface owns the CPU mapping, so PianoDpu verifies its effective mode. |
| Linux `msm_gem.c:750-751`: dumb scanout allocation requests `SCANOUT | WC`. | QuartzCore allocator requests `0x400` or, conditionally, `0x700`; IOSurface supplies the backing. | Different allocator ownership: do not force every IOSurface to WC or assume all imported buffers use the normal display allocation path. |
| Linux `msm_gem.c:251-277`: releasing the pages ends their acquired lifetime. Linux synchronizes a WC BO for CPU access before releasing its pages. | `Flipper::evict` unmaps, syncs the TLB, releases its prepare and clears the epoch. NativeGopDisplay and IOSurface own the producer handoff. | Different release responsibilities. PianoDpu starts a new epoch when it acquires the backing again. |
| Reference sheng `ShengDpuObservation.c:33-93` and its header expose source-bound read-only DPU observations. | Existing PianoDpu owns the actual flip/cache path. | Missing in inspected sheng reference: a qualified native conditional scanout-clean implementation to adopt. Its observations are not coherency evidence. |

Linux source examined: retained v1.4.0 tree, `msm_gem.c` SHA-256
`df036d85b8c19aaaf262cbdb4539409156c0a4f38384db1cf0f74a21cf6e97e4`.
The inspected sheng checkout is `00ba909`. No Linux function text or Apple
listing is incorporated here. For the mapping distinction, Apple's
[IOMemoryDescriptor implementation](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOMemoryDescriptor.cpp)
contains the explicit-mode named-entry update in `memoryReferenceMap`;
the actual live page attributes after our alias is created remain unknown.

## M2 policy and its safety conditions

This is one change to pixel-clean policy, including the mapping and accounting
needed to make that policy correct. It is not a request to change allocator
policy, fences, SMMU synchronization, display timing, color or power.

1. Key the clean epoch by the retained surface **and backing descriptor/page
   lifetime**, using the existing slot ownership. First map, eviction/re-map,
   changed backing, or a newly covered pixel range requires initialization.
   A successful clean of the whole backing can cover later subranges; a clean
   of just the current scanout range cannot cover arbitrary future offsets.
2. Read kernel `getCacheMode()` with the same kind of symbol-bound accessor
   already used for IOSurface geometry. Require exact `0x400`; treat default,
   copyback, real-time `0x700`, unsupported values and failed reads as fallback.
   `getCacheMode()` reads cached metadata and is not the effective-page test.
3. For a WC candidate, avoid introducing the current copyback alias. Verify
   **all** backing pages in the relevant retained span using
   `pmap_cache_attributes(ppn) & VM_WIMG_MASK == VM_WIMG_WCOMB`, including
   after mapping, and revalidate at each ownership handoff/flip unless an
   equivalent cache-attribute lifetime guarantee has been proved. The native GPU
   already uses the exported `pmap_cache_attributes` API. Do not infer WC from the first page only.
   This describes pmap-tracked mappings; it is not a blanket proof about raw
   physical aliases. Cacheable physical-aperture views must be used only for
   reads/maintenance during the epoch. Any pixel writer through such a view
   invalidates the skip premise and requires the full clean.
4. Do one actual PoC clean on acquisition before setting the initialized
   flag. Use a maintenance view that does not change page attributes, such
   as the existing DRAM physical aperture, with its bounds validated. The
   native GPU's physical-page maintenance is a local example of this approach,
   not proof that its function can simply be imported by PianoDpu. Do not
   create a copyback view and assume releasing it restores WC.
5. Skip subsequent **pixel** cleans only while this epoch, geometry coverage,
   effective WC attributes, and producer completion/ownership remain valid.
   Correct CPU writers publish their writes before handing over; no producer
   may write an in-use scanout concurrently. A flip-thread barrier alone is
   not a replacement for publication on a different writer CPU.
6. Preserve a system DSB on the skip path, including same-surface re-shows.
   Today's same-surface early return bypasses register programming, so the
   barriers in `program()` are insufficient to cover that case after removal
   of `cleanRange()`'s DSB. Keep all page-table, guard-buffer and TLB maintenance
   regardless of pixel-clean eligibility. Revalidate immediately before latch
   under the existing handoff; do not race a concurrent cache-mode change.
7. On a non-WC page, changed mode, unsafe alias, failed initialization or
   uncertain handoff, invalidate the epoch and clean the full scanout range.
   An invalid backing span or a failed required fallback clean returns a
   refusal to NativeGopDisplay for the copy path.
   Existing clean policy 1 can show an alias-failure frame without cleaning;
   that existing `without` counter must stay separate from a deliberate safe
   WC skip. Qualification needs strict clean-failure handling.

These conditions do not require a private dirty-bit getter and do not alter
IOSurface's flags. The flags and seeds are useful for bounded diagnosis, but
not as an alternate fast path for cached backing. If preserving WC cannot
be established, the one change remains a fully conservative fallback and
has no claimed performance win.

## Host-test plan

Before any implementation image, host tests must cover the actual policy
integrated with `Flipper` and a model of backing lifetime, producer handoff,
page attributes and clean failure. Required cases:

* Three WC backings, repeated round-robin swaps: exactly one initial clean
  per successful map epoch, then deliberate skips; the first clean completes
  before eligibility is published. Repeat a currently shown surface too.
* WC CPU writes followed by proper publication, GPU-only writes, and alternating
  CPU/GPU updates. Seed changes do not change WC eligibility. Equal, unequal,
  wrapped and zero seeds must not accidentally authorize a cached-buffer skip.
* Copyback/default/real-time/unknown modes: full clean each flip. An allocator
  reporting WC with one effective copyback page must take the full fallback.
  Include a page at the end of the span and an alias that changes attributes.
* Eviction, pointer reuse with a different descriptor, backing replacement,
  enlarged/shifted scanout ranges, purge/reacquisition and a mode transition:
  reset the epoch and reinitialize its coverage. Retention must prevent use
  after release; stale attributes from an old slot cannot qualify a new one.
* Failed map, partial initialization, missing maintenance view, failed clean,
  failed fence wait and aborted swap: no successful safe-skip accounting and
  no successful latch on an uninitialized candidate. Preserve existing copy
  fallback and lifetime/release behavior.
* Assert ordering, not just counts: producer publication -> first clean or WC
  predicate -> DSB -> scanout, including the re-show early-return path. SMMU
  table cleans and TLB sync still occur on every required mapping update.
* Counter invariants distinguish initialization, cached/unknown/attribute-change
  fallback, intentional skips, clean failures and alias failures. Account time
  both per clean and per flip; include attribute-check overhead.

Then compile the kernel translation units/import bindings on the host, without
building an image, and run the existing DPU and NativeGopDisplay suites. A
future symbol-layout or SDK mismatch must fail closed to the existing clean.

The M2 qualification diagnostics passed all 11 PianoDpu and both
NativeGopDisplay host suites. The conditional-clean suite also passed
under ASan/UBSan. Both kexts built as arm64e objects against the pinned
27.3 target with zero unresolved imports. The object build checks for
the IOSurface cache-mode accessor in `KC_BASE`. If binding is unavailable,
M2 treats the cache mode as unknown and cleans every scanout.

## RAM-boot read list

### Boot and capture

Use the same frozen macOS 27.3 RAM image for both arms. Build the combined
qualification image with `PIANO_GPU=2` and `PIANO_VMM_PRESENT=0`, and merge
the following display boot arguments once per key. Keep the other ride-along
settings identical. The timing comparison uses the fixed 120 Hz profile
with adaptive refresh and native panel sleep disabled.

```text
piano-dpu=2 piano-dpu-smmu=2 piano-dpu-flip=4 piano-dpu-clean-skip=1
piano-dpu-snapshot=1 piano-dpu-modes=120 piano-dpu-irq=1 piano-dpu-underrun=1
piano-dpu-watch=10 piano-dpu-latch=50 piano-dpu-panel=0
native-gop-display-flip=1 native-gop-display-flip-lossless=1
native-gop-display-cursor=0 native-gop-display-thumb=0 native-gop-display-defer=2
native-gop-display-gpu-fence=1 native-gop-display-gpu-fence-ms=1000
```

Leave `piano-dpu-refresh` unset and keep `machdep.native_gop_fence_deadline=0`
through both windows. Boot **both** arms with `piano-dpu-clean-skip=1` so
they share the strict, WC-preserving, single-CPU aperture implementation.
Booting with `piano-dpu-clean-skip=0` selects the daily alias path and
yields an invalid control. Set the policy once before each measurement
window. On one boot run A then B. For paired boots run the same procedure
with one arm per boot.

```sh
sudo sysctl machdep.piano_dpu_clean_skip=0   # A: strict full clean
sudo sysctl machdep.piano_dpu_clean_skip=1   # B: verified-WC skip
```

1. Verify RAM image provenance and effective args, a responsive desktop,
   and continuous console collection. Save the boot evidence separately.
   Expect `piano-dpu-clean-skip=1: strict WC policy`, `B1b: serving flips`,
   snapshot registration, the fixed 120 Hz mode on both links, and
   `NativeGopDisplay: flips through PianoDpu` and
   `NativeGopDisplay: GPU fences from PianoA830`. Check the `flip '&BGA'`
   layout line and a `PianoDpu: clean map` line. A WC acquisition should
   show mode `0x400`, positive WC pages and zero other/unknown pages
   before and after the SMMU map, and `init 1 reason eligible` during
   conditional policy. Later re-shows of the same target can skip.
2. Capture one root snapshot immediately before and one immediately after
   each workload. Start when the desktop is ready. Use the same
   approximately 60-second sequence, mode, brightness, power, thermal
   conditions and display topology in both arms. Include continuous
   animation or Metal to exercise the hardware latch and repeat the same
   scrolling, drag/resize/occlusion, translucency and video transitions.
   Keep the console running from a cumulative GOP flip summary just before
   the first snapshot until the next summary after the final snapshot.
   Keep the workload quiet in the padding so it adds at most 25% extra
   flipped swaps. Save the complete console slice between those summary
   lines inclusive for each arm. GOP reports about every 10 seconds
   while swaps are active. Trigger a small redraw if an idle desktop
   delays the closing report. On one boot use separate, ordered slices
   and switch policy only after saving the closing report for A.
3. Record the panel optically (for example with a C920) in both arms.
   Compare against expected visible generation markers on owned test
   surfaces. Include sparse tile and whole-frame updates, published CPU
   writes, GPU writes, repeated frames, cached imports, eviction/reacquisition
   and lock/unlock. Count actual comparisons, including both changed and
   untouched tiles, and record every stale tile, flicker, partial-frame
   residue or other mismatch. Run a separate matched sleep/wake check
   if native panel sleep is being qualified on the combined image.
4. Check WindowServer's PID at both ends and review crash reports and
   process restart evidence for the entire console slice. Count new
   WindowServer crashes in each arm. Record zero only when the evidence
   covers the window. Retain any DPU revert or nonzero underrun, GPU fence
   timeout, damage audit mismatch, and GOP flip-copy fallback lines.

For the two captures in each arm, use the following readout in that RAM
session. Save stdout as the named file. The grader runs offline on the
host.

```sh
m2_snapshot() {
    boot=$(sysctl -n kern.bootsessionuuid) || return 1
    pid=$(pgrep -x WindowServer) || return 1
    printf 'boot %s\nwindowserver-pid %s\n' "$boot" "$pid"
    sudo sysctl -n machdep.piano_dpu_snapshot
}
# Surround the A workload with these two reads.
m2_snapshot > A-before.txt
m2_snapshot > A-after.txt
# Surround the B workload with these two reads.
m2_snapshot > B-before.txt
m2_snapshot > B-after.txt
```

### Evidence and verdict

The read-only snapshot prints `clean: shown … passes … init … skip … failed
… alias-failed … without …`, `clean: fallback …`, `clean: pixel-us …
check-us …`, and, on an M2 boot, `clean: m2 policy … writes … latched …
full … to-boot … underruns … pending … serving …`. Use differences of
the cumulative counts. `shown` includes re-shows. `latched` counts new
latches, `init` counts shown WC initialization frames and `skip` counts
shown verified-WC skips. `full` counts shown strict scanout cleans,
including A's forced control and B's handoff, unknown-mode, non-WC mode
or attribute reasons. A successful initialization clean followed by a
changed mode or attributes can add a pass before the fallback pass.
A required clean failure refuses the frame. The daily path's `without`
count is distinct from the verified-WC skip count.

`pixel-us` is the cumulative `clean_us` spent doing pixel maintenance.
The grader reports measured A and B totals, means per shown flip and
the estimated savings at B's volume, `A_mean × B_shown − B_pixel_us`.
`check-us` separately exposes the attribute-check cost. Observe latch
waits and frame-time percentiles alongside this comparison if collecting
cadence data.

Use GOP's cumulative `flip mode: … swaps flipped … N copied` summaries
for copy fallbacks. DPU's `to-boot` counts returns to the boot framebuffer.
The copy damage audit reports `… N differed` and disables damage copies
on a mismatch. NativeGopDisplay's `notify_crc_ready_gated` is a success-only
callback. There is no panel CRC measurement on this route. `gop-shot`
reads source pixels and can return busy on active scanout. Use the optical
recording as the independent panel evidence.

Save a manifest next to the four snapshot files and two console slices.
Use the actual SHA-256 of the frozen image and the exact workload ID in
both entries. Set `console_complete` after verifying each slice and enter
the measured crash and optical counts. Example values are illustrative.

```json
{
  "control": {
    "before": "A-before.txt",
    "after": "A-after.txt",
    "console": "A-console.txt",
    "console_complete": true,
    "image_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "workload": "mixed-use-60s",
    "windowserver_crashes": 0,
    "optical_checks": 120,
    "optical_mismatches": 0
  },
  "treatment": {
    "before": "B-before.txt",
    "after": "B-after.txt",
    "console": "B-console.txt",
    "console_complete": true,
    "image_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "workload": "mixed-use-60s",
    "windowserver_crashes": 0,
    "optical_checks": 120,
    "optical_mismatches": 0
  }
}
```

```sh
python3 tooling/native/grade-dpu-m2.py capture.json
```

| Grade | Thresholds |
| --- | --- |
| **PASS (0)** | Each arm spans ≥30 s and ≥300 new DPU latches. Both links advance in the same fixed mode. Arm durations and shown counts differ by at most 25% of the smaller value. Strict policy stays at A=0 or B=1 throughout its window. A shows one forced full clean per shown flip with zero skips. B shows ≥100 verified-WC frames (`skip + init`) and skips ≥90% of those frames. B reduces pixel-clean time per shown flip by ≥50%. Both arms have positive optical coverage and zero reported safety events. B's GOP copy count, GOP copy share (`copied / (flipped + copied)`) and DPU returns to the boot framebuffer each stay at or below A. |
| **FAIL (1)** | An underrun in either arm (a consumed indication or pending TOP/IRQ bits), DPU revert/SMMU fault/latch failure, WindowServer crash or PID change, fence timeout, optical or GOP damage mismatch, clean/alias failure, invalid backing, uncleaned show, or inconsistent clean accounting. With adequate comparable data, a miss of the skip/time threshold or an increase in fallback fails the run. |
| **VOID (2)** | Missing, stale, reordered or reset evidence, wrong policy or a policy write inside an arm, unmatched image/workload/mode, unavailable optical or console coverage, insufficient flips or duration, or fewer than 100 verified-WC frames. Each GOP pair must bracket its DPU snapshots with a GOP flip delta between 100% and 125% of DPU shown. Reported failures take precedence over missing evidence. |

If live WC coverage is insufficient, retain the fallback reason counters
and repeat with a workload that exercises a verified WC pool.

## RAM A/B on v131y12u-ram1 (2026-11-27)

With strict WC mode enabled at boot, `PianoGpuBench --cadence` ran two matched
38 s windows in order on the same boot. Arm A (`policy=0`) ran 2522 full cleans
for 2522 shown flips. Arm B (`policy=1`) ran one initialization clean and skipped
cache cleans on **2457/2458** verified-WC flips (**99.96%**), with zero full cleans. Pixel-clean
time fell **99.98%** from **3855.13 to 0.95 µs per shown flip**, saving **9.47 s**
at B's shown count. Attribute checks took 721,749 µs in B. Client
`drawable_wait_ms` p95 fell from **16.37 to 0.92 ms** (−94.4%). Both arms recorded
zero underruns, GOP copies, returns to the boot framebuffer and WindowServer crashes.

C920 stills looked clean in both arms. `grade-dpu-m2.py` returned **`M2 VOID`**
solely because the initial run had `optical_checks=0`.
[November 27 counters and cadence results](../results-2026-11-27.md#dpu-m2-strict-wc-clean-skip)

## Automated optical qualification on v131y12u-m2opt2

The follow-up used the same frozen RAM image at 60 Hz. The ordered A/B grader
returned **`M2 PASS` (`rc=0`, 0 FAIL, 0 VOID)**. A forced a full clean on all 2518
shown flips. B initialized once and skipped **2466/2467** verified-WC flips
(**99.96%**). Pixel-clean time dropped from **3864.24 to 0.95 µs per shown flip**
(**99.98%**), saving **9.53 s** of kernel CPU cache-clean time over 38 s.

Automated C920 grading counted six checks per arm with zero mismatches. Each
arm checked three cadence frames for liveness and three borderless fullscreen
native RGB frames for colour. `flicker-scan` found zero transient frames across
12,370 frames. The reverse-order B → A sanity check passed both policy
transitions and two optical checks with zero mismatches. WindowServer held PID
102 throughout, with zero crashes, GPU faults, kernel panics, underruns, GOP copy
fallbacks or returns to the boot framebuffer.
[Full automated qualification results](../results-2026-11-27.md#v131y12u-m2opt2-2026-11-27)

## Fix-image gate

Path: WindowServer/SkyLight -> QuartzCore GPU composition or software destination
-> IOSurface allocation/cache mode, CPU publication and GPU completion ->
IOMobileFramebuffer swap -> NativeGopDisplay job/fence, linear plane-zero
request -> PianoDpu retained/wired backing and DPU SMMU slot -> first-map or
fallback pixel clean / verified-WC skip + DSB -> VIG0/1, LM0/1, DSPP0/1, DSC0,
INTF1/2, bonded DSI and panel. Linux WC page acquisition/mapping compared above;
the inspected sheng observation code supplies no equivalent coherency policy.

Checks: ours-OK on host: geometry, retention, SMMU/TLB maintenance, the
WC-preserving aperture view, the effective-page predicate and epoch, strict
failure handling, counters and tests, and the pinned kernel import check.
The existing NativeGopDisplay fence path reports confirmed completion to
PianoDpu. The RAM A/B measured effective WC coverage, clean time and attribute
check cost for the cadence workload and passed automated optical grading. The
broader mixed-use RAM coherency result remains open.

Root cause: the legacy `Flipper::flipSurface` cleans each accepted frame although QuartzCore's
display allocator explicitly requests WC on its normal branch. The current
copyback alias can defeat that premise. CPU software writes exist; seeds also
change for device writes, and pending dirty state can be consumed or cleared
under Apple coherency assumptions. Consequently a global clean-off switch or
seed/dirty-only condition does not establish correctness on this DPU.

ONE change: implement only the WC-qualified pixel-clean policy specified above:
preserve and verify WC backing, initialize by a successful acquisition clean,
skip subsequent pixel cleans within that valid epoch with publication/DSB
preserved, and retain the full clean or refuse/copy on any failed predicate or
maintenance failure. Default/cached/real-time/unknown buffers remain conservative.
