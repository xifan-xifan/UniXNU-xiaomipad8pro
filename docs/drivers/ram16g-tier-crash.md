# RAM16G GPU tier crashes after login

The crash evidence below predates the private-only allocation policy. The follow-up `ram16g-tier3w` ran the workload and returned all 126 MiB of private scratch to the tier. The run recorded `mode=1 state=ready`. [RAM qualification](ram-extra.md#turnip-0015-private-tier-ram-qualification-ram16g-tier3w-2026-11-27)

## What the run establishes

`ram16g-tier2` booted kernel 0077 with the tier CPU-view slab pin fix (KC provenance commit `92634abc7c`). Both video extra-RAM switches were off. SSH answered, login completed, and the run retained the same boot session for 960 seconds (`run.log:6,41–44`). The tier-off comparison `ram16g-ctl1` kept WindowServer PID 103, with zero core-dump denials in its console. That control ran 541 seconds with an earlier KC and a different video test payload. Compare the early crash cluster against that overlap. A matched run beyond the late crash window is still required.

All console line numbers below count LF-delimited lines (`rg -an`). Concurrent kernel messages can run together on a single line. A core-dump denial marks an attempt to dump the process. It carries neither the exception nor the faulting instruction.

| Process and crash marker | Console context |
|---|---|
| Siri AI, PID 2833, `10851` | Reads the Metal plugin property and opens the accelerator at `10753–10754` (161 s). The preceding GPU work sample completed 6/6 jobs with zero recovery at `10847–10849`. |
| iconservicesagent, PID 2747, `12085` | Accelerator opens at `10085–10086`. Two copies of the denial are joined on one line. The nearby GPU counters and subsequent accelerator opens at `12085–12091` show the device still serving other connections. |
| VTDecoderXPCService, PID 2934, `12350` | VT owner, queue mapping, decoder inventory reply and successful accelerator opens at `11036–11050`. The inventory was `decs`, with no decode submission or failed surface lookup attributed to this PID. The GPU continues completing work on either side of the marker. |
| VTDecoderXPCService, PID 3011, `12566` | Same inventory/accelerator sequence at `12477–12491`. The last native VT teardown nearby has success status, and `12556–12559` shows zero GPU recovery and 56/56 completions. |
| loginwindow, PID 105, `39552` | Accelerator opened at `6082–6083`. Adjacent VT messages (`39529–39551`) belong to PID 2864. They decline HEVC before PAS/HFI and complete teardown. This is another client's activity. |
| WindowServer, PID 4058, `40784` | Login started with PID 102. Fresh accelerator opens at `40063–40068` (~843 s) precede this explicit denial after 851 s. GPU work completed 39/39 at `40769–40771`, and `40779–40783` reports ongoing display flips with zero failed surface maps and zero fence timeouts. The service opens again at `40831–40845`, and the final PID is 4086. `ws-crash.txt:2,21` lists a current-boot WindowServer report and the listing completion marker. The report itself and the fate of PID 102 are unresolved. |

- `console.out:63,202,1459,1561,1574` confirms the Normal WC allow-list and three passing S8 runs, for 870 MiB total. These are endpoint probes with aligned CPU reads and GPU writes. `rx-tier-boot.txt:1` reports 742 MiB used, 167 allocations and **101 refusals**. `refusals` counts failed attempts inside the tier allocator (whole-span capacity, placement or backing metadata). Such attempts can succeed on ordinary backing. The counter cannot identify an import failure or a PID. At `rx-tier-postx.txt:1` 862 MiB was used with 192 allocations and 120 refusals. Free space and fragmentation can dominate the bench delta. The 420 MiB paired wired gain in `verdict.txt:142` is workload-wide.
- The console contains zero PianoA830 `BoAlloc`, `BoImport`, `BoMap` or `BoCpuMap` method markers. There is no per-call backing or status record to attach to these crashes. `gpu va/sizes` gives live aggregate classes. For example `console.out:19250` reports 22 WindowServer owned BOs at 16–64 MiB (667 MiB total) and 85 imports (190 MiB), without any tier breakdown. A later snapshot shows just 1 MiB mapped by a VTDecoder connection (`19247`). That snapshot cannot identify the earlier crashed decoder's allocations.
- GPU SIDs marked `unmatched: fault` at `1763–1899` and `7035–7040` are route setup, also visible in the control. No GPU page fault, DEVICE LOST or recovery is recorded. Iris later logs raw FSR `0x400` with zero fault bits and `valid 0` (`21907` and `rx-verdict-video-smmu.txt:1`, which records 248 native submits, 13 stale snapshots and zero issues). Successful native AVC copies place data into IOSurfaces. The tool made progress through 12 phases and timed out (exit 124, `verdict.txt:123–130`). The tier2 verdict's zero count for WindowServer AMFI denials at line 156 contradicts the direct positive at console line 40784. Use the literal denial marker anywhere on a line and deduplicate by PID during the next run.

## Allocation and access paths

The decoded 27.3 cache exposes Metal device creation, IOSurface port create/lookup and base address/lock/unlock, CoreVideo pixel buffer IOSurface create/get, and VideoToolbox decompression entry points. It also contains `vtRemoteDecoderClient_handleServiceConnectionError`. These symbols establish the interfaces. The reports needed to identify the failing userspace call are absent.

| Step | Reference and Piano status |
|---|---|
| Owned backing | Ubuntu v1.4.0 pins Linux `83f0f89bed14`. `drivers/gpu/drm/msm/msm_gem.c:218–249` obtains managed GEM pages and builds the scatterlist. Different in Piano. DarwinBoot reserves R1 outside the XNU DRAM window and tags it in `/chosen/piano-extra-ram`. [A830GpuTier.h](../../package/PianoA830/adreno830/A830GpuTier.h) validates and manages these runs in 2 MiB slabs. The tested `BoAlloc` ABI specified size, alignment and contiguous requirement only. Every non-contiguous allocation meeting the size gate could take a slab before the XNU path. The threshold is 2 MiB in mode 1 and one 4 KiB page in mode 2. Turnip sends its cache-class rounded size when applicable. A BO's size and `DEVICE_LOCAL` Vulkan heap designation say nothing about CPU access. Darwin's ordinary memory type is also `HOST_VISIBLE`. |
| Owned CPU map | Linux `msm_gem.c:355–363` sets WC on a managed GEM object's mmap. Sheng-derived Adreno740 `NativeAdrenoPublish.cpp:501,520–545` uses a Physical64 CPU view with a Device-style cache request. Piano's paths differ. For ordinary paged BOs the XNU named entry owns the cache mode. A tier BO gives Turnip a `BoCpuMap` token which resolves to a pinned Physical64 descriptor forcing WC. XNU 0077 changes explicit WC for the listed R1 PAs to Normal WC. It leaves the pages unmanaged. The direct map's pin lifetime is covered by the prior fix. The effective PTE and ordering under real client traffic are unknown. Tier BOs skip CPU cache maintenance. Resident ordinary imports have a separate physical cache path. [CPU view](../../package/PianoA830/adreno830/NativeAdrenoPublish.cpp) · [Normal WC](../../patches/k27/0077-k27-gpu-tier-normal-write-combining.patch) · [Cache maintenance](../../package/PianoA830/adreno830/NativeAdrenoMemory.cpp) |
| Surface and no-copy import | Linux `msm_gem_prime.c:65–86` and `msm_gem.c:1366+` import the exporter and its scatterlist. Same backing ownership in Piano. Metal's IOSurface texture frontend takes the importing task's `IOSurfaceGetBaseAddress`, retains the surface, and wraps the allocation through the bridge and Turnip host-pointer import. Piano's `BoImport` pins the importing task's existing VA and validates the PA spans. It does not allocate slabs, change CPU mapping attributes or pass an owned BO handle across a process boundary. Ordinary cached IOSurface aliases keep their own attributes and use the driver's physical cache maintenance when needed. If an existing task VA does resolve to an R1 PA and gets as far as span validation, `importTask` rejects it at stage 18. Turnip can surface a failed kernel import as invalid external memory, and the Metal frontend can return nil. The recorded crashes cannot be tied to that error path. [Import and map](../../package/PianoA830/adreno830/NativeAdrenoUserClient.cpp) · [Import wiring and rejection](../../package/PianoA830/adreno830/NativeAdrenoMemory.cpp) |
| GPU map, access and retire | Linux `msm_iommu.c:687–700` maps the BO scatterlist into the GPU domain. Same translation shape on Piano. `BoMap` validates the handle, client IOVA window and spans, then programs CB0's 4 KiB leaves. The R1 PAs fit the GPU output width and the tier is qualified with coherent mode disabled. Fence and unmap precede slab release. S8 and the lack of GPU faults support addressability. They cannot test content integrity of every mapped range or exclude stale data. [GPU VM](../../package/PianoA830/adreno830/NativeAdrenoVm.cpp) |
| Video and display | Linux `iris_vb2.c:97` and `iris_buffer.c:550` use the video device's DMA buffers. Piano also gives Iris a separate arena and SMMU context. The VT host resolves an IOSurface via IOSurfaceRoot and copies from the decoder's arena into it. The GPU's display handoff import uses the presenter descriptor; NativeGopDisplay flips the IOSurface through PianoDpu or copies it. Neither route takes `BoAlloc` slabs for its backing. A Metal or VT process may allocate its own staging or conversion BOs on a separate GPU connection. There is no allocator source in the stock Android capture to establish an equivalent path. The local Linux delta from the v1.4.0 pin changes MSM shrinker/fence behavior and supplies no equivalent R1 CPU-view fix. [VT path](../../package/PianoVideo/vt/PianoVideoVTHost.cpp) · [Display path](../../package/NativeGopDisplay/NativeGopDisplay.cpp) |

The verified driver defect is the allocation contract. A legacy, potentially CPU-mapped BO could silently receive unmanaged R1 backing without declaring how it would be used. The leading crash hypothesis is the **owned BO CPU-access path**, including command, staging, upload, readback and conversion storage. It can expose R1 to CPU access through a Physical64 map instead of XNU's managed backing. Normal WC corrects the previous Device typing for explicit WC PTEs, and the slab pin covers late mapping teardown. Both fixes leave the CPU map and CPU/device handoffs to be qualified with the full Metal workload. A failed CPU map, a CPU-side fault, or wrong contents reaching a client can each occur without a CB0 translation fault. Ordinary cross-process IOSurfaces and the native Iris DMA arena have no route into R1. The exact access, whether an affected process was an importer at that point, and why its exception was fatal require the exception/backtrace and allocation correlation.

## Implementation and host verification

- The existing `Query` exchange advertises `NativeAdrenoParamsGpuPrivate` once the tier is qualified. The optional request uses a new value of the existing `BoAlloc.contiguous` field and preserves the wire layout. Legacy requests use ordinary backing in both modes. Private requests below the mode-1 threshold or denied by the slab allocator also use ordinary backing. A tier-backed handle cannot acquire a CPU-map lease or be reimported from a task VA. [Kernel allocation and map policy](../../package/PianoA830/adreno830/NativeAdrenoUserClient.cpp)
- Turnip sends the new request only for the internal two-pass image-copy staging BO. It retains the legacy path with an older or tier-off kernel and with RD dumping enabled. With the private request active, scratch uses its exact size and stays out of the cache that recycles BOs into later allocations. Stage the updated Turnip together with the kernel for the positive test. An older plugin safely exercises legacy allocations only. [Turnip patch](../../tooling/metal/turnip-a830/mesa/0015-turnip-darwin-private-tier.patch)
- The ASan/UBSan host runner passed seven macOS-path tests and four portable-path tests. The focused `test_tier_boalloc` covers legacy fallback in both modes, the threshold, ordinary CPU leases and imports, private CPU-map and task-alias refusal, capacity fallback, metadata rollback, and GPU map/free with slab retirement. The three changed kernel objects compiled with the native XNU recipe. The Mesa patch applied after 0013/0014 without fuzz and both changed Turnip translation units compiled with the arm64e cross recipe. The WC policy and RAM16G verdict tests also passed. [Focused host tests](../../package/PianoA830/adreno830/tests/test_tier_boalloc.cpp)

## Next-run reads

1. From cold login through the late restart window, capture the full console with boot session identity and positive markers for SSH, desktop, 3D/Metal work and native VT submits. Keep both arms on the same KC, seed, ADT, video payload, workload and duration. Sample WindowServer and loginwindow PIDs throughout. Use at least the 960-second observation window of tier2.
2. Pull **every new or updated .ips** from system and per-user DiagnosticReports, including retired reports and both decoder instances. Snapshot report filenames, sizes and timestamps before login and again before shutdown. Keep the artifacts in the run evidence. Extract exception type/codes, termination reason, faulting address, crashed thread, top frames and image offsets, and correlate them with PID, boot time and the nearest allocator/import/map and GPU fence records.
3. Read `machdep.piano_gpu_tier` at prelogin, postlogin, idle, 3D load, after close and during/after VT. Since qualification, `legacy-small` and `legacy-large` split ordinary requests at 2 MiB, `contiguous` and `private-req` count the other owned requests, and `host-import` and `scanout-import` count validated import attempts. `cpu-maps` counts granted leases; `tier-cpu-map-denied` and `tier-import-denied` should stay zero in ordinary operation. Read these alongside `allocs`, `refusals` and `used-mib`. Save per-connection owned size classes and import/map failures. If an exception recurs, record the owning process, caller, BO handle, requested/rounded size, backing, map type, IOVA, cache policy and result. For an IOSurface, correlate exporter/importer by surface ID and compare effective CPU PTE attributes, then capture the matching GPU/Iris SMMU syndrome and display route.
4. Run an ordinary mapped BO upload/readback, a shared IOSurface producer/consumer test, native AVC decode with output checks, and a positive R1 workload. For the latter use `piano-tier-workload` (below), which issues Metal texture copies that take Turnip's device-internal staging BO at 2-64 MiB. Check GPU copy contents via a separate ordinary readback BO, observe private requests and successful tier allocations increasing, and verify slab return at device close. Generic 3D can succeed without using this scratch path. Require zero new client crashes, zero GPU faults/recovery and zero real Iris fault or quarantine.

## Positive tier workload: piano-tier-workload

Generic 3D, composition and video never ask for the tier: 0015 marks exactly one allocation GPU-private, the device scratch of
`tu_get_scratch_bo`, and its only caller is the two-pass staging branch of `tu_copy_image_to_image` (`use_staging_blit`). The
Metal call that reaches it is `copyFromTexture:toTexture:` between two `MTLStorageModePrivate` textures of the same copy unit and
different pixel formats. Traced in the build inputs (source read; not yet confirmed on the tablet):

| Step | Code | What makes the path hold |
|---|---|---|
| Metal frontend | NativeAdrenoMetal `frontend.mm` `NBBlitEncoder copyFromTexture:...toTexture:` → `nmb_blit_copy_texture` | No format check of its own; both textures must be the frontend's own `NBTexture`s. |
| metal2vulkan | `blit_texture.rs` `copy_from_texture_to_texture` → `texture_copies_of_bytes` → `cmd_copy_image` (direct, each image's own format) | `copy_plan.rs` takes macOS 26.7's rule (formats equal *or of one copy unit*); `copies_into` requires colour→colour with the same alpha-only-ness (`image_plan.rs` `copies_between`). The bridge's own scratch buffer is used only for depth/stencil, overlap and mismatched-extent copies, none of which this tool issues. |
| Image creation | `image_plan.rs` `plan_held` | Usage `ShaderRead` only: no `PixelFormatView`/`ShaderWrite`, so no `MUTABLE_FORMAT`; Private storage, so no `HOST_TRANSFER` usage (`texture/host_copy.rs` gives it to Shared/Managed only), which would force an R8G8 image linear (`tu_image.cc`, `has_r8g8 && HOST_TRANSFER`). Optimal tiling → `TILE6_3`. |
| Turnip | `tu_CmdCopyImage2` → `tu_copy_image_to_image` (`tu_clear_blit.cc`, staging block before `tu_get_scratch_bo`) | Formats differ after `copy_format`, neither image is linear, and one of: R8G8 vs another cpp=2 format (checked before mutability and UBWC); a format whose tiled swap differs from linear (BGRA8); both UBWC. The staging layout is the copy extent, linear, at `src_format`; the scratch class is the next power of two of its size. |
| Scratch and kernel | `tu_get_scratch_bo` (0015) → `darwin_bo_init` with `NativeAdrenoBoAllocGpuPrivate` → `allocBoInner` → `allocateTier` | One BO per power-of-two class per VkDevice, kept until the device is destroyed (process exit), handed out for any smaller request once a larger class exists; out of the BO cache; private only when the kernel advertises `NativeAdrenoParamsGpuPrivate` and RD dumping is off; tier only when `tierEligible` (mode 1: ≥ 2 MiB). |

Build with `make -C tooling/native/piano-tier-workload SDK=macosx` to use the installed SDK. The default SDK is 27.3.
Run `python3 tooling/native/piano-tier-workload/test_cli.py` for CLI tests and the ASan/UBSan parser and verdict tests.
`--dry-run` prints the configuration and expected allocation classes without opening Metal or reading the sysctl.

The tool issues three such pairs: `r8g8` RG8Unorm→R16Uint (the surest trigger), `swap` BGRA8Unorm→RGBA8Unorm, `ubwc`
RGBA8Unorm→R32Uint. For each staging size (default 2,4,8,16,32,64 MiB, ascending so every class is allocated) it uploads a fresh
pseudo-random pattern from a Shared buffer, makes the staged copy, and reads both the destination (staged path) and the source
(control, direct texture→buffer copy) back into Shared buffers by GPU blit; every byte is compared, a private texture is never
CPU-mapped. A control mismatch points at upload/readback, a staged mismatch with a clean control at the staging copy. Phases:
`warm` (first copy per size and path, with a tier read after each), `hold` (`--seconds`, the whole working set copied and
verified round robin, `--pairs` texture pairs per size and path), `linger` (`--linger` seconds alive without GPU work). The tool
reads `machdep.piano_gpu_tier` in-process at each step. `--require-tier` checks for `mode=1 state=ready` before opening Metal and
again at the start of linger. This is the state for `piano-gpu-tier=1`. Exit 0 pass, 1 mismatch, Metal error, or interruption,
2 usage, 4 tier requirement failed. `--tier-report BEFORE DURING AFTER` checks three saved lines offline and requires
`mode=1 state=ready` in all three.

Expected counters for the default sizes (the tool prints `expect-private-bos` and `expect-tier-mib` for other lists): while the
tool lingers, `private-req` and `allocs` up by at least 6 and `used-mib` by at least 126 over the pre-run read; `refusals` should not
rise (a rise with `allocs` short means private requests fell back to ordinary backing: the data check then passes but the tier
was not exercised); `tier-cpu-map-denied` and `tier-import-denied` unchanged. After exit `used-mib` should read at most 2 MiB above
the pre-run value. With an older Turnip (no 0015) or a kernel that does not advertise the contract, `legacy-large` rises by 6
instead and `private-req` stays: the run then proves nothing about the tier. The counters are system-wide, so other GPU clients
can add to them; the checks are lower bounds, and the after-exit check should be read with the GUI idle.

Host smoke on an Apple M5 Pro (27.2, tier sysctl absent): `swap,ubwc` with two pairs, 10 s hold, 6.5 GiB copied, zero
mismatches. Apple's own driver corrupts the `r8g8` copy sporadically (4 of 156 hold copies, each one 64-byte block read as `0xff`, at
32 and 64 MiB, control clean); Apple's 27.x validation forbids copies between differing formats, so this is Apple behaviour outside its
contract, not a tool fault. Vulkan defines a raw copy between size-compatible formats, so on the tablet every path must be
exact. Host smokes use `--paths swap,ubwc`.

For the GPU-only RAM profile, opt into kernel patch 0077 with `PIANO_K27_GPU_TIER_WC=1` during 27.3 kernel preparation.
Use the resulting clone as `K273_TREE` and the printed kernel as `K273_KERNEL` for the RAM image build. Stage the
image with `PIANO_EXTRA_RAM=1` to publish the reservation property. Boot with
`piano-gpu-tier=1 piano-extra-ram=0 piano-video-extra-ram=0`. The daily profile keeps all three boot switches at 0. [Kernel patch](../../patches/k27/README.md)

Spec fragment (runner helper `rsudo`, `$RESULTS` on the host; the binary staged at
`/usr/local/libexec/piano-tier-workload`; run after the desktop-login gate, as the console user inside the GUI session like
piano-frametime). Budget about 190 s:

```sh
prefix="$RESULTS/tier-workload"
rc=0
rsudo '/usr/sbin/sysctl -n machdep.piano_gpu_tier' > "$prefix.before" 2> "$prefix.before.stderr" || rc=1
rsudo '
    u=$(/usr/bin/stat -f %u /dev/console)
    test "$u" -gt 0 || exit 1
    exec /bin/launchctl asuser "$u" /usr/bin/sudo -n -u "#$u" -- \
        /usr/local/libexec/piano-tier-workload --paths all --seconds 120 --linger 10 --require-tier
' > "$prefix.out" 2> "$prefix.stderr" || rc=$?
sed -n 's/^tier linger //p' "$prefix.out" > "$prefix.during"
sleep 5   # connection teardown retires the scratch slabs after their fences
rsudo '/usr/sbin/sysctl -n machdep.piano_gpu_tier' > "$prefix.after" 2> "$prefix.after.stderr" || rc=1
out/piano-tier-workload/piano-tier-workload --tier-report "$prefix.before" "$prefix.during" "$prefix.after" \
    > "$prefix.verdict" || rc=1
test "$(awk '/^config / { n++ } END { print n+0 }' "$prefix.out")" -eq 1 || rc=1
test "$(awk '/^RESULT pass / { n++ } END { print n+0 }' "$prefix.out")" -eq 1 || rc=1
if grep -q '^mismatch' "$prefix.out"; then rc=1; fi
test "$rc" -eq 0
```

Next to it the console capture must show no new client crash, GPU fault, DEVICE LOST or recovery inside the run window, and the
DiagnosticReports listing taken before and after (Next-run reads 2). Open points: the metal2vulkan source read is the
`.cache/gpu-reference` copy next to the bridge, not the pinned prebuilt `libnative_metal_bridge.a` itself; the `swap` and `ubwc`
branches depend on Turnip's per-format swap table and `ubwc_possible` for A830 (the `r8g8` branch does not); whether Turnip's linear
staging layout pads beyond the exact copy size (it would only move a class up, which the lower-bound checks tolerate) is unmeasured.

## Fix-image gate

Path: Metal/IOSurface/CoreVideo and VideoToolbox (27.3 symbols) → NativeAdrenoMetal/Turnip or PV VT host → owned BO / task import / scanout → CB0 or Iris's separate DMA path → display. R1 comes from the DarwinBoot reservation through the chosen handoff and S8 qualification. See the Linux and sheng comparisons above.

Checks: Ours-OK are the handoff, three endpoint probes, 36-bit reachability, slab lifetime pin, ordinary IOSurface backing ownership, and separate Iris arena/scanout. Ours-missing in the run is a GPU-private allocation contract. Unknown are the faulting instructions and effect of R1 backing on their specific CPU accesses. The matched long A/B and private scratch readback form the qualification gate.

Root cause: Size-only R1 admission substituted unmanaged PA-backed memory for CPU-mappable owned BOs. The code establishes that policy defect. The console associates the crashes with the tier and leaves the precise CPU fault or corrupt-data chain unproven.

ONE change: Admit only explicitly GPU-private, non-contiguous BOs to R1 under `piano-gpu-tier`. Reject CPU views of tier-backed objects and keep the existing region1 task-import rejection. Opt Turnip's device-internal blit scratch into that contract, outside its general BO cache and CPU dump path when the kernel advertises support. Legacy/plain allocations fall through to the ordinary allocator.
