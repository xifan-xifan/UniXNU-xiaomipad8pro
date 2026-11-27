# Drawable presentation feedback on macOS 27.3

Analysis updated **2026-11-27 for `v131y12t-m120d`, source `3fcf74f4`, RAM `7cba9257`**. Goal 12 requires distinct, positively timestamped drawable presentations at 120/s. The 350 s capture (`rc=0`) passes the display contract with both INTFs at 119.994 Hz, fixed 120 Hz publication, healthy fetch, no underruns and no WindowServer crashes. It still reports **3042 submitted / 3042 zero presented / 0 unique** in the separate 60 s app probe. **ONE change: HOLD.** The owner's two-image analysis gate stops further imaging. No tablet access, production code edit, kext change, image or push is part of this documentation review.

The exact captured source was fetched read-only and inspected through `git show`. The decoded 27.3 / 26C5035r shared-cache and matching IOMobileGraphicsFamily anchors below were checked locally; binaries and listings stay outside Git. Current source references explicitly name `3fcf74f4`; this branch's production source and the retained November 26 analysis are older. See [throughput analysis](m120-throughput.md) for the complete queue contract, 511/511 fallback, stage-fence attribution and read-only reproduction commands. Status terms remain ours-OK / ours-missing / unknown.

## What the November 27 evidence closes

| Evidence population | Observation | Conclusion and limit |
|---|---|---|
| Schema/registration | `queries=6`, `subscribers3=1`, `type3_masks=[3]`, mask changes 3, bad 0; enabled frame-info/audit properties | **ours-OK** for named capability and a type-3 subscriber. The historical Unsupported/no-consumer diagnosis no longer explains this captured source. Subscriber registration is not same-swap consumption. |
| 92.87 s notification window | Programs 3286, complete hooks 3286, valid 99, missing 3187, packets 99 | **ours-missing** coverage: 96.987% of completions lack qualified FI. These counts are not the app's 60 s or the final 511-swap window. |
| Overlapping audit reasons | `same_frame=3186`, `before_flush=1`, `other_frame=1`; all other reasons zero | **ours-OK** localization: almost all missing FI fails the physical frame-counter comparison. **unknown** why the counter is equal at those IRQs. 3186/3187 = 99.969%; reason counts are not a partition. |
| Notification transport delta | Mask `0x80000003`, 6 slots, result 0; attempts +99, accepted +99, errors +0; dropped hooks/cleanup/type3 errors 0 | **ours-OK** for the sends attempted. The missing 3187 were suppressed before transport, not 3187 failed sends. Successful async send is not userland receipt, decode, matching or drawable presentation. |
| Accepted times | First 706773528; last 4050817575→6233046138; last delta 67000775 ticks = 335.006 nominal periods = 2791.702 ms, PASS | **ours-OK** sparse monotonic host-tick series. The last-time difference is 2182228563 ticks, not the last packet delta: endpoint snapshots can include intervening valid packets. A 335-period delta is time between qualified records, not the panel period or proof of per-frame coverage. |
| App probe | Cadence 50.71/s, p50/p95/p99 20.81/27.85/28.20 ms, missed 4502, GPU errors 0, all 3042 presented times zero | **ours-missing** positive app series; **unknown** exact zero-producing CA/SkyLight branch. Even the 99 accepted sends are not proven to correspond to these app drawables. |

## Physical IRQ qualification in the captured revision

At `3fcf74f4`, PianoDpu's `flipFunction` validates the optional `PianoDpuFrameInfo` fourth parameter's size/version and carries its audit marker into the flip transaction. `DpuFlip.h::flipSurface` passes the physical result through only after successful new-surface latch and SMMU/underrun checks. A same-surface reshow does not obtain a new qualified timestamp.

| Order / source anchor at `3fcf74f4` | Exact operation | Meaning |
|---|---|---|
| `DpuFlip.h:321`, `flushTo :371–:374` | Read `startFrame = frames()` from `kMdp + kIntfBase + kIntfStride + kIntfFrameCount`; then arm `frameInfoBegin(path.flushPa())` if IRQ live. | This is **INTF1 hardware FRAME_COUNT +0xac**, not a software sequence or worker serial. The same register is used in the ISR. |
| `DpuFlip.cpp:353–:357` | Store the CTL-flush register address in atomic `frameInfoPa`, returning continuous time. | Arms sampling. It does not clear the last Capture stamp, create an IRQ generation, or increment the physical frame counter. |
| `DpuFlip.h:375–:382` | `vsyncOn` clears/enables INTF1 VSYNC; `program` writes plane addresses and CTL flush with barriers; sample `flushed` in continuous ticks after the barrier, then `waitLatch`. | Qualification uses the post-flush clock as the lower bound. A pre-write IRQ is conservatively rejected. |
| `DpuFlip.cpp:309–:328`, `KernelHw::handler` | Service/ack physical MDSS status; if armed and INTF1 VSYNC, read hardware frame `before`, CTL flush, `mach_continuous_time()`, frame `after`. Record only when `before == after`; publish sticky status and kick waiter. | Stable double-read excludes an intra-sample counter transition. **It does not establish that the counter incremented between pre-flush and IRQ.** The handler does not increment that register in software. |
| `DpuFrameInfo.h::Capture` | Odd/even atomic `sequence` surrounds storing time/frame/flush; bounded read checks the same sequence before/after. | This sequence is a **snapshot-consistency guard**, not an exported physical IRQ serial or a per-flush identity. The IRQ line's own count/waiter generation is separate and not compared by `accepted`. |
| `DpuFlip.h:341–:387` | `waitLatch` accepts CTL bits clear; disable VSYNC; `frameInfoEnd(flushed, startFrame, mask, audit)` disarms and reads Capture plus an end time. | CTL-clear success and qualified IRQ-time success are separate. A successful latch can return FI time zero. Poll/worker return time is never substituted. |
| `DpuFrameInfo.h::rejections/accepted` | Require armed/nonzero mask, `end >= flushed`, `flushed < irq.time <= end`, `(irq.flush & mask) == 0`, and **`uint32_t(irq.frame - startFrame) == 1`**. Zero advance sets `SameFrame`; nonzero advance other than 1 sets `OtherFrame`. | Exact reason for the 3186 audit events: the two sampled frame values subtract to zero modulo 2^32. This is not a duplicate app time, repeated swap ID, or worker-serial check. |
| `NativeGopDisplay.cpp:2647–:2650`, `GopFrameInfo.h::Completion` | Publish `{workerDoneSerial, qualifiedContinuousTime or 0, rejectionBits}` under the copy lock. `jobDoneAction :2176` consumes only the exact expected nonzero worker serial. | SerialMismatch/NoHandoff would diagnose this stage. No such audit counts grew in the supplied notification window. |
| `NativeGopDisplay.cpp:2198–:2200`, `Pending::take`, notify `:3801–:3817` | Bind time/reasons to the same current swap ID; consume at completion; audit; if time is zero, increment missing and return NotReady before packet dispatch. | Physical SameFrame suppression propagates end-to-end through **our kernel** without being repaired by family completion or transport. |

Both worker `frames()` and ISR compute the same INTF1 address from the same constants (`kIntfBase=0x34000`, stride 0x1000, frame offset 0xac). The observer/snapshot path also reads +0xac on each INTF; the available Linux `dpu_hw_intf.c` uses FRAME_COUNT 0x0ac and LINE_COUNT 0x0b0. There is no demonstrated wrong-register or INTF1/INTF2 mix-up in this code. Neither side's frame value is software-updated; physical scanout advances it. Worker job serial, Capture's consistency sequence, IRQ-line/waiter generation and hardware frame count are four distinct identities.

**What is proven:** with `before_flush=1` and all other reason counts zero except `other_frame`, at least **3185 of the 3186 equal-counter samples** pass the time, flush, arming and sample-presence predicates. Thus almost all loss cannot be explained solely by an old retained pre-flush IRQ, absent IRQ samples, serial mismatch, transport failure or a failed latch. Overall 120 Hz counter motion does not contradict equal readings within one hardware-count interval.

**What remains unknown:** whether VSYNC/CTL acceptance normally precedes this register's increment, a flush/latch occurs after an increment but before the routed IRQ in the same count interval, status/arming/IRQ service ordering produces a fresh-time observation of an earlier event, or the hardware counter reads have other visibility/phase behavior. Capture has no per-flush IRQ-generation binding and can retain an old stamp, but the timestamp checks reject an ordinary old stamp; naming that as the dominant cause would conflict with the audit. The rollups retain neither `startFrame` nor `irq.frame`, line position, IRQ generation or per-event start/end/flush tuple. A reason bit proves equality under the code's comparison, not the hardware cause of equality. This is the precise missing physical-IRQ comparison required before closing AU-2.

The previous qualification analysis failed to predict the scale of rejection because host fixtures asserted a one-step counter advance at the modeled acceptance edge. `m120-fi2` added reason visibility while deliberately preserving that predicate; `m120-pipe` changed scheduling while preserving it too. Neither validated the `+1` phase relationship on this hardware. The 99 accepted cases show it sometimes holds, not that relaxing it to accept zero would be correct or uniquely associate an IRQ with a new surface.

## From qualified IRQ time to a drawable

`GopFrameInfo.h` publishes schema order `Presentation_time`, `Presentation_delta`. A type-3 request for mask 3 becomes split-width mask 0x80000003 plus swap ID and four 32-bit halves (6 slots). The delta is relative to that subscriber's preceding successfully packed time. A missing physical event produces **no packet**, not a zero-time packet. Continuous host ticks are used; DPU `latchUs`, worker µs and software VBL are different quantities.

| End-to-end hop | Status | Evidence / open dependency |
|---|---|---|
| App drawable/update seed → CA queue/render present | ours-OK submissions; unknown identity/insert | 3042 submits without GPU errors. Decoded `layer_private_present_impl` / CAImageQueue insertion exists; no matching queue/seed history. |
| SkyLight render present → CA page → IOMFB swap | unknown full depth/association | Decoded `SLCADisplay`, CA page pool and selector-4/5 submission permit concurrency; local 198 current+pending observations disprove universal one-at-a-time submission. They do not establish ready eligibility or a depth ceiling. See [queue trace](m120-throughput.md#decoded-273-queue-and-backpressure-contract). |
| Pending head → pipe ticket/global fence → DPU | ours-missing admission; ours-OK conservative wait | 511/511 fallback; CPU-observed fence mean 10.738 ms, CTL wait mean 6.095 ms. Unknown rejected head fields and target's producer identity. This is independent of FI transport correctness. |
| New surface/physical IRQ → qualified time | ours-OK predicate; ours-missing 97% coverage; unknown physical phase | SameFrame equality dominates, as traced above. |
| Worker serial/current swap → type-3 send | ours-OK for 99 events | No serial/swap-mismatch or cleanup/drop evidence; +99 accepted async sends. Matching clock/ID in our packer does not prove receipt. |
| IOMFB notification → named dictionary | unknown receipt/decode for this run | `_FrameInfoNotifyFunc` `0x1b5ee09e8` reconstructs split ticks by enumerated names and adjusts timestamp before callback. No captured consumer dictionary/receive count. |
| Dictionary → matching CA pending FrameInfo | unknown | `IOMFBDisplay::collect_frame_info` `0x18e20003c` searches ID at record +0, under vector +0x67d8/+0x67e0; `IOMFBServer::frame_info_callback` `0x18e19415c` reads Presentation_time and prefers nonzero Vbl_FrameTime when available. Need the same packet ID and chosen time. |
| CA frame callback → SkyLight DisplayPresentInfo | unknown | `forward_frame_info_callback` `0x18e194a8c`; SkyLight `register_transaction_complete_callback` `0x182acad74`, adapter `0x182acae2c`, handler `0x182ab4c8c`. Need selected callback/version/display flags and transaction identity. |
| SkyLight pending present release → app queue feedback | unknown | Processing `0x182ab5d08` converts host ticks and assigns time to matched presents before CARenderPresentRelease. Need proof of match and nonzero time, not registration alone. |
| CA matching queue seed/time → drawable setter | ours-missing positive app result; unknown branch | `layer_presented` `0x18e1086b4` sets presentedTime only on matching seed and positive time. `didPresentAtTime:` can drain handlers without setting it; zero cleanup remains possible. |

The historical capability omission is fixed on the enabled path, but it is not legitimate to promote the 99 sends into 99 observed drawable presentations, to assign all 3042 zeros to SameFrame alone, or to claim the new packet schema has been validated through SkyLight solely because send returned success. The prior analysis predicted that usable physical feedback could enable the named CA path; it did not prove runtime matching or policy. Those are now explicit unknowns even after sparse transport passes.

## Exact next read-only questions

Use retained capture artifacts and decoded binaries on the host first. **No tablet command, notification registration, trace enablement, mode/arg change, image or hook is authorized by this list.** The current ABI exports rejection counts and accepted times, not a per-IRQ raw tuple; unavailable historic fields cannot be recreated by another snapshot.

The host-only grader reads retained frame-info audit snapshots and compares raw physical counter pairs when supplied. It joins a flush to the recorded IRQ generation and treats missing identities as unavailable. [Offline m120 diagnostics](../../tooling/native/m120-diag/format.md)

1. For the **same** successful latch, recover pre-flush INTF1/2 FRAME_COUNT/LINE_COUNT, flush-bound clock, IRQ status and generation, ISR frame-before/frame-after/line/time/CTL, and qualifier end. Compare a valid `+1` case with an equal-counter case. Does VSYNC or programmable fetch/CTL acceptance precede the increment? Check the physical register pair, not Capture.sequence or the worker serial. Preserve only relative IDs/numeric fields, never addresses/pointers or surface data.
2. Compare arm → enable/status-clear → CTL write/barrier → IRQ → CTL-clear readback → disable/disarm order. Could a handler that sampled the arm address complete after disarm, or could a newly serviced pending status have an in-window CPU timestamp? What links that capture to this flush? The audit alone neither proves nor rules out those races. No timestamp substitution or relaxed equality test follows from this question.
3. For one accepted mask-3 packet, follow swap ID and continuous ticks through actual IOMFB receive/dictionary, CA collect/match/versioned callback, SkyLight transaction/present release, then app queue ID/update seed and setter. Separately identify the suppressed swap's zero/cleanup branch. Positive kernel sends and all-zero app callbacks can coexist without identifying the higher failing hop.
4. For that same workload, answer the [admission/depth/global-fence questions](m120-throughput.md#exact-next-diagnostic-questions). Closing physical FI alone would leave 511/511 fallback and the over-budget service path unresolved. Closing pipe eligibility alone would leave AU-2 and downstream positive-time propagation unresolved.

## Historical November 26 baseline and contracts

Everything in this section describes the pre-frame-info y12l/older source analysis. The installed baseline had 3897/3898 zero times at 60 fps and a panel matcher issue; the tool matcher was updated then. No matcher/tool/source test is being rerun or changed by this documentation task. In particular, the historical Unsupported selector-74 and absent cross-driver timestamp rows below **do not describe enabled `3fcf74f4`**. Public API semantics and decoded positive-time/matching requirements remain relevant.

### Evidence and status convention

- **ours-OK**: our source implements the stated part, or the supplied run demonstrates it. This does not certify the whole display path.
- **ours-missing**: a required facility or piece of evidence is absent from our source/output. It does not by itself prove which runtime branch produced zero.
- **unknown**: the static mechanism is identified, but the installed runtime value, branch, ABI field meaning, or causal link has not been observed.

Sources were examined in this order: public Metal headers/documentation; public IOGraphics and Linux DRM sources; our drivers and existing IOMFB/DPU notes; then the decoded **27.3 arm64e shared cache, build 26C5035r**, and the matching decoded IOMobileGraphicsFamily in the kernel collection. Framework versions are QuartzCore 1223.20.4, SkyLight 959.4, IOMobileFramebuffer 700.51.10, Metal 384.5. Addresses below are unslid addresses for these artifacts, not live process addresses or an ABI promise. Only conclusions, symbols, addresses and field relationships are recorded here; Apple binaries and disassembly listings are excluded from the repository.

Reproduction uses externally supplied `$DSC`, `$KC` and `$IPSW` paths. Do not extract binaries or redirect Apple listings into this clone. For example, inspect `"$IPSW" dyld macho "$DSC" QuartzCore --symbols --demangle`, then `"$IPSW" dyld disass "$DSC" --vaddr 0x18e1086b4 --no-color` in the analysis terminal. Optimized shared Objective-C trampolines must be resolved to their selector; a direct-symbol xref alone misses some calls.

### Public contract and the clocks

[Apple's `MTLDrawable.presentedTime`](https://developer.apple.com/documentation/metal/mtldrawable/presentedtime) defines a host-time value in seconds for actual presentation and zero when the drawable has not been presented, including dropped drawables. [`addPresentedHandler`](https://developer.apple.com/documentation/metal/mtldrawable/addpresentedhandler(_:)) provides the place to read that property. Neither the callback's CPU arrival time nor [`MTLCommandBuffer.present`](https://developer.apple.com/documentation/metal/mtlcommandbuffer/present(_:attime:)) substitutes for the actual display timestamp. The local public `Metal/MTLDrawable.h` agrees about the zero value.

| Check | Status | Evidence and limit |
|---|---|---|
| Probe reads the correct property at callback time | ours-OK | `tooling/native/piano-frametime/piano-frametime.m`, presented handler: reads `shown.presentedTime` inside the handler, before dispatching the result to the main queue. No delayed property lookup. |
| Missing handler versus zero property distinguished | ours-OK | `presentCallback` and `zero_presented_time` are separate. JSON renders nonpositive time as null; the supplied raw zero count is not a count of missing callbacks. Warmup and drain remain separate from the measurement window. |
| App schedule and GPU completion available | ours-OK | Supplied baseline, zero GPU errors and steady 16.67 ms median intervals. These are separate report series. |
| A positive, distinct app presentation series | ours-missing | 0 positive timestamps in both supplied runs; no valid presentation histogram or measured app scanout cadence. |

IOMFB frame-info values use integer host ticks and are converted by CA using `_CATimeWithHostTime`. PianoDpu's `nowUs()` converts `mach_absolute_time()` to microseconds. `latchUs` and `workUs` are **durations**, not absolute host timestamps. They cannot be copied into a presentation-time field. Likewise a requested presentation time is an input, not confirmation that a frame appeared. A correct bridge must preserve the clock, the completed swap identity, and the fact of a successful latch; sampling the worker's return time would provide a later observation with different semantics.

### Metal, CA image queues and WindowServer

The probe has a windowed, composited `CAMetalLayer`. Its drawable belongs to an app image queue, while the framebuffer swap belongs to WindowServer's composed desktop. These are not the same ID. CA tracks the image queue's update seed through the render transaction and relates the transaction to the completed display swap. A callback for an arbitrary recent swap is insufficient.

| Check / hop | Status | 27.3 evidence |
|---|---|---|
| Command buffer schedules drawable presentation | ours-OK | Probe registers handler before `presentDrawable:` / commit. Metal `-[_MTLCommandBuffer presentDrawable:]` at `0x1942c6fd4` installs a scheduled block (`0x1942c71f4`); its successful path sends the drawable `present`. GPU completion is not the timestamp setter. |
| Drawable enters the CA image queue | unknown | CA `layer_private_present_impl` at `0x18de3a8c0` calls `CAImageQueueInsertImage_` at `0x18de3ab84`; `register_drawable` at `0x18de38bac` registers the IOSurface. Need installed queue ID, insert result and update seed to prove this path for sampled baseline frames. |
| Server retains and releases render presents | unknown | SkyLight acquires via `CARenderPresentAcquire` at `0x182726b80` / `0x1827bc878`, retaining a `WSCAPresent` with two initially zero doubles and an opaque present pointer. `WSCALayerBacking::ClaimPresent` (`0x1827282a4`) moves it into a display-present list and writes the second double. Live retention/completion path is unobserved. |
| Display feedback reaches SkyLight | unknown | `SLCADisplay::register_transaction_complete_callback` (`0x182acad74`) calls CA's `setFrameInfoCallback:`. Block `0x182acae2c` adapts the versioned CA frame-info structure into `DisplayPresentInfo`; `display_present_handler` (`0x182ab4c8c`) schedules processing. `supports_present_handlers` (`0x18286cef0`) excludes virtual displays; the installed virtual/paravirtual/builtin flags and selected completion branch are unknown. Paravirtual and virtual are distinct flags. |
| SkyLight releases the render present with time | unknown | The display-present processing block (`0x182ab5d08`) converts the supplied host ticks to seconds at `0x182ab5dc0`, locates the display's pending presents and matches version-dependent completion identifiers, then writes that time into each retained `WSCAPresent` at `0x182ab5e7c`. `unique_ptr<WSCAPresent>::reset` (`0x18272cd28`, call at `0x18272cd4c`) and vector destruction (`0x182ab730c`) pass its two doubles to `CARenderPresentRelease`. An unfilled double remains zero. Need a same-swap runtime observation, not merely proof that these functions exist. |
| CA sends per-image-queue feedback to the app | unknown | `CARenderPresentRelease` (`0x18e01f6dc`) calls `CA::Render::DisplayableObject::did_display` (`0x18df3087c`) with presentation timing; `__CACImageQueueDidPresent` (`0x18e239628`) sends Mach message `0x9dd5`. Receiver `__XImageQueueDidPresent` (`0x18e239d18`) invokes `invoke_presented_callback` (`0x18e15b808`), which dispatches the registered image-queue callback. Need queue/seed/time at both ends. |
| CA accepts positive feedback for the matching drawable | unknown | `layer_presented` (`0x18e1086b4`) checks update seed and `time > 0`. Only the matching positive path sends `setPresentedTime:` (`0x18e1088ec`); the nonpositive/mismatching path at `0x18e1088b4` does not set it. A live branch sample would distinguish zero feedback from queue cleanup. |
| Getter supplies the stored value to our probe | ours-OK | `-[CAMetalDrawable presentedTime]` (`0x18e1081ac`) reads the double at drawable `+0x68`; setter (`0x18e1081b4`) writes it. The probe reads that API directly. |
| Handler invocation alone proves presentation | ours-missing | `didPresentAtTime:` (`0x18e1067c0`) drains handlers without setting `+0x68`. CA can invoke it with zero from `releasePrivateReferences:` (`0x18de3b17c`, call `0x18de3b1d0`), queue cleanup (`0x18de34abc`, call `0x18de34b78`), or drawable release (`0x18e1062ec`). Thus all handlers may run while all getters remain zero. |

The last row is the explanation for the apparently contradictory observation. It does not select between an uninserted/dropped drawable, absent or zero display feedback, a seed/transaction mismatch, and a display path that bypasses positive feedback. Queue collection/release (`CAImageQueueCollect_`, `0x18de343dc`) is resource lifetime management, not proof of visibility.

### CA to IOMobileFramebuffer

`CA::WindowServer::IOMFBDisplay::finish_update` (`0x18e1eeb28`) calls `CA::IOMobileFramebuffer::swap_end` (`0x18e1e9a1c`), which calls `IOMobileFramebufferSwapEnd`. Successful return means the swap request was accepted, not that the display has presented it.

| Operation | Selector / format observed in 700.51.10 | Status for ours |
|---|---|---|
| SwapBegin | `4`, `_kern_SwapBegin` (`0x1b5ed9974`), returns swap ID | ours-OK: our queue accepts swaps. Baseline does not record their IDs. |
| SwapEnd | `5`, `_kern_SwapEnd` (`0x1b5ed9a40`), structure size **0x830**, not the old public reverse-engineered 0x46c | ours-OK: desktop composition works; does not certify all request fields. |
| SwapWait | `6`, `_kern_SwapWait` (`0x1b5ed9c64`), three scalar inputs | ours-OK for resource-completion path; no actual presentation timestamp returned by our override. |
| EnableNotifications | `72` / `0x48`, `_kern_EnableNotifications` (`0x1b5eda024`), four scalar inputs | unknown: need installed subscriber type, state and mask. Type 3 is frame info; type 5 is vsync. Registration is not done by this offline analysis. |
| ChangeFrameInfo | `73` / `0x49`, `_kern_ChangeFrameInfo` (`0x1b5eda124`) | unknown: actual requested tag mask unobserved. |
| SupportedFrameInfo | `74` / `0x4a`, `_kern_SupportedFrameInfo` (`0x1b5eda194`), indexed key enumeration with 64-byte name and count/flags outputs | ours-missing: `NativeGopDisplay::supported_frame_info` always returns Unsupported. |

These selector numbers are from the supplied version, not inferred from a selector list for an older iOS release. The kernel user-client `supported_frame_info` (`0xfffffe000ac1a5a8`) dispatches to provider virtual slot 320. This connects selector 74 to our override rather than merely to an unrelated exported symbol.

| Check | Status | Evidence and consequence |
|---|---|---|
| Enumerate named frame-info keys | ours-missing | `_InfoKeyInitialize` (`0x1b5ed6938`) queries selector 74 index 0, then allocates/enumerates the key array only on success with a nonzero count. There is no built-in replacement key table on the Unsupported path. `IOMobileFramebufferSupportedFrameInfo` (`0x1b5ed6cb0`) returns that array. Our override cannot supply it. |
| CA registers its frame-info consumer | ours-missing | In CA initialization at `0x18df51364`, null SupportedFrameInfo branches to `0x18df5118c`, skipping `IOMobileFramebufferFrameInfo(..., frame_info_callback, ..., 1, NULL)` at `0x18df513e0` and tag enable at `0x18df5142c`. This conclusively blocks **this** frame-info path for our implementation. Other notification and fallback paths remain. |
| Build a named frame-info subscription | ours-missing | `IOMobileFramebufferFrameInfo` (`0x1b5ed6fd8`) calls `_figure_frame_info_mask` (`0x1b5ed70d4`); a missing key array yields BadArgument. A successful no-op notification-state override does not supply names. |
| Decode an asynchronous completion | unknown | `_FrameInfoNotifyFunc` (`0x1b5ee09e8`) reads mask and swap ID first, maps subsequent words through the enumerated names, reconstructs 64-bit values from two halves when bit 31 is set, and applies timestamp adjustment before invoking the callback. Our packer's format resembles this transport, but its field-name mapping and installed delivery are unproved. |
| Associate feedback with the pending CA frame | unknown | `IOMFBDisplay::collect_frame_info` (`0x18e20003c`) matches the completed swap ID against pending `FrameInfo`. `IOMFBServer::frame_info_callback` (`0x18e19415c`) reads `Presentation_time`, prefers a nonzero `Vbl_FrameTime`, reads `Requested_presentation`, converts host ticks to seconds, and forwards the record. Need the actual ID and dictionary. |
| Forward CA frame timing to consumers | unknown | `forward_frame_info_callback` (`0x18e194a8c`) also reads `Min_FrameTime` / `Max_FrameTime` and invokes the registered block. The raw positive tick value must survive into the versioned CA callback and SkyLight render-present release. |
| Vsync notification as an alternative clock | ours-missing | `_IOMobileFramebufferVsyncNotifyFunc` (`0x1b5ee0c84`) decodes timestamp, an adjusted second time and interval from six half-word slots. CA has `IOMFBServer::vsync_callback` (`0x18e192790`). Our software VBL timer never emits that timestamped notification. A vsync tick alone would still need association with the successful swap. |
| Alternative present-on-finish path works for this display | unknown | CA has `IOMFBDisplay::presents_metal_layer_on_finish` (`0x18e1ed1bc`) and other completion/release paths. The baseline does not expose their conditions, transaction IDs or chosen branch. Unsupported frame-info alone therefore does not close the all-zero root cause. |

`SwapSetTimestamp` / `SwapSetTimestamps` (`0x1b5eda428` / `0x1b5eda4ac`) set requested swap times. Renaming or forwarding those inputs as actual `Presentation_time` would manufacture evidence.

### What our two drivers really complete

Source anchors: [`NativeGopDisplay.cpp`](../../package/NativeGopDisplay/NativeGopDisplay.cpp), [`PianoDpuFlipClient.h`](../../package/PianoDpu/PianoDpuFlipClient.h), [`DpuFlip.h`](../../package/PianoDpu/DpuFlip.h), [`DpuFlip.cpp`](../../package/PianoDpu/DpuFlip.cpp), and [`DpuSnapshot.cpp`](../../package/PianoDpu/DpuSnapshot.cpp). Context: [NativeGopDisplay](native-gop-display.md), [DPU](dpu.md), [frame-time probe](../tools/frametime.md).

| Check | Status | Source evidence / limitation |
|---|---|---|
| Software swap scheduling | ours-OK | `vblFired` (line 1224 at analysis revision) schedules `swap_begin_gated`; refresh-contract deadlines may be absolute. The timer may stop while idle. It is not an MDSS interrupt or a continuous panel VBL clock. |
| Completion after own copy/flip job | ours-OK | With the default deferred job mode, `jobDoneAction` (1803) checks `fDoneSerial >= fWaitSerial`, updates pipe bookkeeping, and calls the family `swap_complete_gated` for the current waiting swap. Other defer modes can complete earlier. The installed boot-arg choices are not in the supplied report. |
| Swap wait versus scanout | ours-OK | `swap_wait` (1446) delegates to the family and normalizes Busy for an already-completed nonblocking request. This prevents queue stalls, but creates no presentation timestamp. |
| Swap-complete notification transport | ours-OK | `notify_swap_done_gated` (3101) builds a five-word `SwapInfo`, dispatches it by swap ID, and `do_swap_info_notify_gated` (3119) sends mask/ID/selected words via `sendAsyncResultCanDrop`. This is source evidence of a transport, not evidence of a registered consumer or a positive time. |
| Named payload contract for 27.3 | ours-missing | `supported_frame_info` (3094) returns Unsupported; `set_frame_info_notification_state_gated` (3097) is a no-op success; `set_notify_tag_mask_gated` accepts type 3 only. No published key names tie our five words to the names CA requests. |
| Absolute actual time in the swap blob | unknown | Word 0 is read from family object `+264` (`kSwapInfoTime`), word 1 is calculated as a delta from the preceding word 0, other inputs come from swap `+1072` and `+1136`, plus zero. Our code never writes an absolute latch time to `+264`. Its 27.3 meaning/value is not established. Do not label it a known zero or a known presentation time. |
| Family fills our missing latch time | unknown | The matching KC's `IOMobileFramebufferLegacy::swap_complete_gated(bool)` (`0xfffffe000ac26f9c`) calls slot 462 (`notify_swap_done_gated`) at `0xfffffe000ac2707c`, with null blob input, before a later absolute-clock call at `0xfffffe000ac27128`. Our override builds its own blob. This is not evidence that the family injects the physical latch timestamp into it. |
| DPU accepts the new scanout address | ours-OK | `DpuFlip::flushTo` / `waitLatch` wait for CTL_FLUSH bits to clear, with IRQ wakeups/polling and a timeout. Successful new-surface flips pass SMMU and configured underrun checks before `kPianoDpuFlipShown`. This is stronger than GPU completion but is observed after hardware acceptance, not a measured first-pixel timestamp. |
| DPU latch timestamp reaches IOMFB | ours-missing | `PianoDpuFlipRequest` version 1 returns result, `latchUs` and `workUs` only. It has no absolute latch timestamp or IOMFB swap ID. IRQ handler stores status bits and wakes a waiter without storing IRQ time. The cross-driver completion carries no timestamp tied to a swap. |
| Continuous DPU VBL exported to CA | ours-missing | VSYNC interrupt is enabled around a flush and disabled afterward; no continuous hardware-vsync callback feeds NativeGopDisplay/IOMFB. A software deadline cannot replace it. |
| Per-flip cadence and hardware cross-check | ours-OK | `cadenceFlip(hw.nowUs(), ...)` records statistics at successful new-surface flips; reshows are separate. Optional `machdep.piano_dpu_snapshot` reads INTF1/2 frame counters and monotonic sample time. Both are useful checks, but neither maps an app drawable/update seed to the displayed desktop swap. |

The payload offsets are inherited from the paravirtual implementation, with 27.3 family/request offsets selected by `NATIVE_GOP_IOMFB_ABI=2`. Five recognizable words are not a verified 27.3 frame-info schema. Do not turn on selector-74 names by guessing their order, stamp every completion with `mach_absolute_time()`, or treat same-surface reshow, failed latch, copy fallback and new-surface flip as one event.

For comparison, the public [IOGraphics `IOFramebuffer::handleVBL`](https://github.com/apple-oss-distributions/IOGraphics/blob/main/IOGraphicsFamily/IOFramebuffer.cpp) captures a hardware-event time and maintains VBL timing for clients. This is a conceptual reference, **not** this IOMFB private ABI. Public [Linux DPU video-mode encoder](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/dpu_encoder_phys_vid.c) handles a hardware vblank, checks pending CTL flush acceptance and signals the associated frame completion; the CRTC path reports vblank/page-flip events. Those event/identity distinctions matter here. The locally available GopFramebuffer reference at revision `d711e8c9` likewise leaves SupportedFrameInfo unsupported, despite having a separate software-vsync notification; it is not proof that our copied blob carries actual presentation timestamps. No implementation was copied from these comparisons.

### Panel identity: driver attributes versus CoreGraphics

| Check | Status | Evidence |
|---|---|---|
| Published panel identity | ours-OK | `createDisplayAttributes` (2808): `ManufacturerID="XMI"`, `LegacyManufacturerID=0x61a9`, `ProductID=0x8750`, fixed product name `Xiaomi Pad 8 Pro`, native dimensions from GOP (normally 3200×2136), fixed panel `IOMFBUUID`. These are synthesized display attributes, not an EDID read from the DSI panel. The fake second display has a different UUID/product (`0xd6`). |
| CG always returns those published numbers | unknown | The old source comment and old tool assumed this unconditionally. The supplied baseline disproves that assumption for its default selection, but contains no actual CG vendor/product values. They cannot be recovered from the reported failure alone. |
| 27.3 external identity propagation | unknown | CA `IOMFBDisplay::update_framebuffer_locked` (`0x18e201500`) reads ProductID / LegacyManufacturerID at `0x18e2019dc` / `0x18e201a1c` into its EDID-attribute structure. SkyLight `SLCADisplay::process_display_connect` (`0x182ac24ec`) copies non-null `CADisplay.externalDisplayAttributes` into CG identity. The installed display type/flags are unobserved. |
| Conditional internal fallback identity | unknown | `-[CADisplay externalDisplayAttributes]` (`0x18df1250c`) returns attributes only for display types 1/2. SkyLight's nil-attributes fallback seeds vendor `0x610`; if no supported encoded model is found, `get_gestalt_model` (`0x182ac56f8`) can return `0x756e6b6e` (`unkn`). This pair is a proven **conditional code path**, not an observed ID pair for y12l. Virtual-display handling can bypass attribute refresh too. |
| Matcher no longer requires raw CG IDs on the internal fallback | ours-OK | Default first prefers the driver's canonical `0x61a9/0x8750`. Fallback requires a NativeGopDisplay with fixed panel UUID and canonical ProductAttributes, matching native pixel dimensions, then either the fixed public product name on both sides, or builtin on both sides plus the specific Apple/unknown fallback pair. Ambiguous best matches fail. `--main-display` remains explicit and unchanged. |
| Actual CG identity and matcher success on y12l | unknown | New `--list-displays` reports numeric CG vendor/product, builtin/main flags, pixel dimensions, filtered driver attributes and `panel_match_available`, without opening the animation window. Capture JSON now also records the selected numeric identity; failed selection prints numeric candidates. A new read is required to state the actual pair or claim tablet validation. |

The fallback is deliberately tied to our driver's identity rather than accepting every Apple display or silently choosing the main display. It does not claim a generic ID uniquely identifies Xiaomi hardware. It avoids serials, arbitrary display names, EDID dumps and machine UUIDs. The old exact-ID path remains usable when the driver attributes reach CG unchanged.


### Historical host validation of the tooling change

**ours-OK:** portable tests run with ASan/UBSan and strict warnings, covering canonical IDs, fake-display rejection, guarded internal fallback, dimension/builtin/driver-identity rejection and fixed-name fallback. The arm64 Objective-C/C executable compiles with `-Wall -Wextra -Werror` against the available **27.2 SDK**; `--help` exposes the new enumeration option. A 27.3 SDK compile, GUI enumeration and tablet matcher validation are **unknown**, not claimed by these host checks. Existing cadence-analysis tests also pass. No driver tests were added because ONE change remains HOLD.

## Root cause assessment

**Closed at the immediate kernel-mechanism level:** 3186 SameFrame audit events arise from zero modulo advance between the physical pre-flush and stable ISR INTF1 counter reads. This yields time zero, survives exact worker-serial/current-swap handoff, and suppresses type-3 dispatch before transport. The accepted sparse transport has zero send errors. The obsolete Unsupported-schema explanation is superseded on this captured path.

**Not closed end-to-end:** the hardware reason for the equal counters, full WindowServer/CA/IOMFB occupancy and eligibility timing, global-fence queue/execution/observation attribution, and same-swap dictionary/transaction/drawable propagation. The supplied aggregate evidence cannot select a single correction that proves both root causes. The absent raw event/branch fields are evidence limitations, not permission to image again.

## Fix-image gate

```text
Path: app queue/update seed -> SkyLight render present/CA page -> IOMFB swap
      -> Native queued eligibility/global producer fence -> DPU flush/latch
      -> physical IRQ frame-count comparison -> exact worker serial/current
      swap -> named type-3 packet -> CA dictionary/pending FrameInfo ->
      SkyLight matched transaction -> positive time on the same drawable.
Checks: ours-OK fixed-120 timing, safe latch, exact qualifier/handoff and sparse
        schema/transport; ours-missing reliable physical FI, load pipe admission,
        surface-specific GPU/event correlation and positive app feedback;
        unknown physical count/IRQ phase, actual queue/ready branch and
        downstream selected receive/match/release branch.
Root cause: SameFrame and fallback mechanisms located; BOTH root causes still
            require the explicit physical-counter and queue/fence comparisons.
            No single proven change closes both. Main review remains HOLD even
            if later evidence resolves them.
Owner gate: two images in the cadence/presentation lane without causal closure;
            STOP imaging. Analysis-only documentation commit; no production
            C/C++/Objective-C or kext edits, no tablet access, no push.
Next: retained read-only evidence for the four questions above; missing fields
      remain unavailable. Do not synthesize time from worker/timer completion,
      relax the physical comparison, complete early or select another image.
ONE change: HOLD
```
