# B11 full-resolution RAW10, ISP and AE/AWB analysis

Static audit, 2026-11-27, Xiaomi Pad 8 Pro (`piano`, SM8750). The current camera path is usable;
full-resolution capture is **not implemented end to end**. All five sensor tables and their guarded
DMA sizes are represented and host-tested, but the driver selects only two modes. The 40 MiB front
and 62 MiB rear allocations exceed both the early DMA reservation and every RAW transport slot.
Hardware ISP support exists in the pinned Linux reference, but PianoCamera uses the lite RAW path.
The extension converts Bayer data on the CPU. No camera-specific Metal demosaic exists here.

This document changes no driver, app, image or boot profile. The daily **640×480 `420v` at 15 fps**
path stays untouched. No tablet access, capture, preference change or image qualification was
performed for this audit. Existing daily evidence is recorded in [camera.md](camera.md) and the
[November 26 installed results](../results-2026-11-26.md#install): Photo Booth preview/effects and
10 s H.264 movies from both cameras. Those results do not qualify the larger modes or establish
hands-on QuickTime save/reopen/playback.

## Evidence boundary and reference pin

Native source baseline: `5e9958f81c16bb7b70fa2811f37dff3a2ff79188`. Linux reference **K** is
[`yzddmr6/linux-sm8750-piano`, commit `7f8d6bedde479a01f4a05d6d0581b43c7fd20a01`][kernel].
The reference checkout had a later HEAD; this audit read the requested commit's Git objects,
not the working tree. Every K citation below is fixed to that commit. Linux is read as a
specification; no Linux implementation is reproduced here.

| Evidence | Exact files and entry points |
|---|---|
| Sensor data and provenance | [data/camera/README.md][tables], [MANIFEST][manifest], [s5kjn1.tbl][rear-table], [ov32d40.tbl][front-table], [csiphy-v2.4.tbl][phy-table]; [sensor-tables.py][table-tool]: `SENSORS`, `PHYS`, `cmd_header`, generated `CameraTableMode` |
| Native sensor/control facts | [CameraHw.h][hw]: `kSensors`, `kRearL3Width`, `kFrontL3Width`, `kRearExposure`, `kFrontExposure`, `csiphySettle`; [CameraPower.h][power]: `Session::sensorWrite`, `Session::sensorRead` |
| Native acquisition and memory | [CameraStream.h][stream]: `frameLayout`, `Stream::run`, `Stream::up`, `Stream::sensorTables`, `Stream::streamLive`, `Stream::down`, `DmaReserve`; [PianoCamera.cpp][driver]: `Ops`, `PianoCamera::serveLive`, `PianoCameraUserClient::startLive`; [CameraSmmu.h][route]: `planRoute`, `releaseRoute` |
| Native image processing and delivery | [CameraIsp.h][isp]: `pianoRaw10`, `pianoIspLayout`, `pianoIsp`; [PianoCameraABI.h][abi]; [PianoCameraExtension.m][extension]: `validLiveFrame`, `convert:pixel:`, `readLive`, `tick`; [CameraOutput.h][output] |
| K sensor specification | [drivers/media/i2c/s5kjn1.c][k-rear]: `s5kjn1_supported_modes`, `s5kjn1_enable_streams`, `s5kjn1_set_ctrl`; [drivers/media/i2c/ov32d40.c][k-front]: `supported_modes`, `__ov32d40_start_stream`, `ov32d40_set_ctrl`, `ov32d40_update_exposure_range` |
| K board and receiver | [arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-camera.dtsi][k-dt]: `camera_wide`, `camera_front`, `camss`, `cam_wide_vcm`; [camss.c][k-camss]: `sm8750_resources`, `csiphy_res_8750`, `csid_res_8750`, `vfe_res_8750`, `icc_res_sm8750`; [camss-csiphy-3ph-1-0.c][k-phy]: `csiphy_init`, `csiphy_settle_cnt_calc` |
| K RAW and hardware ISP | [camss-csid-980.c][k-csid]: `__csid_configure_rdi_stream`, `__csid_configure_ipp_stream`; [camss-vfe-gen4.c][k-vfe4]: `vfe_wm_start`, `vfe_pix_start`, `vfe_isp_update`; [camss-vfe.c][k-vfe]: `vfe_line_has_isp`, `vfe_ctrl_init`; [camss-video.c][k-video]: `video_buf_init`, `msm_video_register` |

Re-extraction from K with `sensor-tables.py check data/camera --src <reference>` matched all
three table files, including MANIFEST section counts. K source SHA-256 values matched the
tracked provenance: S5KJN1 `4d1e2dad37d06fb594ac044903fe2eb674b3bf1ab3135da56913b53d6b244a92`,
OV32D40 `f784a23f088de3dccc165d61b7ebbce6484c4f7a7c0420cf901ede417ffba684`,
CSIPHY `51c1101c69c5858d0a72cec74e810ea21c85561b170818ef786c198ce31ce12f`.
The placeholder above denotes an untracked extraction of pinned sources, not a published local path.

## Five modes: sensor timing, link and DMA geometry

The daily output size is **not a sensor mode**. `Stream::run` selects rear 4080×3072 or front
3264×1836 using [CameraHw.h][hw] constants; neither ABI accepts a mode index or requested dimensions.
`Stream::streamLive` publishes on a 66,667 µs schedule and the extension offers native BGRA at
15 fps or compatibility `420v` at 15–30 fps. The sensor tables themselves describe nominal
30 fps binned modes. There is no corresponding daily VTS rewrite to make the wire rate 15 fps:
the 15 fps publication limit must not be used to halve receiver/DMA bandwidth estimates.
This distinction follows [CameraStream.h][stream]: `sensorTables`, `streamLive`, and
[CameraOutput.h][output]: `kPianoNativeFps`, `pianoOutputFormat`; the latter's introductory
comment about sensor rate is less precise than the acquisition implementation.

| Sensor / table section | Timing from K and table parameters | MCLK; link clock → D-PHY rate per lane | Selection today |
|---|---|---|---|
| Front OV32D40 `[mode 3264x1836]` | `line_length=4728`, output frame length 2820, sensor VTS 2820; nominal 30 fps | 19.2 MHz; 842.4 MHz → 1.6848 Gbit/s | Selected for daily front acquisition; output is reduced |
| Front OV32D40 `[mode 3264x2448]` | 4728, output frame length 2820, sensor VTS 2820; nominal 30 fps | 19.2 MHz; 842.4 MHz → 1.6848 Gbit/s | Table only; fits 16 MiB capacity but no selector |
| Front OV32D40 `[mode 6528x4896]` | 7840, output frame length 5102, **sensor VTS 2551**; nominal 10 fps | 19.2 MHz; 842.4 MHz → 1.6848 Gbit/s | Table only; needs 40 MiB DMA arena |
| Rear S5KJN1 `[mode 4080x3072]` | `hts=4352`, `vts=4288`, exposure default 3840, margin 22; nominal 30 fps | 24 MHz; 700 MHz → 1.4 Gbit/s | Selected for daily rear acquisition; output is reduced |
| Rear S5KJN1 `[mode 8160x6144]` | `hts=8688`, `vts=6400`, exposure default 6144, **margin 44**; nominal 10 fps | 24 MHz; 700 MHz → 1.4 Gbit/s | Table only; needs 62 MiB DMA arena |

Sources: [front table][front-table] / K `supported_modes`, `link_freq_menu_items`,
`OV32D40_PIXEL_RATE`; [rear table][rear-table] / K `s5kjn1_supported_modes`,
`s5kjn1_link_freq_menu`, `s5kjn1_freq_to_pixel_rate`; K [board endpoints][k-dt].
OV timing is in output pixel/line units for frame interval calculations, with separate raw VTS
for exposure. Its declared pixel rate is 400 MHz, distinct from the CSI transport capacity.
Rear's declared pixel rate is 560 Mpixel/s. The nominal rates are source descriptions and timing
calculations, not new measurements. Both rear tables actually write `0x0136=0x1800`;
the nearby native comment implying only the smaller table uses 24 MHz is not a full-mode validation.

All five modes use four data lanes, VC0, CSI-2 RAW10 data type `0x2b`. K sensor endpoints use
lane numbers 1–4; CSIPHY endpoints use positions 0–3. Native `csidRxCfg0` programs map `0x3210`
and chooses rear PHY1 or front PHY4. Aggregate serial bit rates are 5.6 Gbit/s rear and
6.7392 Gbit/s front, before CSI protocol overhead. None of the tables adds lanes for full resolution.
At the current 400 MHz timer, `csiphySettle` evaluates to **29 for either link**, using K's
85 ns + 6 UI preparation bound, integer picoseconds and the minus-six timer adjustment.
`Stream::up` uses the same [103-entry D-PHY tuning table][phy-table], replaces its five settle
entries and skips its 16 skew entries. The table's “2p5Gbps” name describes the tuning array;
it is not the selected sensor link rate. Actual full-mode link reliability is unknown.

`frameLayout` computes packed row bytes `width/4*5`, a 16-byte-aligned row stride, payload
`stride*height`, then rounds `payload + 256 KiB` up to 2 MiB. MiB below means 1,048,576 bytes;
MB/s means decimal bytes/s. Bandwidth is the padded RAW DMA payload at nominal sensor rate,
excluding CPU copies, cache maintenance, ISP work and other masters.

| Mode | Packed row / DMA stride (B) | RAW DMA payload (B) | Guarded arena | Actual guard (B) | `FRAME_INCR` (`payload >> 8`) | Nominal RAW MB/s |
|---|---:|---:|---:|---:|---:|---:|
| Front 3264×1836 | 4080 / 4080 | 7,490,880 | 8 MiB | 897,728 | 29,261, with 64 B remainder | 224.7264 at 30 fps |
| Front 3264×2448 | 4080 / 4080 | 9,987,840 | 10 MiB | 497,920 | 39,015 | 299.6352 at 30 fps |
| Front 6528×4896 | 8160 / 8160 | 39,951,360 | **40 MiB** | 1,991,680 | 156,060 | 399.5136 at 10 fps |
| Rear 4080×3072 | 5100 / 5104 | 15,679,488 | 16 MiB | 1,097,728 | 61,248 | 470.38464 at 30 fps |
| Rear 8160×6144 | 10200 / 10208 | 62,717,952 | **62 MiB** | 2,293,760 | 244,992 | 627.17952 at 10 fps |

These numbers are independently reproducible from [CameraStream.h][stream]: `frameLayout` and
are explicitly checked by [camera_host.cpp][host]: `testModeLayouts`. The 40/62 MiB names mean
**allocation sizes**, not exact packed image sizes. In particular the 1836-line front payload
must retain its final 64 bytes; truncating the allocation/guard boundary to `FRAME_INCR << 8`
would put live pixels in the guard. The WM is supplied a fresh explicit address per target.
The frame increment register does not define the CPU payload length.

### Receiver, clocks and memory fabric

The current path is sensor → CSIPHY1/4 → **CSID-lite0 RDI0** → **TFE-lite0 WM0** → private RAW
DMA buffer. `Stream::up` enables TITAN_TOP, MM1 and GCC camera HF AXI, configures CAMCC PLL0
nominally at 1200 MHz, sets CPHY RX, sensor PHY timer, IFE-lite and IFE-lite CSID sources to
400 MHz and CAMNOC RT AXI to 200 MHz. It resets CSID, prepares frame-mode WM0, enables RAW10
payload-only MIPI packing and applies the sensor start/init/mode/controls/on sections last.
These are [CameraStream.h][stream]: `up`, `sensorTables`, and [CameraHw.h][hw]: `kL3Rcgs`,
CSID/TFE constants. RAW routing has no demosaic, colour correction or NV12 output. Some lite-980
SOF/EOF/error bit meanings are inferred from CSID680 in `CameraHw.h:kRdiErrors` and adjacent
comments; status decoding alone cannot certify full-mode timing without correlated frame evidence.

K [camss.c][k-camss] offers multiple clock points: CSID 400/480 MHz, PHY RX 400/480 MHz with a
400 MHz timer, lite VFE 266.667/400/480 MHz and CAMNOC RT AXI 200/300/400/480 MHz. Full TFE
resources additionally have main rates up to 833 MHz and Bayer-domain clocks. These tables
are available rates, not proof of a required per-mode frequency. K's `icc_res_sm8750` fixes
`hf_mnoc` at 471,860 kB/s average / 925,857 kB/s peak, based on a smaller 30 fps reference mode.
Native [CameraHw.h][hw] votes MM1 and relies on existing display/GPU votes for other memory
paths (`kCamMxcNeed`, `kCamMmcxNeed`, `kCamMmcxFloor` and adjacent rationale).
The 62 MiB mode's nominal 627 MB/s exceeds that reference average before any CPU/GPU work.
Changing only the buffer size does not qualify the NoC vote, receiver clocks, burst behavior
or guard timing. No exact full-mode native clock/bandwidth requirement is established by this audit.

## Allocation, SMMU, staging and XNU budget

The allocation chain is [PianoCamera.cpp][driver]: `PianoCamera::run` → `Ops::reserveDma` →
`Ops::dmaAlloc`. At level 5 it reserves **16 MiB plus a 16 KiB table descriptor allocation**
on the early camera thread, before desktop fragmentation. `IOBufferMemoryDescriptor::inTaskWithPhysicalMask`
requests one physically contiguous, cacheable, 2 MiB-aligned frame range with a 40-bit physical
address limit. `prepare`, segment length, alignment and end address are checked. Two 4 KiB
SMMU table pages occupy the separate 16 KiB allocation; these are not 16 KiB translation granules.
`DmaReserve::take` leases a bounded view of that backing and refuses any request above 16 MiB.
It does not try a larger fallback allocation after the reserve exists. Lower capture levels can
ask `dmaAlloc` for another size, but `Stream::run` still hardcodes the two daily modes.

`Stream::up` requires **translation**, even if `planRoute` can describe a boot-chain bypass.
[CameraSmmuWindow.h][window]: `buildWindow` maps contiguous physical 2 MiB blocks from IOVA
`0x20000000`, with an end limit of `0x40000000`: a usable **512 MiB** aperture. Thus the
40/62 MiB arenas need 20/31 block descriptors and fit without enlarging the aperture or tables.
Unmapped addresses after the arena terminate rather than extending into spare reserved capacity.
`Stream::up` separately checks the frame against `kWmAddressLimit` (1 TiB) and both frame/table
addresses against the SMMU-reported output address size. A fitting IOVA is not a physical allocation.
K's [board `camss` node][k-dt] and native `kCamStream` identify apps SMMU SID `0x1c00`.

The guard **inside** the mapped arena is a canary, not an unmapped red zone. `checkLiveDma`
checks it and context faults; [CameraSmmuWindow.h][window]: `kSctlrRequired`, `kSctlrMustClear`
require translation with fault reporting and termination. `streamLive` queues one address at a
time, consumes BUF_DONE, synchronizes caches, copies/samples while unarmed, then rearms. Cancellation
drains an already submitted target through `waitFrame`. `stop` disables WM and RDI, streams the
sensor off (or asserts reset after an off-table error), resets PHY/CSID, then `down` releases
translation while the NoC is still clocked. Unconfirmed WM disable/reset, guard damage, faults
or route-release failure retain the unsafe DMA lease/resources. `gHardwareUnsafe` blocks another
hardware start. Larger modes must preserve those lifetimes and `setPowerState` cancellation.

There are **three independent 16 MiB limits**, plus geometry gates:

| Boundary | Current implementation | Full-mode consequence |
|---|---|---|
| Private hardware DMA | `Ops::reserveDma`, `DmaReserve::take`: 16 MiB backing, one lease | 40/62 MiB refused before stream setup can finish |
| ABI1 retained still RAW | `kRawCap`, `gRawMem`, `Ops::frameReady`: 16 MiB, bounds checked before copy | Merely increasing private DMA would still not retain a full RAW still |
| ABI2 CPU transport | `kPianoCameraSlots=3`, `kPianoCameraSlotBytes=16 MiB`; `startLive`, `Ops::liveFrame` | 48 MiB CPU ring; larger frames dropped, never mapped as DMA targets |
| Provider admission/output | `validLiveFrame`, `PianoProviderSource` initialization, `convert:pixel:` | Only front 3264×1836/4080 stride and rear 4080×3072/5104 stride accepted; native output fixed to 816×459 and 1020×768 |

The ABI start structures contain only sensor selection, not dimensions or a capacity negotiation.
Changing slot size would require coordinated kernel/provider changes and an explicit ABI compatibility
policy. `openLiveAndReturnError:` also demands the exact three-slot mapping size. Preserve ordinary page-aligned
CPU ring allocation: `startLive` documents why requesting larger alignment accidentally forced
contiguity in this IOBMD path. Private DMA backing and user-readable CPU ring are distinct.

Current RAW backing can total about **80 MiB** while live: 16 MiB private reserve + 16 MiB ABI1
copy + 48 MiB ABI2 ring, plus tables and metadata. If all those capacities were mechanically
raised to 40 or 62 MiB, corresponding totals would be **200 or 310 MiB**, before image processing
or app buffers. These are sizing examples, not an allocation design. One sensor owns the worker
at a time (`startLive` arbitration); do not add two simultaneous hardware reserves without a need.
At 10 fps a full frame copy alone moves 399.5/627.2 MB/s of payload for front/rear, with both a
CPU read and write in addition to DMA and subsequent ISP reads.

K uses a different memory path: [camss-video.c][k-video]: `msm_video_register` selects
`vb2_dma_sg_memops`; `video_buf_init` consumes the DMA-mapped scatterlist and derives contiguous
NV12 chroma offsets where applicable. [camss.c][k-camss]: `camss_probe` sets a 32-bit DMA mask,
and the DT supplies an IOMMU. This is not evidence for a mandatory 40/62 MiB physical CMA run.
PianoCamera has no Linux CMA allocator and its current two-level window maps one physical run;
using scatter/gather pages would require a different native backing/mapping implementation.

The daily XNU window is about 13 GiB (13,310 MiB measured in the
[existing RAM notes](../extra-ram.md)). Total installed 16 GiB does not guarantee a contiguous
62 MiB run after login. The earlier late 8/16 MiB allocation failures documented in
[camera.md](camera.md) explain early reservation. Capacity, contiguity and ownership are separate checks.

RAM16G is not an existing camera allocator: [PianoExtraRam.h][extra]: `UseCamera=4`,
`Pool::load`, `Pool::take` define tags and an all-or-nothing contiguous slab option, but
PianoCamera does not call that pool. Region1's allowed runs are GPU-only. The generic region0
`cma-block` is stage 2, while the [tracked DarwinBoot patch][ram-patch] caps region0 publication
at stage 1. A `UseCamera` enum value therefore grants no available camera backing. Any later
40/62 MiB pool integration would need 20/31 adjacent owned slabs, a compatible CPU mapping,
cache policy, device reachability and lifetime proof. GPU/video extra-RAM successes do not qualify
cacheable camera access. [RAM16G ownership analysis](ram16g-owners.md) retains HOLD for the
unproven candidates; this analysis neither reclaims nor assigns any memory.

## Real-scene exposure, white balance and focus

The control route is real sensor I2C, distinct from digital brightening. Rear uses CCI0 master 1
at address `0x10`, native 400 kHz timing from the 19.2 MHz CCI source (100 kHz option); front
uses software I2C on GPIO4/5, also address `0x10` on a separate bus. K instead describes
1 MHz buses and assigns CCI 37.5 MHz; its front bus is QUP2 SE1 / `i2c9`. Native control timing
must follow its own `CameraPower.h` / [CciMaster.h][cci] setup, not reuse the DT rate blindly.
Rear MCLK1 is 24 MHz; front MCLK4 is 19.2 MHz. `Session::sensorWrite`/`sensorRead` select the
proper bus and register/value widths. `Stream::sensorTables` writes start/init/mode, controls,
readbacks, then stream-on. The live worker explicitly disables the rear pattern (`pattern=false`).

| Control | Rear S5KJN1 | Front OV32D40 |
|---|---|---|
| Exposure | `0x0202`, 16-bit coarse integration, min 8 lines | `0x3500`, 24-bit exposure, min 4 sensor VTS rows |
| Analogue gain | `0x0204`, 16-bit code 32–2048; 32 = 1×; default 192 | `0x3508`, 16-bit code 288–15872; 256 = 1×; default 7936 |
| Frame timing | `0x0340` VTS; daily max exposure 4288−22=4266 | `0x380e` raw VTS; selected modes max 2820−12=2808 |
| Full-mode exposure limit | **6400−44=6356**; native `eMax` would currently use 22 and permit 6378 if selection alone changed | **2551−12=2539**; native `eMax` correctly uses generated `mode_->c`, not output frame length 5102 |
| Pattern / orientation | `0x0600`, 16-bit, 2 = bars, 0 = scene; `0x0101`, native 0 | Pattern-off writes `0x3019=0xd2`, `0x4308=0`; K itself questions the test-pattern register |

Sources: [CameraHw.h][hw]: `kRearExposure`, `kFrontExposure`, pattern constants;
[CameraStream.h][stream]: `sensorTables`, `eMax`, `aeApply`; K sensor `*_set_ctrl` functions.
K explicitly documents OV gain as ×256, stronger evidence than the older native comment calling
that unit inferred. Rear full-mode `exposure_margin=44` is present in data and generated
`CameraTableMode::d`, but the current AE limit reads the fixed `ExposureFacts::eMargin=22`.
Any future selector must consume the mode margin. No runtime front VBLANK control is exposed;
its VTS is initialized by the mode table. Register writes are sequential, with no group-hold
transaction in `aeApply`. Atomic frame-boundary control application and full-mode latency remain
unknown; an exposure success followed by gain failure can leave partially changed hardware state.

`aeSample` samples 64 row pairs across the RAW frame, all Bayer positions, computes means above
black 64, eight histogram bins and saturation at code 1000. `aeRule` targets 160 above black by
default, accepts ±25% with under 5% saturation, bounds an EV change to 8× except its saturation
rule, and diagnoses dark/dim/saturated/no-response outcomes. `aeSplit` uses exposure first at
minimum analogue gain, then raises gain. Bounded `exposureLoop` uses at most eight samples with
two settle frames between changes; `streamLive` instead resamples every six completed hardware
frames indefinitely. Its response time consequently depends on actual frame completion rate.
These mechanisms exist and have model tests; they do not prove full-mode convergence, flicker
avoidance or calibrated exposure. The ABI reports exposure/gain and AE status but exposes no
standard CMIO manual exposure, metering-region or focus control here.

AWB is currently **software grey-world**, recomputed per frame by [CameraIsp.h][isp]: `pianoIsp`.
After black subtraction it brings R and B means toward G, limits gains to 0.25–4× and derives
separate digital gain 1–16× from the 99th-percentile luma. It applies an sRGB curve. This is not
sensor AWB feedback, and digital gain cannot recover clipped highlights or improve photon noise.
There is no temporal smoothing, illuminant model, calibrated sensor-to-sRGB CCM, lens shading,
denoise or full-resolution edge-aware demosaic in this implementation. K's sensor controls also
leave AE/AWB policy to software; its full TFE exposes red/blue/digital gains and a CCM, not an
automatic scene controller. Real-scene colour accuracy, skin tones, mixed lighting and full-mode
Bayer/remosaic interpretation need optical evidence; uniform synthetic RAW cannot settle them.
Per-unit OTP/calibration and scene images must stay outside tracked files.

Focus is **unknown**, with a specific lead rather than an absent-node assumption. K's
[camera DTS][k-dt]: `cam_wide_vcm` defines disabled `vcm@c`, address `0x0c`, compatible
`giantec,gt9769`, supplies L6B/L7B; its comment identifies stock GT9764BA and an **unverified
register protocol**. `camera_wide` has no `lens-focus` link to it. No front actuator is declared
there. Native `Session`, `sensorTables` and ABI have no VCM/focus path. Do not enable the disabled
node, send speculative lens writes or claim either camera is fixed focus from this alone.

## CPU ISP, proposed Metal path and hardware ISP alternative

Today the extension is CPU-driven:

1. `Ops::liveFrame` copies completed private RAW into a leased CPU ring slot. `readLive` waits
   for metadata, and `validLiveFrame` checks exact geometry, bounds, Bayer order and monotonic sequence.
2. `convert:pixel:` calls `pianoIsp(..., scale=2, ...)`. One Bayer 2×2 quad becomes RGB, with
   2×2 such quads averaged per output pixel: **fourfold reduction along each dimension**, not a
   full-sized interpolating demosaic. Output is front 816×459 or rear 1020×768. Float work is
   three channels/pixel, plus an RGB byte intermediate. The rear is rotated 180° while copying
   to BGRA; selfie mirroring is left to apps.
3. The RAW lease is released after CPU conversion; the immutable BGRA source is retained for
   output pacing. `tick` / `pianoOutputScaleBGRA` fit it into 640×480 with black letterboxing,
   then `pianoOutputBGRAto420v` writes both NV12 planes using their actual pitches. Range is
   video-range BT.709 with sRGB transfer metadata. Native BGRA is format index 0; `420v` is index 1.
4. Output can repeat the latest source at up to 30 fps; its 250 ms freshness limit is measured
   from converted source arrival, not SOF. Source drops, repeats, discontinuities and ISP average/max
   time are logged. More output timestamps do not mean more optical exposures.

Exact implementation: [PianoCameraExtension.m][extension]: `convert:pixel:`, `readLive`,
`acceptSource:sequence:drops:sourceNs:`, `tick`; [CameraOutput.h][output]: `pianoOutputFit`,
`pianoOutputScaleBGRA`, `pianoOutputBGRAto420v`, `PIANO_OUTPUT_MAX_SOURCE_AGE_NS`.
Native source pools borrow up to three buffers; delivery allocations have a 24-buffer threshold
because software H.264 retains about 15 inputs before producing output. Pool limits must be
budgeted alongside client retention, not multiplied away by a claim of zero-copy output.

`kCVPixelBufferMetalCompatibilityKey` permits a compatible buffer; it does not execute Metal.
The extension has no compute pipeline or command-buffer demosaic. The still verifier's
[PianoCameraCapture.m][capture]: `FrameSink` uses a **software CIContext** to save PNGs.
[coreimage-compat.m][ci-compat]: `main` selects legacy CIKL metadata through
`CI_USE_MTL_DAG_FOR_CIKL_SRC=0` for app effects, preserving/restoring prior typed values.
That workaround concerns Core Image effect compilation/reflection; it is not camera ISP acceleration.
The existing [GPU userspace](gpu-userspace.md) and [installed Core Image results](../results-2026-11-26.md#core-image-and-metal)
establish a useful platform starting point, not full-resolution camera compute performance.

A future Metal implementation should first replay synthetic and retained RAW offline against
the CPU unpack/black/WB reference. Use byte-addressed RAW10 reads respecting padded rows and
10-bit low-bit packing, explicit Bayer order and edge handling, a real interpolating demosaic
when retaining sensor resolution, and separate statistics/reduction passes for bounded AE/AWB
metadata. Keep exposure I2C in the existing serialized worker. Apply validated colour correction
and transfer/range conversion before writing BGRA or separate Y/UV surfaces. The chosen NV12
matrix/range and attachments must agree; two-plane chroma must be complete before publication.
These are proposed requirements, not present functions or a promised frame rate.

The critical GPU ownership rule is that `kPianoCameraLiveRelease` must occur only after GPU reads
of a mapped slot finish, or after a completed copy into GPU-owned staging. Otherwise the kernel
can overwrite leased RAW while the GPU still reads it. Never treat the private camera DMA range
as a ready IOSurface/Metal buffer. The camera and GPU use separate translations; RAW import,
CPU/GPU cache transitions, sandbox entitlement, completion errors and stop cancellation all need
explicit verification. Keep retained output immutable until clients release it. Measure end-to-end
latency and actual unique source frames; require bounded work within 66.7 ms for a 15 fps target,
or 100 ms for nominal full-mode 10 fps, with a defined fallback when that budget is missed.

Full sensor capture also does not automatically mean full-sized RGB output. Keeping today's
`scale=2` on the large modes would yield front **1632×1224** and rear **2040×1536**. At actual
sensor dimensions an unpadded BGRA frame would be about **121.9/191.25 MiB** front/rear and NV12
about **45.7/71.7 MiB**. Multiplying those by today's client retention allowance would dominate
memory. Start with bounded full-RAW still capture and deliberately reduced preview, then qualify
full-sized photo output separately. `pianoIsp` at even `scale=1` halves dimensions and cannot
provide full-sized demosaiced output.

### The pinned hardware alternative is a full TFE path

K [camss-csid-980.c][k-csid]: `__csid_configure_ipp_stream` configures full CSID IPP port 3,
decodes the incoming Bayer code and selects CFA for TFE hardware context 0. K
[camss-vfe.c][k-vfe]: `vfe_line_has_isp` restricts ISP to `VFE_LINE_PIX` on **non-lite** hardware.
[camss-vfe-gen4.c][k-vfe4]: `vfe_pix_980_defaults`, `vfe_pix_start`, `vfe_isp_update`,
`vfe_pix_glut_load`, `vfe_pix_scaler_config`, `vfe_pix_crc_config` describe:

sensor → full CSID IPP/CAMIF → full TFE Bayer domain → black-offset/white-balance →
BDS2/demosaic → colour stages/gamma → RGB-to-YCbCr → MN downscaler/crop/round/clamp →
FD Y/C write masters **7 and 8**, in line mode → NV12.

This is materially different from lite RDI0/WM0 frame mode. K exposes red/blue balance,
digital gain, saturation, contrast and CCM through `vfe_ctrl_init`; userspace supplies AE/AWB
policy. Its comments describe partially reverse-engineered module layouts and test-pattern checks,
not a complete sensor calibration. The CCM is folded into the post-gamma transform, an approximation
explicitly acknowledged by `vfe_pix_cst_config`. Its default YCbCr output is **full-range BT.601**,
which cannot simply be labelled as the daily **video-range BT.709 `420v`**.

Porting this option would need new allowlisted full-CSID/TFE mappings, full TFE GDSC and main/Bayer
clocks, IPP updates/completions, two-plane DMA geometry, fault/stop fencing and colour/range handling.
PianoCamera implements none of those full PIX functions. The reference's demosaic setup explicitly
mentions BDS2 off below 7008 input pixels; rear 8160 crosses that width, with no complete large-width
qualification established here. Even front 6528 being below that threshold proves no optical quality
or throughput. A hardware ISP experiment could reduce software work, but enabling it cannot be
treated as an already qualified switch, nor as requiring an unverified firmware blob merely because
other vendor camera stacks use firmware. This K path uses direct register programming.

## Stock apps, default selection and capture verification

[PianoCameraExtension.m][extension]: `PianoProviderSource` publishes Front then Rear; pattern
camera publication is opt-in. Registration/discovery is separate from app preference and from
successful capture. [camera-default.m][defaults]: `targetDomains`, `applyTargets`, `checkTargets`
target Photo Booth and QuickTime. Automatic `--apply` preserves real-camera choices, changes
only recognized empty/synthetic histories and defers unreadable layouts. A receipt can make
automatic apply report already-applied; that is not evidence the front is currently selected.

`--check` inspects preferences without starting a capture. `front-not-selected` specifically
means a valid nonempty AVFoundation history whose first entry is another camera, with no overriding
QuickTime choice. QuickTime's
`MGDeviceRecordingDocumentViewControllerVideoDeviceSelectionUniqueID` takes precedence and yields
`front-ready-app-choice` or `other-camera-app-choice`. Missing/empty history is considered ready
by Front-first discovery order; malformed history/choice produces unknown results. These diagnostics
are not sensor/AE errors. Deliberate `--set-front` calls the AVFoundation setter with readback,
but does not clear QuickTime's separate saved choice. Future qualification must inspect both and
verify the selected camera in the app. No such preference mutation was done here.

For bounded RAW still inspection, [piano-camera-grab.c][grab]: `grab` calls ABI1 capture,
validates `PianoCameraFrameInfo`, maps memory type 0 read-only and can save `.raw10` plus a CPU-ISP
PPM. It inherits the selected modes and 16 MiB retained-copy ceiling; `--raw` does not unlock a
larger sensor mode. Any future evidence should pair the private RAW with its stride/Bayer/controls
metadata rather than guessing layout from the filename.

[PianoCameraCapture.m][capture] has separate paths:

- `inventory` enumerates devices/formats without acquisition. `capture` chooses a native BGRA
  format at 15 fps, checks dimensions, monotonic PTS, varying/nonblack samples and saves a PNG
  through `FrameSink`; this is an app-frame still, not a full-resolution RAW photo API.
- `movie` selects 640×480 `420v` at 30 fps and H.264 through `AVCaptureMovieFileOutput`.
  `movieFile` checks the saved track, duration, one nonempty edit segment, media coverage and
  compressed sample count. This reproduces the QuickTime High capture graph; it does not
  itself prove the QuickTime GUI saved or replayed the file.
- The [stock-app check](../tools/cam-apps-check.md) and
  [cam-apps-check implementation][app-check] add actual QuickTime recording/save/reopen and
  playback-clock observation; `movie.m:movieStats` decodes frames and `grade.py:movie` /
  `playback` grade nonblack video and playback. FaceTime self-view is a separate branch.
  Photo Booth live effects evidence does not verify a saved Photo Booth photo. Full-resolution
  photo file dimensions, decode and image quality remain unqualified and need a separate save test.

Do not infer capture quality from camera discovery, a front-ready preference, repeating output
timestamps, a file's existence or the absence of a crash. Keep app permission/default evidence,
unique source progress, nonblack image evidence, saved-file decode and clean DMA teardown together.

## Path & Check Matrix

`ours-OK` means the named, narrowly scoped mechanism exists in this baseline and its cited evidence
supports that scope. It is not a blanket hardware pass. `ours-missing` means a required native
implementation/control is absent or actively rejects the requested mode. `unknown` means static
sources cannot establish the hardware/optical result. All rows refer to full-resolution extension
of the existing daily path unless explicitly scoped to current behavior.

| Path / check | State | Exact evidence and remaining boundary |
|---|---|---|
| Pinned register provenance, all five modes | ours-OK | `data/camera/MANIFEST`; `tooling/camera/sensor-tables.py:SENSORS`, `PHYS`; fresh extraction matches K |
| Daily front/rear mode choice | ours-OK | `package/PianoCamera/CameraStream.h:Stream::run`; `CameraHw.h:kFrontL3Width`, `kRearL3Width` |
| Front 3264×2448 selection | ours-missing | `Stream::run` fixes front height 1836; `PianoCameraABI.h:PianoCameraLiveStartArgs` has no mode |
| Front 6528×4896 selection | ours-missing | `data/camera/ov32d40.tbl:[mode 6528x4896]` exists; same selector/ABI restriction |
| Rear 8160×6144 selection | ours-missing | `data/camera/s5kjn1.tbl:[mode 8160x6144]` exists; `Stream::run` fixes rear width 4080 |
| All five guarded RAW layouts and arithmetic bounds | ours-OK | `CameraStream.h:frameLayout`; `tests/camera_host.cpp:testModeLayouts` checks payload, padding, guard, aperture and synthetic ISP |
| Rear/front power, control bus and stream tables | ours-OK | `CameraPower.h:Session`; `CciMaster.h:CciMaster`; `CameraStream.h:Stream::sensorTables`; this scope is the existing modes |
| Four-lane RAW10 PHY/CSID/WM setup | ours-OK | `CameraHw.h:kSensors`, `csidRxCfg0`, `kMipiRaw10`; `Stream::up`; receiver is lite RDI |
| Full-mode PHY, CSID, WM timing/error-free frames | unknown | `Stream::waitFrame`, `traceLiveFrame`; K `camss.c:csid_res_8750`; no full-mode tablet evidence |
| Full-mode clocks and NoC sufficiency | unknown | `CameraHw.h:kL3Rcgs`, MM1 rationale; K `camss.c:icc_res_sm8750`; rear 10 fps payload exceeds reference average vote |
| 40/62 MiB SMMU descriptor geometry | ours-OK | `CameraSmmuWindow.h:iova::buildWindow`, `translate`; `testModeLayouts`; 20/31 blocks within 512 MiB |
| PA/OAS/address width validation | ours-OK | `CameraStream.h:Stream::up`; `CameraHw.h:kWmAddressLimit`; `CameraSmmuWindow.h:kPaBits`, `fitsPhysical` |
| Early 40/62 MiB physical DMA reservation | ours-missing | `PianoCamera.cpp:Ops::reserveDma` uses `kRawCap=16 MiB`; `CameraStream.h:DmaReserve::take` refuses oversize |
| Actual large contiguous allocation under XNU pressure | unknown | `PianoCamera.cpp:Ops::dmaAlloc`; old late-allocation failures in `docs/drivers/camera.md`; no new allocation trial |
| ABI1 full-RAW retained still | ours-missing | `PianoCamera.cpp:Ops::frameReady`, `kRawCap`, `gRawMem` cap retained bytes at 16 MiB |
| ABI2 larger RAW ring and negotiation | ours-missing | `PianoCameraABI.h:kPianoCameraSlotBytes`, `PianoCameraLiveStartArgs`; `PianoCamera.cpp:Ops::liveFrame`, `startLive` |
| Full-mode provider geometry/formats/pools | ours-missing | `PianoCameraExtension.m:validLiveFrame`, `PianoProviderSource`, `convert:pixel:` require fixed current dimensions |
| CPU ring is separate from hardware DMA | ours-OK | `PianoCamera.cpp:PianoCameraUserClient::startLive`, `Ops::liveFrame`; copy transport, bounded slot metadata |
| Current single-target/cache/guard/stop lease discipline | ours-OK | `CameraStream.h:streamLive`, `waitFrame`, `checkLiveDma`, `stop`, `down`; `PianoCamera.cpp:setPowerState` |
| Full-mode cancellation and sleep on real hardware | unknown | Same stop functions; `camera_host.cpp:testReservedLiveDma` and cancellation model coverage do not exercise full-mode hardware |
| Camera RAM16G integration | ours-missing | `PianoCamera.cpp:Ops::dmaAlloc` uses XNU; `package/include/PianoExtraRam.h:UseCamera` is only a tag here |
| Extra-RAM ownership/cacheable camera access | unknown | `PianoExtraRam.h:kAllowed`, `Pool::load`; tracked DarwinBoot stage cap; `docs/drivers/ram16g-owners.md` HOLD |
| Sensor exposure/gain writes and startup readback | ours-OK | `CameraStream.h:sensorTables`, `aeApply`; `CameraHw.h:kRearExposure`, `kFrontExposure`; K sensor `*_set_ctrl` |
| Rear full-mode exposure margin | ours-missing | `CameraStream.h:eMax` uses fixed `kRearExposure.eMargin=22`; generated `CameraTableMode.d` carries required 44 |
| Front raw-VTS/output-frame-length separation | ours-OK | `sensor-tables.py:cmd_header` keeps `sensor_frame_length` in `CameraTableMode.c`; `Stream::eMax` uses `c−12`; K `ov32d40_vts_ratio` |
| Real-scene continuous AE mechanism | ours-OK | `CameraStream.h:aeSample`, `aeRule`, `aeSplit`, `aeApply`, `streamLive`; CPU RAW statistics and serialized I2C |
| Full-mode AE convergence, latency and flicker | unknown | `CameraStream.h:streamLive` samples every six completions; `aeApply` has sequential writes and no frame-atomic hold |
| Current software AWB/black/digital/sRGB | ours-OK | `CameraIsp.h:pianoIsp`, `pianoSrgb8`; grey-world and p99 gain, synthetic coverage in `camera_host.cpp:testIsp` |
| Temporal/calibrated AWB, CCM, shading and denoise | ours-missing | `CameraIsp.h:pianoIsp` has no such stages or calibration input |
| Full-mode CFA/remosaic and optical colour quality | unknown | K sensor mode arrays and `CameraHw.h` Bayer facts are insufficient to prove optical full-mode reconstruction |
| VCM register protocol, front lens type and focus quality | unknown | K `sm8750-xiaomi-piano-camera.dtsi:cam_wide_vcm` is disabled with unverified protocol; no front lens declaration |
| Native VCM/AF or CMIO manual IQ controls | ours-missing | `CameraPower.h:Session`, `PianoCameraABI.h`, `PianoCameraExtension.m:availableProperties`; no focus/control ABI |
| CPU RAW10 → reduced BGRA → video-range NV12 | ours-OK | `CameraIsp.h:pianoRaw10`, `pianoIsp`; `PianoCameraExtension.m:convert:pixel:`, `tick`; `CameraOutput.h:pianoOutputBGRAto420v` |
| Native full-sized interpolating demosaic | ours-missing | `CameraIsp.h:pianoIsp` collapses Bayer quads even at scale 1 |
| Extension Metal RAW10 ISP and completion fencing | ours-missing | `PianoCameraExtension.m:convert:pixel:`, `readLive` are synchronous CPU; Metal buffer attribute alone does not supply compute |
| GPU full-mode throughput and bounded memory | unknown | `CameraOutput.h:kPianoDeliveryBuffers`; no camera compute benchmark or full-mode retention trial |
| Native full-CSID/TFE hardware ISP | ours-missing | `CameraStream.h:Stream::up` configures lite RDI only; K `camss-vfe-gen4.c:vfe_pix_start` and `camss-csid-980.c:__csid_configure_ipp_stream` are reference alternatives |
| Hardware 8160-wide demosaic and output colour contract | unknown | K `camss-vfe-gen4.c:vfe_pix_980_defaults`, `vfe_pix_cst_config`; width note at 7008, full-range BT.601 differs from daily `420v` |
| Core Image legacy CIKL selection/check/restore | ours-OK | `tooling/camera/extension/coreimage-compat.m:main`; effect compatibility, not Bayer conversion |
| Daily output cadence, stale limit and buffer bounds | ours-OK | `CameraOutput.h:pianoOutputDurationValid`, `pianoOutputSourceFresh`; provider `readLive`, `tick`; local-buffer provider tests |
| Front-first discovery and read-only default diagnosis | ours-OK | `PianoCameraExtension.m:PianoProviderSource`; `camera-default.m:checkTargets`, `appChoiceKey`, `validHistory` |
| Guaranteed front selection in current stock-app state | unknown | `camera-default.m:applyTargets` preserves real choices; QuickTime separate saved choice overrides history; no live preferences read |
| Native app-frame PNG / AVFoundation H.264 verification | ours-OK | `PianoCameraCapture.m:FrameSink`, `capture`, `movie`, `movieFile`; daily evidence exists, full resolution is outside these formats |
| Current bounded RAW/PPM inspection tool | ours-OK | `tooling/native/piano-camera-grab/piano-camera-grab.c:grab`; ABI1 metadata/map bounds, optional RAW output and CPU ISP; still limited to current modes |
| Full-resolution saved photo/movie and actual GUI replay | unknown | `PianoCameraCapture.m:movieFile`; `tooling/native/piano-cam-apps-check/movie.m:movieStats`, `grade.py:movie`, `playback`; no full-mode files or new stock-app trial |

## Staged read-only-first qualification plan

These are future gates, not instructions executed during this task. Stop at the first unmet
prerequisite and keep the installed daily profile. Each later hardware experiment needs its
own reviewed implementation and explicit authorization; none is authorized by this analysis.

1. **Host/source inventory first.** Freeze native/K revisions; retain table hashes/counts,
   mode geometry, AE unit/margin review and the matrix above. Review existing sanitized reports
   before requesting any new device evidence. Resolve mode negotiation, memory budgets and the
   rear margin bug on a separate implementation branch. Do not reinterpret a host model as a
   physical capture. No new image is needed for this stage.
2. **Offline IQ and lifetime tests.** Replay synthetic RAW with padded rows, low-bit patterns,
   edge/colour targets and malformed descriptors. Compare any proposed GPU unpack/black/WB/range
   operations to the CPU oracle, then evaluate demosaic quality separately. Test delayed GPU
   completion, slot reuse, cancellation, allocation failures and output retention. Audit shader
   compilation/reflection and the actual extension sandbox. Use retained private scene captures
   only if already available and authorized; publish metrics, not private imagery/calibration.
3. **Future read-only device baseline.** In a separately authorized session, inventory existing
   IOKit properties and camera formats, `camera-default --check`, Core Image policy checks and
   preexisting logs without opening capture. Check current resolution, buffer policy and app
   choices. Any CAMCC/SMMU survey must respect clock/power read gates: do not force a power-up
   merely to call a register read-only. Missing evidence stays unknown.
4. **Future allocation-only RAM test.** After code/host review, first prove early 40/62 MiB
   reservation and the entire CPU/output budget on a RAM image, before sensor/receiver stores.
   Verify actual alignment/contiguity/OAS, ownership and release. Try one sensor at a time, with
   an untouched daily fallback. Prefer existing XNU-owned backing; an extra-RAM route has the
   separate ownership/cache gates above. Failure is a refusal, not an invitation to enlarge
   a memory window or reclaim CMA blindly.
5. **Future bounded RAW acquisition.** Start with the already supported modes as controls,
   then the 10 MiB front mode, then 40 MiB front and 62 MiB rear individually. Inspect complete
   dimensions/stride, CSI errors, SOF/EOF/BUF_DONE timing, canaries, translation faults and clean
   stop/release. Rear known bars can separate transport from scene IQ; front test-pattern support
   remains unverified. Test scene exposure limits/readback, focus only after protocol evidence,
   bounded AE response and ordinary cancellation/sleep. No full-mode continuous app stream until
   every bounded capture retires safely.
6. **Future IQ/performance and app files.** Compare fixed-light charts and light steps, clipping,
   colour/neutral accuracy, temporal pumping, noise and focus at specified distances. First
   qualify reduced preview from full RAW, then any full-sized demosaic and saved photo dimensions.
   Measure CPU/GPU time, unique optical frame rate, copies, pool high-water marks and contention
   with display/video. Qualify Metal and full TFE as separate alternatives, with a CPU fallback
   chosen only when it fits the tested budget. Finally verify Photo Booth selection/preview/photo
   save, QuickTime High recording/save/reopen/playback, both cameras and clean stop. Distinguish
   the existing 30 fps compatibility repetition from nominal 10 fps full-mode acquisition.

Host validation performed for this analysis: pinned source/table re-extraction; existing camera
host suite (including `testModeLayouts`); CMIO provider suite with local CoreVideo buffers; and
default-selection host tests. Local-buffer provider success does not test IOSurface services,
real Metal execution, file recording or the tablet. No new tests or implementation were added.

## Fix-image gate

Status: HOLD

Analysis-only. No fix image, tablet action, install, profile change or full-resolution enablement.
Daily 640×480 `420v` at 15 fps stays untouched.

Release requires reviewed mode/ABI/provider support, 40/62 MiB owned DMA backing and a bounded
whole-pipeline budget; corrected mode-specific AE limits; verified IQ/demosaic and focus disposition;
qualified clocks/NoC/PHY/DMA/cache/stop lifetimes; and saved-photo/movie plus stock-app evidence
from separately authorized RAM qualification. A table match, fitting SMMU aperture, existing GPU,
Linux ISP implementation or prior daily movie result cannot independently release this gate.

[kernel]: https://github.com/yzddmr6/linux-sm8750-piano/tree/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01
[k-rear]: https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/media/i2c/s5kjn1.c
[k-front]: https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/media/i2c/ov32d40.c
[k-dt]: https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-camera.dtsi
[k-camss]: https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/media/platform/qcom/camss/camss.c
[k-phy]: https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/media/platform/qcom/camss/camss-csiphy-3ph-1-0.c
[k-csid]: https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/media/platform/qcom/camss/camss-csid-980.c
[k-vfe4]: https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/media/platform/qcom/camss/camss-vfe-gen4.c
[k-vfe]: https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/media/platform/qcom/camss/camss-vfe.c
[k-video]: https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/media/platform/qcom/camss/camss-video.c
[tables]: ../../data/camera/README.md
[manifest]: ../../data/camera/MANIFEST
[rear-table]: ../../data/camera/s5kjn1.tbl
[front-table]: ../../data/camera/ov32d40.tbl
[phy-table]: ../../data/camera/csiphy-v2.4.tbl
[table-tool]: ../../tooling/camera/sensor-tables.py
[hw]: ../../package/PianoCamera/CameraHw.h
[power]: ../../package/PianoCamera/CameraPower.h
[cci]: ../../package/PianoCamera/CciMaster.h
[stream]: ../../package/PianoCamera/CameraStream.h
[driver]: ../../package/PianoCamera/PianoCamera.cpp
[window]: ../../package/PianoCamera/CameraSmmuWindow.h
[route]: ../../package/PianoCamera/CameraSmmu.h
[isp]: ../../package/PianoCamera/CameraIsp.h
[abi]: ../../package/PianoCamera/PianoCameraABI.h
[host]: ../../package/PianoCamera/tests/camera_host.cpp
[extension]: ../../tooling/camera/extension/PianoCameraExtension.m
[output]: ../../tooling/camera/extension/CameraOutput.h
[capture]: ../../tooling/camera/extension/PianoCameraCapture.m
[grab]: ../../tooling/native/piano-camera-grab/piano-camera-grab.c
[defaults]: ../../tooling/camera/extension/camera-default.m
[ci-compat]: ../../tooling/camera/extension/coreimage-compat.m
[app-check]: ../../tooling/native/piano-cam-apps-check
[extra]: ../../package/include/PianoExtraRam.h
[ram-patch]: ../../firmware/patches/darwinboot/0002-DarwinBoot-region1-extra-ram-reserve-and-publish.patch
