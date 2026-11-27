# ram16g-hevc1 / ram16g-hctl1: pending AVC cancellation

The `ram16g-hctl1` control **exonerates RAM16G mixed HEVC backing**. Both
runs reach the same eight-accepted/four-pending AVC cancellation boundary,
complete native teardown, and lose public progress. Apple 27.3 disassembly
establishes the missing contract: accepted asynchronous frames need terminal
`Demt` dispositions **processed before the `dinv` reply clears the guest's
callback target**. Native STOP/CLOSE and `dinv=0` alone do not retire them.

The fix in `a426ce70` implements that contract and has host regression coverage.
It passed [RAM qualification](#ram-qualification-2026-11-27) on `v131y12u-ram1`
with the standard 256 MiB XNU arena. Installed `v13.1y12q` predates the fix.

## Deciding control evidence

Evidence labels in this section mean the archived **2026-11-27
`ram16g-hctl1`** run. They deliberately omit its storage location.

- `run.log:2`: image commit **`1da1485af7`**, the same image source as
  `ram16g-hevc1`. `ba2.txt:4` and the archived chosen properties contain
  `piano-extra-ram=0`, `piano-video-extra-ram=0`, and
  `piano-video-extra-ram-hevc=0`.
- `RESULT.md:59-65,211-217`: extra pool disabled, no mixed reservation,
  **268435456-byte XNU arena**, 1080p HEVC admission cap retained, boot
  reservation ready. This is a real backing control, not a zero-pending run.
- `run.log:37-40`: unchanged payload pin `252cb0d465e6…`, tool pin
  `b0072c969490…`, twelve AVC phase passes, then the separate HEVC control.
  The qualification tool is [tooling/native/video-v6.mm](../../tooling/native/video-v6.mm).
- `rx-v6.txt:1`: AVC process **PID 3908**, start **608742734 us** uptime.
  Lines 2-1948 contain exactly twelve phase passes: rejection, manifest,
  two demux, two audit, decoder-spec, single-regression, ibp-async,
  ip-temporal, ibp-temporal, and ibp-no-output. No HEVC phase consumes this
  invocation's budget.
- `rx-v6.txt:1949-1987`: epoch 20 creates in **64249 us**. Public tokens
  **85899345921-85899345928** are accepted; the first four have image
  callbacks, the last four remain pending. The exact boundary is
  `cancel-begin epoch=20 accepted=8 pending=4 no-wait-before-invalidate=1`,
  followed by `PASS cancel-had-accepted-work` and `PASS cancel-had-pending`.
- `rx-v6.txt:1988`: host SSH wrapper **`Alarm clock: 14`**; no
  `V6_TOOL_EXIT`, `cancel-joined`, epoch-21 recreate, or qualification pass.
  `RESULT.md:123,126-127` independently records the missing exit marker,
  twelve passes and 248 native submits with zero video-parser issues.

The exact **PID 3908** native-session breakdown in `rx-v6-console.txt` is:

| Native session | Phase | Open uptime (us) / line | Close line | ETB/EBD/decoded |
| --- | --- | --- | --- | --- |
| `56360001` | single-regression | 608866095 / 131 | 727 | 1/1/1 |
| `56360002` | ibp-async | 609280618 / 780 | 3765 | 60/60/60 |
| `56360003` | ip-temporal | 609980556 / 3812 | 6794 | 60/60/60 |
| `56360004` | ibp-temporal | 610679195 / 6841 | 9800 | 60/60/60 |
| `56360005` | ibp-no-output | 611390918 / 9847 | 12789 | 60/60/60 |
| **`56360006`** | **epoch-20 cancel-pending** | **612090883 / 12836** | **13272** | **7/7/7** |

Epoch 20 is selected for PID 3908 at console line **12812**. Its **wire**
tokens 1-8 correspond to the eight public source-frame tokens above; these
are different identifier domains. At lines **13215-13232**, token 5 is in
`Dbuf`/surface lookup, token 8's held admission is successfully published,
and `dinv` request **115** arrives. Token 8 never reaches ETB. Only tokens
1-4 sent successful `Demt`/received callbacks. The remaining records span
surface delivery, submitted firmware work, and queued-before-submit work.

Lines **13230-13285** are the deciding teardown, not the later background
sessions:

1. Token 5's acquired surface is disposed without a successful frame emit;
   lookup/invalidate/release totals are **5/5/5**, held=0, maps=0,
   surface-open/close=1/1 (13231-13238).
2. STOP RAW, STOP input and CLOSE succeed. Session **`56360006`** closes
   with result=0, loans=0, watchdog=0, fault-bits=0, no quarantine; nine
   resident HFI ledger entries are released on close (13246-13272).
3. Worker reports **cancelled 4**, pending0; **`invalidate safe
   worker-joined 1` at 13274** precedes **`dinv` 115 status 0 at 13275**.
   The reply is enqueued at 13276.
4. The next request is **`dfin` 116**, **PID 3908**, answered **-12903**
   at 13282 and enqueued at 13283. No `-dec` for this session or epoch-21
   create follows before the public stall.

First native input is at 612166524 us (13026); `dinv`/close/`dfin` all
precede the next clock marker **612285718 us** (13286). Thus the clean
teardown is already over by **3.542984 seconds after tool start**, leaving
hundreds of seconds of the nominal 360-second SSH budget. The 363-second
`run.log:37-38` block includes setup/capture and is not decoder runtime.

**Other processes must not be merged into that session.** Lines
**17638-18466** contain **sixteen PID 2751 HEVC decoder selections** and
short-lived rejected sessions, each using `dinv` request **104** and then
`-dec`; none opens a native session. Their `invalidate safe` lines are not
epoch 20 completing. The unlabelled `invalidate safe` at **19654**, near
908.69 seconds uptime (about 300 seconds after AVC start), is consistent
with process-alarm cleanup, but does not identify a PID or prove a public
invalidate return. No sampled Apple thread stack or independent remote exit
status exists in this archive.

The independent HEVC control is **PID 3970**, not PID 2751 or PID 3908:
`rx-hevc.txt:1,49-59` passes fixture validation and **`hevc-control`**, rejects
the wide 4K format with **-12910**, creates no decoded frames, and ends
**`HEVC_TOOL_EXIT=0`**. `RESULT.md:128-134` confirms that verdict. Its
no-native-AVC-marker grading failures are in this deliberately declining
HEVC window, not missing AVC evidence. `rx-grade-compare.txt` passes final
wired delta **-131 MiB** and postx **-279 MiB**, but fails idle/postload/
postvideo deltas **-416/-477/-374 MiB**; those coarse memory comparisons
are not a cancellation diagnosis and are not being relabelled as passes.

For comparison, the **2026-11-26 `ram16g-hevc1`** run used the mixed arena
(274 MiB region0 + 410 MiB XNU). Its AVC PID **3876**, cancel session
**`56360012`**, `dinv` **115**, `dfin` **116**, eight admissions/four image
callbacks/seven ETBs and four local cancellations are the same pattern
(`rx-v6.txt:1949-1988`, `rx-v6-console.txt:12933-12995`). Clean teardown
finished by elapsed **4.304066 seconds**. Separate HEVC PID **3932** passed
24 golden P010 callbacks/reopen and `HEVC_TOOL_EXIT=0`; background HEVC PID
**2783** was unrelated to AVC qualification. Both captures therefore
exclude HEVC phase budget exhaustion. Reproduction with the XNU-only arena
also excludes mixed backing as the cause of this stall.

## Apple 27.3 binary contract

The supplied decoded **arm64e** cache was inspected directly with
`ipsw dyld macho -n/-l`, `ipsw dyld disass -i`, and `ipsw dyld dump -a`.
Addresses below are **unslid virtual addresses in these exact binaries**,
not addresses from a running tablet or guesses from symbol names:

| In-cache install path | Identity |
| --- | --- |
| `/System/Library/Frameworks/VideoToolbox.framework/Versions/A/VideoToolbox` | version **3395.5.1**, UUID **DE6D299C-3D76-3F97-9A24-9F9C78F779C7**, macOS MinOS/SDK **27.3** |
| `/System/Library/PrivateFrameworks/VideoToolboxParavirtualizationSupport.framework/Versions/A/VideoToolboxParavirtualizationSupport` | UUID **76809DA1-E06B-3EAB-BABB-73C2112C13C0**, macOS MinOS/SDK **27.3** |

The supplied root and cryptex trees contain no additional VideoToolbox or
paravirtual-video component. `AppleVideoToolboxParavirtualizationDriver`
and `AppleVideoToolboxParavirtualizationUserClient` are the service names
used by the support framework and implemented by our kext. The decoder
itself is **`ParavirtualizedVideoDecoder` inside VideoToolbox**, not a
separate userspace library named AppleVideoToolboxParavirtualization.
No extracted dylib or binary/disassembly dump is tracked.

### Invalidate, finish, and the outstanding-frame semaphore

| Function / address | Instructions establishing the behavior |
| --- | --- |
| `VTDecompressionSessionInvalidate` **0x196dfd5bc** | **0x196dfd848-0x196dfd89c** calls the decoder CMBaseObject base-class Invalidate slot (+0x18); **0x196dfd8a4** calls `VTDecompressionSessionWaitForAsynchronousFrames`; only afterwards does **0x196dfd8b0** `CFRelease` the decoder. |
| `kParavirtualizedVideoDecoderVTable` **0x1e4028848**, base class **0x1e4028880** | Decoded pointer dump maps base-class Invalidate to **0x1972fba08** and Finalize to **0x1972fbbc4**. This closes the indirect call above. |
| `ParavirtualizedVideoDecoder_Invalidate` **0x1972fba08** | Builds `dinv`, request flags=1 (**0x1972fba44-0x1972fba60**), calls **`VTParavirtualizationGuestSendMessageToHostAndCopyReplySync` at 0x1972fba84**, reads `,err`, then locks storage+0x10 and **zeros both callback/session targets at storage+0/+8** (**0x1972fbaac-0x1972fbabc**). It does not decrement outstanding frames or consume a list of cancelled tokens from `dinv`. |
| `VTDecompressionSessionWaitForAsynchronousFrames` **0x196dfb2f0** | Calls `VTDecompressionSessionFinishDelayedFrames` at **0x196dfb3e0**, ignores that return value, then waits on **public session+0x40** with `FigSemaphoreWaitRelative` (**0x196dfb3e4-0x196dfb3ec**). Timeout **-12792** retries with doubled intervals (**0x196dfb3f0-0x196dfb458**), initially **5 seconds**, with no total deadline. It re-signals the semaphore before returning 0. This is a frame-accounting semaphore, not the reply-clerk semaphore. |
| `vtDecompressionDuctDecodeSingleFrame` **0x196e1ad28** | **0x196e1ae70-0x196e1ae90** increments **public session+0x38**; the transition to 1 consumes session+0x40's semaphore. **0x196e1b070-0x196e1b0c4** records a fresh frame token in duct+0x38's tracking dictionary before calling the decoder. |
| `vtDecompressionDuctEmitDecodedFrame` **0x196e189b4** | **0x196e18f58-0x196e18f90** finds/removes that token, decrements **public session+0x38**, and signals **session+0x40 only when the count reaches zero**. A `dinv` status reply cannot execute this path. |
| `ParavirtualizedVideoDecoder_FinishDelayedFrames` **0x196e12190** | Sends **`dfin` synchronously at 0x196e1221c**, reads `,err` at **0x196e12234**, logs a nonzero result and returns it. It does not complete pending tokens on failure. |
| `ParavirtualizedVideoDecoder_Finalize` **0x1972fbbc4** | Sends **`-dec` asynchronously at 0x1972fbcf4**, then removes the UUID handler. This follows decoder release, which public invalidate has not reached while waiting for outstanding frames. |

The PV invalidator itself waits for the **`dinv` reply**, not a per-token
PV dispatch group. The **public VT layer then waits for its outstanding
frame count to reach zero through token completions**. Rejected `dfrm`
transport/admission errors have their own completion path; after a successful
asynchronous `dfrm` admission, the host owns terminal `Demt` delivery.
`ParavirtualizedVideoDecoder_DecodeFrameWithOptions` **0x1972fbe6c** builds
`dfrm` with `,frm` and `,dfl` (**0x1972fc3d4-0x1972fc400**), receives its
admission reply synchronously (**0x1972fc520**), reads status and info flags
(**0x1972fc528-0x1972fc568**), and preserves asynchronous completion ownership
on a successful async reply. Admission is not an image completion.

This explains **why `dfin` arrives after `dinv`** in both failed and
successful sessions: it is the public invalidate implementation's subsequent
`WaitForAsynchronousFrames` call. **-12903 (`kVTInvalidSessionErr`)** is
returned by our invalidating state and propagated by PV FinishDelayedFrames;
the public waiter ignores it and still waits on the frame semaphore. In an
already-empty session the semaphore is available and destruction proceeds.
With four abandoned tokens it is unavailable. Changing `dfin` to 0 would
not decrement the count and cannot fix this defect.

### Exact `Demt` disposition and ordering

The handler is the registered block
**`___ParavirtualizedVideoDecoder_CreateInstanceWithSpecification_block_invoke`
at 0x196e0f328**. Its switch compares **`0x44656d74` (`Demt`) at
0x196e0f3d4-0x196e0f3e0** and branches to **0x196e0f874**. That case:

1. Reads **`,frm` as SInt64** at **0x196e0f894-0x196e0f8a8**,
   **`,err` as SInt32** at **0x196e0f8b0-0x196e0f8c8**, and
   **`,inf` as UInt32** at **0x196e0f8cc-0x196e0f8e4**.
2. Copies optional **`,uui`** at **0x196e0f8e8-0x196e0f904**. When
   absent/null, **0x196e0fedc sets the pixel buffer to NULL**. The reader
   `VTParavirtualizationMessageCopyCFUUID` **0x1973087c4** accepts absence:
   **0x197308850-0x197308858** calls the outlined zero-result/NULL-output
   helper **0x196ff994c**. When present,
   the UUID must name a pending pixel buffer; the handler removes it from
   the pixel-buffer dictionary and applies attachments. A fabricated UUID
   or a reference to a released surface is not an error-frame substitute.
3. Under the target lock, loads storage+0 and, if non-NULL, calls
   **`VTDecoderSessionEmitDecodedFrame(target, token, status, info, image)`
   at 0x196e0fee0-0x196e0ff00**. There is **no whitelist or special comparison
   for -12903, -12911, or cancellation status** here: the signed status and
   info flags are passed through. `VTDecoderSessionEmitDecodedFrame`
   **0x196dfc0f8** routes to `vtDecoderSessionEmitDecodedFrameCommon`
   **0x196e19388**; its **0x196e1951c-0x196e1953c** nulls the image for
   nonzero status or the dropped-output bit, and its transferred-frame path
   **0x196e1b7b8-0x196e1b7cc** reaches the token retirement above.
4. If storage+0 is already NULL, **0x196e0feec branches to 0x196e1015c**:
   no emit/accounting happens, **but the handler still sends an ACK with
   status 0**. A late `Demt` ACK after `dinv` is therefore not proof of token
   retirement. The normal ACK is a guest reply, flags **8**, with `,err`
   equal to the **emit handler's result**, not the frame's decode error
   (**0x196e10168-0x196e1019c**).

The valid error disposition used by this fix is a **112-byte** big-endian
message, with the same session UUID and the accepted **wire** token:

| Atom / field | Value |
| --- | --- |
| `vtpv` header | 48 bytes, type `Demt`, flags **2** (host request), fresh request ID |
| `para ,frm` | `=s64`, original accepted wire token (24-byte atom) |
| `para ,err` | `=s32`, **-12911 (`kVTVideoDecoderMalfunctionErr`)** (20-byte atom) |
| `para ,inf` | `=u32`, **3 = asynchronous (1) + frame dropped (2)** (20-byte atom) |
| Image/attachments | **No `,uui`, no image, no surface ID or attachments**; `image=0` |

-12903 is not required as a `Demt` cancellation code. The inspected handler
accepts nonzero OSStatus values, including -12903 or -12911; this fix uses
our existing **`kDecodeError=-12911`**, and explicitly sets dropped output.
The exact SDK name is **`kVTDecodeInfo_FrameDropped` (1 << 1)**, paired with
**`kVTDecodeInfo_Asynchronous` (1 << 0)**; dropped output is an info flag,
not an OSStatus. The names/status values were also checked in the local
SDK's `VideoToolbox.framework/Headers/VTErrors.h`. Apple's own
`VTParavirtualizationHostDecoderSessionEmitDecodedFrame` **0x197208d78**
builds the same fields, omits UUID/attachments for NULL images
(**0x197208e5c-0x197208e68**, **0x197209044**), and **waits synchronously for
the guest ACK at 0x197208ebc**. This corroborates the receiver, rather than
inferring a wire format from our implementation.

Ordering is part of the contract, not just payload shape:

- Guest dispatch **`___vtParavirtualizationGuestSetup_block_invoke_3`
  0x19730a020** tests reply flag bit 2 at **0x19730a174-0x19730a178**.
  Non-replies, including `Demt`, go to **`dispatch_channel_async` at
  0x19730a1d8**, then the registered decoder handler. Replies, including
  `dinv`, go **directly to `VTParavirtualizationReplyClerkDeliverReply` at
  0x19730a270**, bypassing that handler queue.
- Consequently, enqueueing four errors and then `dinv=0` is insufficient:
  the reply can wake the invalidator, which clears storage+0, before a
  queued error reaches its emit call. **Wait for terminal ACKs before
  replying to `dinv` while the callback target is still live.**
- In the support binary, **`VTParavirtualizationGuestSupportSendRawMessageToHost`
  0x1a7f27dd0** holds a global unfair lock from **0x1a7f27e34**, including
  **selector 3's `IOConnectCallScalarMethod` at 0x1a7f27f50**, until unlock
  after **0x1a7f27f5c**. Our UC also holds its external-method lock while
  dispatching selector 3. Waiting for a guest ACK inside the `dinv`
  selector would deadlock that ACK's send. The selector must return;
  the worker can own the deferred invalidate reply.
- The default timeout at **0x19735c068** is CMTime **11/10 seconds**;
  decoder creation copies it to storage+0x74 at **0x1972fb4b0-0x1972fb4c4**.
  `VTParavirtualizationReplyClerkWaitForReply` **0x196ff758c** uses a
  separate dispatch semaphore. A timed-out `dinv` also clears the target,
  so ACKs must precede that timeout, not merely precede our eventual reply.

## Comparison with PianoVideo and the minimal fix

The actual source path is
[package/PianoVideo/vt/PianoVideoVTHost.cpp](../../package/PianoVideo/vt/PianoVideoVTHost.cpp).
The image revision's worker cleanup at **lines 571-627** is unchanged in
base `48e75b94`; compare with `git show 1da1485af7:package/PianoVideo/vt/PianoVideoVTHost.cpp`.
Those cancellation/teardown regions were compared byte for byte. This branch
starts at `48e75b94` and does not import the image revision's other mixed HEVC
changes; read `1da1485af7` when inspecting that image's backing implementation.

Before this fix, teardown set invalidating/stopping and synchronously joined
the worker inside selector 3. Worker cleanup suppressed accepted asynchronous
error `Demt` via **`if (!invalidated)`**, then **`frames.cancel()`** erased
local records. It still replied to synchronous originals/held admissions,
released surfaces and loans, closed HFI, and returned `dinv=0`. Local pending
became zero while Apple's outstanding count remained four. Apple then sent
`dfin`, ignored -12903, and waited on the unsignalled frame semaphore.

This is a **general frontend/PV pending-cancel contract defect**. It is
independent of AVC versus HEVC backing allocation. Resident arena retention
is intentional and is not the missing public completion. Native cancelled=2
and frontend cancelled=4 are separate ledgers; seven ETBs for eight accepted
public samples are consistent with cancellation before token 8 submission.
Linux's separate ERROR buffer completion after hardware stop is a useful
analogy, but the Apple binary above, not V4L2 behavior, establishes this fix.

The one production change is an acknowledged, error-only cancellation
transaction:

- `dinv` atomically fences success/admission, records its request ID and
  **returns selector 3**. The existing worker performs cancellation; no
  extra thread, boot argument, timeout increase, pre-invalidate drain or
  backing change is introduced.
- Worker finishes any in-progress surface disposal, cancels native work,
  closes surface owners, releases the RAW loan and staged inputs, and
  closes the native session. Error dispositions contain only token/status/
  flags, so they never refer to a released surface.
- Already-sent `Demt` messages keep their sole completion ownership; the
  worker joins their ACK instead of sending a second message. Every other
  successfully admitted async token receives exactly one null-image error
  `Demt`. Synchronous originals and unpublished held admissions keep their
  original error-reply path, with no duplicate `Demt`.
- Only matching, unexpired **`Demt` ACKs** remain admissible while stopping.
  Each ACK wait uses the existing **2-second guest deadline**, capped during
  invalidate by **900 ms from `dinv` arrival**, leaving 200 ms inside Apple's
  default reply timeout. Send failure, malformed ACK, ACK error or either
  deadline makes `dinv` fail; it cannot claim a successful
  cancellation. Client destruction explicitly abandons guest waits.
- After all dispositions/ACKs, local records are cancelled and the worker
  publishes the deferred `dinv` status and its exit under the session lock.
  Destroy/recreate joins that exit before releasing the session. No success
  frame can be sent after the cancellation fence. Subsequent `dfin` retains
  **-12903**.

The binary establishes the causal contract defect and predicts the archived
public wait. It does not retrospectively supply a sampled thread stack or
prove the remote process's final lifetime/output delivery. The inner
300-second process alarm and the outer 360-second **host SSH** alarm remain
distinct; `Alarm clock: 14` alone is not a decoder exit status.

## Host validation

[vt_cancel_host.cpp](../../package/PianoVideo/tests/vt_cancel_host.cpp)
includes the **production VTHost translation unit**, with kernel API and
native/surface substitutes in
[vt_cancel_kernel_shim.h](../../package/PianoVideo/tests/vt_cancel_kernel_shim.h).
There is no copied cancellation loop or second worker in the test. Four image
completions are primed through the real emitter/ACK/retirement functions;
the production worker then executes the pending cancellation.

The scripted peer accounts each accepted token and clears its callback target
on the `dinv` reply, reproducing the exact binary-derived ordering constraint.
Tests cover eight accepted async tokens, four image completions, four pending
(including a paused **real `lookupSurface` call** and queued-before-submit
input), four exactly-once **112-byte** error dispositions, zero late-success
emits, no duplicate completions, resource release before error emission,
clean worker join, `dinv=0`, post-invalidate `dfin=-12903`, and a fresh
**epoch-21 UUID/session with an image completion**. The test deliberately
holds an ACK to prove `dinv` cannot race ahead of it. It also covers an image
already delivered with its ACK still outstanding, wrong-ID/UUID and duplicate
ACKs, repeat invalidate, held admissions, synchronous originals, enqueue
failure, malformed/nonzero ACK, lost-ACK and late-successful-ACK deadlines,
and client abandonment.

`bash package/PianoVideo/tests/run-bringup-host.sh` runs all C/C++ host suites
with **C++20/C11, -O1 -g -Wall -Wextra -Werror,
-fsanitize=address,undefined -fno-sanitize-recover=all**, plus owned AVC/HEVC
fixture checks and the native CoreMedia serializer check on Darwin. The
optional owner's firmware-file check remains skipped when no firmware is
supplied; scripted firmware checks still run. These tests are protocol and
ownership evidence, not Apple guest execution or tablet decode evidence.

The modified kext translation unit was also compiled using
`tooling/k27/build-native-kext.py PianoVideo package/PianoVideo/vt/PianoVideoVTHost.cpp --out <local-output>`:
the configured kernel **IOPlatformExpert C++ recipe**, target
**arm64e-apple-ios**, real kernel headers, and the builder's
**-Wall -Wextra -Werror** flags. Output is a Mach-O **arm64e** object;
no compiler warnings or errors. No image or KC was built.

Validation on **2026-11-27**: the complete host runner exited **0**, including
the production-worker regression, with no ASan/UBSan findings. Privacy scan
against `48e75b94` found **0 hits/0 refused files**; repository document links
had **0 errors**; `git diff --check` had **0 issues**.

## Fix-image gate

Path: public `VTDecompressionSessionInvalidate` → PV synchronous `dinv` →
frontend success fence/selector return → native/surface/loan cleanup →
terminal error `Demt` for accepted async work → matching guest ACKs while
callback target is live → deferred `dinv` reply/worker exit → public `dfin`
(-12903) → outstanding-count semaphore → decoder release/`-dec` → epoch 21.

Checks: **ours-OK**: both archived runs, PID/session attribution, XNU-only
control, clean native teardown, exact 27.3 vtable/call/wait/accounting/handler/
reply-order trace, terminal wire contract, production-worker regression,
ASan/UBSan host suites and arm64e kernel compile. The standard-arena RAM run
below passed the AVC qualification, including `cancel-recreate`. Keep the
existing tool/image provenance discipline and **360/420-second budgets**.
The mixed-backing fix image still needs its separate HEVC mode/verdict and
must preserve its HEVC golden/reopen results.

Root cause: **Closed at the contract/source boundary**. `if (!invalidated)`
omitted four terminal async dispositions and `frames.cancel()` discarded
only our ledger; Apple waits on a different count. A `dinv` success reply
neither decrements that count nor cancels the wait, and it clears the target
needed by later `Demt`. `dfin=-12903` is a consequence of invalidate order,
not the cause. RAM16G mixed HEVC backing and HEVC phase budget exhaustion
are excluded by the control and time accounting.

The fix defers `dinv` until the worker releases resources and obtains bounded
terminal `Demt` ACKs for its accepted async work.

## RAM qualification (2026-11-27)

**PASS on `v131y12u-ram1`**, using the standard **256 MiB XNU arena**. Unchanged
`video-v6 --qualify` passed all **13/13 phases** in 11 s with `failures=0`,
`bounded-stream-correctness-only=1` and **`V6_TOOL_EXIT=0`**.

- `cancel-begin epoch=20 accepted=8 pending=4 no-wait-before-invalidate=1` was
  followed by **4/4** terminal `Demt` dispositions for tokens 5–8 (`status=-12911`,
  `flags=3`, `image=0`, 28,133–78,527 µs). `cancel-joined` reported eight callbacks
  with `remaining-public-callbacks-not-required=0`. Native close cancelled four
  frames, paired one surface open with one close and left zero maps. Invalidate
  reported worker joined, surfaces released and `close-status 0`.
- Epoch 21 recreated at an IDR and returned **30/30 images at 83.98 fps**, with
  zero null callbacks or errors, high-water 5, maximum callback latency 54,366 µs
  and longest output gap 10,351 µs.
- Video SMMU reported `PASS RXV native-submits=278 stale-snapshots=15 issues=0`,
  with zero faults or quarantine. The harness grep matched benign `watchdog 0`
  power-on lines.

This closes the pre-fix `RXV` cancellation timeout on standard-arena RAM
hardware. Installed `v13.1y12q` predates the fix.
[November 27 hardware results](../results-2026-11-27.md#videotoolbox-pending-cancellation)
