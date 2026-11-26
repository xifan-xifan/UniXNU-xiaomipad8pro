# RAM16G extra RAM for GPU and video

The tablet has 16 GiB of DRAM. ABL hands the HLOS 15.34 GiB, and XNU manages one contiguous window of 13310 MiB at 0x8c0004000.
About 1522 MiB of HLOS memory sits in region0, the 2 GiB bank at 0x80000000, split into 11 pieces between about 50 carve-outs.
arm64 XNU cannot add that memory to its VM: it manages one range (one `physBase`/`memSize`, and `pa_valid` is one compare). Drivers
can still use it as private DMA pools. R3 is that option. R1 is the sibling option: the region1 runs below the window for the GPU.
DarwinBoot publishes both banks in the same ADT property.

## The interface

The opt-in `--extra-ram` stages a 384-byte placeholder at `/chosen/piano-extra-ram`. DarwinBoot fills it with up to
16 little-endian records of 24 bytes each. An unused slot is all zeros. An FD without extra-RAM support leaves the entire
property zeroed and both consumers see zero usable runs. The earlier R1-only firmware validates the legacy 136-byte size,
so use the combined 0002 patch with the new placeholder. Without the property, DarwinBoot keeps its ordinary memory allocation path.

| field | type | meaning |
|---|---|---|
| base | u64 | the run's first byte |
| size | u64 | its length |
| bank | u32 | 1 = region1 (R1), 0 = region0 (R3) |
| use | u32 | 0 = any driver pool, 1 = GPU, 2 = DSP, 3 = video, 4 = camera, 5 = display |

DarwinBoot clips region1 conventional memory to the three HLOS spans in
[the running DT](hyp-evidence/memory-map-running-dt.txt) and to the selected window base. It reserves whole 2 MiB slabs by
address and publishes each successful run with bank 1 and use 1. It collects up to eight GPU runs.

DarwinBoot takes the region0 runs from `firmware/patches/darwinboot/region0-allow.json`. That file also has the never-use list and
DarwinBoot's rule. A run is published only in two cases:
- UEFI conventional memory descriptors cover the whole run. DarwinBoot then reserves it with `AllocatePages(AllocateAddress)`
  and publishes it only if that succeeds.
- An EfiReserved descriptor covers it wholly. This is PIL_Reserved, where UEFI never allocates.

A descriptor of any other type, a gap or partial reserved coverage causes refusal. The console records each overlapping
descriptor's physical span and type, and each publication, refusal or stage skip.

Region0 publication is capped at stage 1. `piano-extra-ram-stage=0` disables region0 publication. The stage-2 `cma-block`
is skipped even if the boot requests a higher stage. `package/include/PianoExtraRam.h` checks the bank, allowed span, use tag,
never-use spans, overlap with XNU's window and overlap with earlier records. `region0_allow_check.py` checks the JSON, firmware
table and consumer table for agreement, and compares both copies of the region1 bounds.

## Region0, run by run

Sources:
- ABL `/memory` as seen in Android's live DT and in the Ubuntu running DT (`docs/hyp-evidence/memory-map-running-dt.txt`).
- Android `/proc/iomem` System RAM and the dmesg `OF: reserved mem` lines (rooted stock capture, 2026-11-20).
- pianoPkg `MemoryMapLib.c`, plus Mu-Silicium `RamManagerDxe`. RamManagerDxe adds every gap of the static map inside an SMEM RAM
  partition as EfiConventionalMemory.

| HLOS piece (ABL /memory) | MiB | Android | mainline DT | UEFI | verdict |
|---|---|---|---|---|---|
| 0x81960000.., 4 slivers | 2.66 | RAM | RAM | RSRV0/XBL_DT etc. | never (no whole 2 MiB slab) |
| 0x82800000..0x84a00000 | 34 | qdss CMA (coresight ETR assigns it) | qdss no-map | gap | never |
| **0x89d80000..0x9b000000** | 274.5 | **System RAM**, no mpss node | mpss@8ba00000 no-map, status disabled | **PIL_Reserved** (EfiReserved) from 0x89e00000 | **allow "modem" 0x89e00000 + 274 MiB, use 3 video, stage 1** |
| **0x9ee00000..0xb8000000** | 402 | System RAM, ramoops 0xa3500000 | RAM, ramoops | PIL_Reserved to 0xa3500000, then Display_Demura, FD, Kernel | **allow "pil-tail" 0x9ee00000 + 70 MiB, use 2 DSP, stage 1**; the rest never (Demura, UEFI FD/Kernel) |
| 0xbd980000..0xd4e23000 | 372.6 | System RAM (secure_cdsp, non_secure_display CMA) | RAM | DXE_Heap | never until a DarwinBoot map print shows it free of runtime data |
| 0xd5100000..0xd8000000 | 47 | RAM | RAM | DXE_Heap, Piano_Log 0xd7f00000 | never (Piano_Log) |
| **0xe3720000..0xf8800000** | 336.9 | System RAM: reusable CMA only (cnss_wlan, qmc_dma, va_md, adsp_heap, linux,cma, vm_comm, qseecom_ta, qseecom, sp_region), trust_ui_vm 0xf3800000 + 80 MiB (gh_secure_vm_loader assigns it) | RAM + trust-ui-vm no-map | not in the static map, so RamManagerDxe makes it conventional | **allow "cma-block" 0xe3800000 + 256 MiB, use 0 any, stage 2**; trust_ui_vm never |
| 0xfc400000..0xff800000 | 52 | splash (map), cdsp_eva CMA | splash no-map | Display_Reserved | never (the boot framebuffer is scanned out) |

The allow-list holds 600 MiB in 300 slabs: stage 1 has 344 MiB, stage 2 has 256 MiB. The never list also covers everything the brief
names: qdss, splash, Display_Demura, Piano_Log, cvp, cdsp and video. The host tests check each of these addresses.

The modem run holds the AVC arena whole. Android treats it as System RAM, Ubuntu disables the modem, and UEFI keeps the run reserved.
DarwinBoot can hand it off directly when the live map confirms that reservation. The RAM boot still needs to show that the CPU self-test
and the Iris golden-frame decode both pass with this backing.

## Which drivers, and in which order

| driver | pool today (from XNU's window) | DMA path | CPU view | fit for region0 | risk |
|---|---|---|---|---|---|
| GPU (PianoA830) | 1 GiB BO pool + paged BOs | own SMMU bank, translated (client IOVA 0x20000000..0x3ffffe000) | WC/cached | R1 serves it from region1 | R1's |
| **video (PianoVideo level 6)** | **256 MiB AVC arena, boot-lifetime reservation ("V9h"), 4 KiB pages; HEVC sessions up to 768 MiB** | own context bank, translated at 4 KiB (VideoSmmu.h/VideoArena.h); PA below the bank's output size | WC only (cached zero + clean, then never cached) | **yes: no contiguity, whole slabs, WC is the attribute already proven for out-of-window memory (cdsp slice)** | moderate: an active lane; the fallback is the XNU path unchanged |
| camera (PianoCamera level 5) | 16 MiB + 16 KiB contiguous, 2 MiB aligned, reserved early against fragmentation | translated window, 2 MiB blocks of one contiguous range (CameraSmmuWindow.h), PA < 1 TiB | cacheable + `dc civac` | yes (8 contiguous slabs; fragmentation goes away) | low, but the gain is small; needs a cacheable mapping of non-managed memory (unproven) |
| audio (PianoAudio) / sensors FastRPC (PianoSensors) | 16 + 8 MiB in the cdsp carve-out | bypassed streams: IOVA = PA, must be below 4 GiB | WC | region0 is the only place that works; pil-tail (use 2) is reserved for them | low; no need today (the slices are not full) |
| Wi-Fi (PianoWifi) | 24-64 MiB contiguous below 2^36 (`piano-wifi-dma`) | bypass at L3/L4, a 32-bit translated window at L5 | WC | possible later (below 4 GiB would make a bypass stream 32-bit clean) | high: the top-priority lane |
| DPU (PianoDpu) | scans IOSurfaces (XNU pages) through its own L3 windows | its bank identity-maps 0x80000000..0xffffffff read-write | n/a | not a pool user | note: the DPU stream can reach every region0 run, as it already reaches cdsp/cvp |
| UFS (NativeUfs), ADSP metadata, Bluetooth | KiB-sized tables, bounces, PAS metadata | various | various | no | not worth it |

Order:
1. Video, the first integration. It is the largest pool that is already out of a hard path: a boot-lifetime 256 MiB that frees
   256 MiB of window RAM for the whole boot. Its CPU attribute and SMMU translation need nothing new.
2. Camera, the 16 MiB early reserve, once a cacheable mapping of a region0 slab is proven, or with its view switched to WC.
3. DSP (pil-tail), when audio or sensors outgrow the 25 MiB cdsp slice.
4. Stage 2 (cma-block, use 0). This comes after the modem run passes on the tablet and DarwinBoot's map print shows the block as
   conventional and reserved. Its likely owner is video, for HEVC: `piano-extra-ram-any=3`, but 274 + 256 MiB is still under the
   768 MiB HEVC maximum, so HEVC also needs a mixed backing. That is an open item.

## The helper

The helper has three parts:
- `package/include/PianoExtraRam.h` is portable. It parses the records, holds the allow, never and window checks, and decides
  ownership by use tag. Use-0 runs go only to the one use named by `piano-extra-ram-any`, so two kexts never share a slab and no
  kernel-wide word is needed. It also hands out 2 MiB slabs (contiguous or not, all or nothing), runs the self-test and builds the
  report.
- `package/include/PianoExtraRamKernel.h` reads the ADT and the boot arguments and runs the self-test through IODeviceMemory WC maps.
  It hands out slabs under a lock and describes taken slabs as one `kIOMemoryTypePhysical64` descriptor.
- `package/include/PianoExtraRamSysctl.h` provides `machdep.piano_extra_ram_<use>`, read-only and root-only. It sits in its own source
  file, because `<sys/sysctl.h>` declares `struct session`, which collides with `pianovideo::session`.

Self-test, per run, in two passes over every slab:
- Pass 1 writes a per-address pattern into three words of each 4 KiB page.
- Pass 2 reads every pattern back, then zeroes the slab and checks the zeros.

Because the pattern depends on the address, an alias inside the run fails. A failed run is refused, and none of its slabs are handed
out. A device-side hook, `device(pa, bytes)`, is called per run. Video has no hook of its own: the first decoded AVC frame landing in a
region0 arena, matching the fixture's golden SHA-256, is its device-side proof.

The GPU passes the same ADT bytes and XNU window bounds to the shared validator. Its tier takes only bank-1/use-1 records.
It keeps its existing S8 GPU endpoint self-test, BO allocator, and `machdep.piano_gpu_tier` counters.

Switches:
- `piano-gpu-tier=1` enables eligible paged BOs of at least 2 MiB. Mode 2 also tries smaller BOs. Default is 0.
- `piano-extra-ram=1` enables shared kernel pools, including PianoVideo. Default is 0.
- `piano-video-extra-ram=1` turns on video's use. Default is 0.
- `piano-extra-ram-any=<use>` names the one use served the use-0 runs.
- `piano-extra-ram-stage=N` caps the stage served (default 1).
- `piano-extra-ram-test=0` skips the shared pool self-test and keeps that pool closed.

Tests:
- `package/include/tests/extra_ram_host.cpp` and `region0_allow_check.py` run in `package/include/tests/run.sh`.
- `PianoVideo/tests/geometry_host.cpp` builds the arena on region0 slabs, in reverse slab order and mixed with XNU pages. It also checks
  that pages just outside the run, and the video and cdsp carve-outs, are refused.

## Fix-image gate

Path: ABL `/memory` and carve-outs → pianoPkg `MemoryMapLib` and RamManagerDxe UEFI descriptors → DarwinBoot allocates
the XNU window and qualifies region1 GPU runs plus the region0 stage-1 allow-list → fixed-size `/chosen/piano-extra-ram`
array → `PianoExtraRam.h` validation.
- GPU → PianoA830 S8 `CP_MEM_WRITE`, fence and physical readback at both run endpoints → S9 Metal publication →
  macOS 27.3 WindowServer/Metal via NativeAdrenoMetal and the AGX IOKit ABI → `BoAlloc`, qualified slabs, WC Physical64
  view and CB0 SMMU leaves → fence retirement and `BoFree`. The Linux msm GEM and sheng Adreno740 paths establish the
  scatter/gather mapping and descriptor model. See [GPU reference path](drivers/ram-extra.md#reference-path).
- Video → PianoVideo level 6 `KernelPool::start`, CPU self-test via WC maps, AVC arena takes 128 slabs →
  `kIOMemoryTypePhysical64` WC descriptor → `rememberPage` and `buildArena` Iris SMMU leaves → decoded NV12 →
  RAW loans from `arenaDesc` → the macOS 27.3 VideoToolbox frontend's IOSurface copy. Linux SM8750 Iris uses the same
  video context and firmware carve-out. See [Video hardware description](drivers/video.md#2-hardware-facts).

Checks:
- **ours-OK** — XNU window selection is independent of the extra runs. The host tests cover map qualification, refusal
  and wire encoding, exact ADT placeholder length, zero padding, the allow/never/window/use checks and exclusive ownership.
- **ours-OK** — GPU tests cover both endpoint probes, failed runs, paged BO fallback, CPU and SMMU mapping constraints and
  tier counters. Video tests cover self-test failures, slab accounting, AVC L3 leaves for region0 and mixed backing,
  guard pages and XNU fallback. Both kext builds have zero unresolved imports.
- **ours-missing** — Stage-2 publication, DSP and camera consumers, and mixed HEVC backing.
- **unknown** — The live UEFI map and reservation results, Gunyah stage-2 access from each device, WC map and zeroing
  behavior on both banks, actual slab use and release, and the effect on memory pressure and frame cadence.

Root cause: XNU manages one contiguous physical window, so the region1 runs below it and region0's eligible fragments
require explicit driver ownership. R1 carried a 136-byte version/count and GPU pair table. R3 consumed tagged 24-byte
records from the same property. The R1 length is not a multiple of 24, and its header carries neither bank nor use.

ONE change: Standardize one DarwinBoot-to-driver ADT handoff with bank and use tags. Publish only qualified ranges,
route bank-1/use-1 records into the existing GPU tier and bank-0/use-3 into the existing AVC arena pool, and retain
both drivers' fallback allocation paths.

## RAM-boot read list

Use one 27.3 RAM image with DarwinBoot patches 0001 and 0002, both kexts, their firmware, `PIANO_EXTRA_RAM=1` and the
volatile Data profile with its boot guard. Start from the desktop paged GPU profile with
`piano-gpu=2 piano-gpu-mem=paged piano-gpu-user-vm=1 piano-video=6 piano-video-vt=1`. The enabled boot adds
`piano-gpu-tier=1 piano-extra-ram=1 piano-video-extra-ram=1`. Use the same payload and adjust one argument per control boot.

| control | expected readout |
|---|---|
| `piano-gpu-tier=0` | GPU tier mode 0, no endpoint probes or tier allocations. Video can still use the modem slabs. |
| `piano-extra-ram=0` | Video reports master 0 and parses the ADT with no self-test or slabs taken. The GPU still follows `piano-gpu-tier`. |
| `piano-video-extra-ram=0` | Video reports driver 0 and uses XNU for the AVC arena. The GPU can still use bank 1. |

Record the console from boot through the workload. Verify `kern.bootargs`, the source build's full argument record if
the kernel read is clipped, successful login and nonempty captures from this boot before interpreting any counts.

| question | read | pass |
|---|---|---|
| Firmware ownership | `DARWINBOOT: window` and `DARWINBOOT: extra-ram` lines, plus `NativeRamBlock: RAM window` | Same XNU window across boots. For each published GPU run, record its successful reservation. Record modem, pil-tail and cma-block decisions. Expect modem and pil-tail to be wholly covered by PIL_Reserved (type 0), and a stage skip for cma-block. |
| ADT handoff | `ioreg -p IODeviceTree -r -n chosen -d 1 -l -w0 \| grep piano-extra-ram` | Decode the 384-byte value in 24-byte little-endian slots. Match the console reservations to bank 1/use 1, modem `0x89e00000 + 0x11200000` to bank 0/use 3 and pil-tail `0x9ee00000 + 0x04600000` to bank 0/use 2. Unused slots zero. |
| GPU admission and allocations | Console `PianoA830: gpu tier:` and `adreno830_start:`; `sysctl machdep.piano_gpu_tier machdep.piano_gpu_paged machdep.piano_gpu_pt machdep.piano_gpu_state machdep.piano_gpu_recovery` at desktop idle, under a fixed textured 3D workload, after close and at +10 min | Both endpoints qualify each accepted run and at least one run passes. Match `pass-mask` to console. `allocs` and `used-mib` rise, `free-mib` falls, and workload slabs return on close. Record refusals, faults and recovery counters. |
| AVC admission and DMA | `sudo sysctl machdep.piano_extra_ram_video`; console `RAM16G arena from extra RAM` and `V9h boot reservation ready`; run the [level-6 VT AVC qualification](drivers/video.md#14-v5a-bounded-native-avc-vt-bridge-tablet-qualification-pending) | Modem is `state=ours`, `self-test=pass pages=70144 bad=0`, `used=128 free=9`. The arena line says `128 slabs` and first PA `0x89e00000`. Active NV12 matches `bd230ec4a9cd22c274831d1b526c43d3ceb39a089257c3cb4114fbea0a8102dc`. |
| Memory and presentation | `sysctl hw.memsize hw.pagesize vm.swapusage`, `vm_stat`, `memory_pressure`; timed presentation intervals and C920 capture during the same GPU workload | Record free and wired memory on the video control (approximately 256 MiB effect) and the GPU control. Compare p50/p95/p99 intervals and frames over 16.67 ms at the same refresh rate. Retain SMMU fault logs and inspect the capture for pixel errors. |
| Composite smoke | Boot, login, Wi-Fi lease, audio, charging and panic log | The smoke passes with both consumers enabled and on each control. |
