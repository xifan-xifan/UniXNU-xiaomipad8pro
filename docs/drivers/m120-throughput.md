# 120 Hz composition throughput on piano

Analysis: **2026-11-27, `v131y12t-m120d`; ONE change: HOLD.** The owner's two-image analysis gate stops further imaging in the 120 Hz cadence/presentation lane. This review changes documentation only: no production source, kext, image, tablet access or push. The older experiment proposal below is superseded. The immediate failure mechanisms are traced, but **neither root cause is closed end-to-end**: the capture lacks the admission-field history and physical-IRQ counter pairs needed to select a causal correction. Positive frame-info transport also has not been followed through the matching CA/SkyLight transaction to an app drawable.

## Evidence boundary

The owner-supplied 350 s capture completed with `rc=0`, RAM identifier `7cba9257`, source **`3fcf74f45afc97876d7ea7ed80e85ab5cc345918`** (`lead/v131y12t`). That revision was fetched read-only and inspected with `git show`, including `GopSwapPipe.h`, `GopFrameInfo.h`, `GopFence.h`, `GopPacing.h`, `NativeGopDisplay.cpp`, PianoDpu's `DpuFlip.h/.cpp`, `DpuFrameInfo.h`, `PianoDpuFrameInfoAudit.h`, and A830's fence provider/accountant. Source anchors in the current analysis refer to **that revision**, not this branch's older production files. The prior prediction is recorded in `docs/drivers/m120-pipe.md` at that revision; it is not imported into this branch.

The local capture's filtered display/GPU lines were also read. Additional observations below are identified as local-console evidence. Decoded **27.3 / 26C5035r** QuartzCore, IOMobileFramebuffer and SkyLight were reread in place with `ipsw`, along with the matching `kc273-mac17j.bin` IOMobileGraphicsFamily. Versions: QuartzCore 1223.20.4, IOMobileFramebuffer 700.51.10, SkyLight 959.4. Addresses are unslid static anchors. No Apple payload, instruction listing, private path, kernel pointer or personal identifier is added here.

| Supplied check | Result / scope |
|---|---|
| Display contract | `piano-dpu-modes=120`; both INTFs 119.994 Hz; `prog-fetch 0013b6c1`; `ACTIVE 120.000`; 0 underruns. Physical timing passes independently of new-frame throughput. |
| Published pacing | `IOMFBDisplayRefresh min=max=0x2222222` (199999 Mach ticks); frame-info, audit and swap-pipe enabled; 0 WindowServer crashes. Enablement does not establish eligibility or feedback consumption. |
| Final complete load window | At 252133064 µs, duration 10008020 µs: 511 PROGRAM and 511 complete-hook events, about 51.1/s; 739 software ticks. |
| Pipe delta from preceding report | `captured 1716→1716`, `used 1715→1715`, `fallback 2403→2914`: **511/511 fallback**. `polls=3100`, `ready=258`, `pending_used=1458`, `cancelled=1`, `failed=0`, `lead_us=1397835`, `overlapped=235`, `overlap_us=406721` all remain unchanged. There is zero added demonstrated overlap in this window. |
| App, separate 60 s population | `piano-frametime --fps 120`: `cadence_hz=50.71`, p50/p95/p99 20.81/27.85/28.20 ms, `missed=4502`, `gpu_errors=0`; submitted 3042, zero presented 3042, unique 0. Callback cadence is not a positive presentation series. |
| FI, separate 92.87 s population | Programs/hooks 3286/3286; valid/packets 99/99; missing 3187. Overlapping audit reasons: `same_frame=3186`, `before_flush=1`, `other_frame=1`; dropped hooks, cleanup calls and type-3 errors all 0. See [the physical-counter trace](presented-time.md#physical-irq-qualification-in-the-captured-revision). |

The FI population and final 511-swap window must not be equated. Nor do cumulative startup overlap totals demonstrate overlap under the app load.

## What m120-pipe predicted and why it missed this result

The host fixture admitted three rotating buffers every 8333 µs, with at most two submitted-but-uncompleted swaps and a 9625 µs producer-fence latency. Both event-driven retirement and production 1 ms polls delivered 7200 modeled latches at 120.0048/s. Mean residual fence waits were 1623/2001 µs; demonstrated overlap was at least 5362/4984 µs per paired latch. The default-off model remained near 60/s. Those were **conditional scheduling results**, not measured tablet producer throughput.

The analysis did not predict the load result because it supplied an eligible, ready, main-only queued head and a 120/s producer schedule instead of proving either on the tablet. It proved that an admitted ticket could overlap a previous latch; it did not prove that the family head's ready bit would satisfy the adapter throughout WindowServer's real fence lifecycle. Under load the adapter admits no new ticket, so the modeled residual-wait advantage is absent. Startup's 1716 captures and 235 overlap observations show that the feature worked in some earlier states; they do not validate its load assumptions. The existing FI exact-`+1` counter assumption was retained independently and also lacked hardware phase validation. A host test that rejects a synthetic equal counter verifies policy, not the physical counter's phase at VSYNC.

Now unknown: which admission predicate fails (or whether filtering changes the head), the actual CA page-pool/request occupancy over time, which surface's producer sequence the global watermark covers, GPU retirement time versus CPU observation, the physical counter's update relative to VSYNC/flush acceptance, and same-transaction feedback delivery to the drawable. The prediction did not close these links. Another image is not justified by the model.

## Why 511 load swaps used the fallback

At `3fcf74f4`, `NativeGopDisplay.cpp:1447` calls `prepareQueuedFence()` **before** `clean_queue_gated()` checks the current swap; `:1466` calls it after programming. The independent timer also calls it at `:2475`. `prepareQueuedFence` (`:2445`) examines the **finalized pending head at display +0x2008**, not the current request at +0x2000 or the created list at +0x2018. The exact predicate in `GopSwapPipe.h::eligible` is:

```text
head exists AND request.layers == 1 AND (request.readyLayers & 1) != 0
            AND (request.dropReasons & ~8u) == 0 AND mainSurface != null
```

The inputs are 32-bit request +0x478 (layers), +0x47c (family fence-ready mask), +0x48c (drops), and surface +0xd08. This is an exact layer mask test, not a layer count. It applies before cursor masking in PROGRAM. Power-off clears a ticket. An ineligible/empty head clears the slot and cancels its timer; it does not increment `fallback`.

A new eligible identity (request, surface, swap ID) freezes the current **global** A830 submitted sequence. Successful snapshot increments `captured`; provider failure increments `failed`. Repeated preparation of the same key does not chase a later tail. At PROGRAM (`:2029`), `Slot::take` accepts only a matching Pending/Ready ticket. Otherwise it increments `fallback` and `selectFence` takes the original program-time watermark. `pending_used` is a successful take that still requires the worker's residual wait, not a failure. `used` is not proof of actual overlap; a ticket captured immediately before PROGRAM also counts.

Consequences of the final delta:

- There was **no successful new admission**, no provider-failure increment, no further poll, and no additional cancellation. Failed or expiring captured fences do not explain this plateau.
- A universal one-at-a-time submitter alone would remove early lead but would not force fallback: an eligible first head can still be captured at entry to `swap_begin_gated` immediately before becoming current. The host/runtime mismatch must include what the adapter actually sees.
- The local console reports **198 queue-behind observations** during the final window. `fBehind` increments when pending head and current are both nonnull at the end of `swap_begin_gated` (`:1483`); it resets per report. This proves occupancy of at least current + one pending on those observations. It is not 198 distinct submissions or a measurement of the maximum queue depth.
- The local layer log records masks 0x7 at startup and 0x1 thereafter, with no reported new mask when load begins; all final flips are direct and have zero cursor-forced copies. This weakens a cursor/multilayer explanation but does not supply every raw 32-bit predicate input.
- The strongest remaining source-level candidates are the ready-bit/eligibility timing and the ordering of preparation versus queue filtering. `clean_queue_gated` filters obsolete/no-op/aborted/dummy requests after preparation; a changed head can reach PROGRAM without another preparation of that head. **The capture has no rejection counters or per-head field samples to distinguish these branches.** Calling this proven WindowServer depth-one throttling, or proven `readyLayers=0` on all 511 swaps, would overstate the evidence.

## Decoded 27.3 queue and backpressure contract

| Binary anchor | Proven control flow | Remaining runtime question |
|---|---|---|
| IOMFB `_kern_SwapBegin` `0x1b5ed9974`; `_kern_SwapEnd` `0x1b5ed9a40` | Selectors 4/5 allocate an ID and submit a 0x830-byte record. One connection construction record does not limit already ended requests to one. | IDs and submission/return times during the load. |
| Family `swap_start_gated` `0xfffffe000ac29660`; `swap_submit` `0xfffffe000ac29dc4` | Separate requests enter created list +0x2018. Acceptance is distinct from making one request current. | Created/pending occupancy and which accepted requests have ready producer fences. |
| Family `swap_queue_finalize_gated` `0xfffffe000ac2b208` | Applies IOSurface fences at `...ac2b318`, moves created → pending +0x2008 at `...ac2b330`, then calls queued/begin hooks. Queue membership alone does not establish ready bits. | Which head/ready mask exists when Native's preparation runs. |
| Family `swap_apply_fences_gated` `0xfffffe000ac2b410`; `io_fence_notify_gated` `0xfffffe000ac245e8`; `swap_signal_gated` `0xfffffe000ac2473c` | Absent or synchronously satisfied IOSurface fences set +0x47c; pending fences retain requests and signal through a callback. Signal path ORs the layer bit at `...ac248b8–...ac248c0`. This is separate from Native's A830 watermark wait. | Callback ordering versus Native's immediate PROGRAM, particularly under load. No raw masks/callback timeline in the rollups. |
| QuartzCore `IOMFBDisplay::finish_update` `0x18e1eeb28` | Calls `swap_end` at `0x18e1f0834`. Later waits at `0x18e1f0858` / `0x18e1f0870` are conditional; there is no unconditional wait after every end on this path. | Installed policy flags and whether either conditional wait executes. |
| QuartzCore `IOMFBDisplay::is_ready` `0x18e1e12cc`; `swap_wait` `0x18e1e2654` | Readiness uses shared-state bytes +4/+5 under object +0x3a0 and may probe `CA::IOMobileFramebuffer::swap_wait(0, 0x80000003)` at `0x18e1e1318`. The display wait can consult pending FrameInfo records (+0x67d8/+0x67e0, record +0xc8) or delegate to IOMFB. | This does not imply all readiness is tied to type-3 receipt, or a fixed number of buffers. Need selected branch and outstanding IDs. |
| QuartzCore `begin_update` `0x18e1e4624`, `allocate_buffer` `0x18e1e415c`, `collect_buffers` `0x18e1e9a78` | Page/buffer lists and reuse/refcount checks coexist with submission; buffer construction/reuse and family current-swap serialization are different constraints. | Actual page availability, producer scheduling and maximum outstanding requests remain unmeasured. |
| SkyLight `SLCADisplay::last_submitted_swap_id` `0x182ad3dec`; transaction callback `0x182acad74`; present processing `0x182ab5d08` | SkyLight queries CA's last-submitted ID conditionally through Objective-C and receives versioned CA frame info; it separately retains/releases app render presents. | The getter/callback is not a depth counter. Pending app presents, CA pages, GPU ring depth and IOMFB pending swaps must be measured separately. |
| Family `swap_complete_gated(bool)` `0xfffffe000ac26f9c` | Notification precedes final current release/clear at `...ac27368` / `...ac2736c`; Native's worker completion then drains the next request. | A safe completion boundary is established; it does not guarantee an early eligible next ticket. |

Thus the binary allows more than one ended request outstanding; the local queue-behind evidence demonstrates that occurring during this load. **No fixed maximum of two/three, nor a complete WindowServer/CA occupancy timeline, is proven.** Ring `depth=3` in A830 telemetry is GPU job depth, not framebuffer-swap depth. The admission rejection and queue-depth root cause remains open at precisely these fields/branches.

## What stage-fence actually measures

`NativeGopDisplay::copyLoop` (`:2627–:2653`) samples CPU monotonic µs immediately before/after **one** `waitGpuFence(job.fence, &answered)` call, before `runJob`/DPU entry. `gpuFence` reads the ring watermark at PROGRAM on fallback. `native_adreno_fences` (`NativeAdrenoPublish.cpp:2174`) is lock-free: last submitted, completed record and fresh CP memptrs[1], with wrap-safe comparison. It neither submits GPU work nor waits on a GPU mutex. `gopfence::wait` polls one immutable target; its timeout is 1000 ms. The post-query CPU wake time is not the GPU's start/end timestamp.

| Final window, 511 paired swaps | Mean (range), µs | Interpretation |
|---|---|---|
| PROGRAM → fence wait start | 29 (8–63) | Worker dispatch overhead. |
| **Fence wait** | **10738 (6894–12219)** | Queue/execution before the chosen global target retires, plus polling/query/scheduling observation delay. |
| Fence observed → flush observation start | 331 (104–393) | Includes DPU admission/map/cache-eligibility work; separate `clean` mean 318 (94–378) is a subset. |
| **Flush observation start → CTL-clear readback** | **6095 (32–8289)** | Remaining scanout phase and IRQ/poll wake/readback; not an IRQ timestamp or app scanout proof. |
| CTL-clear readback → complete hook | 43 (12–2208) | Includes worker-done → hook mean 32 (8–2202); do not add that subset again. |
| PROGRAM → complete hook | 17237 (8299–21092) | Exact stage sums total 8808549 µs; 1/511 below one 8333 µs period. Rounded stage means differ by 1 µs. |
| PROGRAM / complete-hook spacing | 19569 / 19569 | Roughly 51/s. Service and spacing have different endpoint populations; their mean difference alone does not identify a CA stall. |
| Software timer spacing, 739 samples | 13542 (7319–22465) | Stoppable queue-service callbacks, not physical scanout. |

Local-console fence diagnostics report **511 legacy waits, 4769 sleeps, 5477771 µs sleeping, zero sleeps over 2 ms**. Against stage-fence's sum 5487141 µs this is 99.83%, about 9.33 sleeps per wait and 1.149 ms per sleep. This establishes a CPU sleep/poll interval, not 10.74 ms of exclusive GPU execution. It also does not prove the sleep caused the entire latency: the GPU runs while this worker sleeps, and the exact retirement instant between polls is absent. The previous y12q poll-leeway A/B/A did not materially improve cadence (recorded in the captured revision's throughput history); rerunning that image experiment is not justified here.

Local-console rows ending 243.007–252.012 s report 100–105 render-server jobs/s and 50–53 external jobs/s, ring depth 3, `polled=1`, `q-us` 9997–10161 and `rs-us` 6153–6309. This is roughly two render jobs per app/external submission. The GPU is level 12, 967 MHz, capped by the skin band in this interval. `A830Fence::queueCompleted` estimates service between software-observed completions; batched polled retirements are not per-job hardware execution timestamps. These observations are consistent with composition/queue work ahead of the global target, but cannot identify which job belongs to which desktop swap. The installed y12q 256-draw scene's 5.9 ms does not bound this differently sized, multipass, separately queued WindowServer workload or its observation latency.

The family IOSurface-fence phase precedes PROGRAM, so it is not included in `stage-fence`. The worker waits once on the global sequence; DPU receives `producerComplete` and uses it for clean eligibility, with no second A830 fence wait in this stage. The app's `nextDrawable`/drawable semaphore can block the producer or change when work is queued, but it is not called inside this measured kernel interval. Frequency, exclusive execution, unrelated queue work, and last-poll observation delay still require a same-sequence timeline; none is selected as the sole cause.

## End-to-end path

| Hop | Status | Current evidence / missing link |
|---|---|---|
| App display link → drawable/Metal submit | ours-OK submission; ours-missing positive presentation; unknown blocking branch | 3042 submissions, zero GPU errors and 3042 zero times. Need acquire/encode/commit split, queue/update seed and matching completion. |
| CA image queue → SkyLight composition / CA pages | unknown | Decoded page reuse and present-retention paths exist. Actual page count/occupancy, app backpressure and per-pass GPU timestamps are absent. |
| WindowServer/CA → IOMFB created/pending swaps | ours-OK acceptance; unknown full depth | Decoded selector 4/5 and separate queues; 198 current+pending observations rule out universal depth one. No per-ID submission/ready/complete timeline. |
| Finalized head → pipe ticket | ours-OK conditional implementation; ours-missing load admission | Exact eligible/ready/main-surface/drop predicate and fallback traced. No captures on 511 swaps, no failed/cancelled growth. Need head predicate/replacement history. |
| PROGRAM → A830 retirement | ours-OK conservative wait; ours-missing surface-specific attribution | 10.738 ms CPU-observed wait on program-time global target. Need target/producer relation and actual retirement time/clock. |
| DPU clean/CTL → accepted new surface | ours-OK | 511 paired direct flips, zero copies, 0 missing/unordered stage records, healthy hardware contract and underruns. Clean/worker/hook costs do not dominate. |
| Physical INTF1 IRQ counter → FI qualifier | ours-missing reliable qualification; unknown counter phase | 3186 equal pre-flush/IRQ counters reject feedback. Register selection and serial handoff traced in [presented-time](presented-time.md); raw pairs/line/IRQ generations are absent. |
| Worker serial → current swap/type 3 | ours-OK sparse transport; ours-missing coverage | 99 valid/packets, zero transport errors; 3187 suppressed. No audit evidence of serial/swap mismatch. |
| IOMFB receive → CA pending swap → SkyLight present release | unknown | Accepted sends are not receipt or matching. Need dictionary/time/ID at each hop and selected policy. |
| CA image queue seed → positive app presentedTime | ours-missing; unknown exact failing downstream branch | 0/3042 positive times; decoded setter requires positive matching feedback. No fabricated timestamp is admissible. |

## Exact next diagnostic questions

These are evidence requests for offline analysis/main review, **not instructions to access the tablet, enable tracing, change args or build another image**. First inspect already retained logs/state. If a field was not recorded, mark it unavailable; aggregate counters cannot reconstruct it. The host-only record design and offline grader preserve that distinction. Any driver wiring requires separate owner review under HOLD. [Offline m120 diagnostics](../../tooling/native/m120-diag/format.md)

1. **Admission versus depth:** for a fallback swap ID, what were current ID, created/pending counts, pending head ID, raw +0x478/+0x47c/+0x48c and presence of +0xd08 at preparation and immediately before PROGRAM? Did queue filtering replace that head? When did its family IOSurface-fence callback signal? Retain relative identities/counts only, never pointers or surface contents. This distinguishes no early submission, rejected ready/layer predicate and a changed head.
2. **CA/WindowServer depth:** align selector-4/5 entry/return, CA `is_ready`/conditional `swap_wait` branch and page acquisition/release against current complete-hook. Count accepted-but-uncompleted IDs and occupied/reusable CA pages separately from app drawables and GPU jobs. Is the next eligible head present while the previous worker is in latch? The 198 queue samples alone cannot answer its readiness or maximum depth.
3. **Global-fence delay:** for that same swap, identify capture/program target sequence and submitted/retired sequences, ring source/submit time, true GPU start/end or retirement observation, CPU query/sleep/wake times, and contemporaneous level/cap. Does the target cover the displayed surface, later composition work or unrelated jobs? Account for the two render jobs per external job without substituting polled service estimates for GPU timestamps. Split `nextDrawable` wait separately on the app thread.
4. **Physical qualification:** recover the pre-flush INTF1 frame/line, post-barrier time, IRQ status/frame-before/frame-after/line/CTL readback/time, IRQ generation and worker serial for the same latch; compare INTF2 as well. Is the counter increment before/after the routed VSYNC and fetch/CTL acceptance? Is the IRQ capture fresh for this flush? See [the exact comparison and evidence limit](presented-time.md#physical-irq-qualification-in-the-captured-revision).
5. **Feedback:** follow one accepted packet's swap ID/time through the IOMFB decoder, CA pending FrameInfo and SkyLight transaction to app queue/update seed; also follow a suppressed swap to its eventual zero callback/cleanup. Kernel send acceptance does not close either path.

Read-only host reproduction uses caller-supplied artifact paths, never literal machine paths:

```sh
git show 3fcf74f4:package/NativeGopDisplay/NativeGopDisplay.cpp
git show 3fcf74f4:package/NativeGopDisplay/GopSwapPipe.h
git show 3fcf74f4:package/PianoDpu/DpuFrameInfo.h
"$IPSW" dyld disass "$DSC" --vaddr 0x18e1e12cc --count 73 --quiet --no-color
"$IPSW" dyld disass "$DSC" --vaddr 0x18e1f0820 --count 38 --quiet --no-color
"$IPSW" macho disass "$KC" -t com.apple.iokit.IOMobileGraphicsFamily --vaddr 0xfffffe000ac2b208 --count 110 --quiet --no-color
```

## Historical 2026-11-26 baseline

The following pre-pipe/pre-frame-info analysis is retained for comparison only. Its Unsupported selector-74 and missing timestamp ABI statements describe that older source, not `3fcf74f4`. Its stage populations and thermal readings do not describe the November 27 load. Its poll-sleep experiment no longer authorizes work.

Goal 12 requires distinct desktop frames at the panel's cadence. The 2026-11-26 12:58 RAM run with `piano-dpu-modes=120` verified 119.990 Hz on both links at mode set and exposed only 3200×2136 at 120 Hz in CoreGraphics. During the probe, the desktop flipped at about 56 Hz. The scanout still refreshed the old surface on frames without a new flip.

### Capture and time base

The `v131y12p-m3-120` result has a GUI login, a completed 60 s load-0 capture, 3,386 display-link callbacks, 3,385 Metal submissions/completions and zero GPU errors. The app drawable was **2400×1424**. WindowServer handed the display a **3200×2136 '&BGA'** desktop surface. The probe received every presented handler but all 3,385 `presentedTime` values were zero, so the per-drawable presentation grade is VOID. The app's snapshot reads were denied. The separate root snapshots both succeeded and both INTF frame counters advanced by 8,962 ticks over 74.679211 s bracketing the probe (**120.007 Hz** on each link). The `COUNT_EN` readback was zero, so the current grader rejects these otherwise moving counters. M3 independently verified 119.990 Hz from counter motion at mode set.

Evidence comes from `console.out`, `m3-ft120.json`, the before/after snapshots and `m3-ft120-kernel.*`. Correlate `R3b DISPLAY at-us` with the adjacent NativeGopDisplay report and the app's monotonic window, 163.9365–223.9365 s. The five complete GOP windows inside that interval span 168.8219–218.8500 s. The opening report at 168.8219 s also contains pre-probe time. Some kernel log lines join mid-line, so match the driver prefix anywhere and require the expected fields. Across the five complete windows the GOP count rose from 1,963 to 4,763, or **55.97 accepted desktop flips/s**. GOP includes same-surface reshows. The last DPU rollup reports one reshow since boot, separately from its 2,858 new-surface flips. The tools observed neither per-drawable swap IDs nor individual latch times in this run.

### Stage budget at 120 Hz

One hardware period is **8.33 ms**. App timestamps, kernel window means and cumulative DPU counters have different sample populations. Where spans overlap, adding them would double-count time.

| Stage | Measured time or rate | Budget interpretation |
|---|---|---|
| Display-link delivery | 56.42 Hz. Target interval p50 18.36 ms in the in-window tool grade, 18.41 ms across raw adjacent rows, p95 27.79 ms and p99 28.18 ms. Callback spacing p50 16.70 ms. | Per-callback `targetTimestamp − timestamp` is 8.33 ms. Nominal interval and callback cadence diverge. At 60 Hz the supplied comparator had target p50 16.67 ms and p99 about 17.9 ms. |
| App callback → Metal commit | p50 16.47 ms, p99 24.85 ms up to the pre-commit sample. | Includes `nextDrawable`, encoding and `presentDrawable:`. The capture cannot split them. Time on the main thread can delay the next callback. |
| App GPU | Submit → GPU start p50 7.17 ms, p99 8.51 ms. GPU start → end p50 0.661 ms, p99 0.707 ms. Adjacent GPU end stamps p50 16.72 ms. | The 16.72 ms number is completion **spacing**. The sub-millisecond measurement is for the app pass at 2400×1424. |
| WindowServer GPU composition | `R3b WORK`, 60 one-second rows ending inside the probe, reports 6,778 render-server jobs and a job-weighted 6.12 ms estimated service per job. Row means run 5.75–6.37 ms. Ring-wide row mean latency is 9.06–11.07 ms. | About two render-server jobs per app submission, with observed peak ring depth 3–5. Every row is `polled=1` and the queue accountant can assign several completions to one poll. The total includes other desktop work and has no swap-to-job association. Multiplying gives a rough 12.2 ms of **accounted** render-server service per app submission. Actual GPU execution time for each 3200×2136 composition remains unknown. |
| Swap submit → program | GOP mean 0.363–0.589 ms, max 5.25–8.87 ms in the five complete windows. | Only 31–37 sampled swaps per window versus 501–583 completions. The driver uses a 16-slot submit stamp table and consumes a stamp once. This is sparse timing of family queue/fence admission. |
| Program → GPU fence observed | Worker fence wait mean **10.93–11.35 ms**, maxima 11.41–12.57 ms, zero timeouts, 500–583 samples/window approximately. The mixed opening window has 9.55 ms mean and 31.31 ms max. | The worker waits for the global Adreno ring tail sampled at programming. This includes GPU execution/queueing ahead of that fence and the delay until a CPU poll sees retirement. The steady mean alone is 131–136% of a period. |
| Map and CPU cache clean | 3 maps, mean 1.468 ms. 3 full cleans, mean **3.902 ms**, 11.706 ms total. M2 skipped 3,676 of 3,676 shown requests between root snapshots with no increase in clean passes. Inspection cost grew 551.543 ms over that span, about **0.150 ms/request**. | `piano-dpu-clean-skip=1` appears at startup and the counters confirm it was active. The visible plane at stride 12,800 is 27.34 MB and each mapped backing is 27.98 MB. M2 cleaned each backing once using the physical aperture on the issuing CPU. `clean-threads=4` applies to the legacy alias clean. There is no four-thread per-flip timing in this capture. All five GOP windows have zero CPU-copy fallback. |
| CTL latch | 4.291 ms cumulative mean, 8.355 ms max through flip 2,858. Differencing the preceding cumulative row (1,444 at 3.681 ms) gives approximately 4.91 ms for the next 1,414 flips, including pre-probe time. | INTF1 VSYNC SPI 83 wakes the CTL_FLUSH acceptance check. The 50 ms setting is a failure ceiling. The observed max is about one period. `cadence=0` means no `PianoDpu: cadence N` windows were emitted. |
| Program → deferred swap done | Mean **15.44–16.26 ms**, maxima 19.46–20.72 ms in the five complete windows. | Includes the worker fence wait, DPU latch, other worker costs and gated completion handoff. The paired window means differ by 4.26–4.94 ms, consistent with the latch consuming most of the remainder. This aggregate cannot isolate work-loop handoff latency. |
| Software VBL | 500–836 timer callbacks per roughly 10 s window and 261–503 idle stops per complete window. | These are queue-service wakeups. They count neither panel scanout nor per-drawable presentation. The diagnostic threshold for late wakes is 20 ms, so its zero count cannot exclude missed 8.33 ms periods. |

The GPU reached the docked skin cap throughout the probe. Of 59 `R3b LV` rows inside the app window, 22 show level 13 at **1050 MHz** and 37 show level 12 at **967 MHz**, all with `bind=skinband`. At 186.73 s the skin reading reached the 45 °C boundary for level 12. Across those samples skin was 42.7–45.1 °C, XO was 50.2–54.9 °C and filtered SoC temperature was 83.8–90.6 °C. The SoC ladder was already at step 14 with `board-cap=0`. The binding cap is in `R3b LV` and `R3a RES ceiling`. The separate `R3b THERM effective` field reports the qualification guard and does not give the voted OPP on this full-OPP path. 222/342 MHz appear earlier in the boot. `R3b RESF` shows about 58.4 s added at levels 12–13 between the first and last in-window level samples. The 222 MHz floor and higher allowed caps do not justify raising the floor in this capture.

### Historical end-to-end path

| Hop | Status | Checks and evidence |
|---|---|---|
| App → CA → WindowServer | ours-OK for probe submission, unknown for per-frame timing | The app obtains the screen-bound link from the view, uses an exact preferred rate, pulls a drawable and submits a Metal command. For 27.3 build 26C5035r, `CA::Display::DisplayLink::callback` / `dispatch_items` are separate from `CA::WindowServer::IOMFBDisplay::is_ready` / `swap_wait`. `CAMetalLayer` inserts an IOSurface into a CA image queue, where WindowServer associates a render present with a desktop transaction. Exact CA dispatch mode, drawable wait and per-pass GPU start/end are unknown. See [the 27.3 framework and KC trace](presented-time.md). |
| WindowServer → IOMFB → NativeGopDisplay | ours-OK | CA's `IOMFBDisplay::finish_update` → `swap_end` → selector 5 (0x830-byte 27.3 request). ABI 2 `program_hardware_gated` consumes the queued swap ID, surface layers, dimensions/format and optional damage. `clean_queue_gated` defers while a current swap exists and filters obsolete, no-op, aborted and dummy swaps. Programming snapshots the latest **global** submitted A830 sequence. The app command, a render-server job and this swap are distinct identifiers. |
| NativeGopDisplay → GPU retire → DPU request | ours-OK for the safety fence, ours-missing for buffer-specific attribution | Worker `waitGpuFence` uses `native-adreno-fences` to read the ring's last submitted and latest completed sequence from `NativeAdrenoPublish.cpp`. The old path uses `IOSleep(1)` between reads and wrap-safe sequence comparison. IRQ 332 observed zero interrupts and Work rows were polled. The 1 s guard leaves an unfinished frame undrawn and schedules a retry. A retired fence supplies producer-complete evidence to the DPU. No scanout-surface-specific completion sequence is exposed. |
| DPU request → panel | ours-OK | The flip client offers linear BGRA or supported lossless plane 0. `flipSurface` checks size/stride/alignment, layouts, SMMU slot ownership and cache attributes, preserves the current surface on rejection, then sets the VIG addresses and flushes the CTL. WC epoch checks run before a skip. `waitLatch` verifies CTL_FLUSH cleared, with IRQ wake/poll, fault/underrun checks and rollback. Same-surface reshows have a separate counter. |
| Panel → work loop → family completion | ours-OK for resource ownership | After the job, `jobDoneAction` checks its serial and completes the same current swap before draining the next queue entry. The 27.3 family completion calls slot 462; `notify_swap_done_gated` packages the current swap ID and five blob words. `swap_wait` passes Busy for a still-current swap and normalizes Busy only for the last completed swap when called nonblocking. A late latch can keep the next composition waiting if CA polls before completion. During the probe the sampled Busy calls name the current swap, one ahead of last-completed. |
| VBL/feedback → next CA frame | ours-missing for timestamped hardware feedback, unknown for runtime pacing | NativeGopDisplay's timer calls `swap_begin_gated`. With pending work an early call keeps the absolute deadline and a late one advances past expired deadlines. The timer may stop when idle. It has no type-5 vsync sender. Selector 74 reports Unsupported and DPU flip ABI v1 returns durations, so the named frame-info path carries no swap-associated absolute latch time. The local 27.3 symbols include `AppleInternalDisplay::heartbeat_rate` and `IOMFBServer::vsync_callback`. The fixed 120 Hz minimum interval and the 8.33 ms target lead are established. A completion can miss CA's next readiness check, but this capture cannot prove that an individual late completion skips exactly one CADisplayLink tick. The target intervals are often off the 8.33 ms grid. |

### References checked before the change

The Ubuntu v1.4.0 source manifest pins the piano kernel at `83f0f89bed14`. The sheng GopFramebuffer reference was read at `d711e8c9`. The available stock Android capture exposes SurfaceFlinger/KGSL diagnostics, with GPU frequency sysfs denied. It has no time-correlated trace for this macOS workload. These comparisons use control flow, with no reference code imported.

| Reference file and line | Ours | Same / different / missing |
|---|---|---|
| Ubuntu `msm_gem_submit.c:751`, `msm_fence.c:126`, `adreno/a6xx_gpu.c:1991`, `msm_gpu.c:462` | `NativeGopDisplay.cpp` `program_hardware_gated` / `waitGpuFence`, `NativeAdrenoPublish.cpp` `native_adreno_fences`, `A830Irq.cpp` fence interrupt | Different. Linux exposes per-submit DMA fences and uses the GPU completion interrupt to retire/signal them. The macOS bridge exposes a ring watermark and can operate with GPU IRQ disabled. Keep the conservative watermark until the producer fence for the handed-off surface can be identified. |
| Ubuntu `msm_atomic.c:213–233`, `dpu_encoder_phys_vid.c:335–369`, `dpu_crtc.c:638–641` | `DpuFlip.h` `flushTo` / `waitLatch`, `DpuFlip.cpp` INTF1 waiter | Same CTL acceptance principle. Different event delivery. Linux checks pending flush at hardware vblank and signals DRM completion. Ours waits for latch on one worker and then completes the current family swap. |
| sheng `NativeLegacyDisplay.cpp:375–379, 673–683, 801–804` | `NativeGopDisplay.cpp` `waitGpuFence` / `jobDoneAction` | Same conservative GPU retire guard and surface lifetime concern. Different wait implementation and panel latch path. |
| sheng `NativeLegacyDisplay.cpp:430–450, 557–577` | `NativeGopDisplay.cpp` `vblFired` / `get_timing_attributes_gated` | Same published period concept. Missing compositor vsync notification in ours. Sheng's software notification mechanism does not establish a hardware event or a CA subscription on piano. |
| XNU `IOLib.cpp` `IOSleep`, `clock.c` `_clock_delay_until_deadline_with_leeway` | `waitGpuFence`, existing critical timeout use in `NativeUfs.cpp:1205` | Different test arm requests an explicit system-critical wait with zero requested leeway. The CPU still has to be scheduled after the deadline. [XNU IOLib](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOLib.cpp) · [XNU clock waits](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/clock.c) |


## Fix-image gate

```text
Path: app drawable/CA queue -> SkyLight composition/CA pages -> IOMFB
      created/pending request -> Native eligible ticket or fallback -> global
      A830 target/worker wait -> DPU clean/flush/latch -> physical IRQ counter
      qualification -> worker serial/current swap -> type 3 -> CA/SkyLight
      matching transaction -> same drawable/update seed positive time.
Checks: ours-OK mode/refresh/ownership/CTL acceptance, queue-behind lower bound,
        exact predicates, same-swap sparse send; ours-missing load admission,
        producer-specific GPU timing, per-IRQ counter pairs and positive app
        feedback; unknown full queue depth/ready lifecycle, actual GPU delay,
        counter/IRQ phase and downstream same-transaction consumption.
Root cause: immediate fallback and SameFrame suppression mechanisms traced.
            Neither causal explanation is proven end-to-end. No single change
            closes both; no admission relaxation, earlier completion, clock
            vote, fabricated timestamp, driver/kext edit or fix image selected.
Owner gate: two images in this lane without root-cause closure; STOP imaging.
Next evidence: the five questions above, using retained read-only artifacts;
               unavailable fields stay unknown. Main review is required even
               if subsequent analysis closes both mechanisms.
ONE change: HOLD
```
