# DPU M2: conditional scanout cache clean

Analysis and host implementation, 2026-11-26. RAM qualification is pending.

## Decision

Use a **write-combined-only** conditional clean, with a full scanout clean
as the fallback.
The kernel can read `IOSurface::getCacheMode()`, but must also verify the
backing pages' effective cache attributes. A retained, initialized backing
whose CPU writers remain write-combined can skip repeated pixel cleans.
CPU-write absence is neither required for that case nor established by M1.

The policy is implemented behind `piano-dpu-clean-skip=1`, off by default.
Live WC coverage, performance and coherency remain RAM-boot checks.

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

Host verification used `tooling/k27/build-native-piano-dpu.sh`. All eight
PianoDpu suites passed, including the five baseline suites, the mode planner,
the snapshot model and the new conditional-clean suite. The arm64e object
build and import check passed against the pinned 27.3 kernel with zero
unresolved imports. The IOSurface accessor was also confirmed in the pinned
27.3 collection. The object build checks for that definition in `KC_BASE`;
if the accessor cannot be bound, M2 treats the cache mode as unknown and
cleans every scanout. The NativeGopDisplay host suite and its 27.3 object
build also passed. No boot image was built.

## RAM-boot read list

Use one frozen image with `piano-dpu=2 piano-dpu-flip=4`,
`piano-dpu-clean-skip=1 piano-dpu-snapshot=1`, and the known-good
NativeGopDisplay flip and fence settings. Keep refresh, display mode and
workload fixed through A/B. The root sysctl selects the control on the same
boot. Both arms use the same physical-aperture clean implementation.

```sh
sudo sysctl machdep.piano_dpu_clean_skip=0   # full-clean control
sudo sysctl -n machdep.piano_dpu_snapshot  # before and after each workload
sudo sysctl machdep.piano_dpu_clean_skip=1   # WC conditional
sudo sysctl -n machdep.piano_dpu_snapshot  # before and after each workload
```

1. Verify image provenance, a working console/capture window and a responsive
   desktop. Save a known-positive flip, fence and snapshot line. On early
   acquisitions, capture `PianoDpu: clean map` with its acquisition number,
   backing span, offset, stride, requested IOSurface mode, WC/other/unknown
   page counts on both sides of SMMU mapping, initialization result and
   reason. Check the existing flip/layout line for format and planes.
2. Take counter deltas from the `clean:` lines in snapshots or periodic
   flip summaries. Record `shown` (including re-shows), `maps`, `evictions`,
   `passes`, `init`, `skip`, `failed`, `alias-failed`, `without`, the
   fallback reasons, `pixel-us` and `check-us`. The fallback reasons cover
   forced control, missing producer completion, unknown or non-WC mode,
   an unverified page attribute, an invalid backing span and failed
   initialization. The `backing` reason counts refused geometry. A failed
   initialization can add both an attempted-clean time and a fallback clean.
   Require `without=0` in both M2 arms. A failed required fallback clean
   must produce a flip refusal and the normal copy path.
3. Run desktop idle, scrolling, window movement, resize, UI animation,
   video and Metal. In the control, verify zero skips and a clean for each
   successful flip. In a stable eligible WC pool, verify that clean count
   follows first-map or reinitialization epochs and that later flips and
   re-shows accumulate skips. Compare **`Δpixel-us / Δshown`** between arms.
   Starting cold, the conditional total should approximate the sum of the
   first-map cleans plus any counted fallback passes. In steady eligible
   use, the clean mean per flip should approach zero. Also capture
   `Δcheck-us / Δshown`, frame-time percentiles, latch waits and
   missed/repeated frames so the scan cost is visible.
4. Record the C920 pointed at the panel through mixed use in both arms. Use
   owned test surfaces with visible generation markers in sparse tiles and
   whole frames. Mix published CPU writes, GPU updates, read-only locks,
   cached imports, repeated presentation, eviction and reacquisition.
   Include scrolling and text entry, drag/resize/occlusion, translucency,
   cursor motion, video/Metal transitions, display sleep/wake and lock/unlock.
   Compare changed and untouched tiles with the expected generation on each
   handoff. Require zero stale tiles, flicker or partial-frame residue.
5. Record GPU fence completions and timeouts, copy fallbacks, SMMU faults,
   underruns and latch timeouts. Capture both display links in the snapshots
   and preserve the expected refresh, Night Shift and panel behavior in both
   arms. Confirm the recorded samples come from that boot and workload.

If no live pool qualifies, report its mode/page-attribute or handoff counters
and retain the full-clean fallback. Performance and optical results are open.

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
PianoDpu. Unknown on hardware: live WC coverage and mapper preservation,
frequency of missing handoffs, cost including attribute checks, and the
mixed-use RAM coherency result.

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
The RAM read list above is the remaining qualification gate.
