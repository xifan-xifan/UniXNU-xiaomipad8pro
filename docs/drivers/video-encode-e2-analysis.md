# Iris E1 720p/1080p scaling and VideoToolbox E2 contract analysis

Analysis dated 2026-11-27, against repository base `e606cd41b` and Linux Iris
**K = `yzddmr6/linux-sm8750-piano b5100d5f2e0bb80e1191d521a48596674608eb3d`**.
The reference checkout reports K and has no tracked Iris diff. Apple's decoded
arm64e cache reports **macOS 27.3**. This change contains documentation only:
no driver edits, image construction, deployment, or new tablet observation.

**Decision: existing E1 720p and 1080p are statically ready for separate,
bounded, zero-driver-code-change RAM observations at 30 fps on the 256 MiB XNU
arena. There is no outstanding VPSS allocation/queue gate in these sources.
E2 `VTCompressionSession` encoding remains strictly HOLD.** Sixty fps has the
same buffer requirements but is not selectable by the existing RAM boot path;
neither 30 nor 60 fps at these larger sizes is hardware-qualified here.

The owner reports `v131y12u-e1fix1`: **105/0, rc=0**, E1 640×480 AVC **10/10**,
PSNR-Y **min 51.2 / average 51.6 dB**, **314 ms**, **22,439 bytes**, zero
errors/quarantine, followed by **13/13 AVC decode** with `video-v6 --qualify`
using the resident core on the same boot. This is supplied hardware evidence,
not a run reproduced by this analysis. It establishes the 640×480 session and
resident-core reuse; it does not establish larger-size performance, 60 fps,
IOSurface encode input, or VT compressed output.

## Evidence and status vocabulary

- **`ours-OK`**: the inspected implementation satisfies the stated static
  requirement. This does not imply hardware qualification beyond the supplied
  640×480 result.
- **`ours-missing`**: an identified requirement has no implementation in the
  inspected path. An intentional empty encoder inventory is correct containment,
  but an advertised E2 encoder is still missing.
- **`unknown`**: evidence does not establish the fact, or a remaining binary
  layout/behavior needs a host experiment. Unknowns are not permission to infer
  an ABI from decoder names or publish encoder support.

Native sources: [geometry, sizing and properties](../../package/PianoVideo/VideoEncode.h),
[HFI transaction](../../package/PianoVideo/VideoEncodeSession.h),
[guarded layout](../../package/PianoVideo/VideoBuffers.h),
[RAM worker](../../package/PianoVideo/PianoVideo.cpp),
[VT host](../../package/PianoVideo/vt/PianoVideoVTHost.cpp),
[VT user client](../../package/PianoVideo/vt/PianoVideoVTParavirt.cpp),
[wire helpers](../../package/PianoVideo/vt/PianoVideoVTWire.h) and
[bounded decoder contract](../../package/PianoVideo/vt/PianoVideoVTContract.h).
The actual VT host path includes the `vt/` directory.

Apple evidence below is **direct symbol/disassembly inspection with `ipsw`**,
not an inference from symbol spelling alone. Addresses are unslid addresses in
this 27.3 cache, not stable API offsets. Inspected images:

| Image | Evidence / status |
| --- | --- |
| `VideoToolbox.framework/Versions/A/VideoToolbox` | Public compression entry points, encoder registry, guest encoder, Apple host peer, serializers and teardown were inspected. **`ours-missing`**: native encoder peer. |
| `VideoToolboxParavirtualizationSupport.framework/Versions/A/VideoToolboxParavirtualizationSupport` | Service discovery and selectors 0–4 were inspected. **`ours-OK`**: reusable transport shapes, within current bounds. |
| `MediaToolbox.framework/Versions/A/MediaToolbox` | Imports `VTCompressionSessionCreate`, `CreateWithOptions`, `EncodeFrame`, `CompleteFrames`, `Invalidate` and `VTParavirtualizationIsRunningInGuest`. **`ours-missing`**: an E2 backend for these callers. No separate MediaToolbox encoder selector ABI is established. |
| A separately named `AppleParavirtVideoEncoder` image | No image of that name appears in the cache map. The operative code is `ParavirtualizedVideoEncoder_*` inside VideoToolbox. **`unknown`**: any separately shipped component outside this cache; it is not a prerequisite invented for this design. |

Reproduction uses `ipsw dyld info "$DSC"`, `dyld macho "$DSC" VideoToolbox
--symbols/--strings`, equivalent reads of the other two images, and `dyld disass
"$DSC" --symbol-image VideoToolbox --symbol _SYMBOL --no-color`. Resolve constant
CFStrings with `dyld dump` through their pointer and CFString character pointer;
do not mistake encoded pointer fixups for integer field values. `$DSC` denotes
the owner-supplied decoded cache; no host-specific location is published here.
No Apple payloads or disassembly dumps are added to the repository.

## E1 geometry and sizing inputs

SM8750 uses VPU35 hardware with **four VPP pipes**, but selects
**`iris_vpu33_buf_size`**, including its distinct line-buffer calculation.
There is no `iris_vpu35_buf_size` entry point at K to substitute. The VPU33
dispatcher uses shared encoder BIN/COMV/NON_COMV/ARP/VPSS/SCRATCH_2 helpers and
`iris_vpu33_enc_line_size` for LINE. **`ours-OK`**: native constants use that
combination, not the older generic VPU line sizing.
[Platform selection](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_vpu3x.c#L47-L50),
[SM8750 descriptor](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_vpu3x.c#L198-L228),
[sizing dispatcher](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2445-L2470).

The audited configuration is H.264 High, 8-bit NV12, stage 2, VBR-CFR,
rotation/blur off, zero B frames, LTR and hierarchical layers off. Linux's
reconstruction helper therefore returns **one reference**. Dimensions passed
to internal sizing are the **coded bitstream dimensions**; the scaling predicate
compares Linux's **padded** source and destination formats.
[Format setup](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_venc.c#L223-L323),
[coded dimensions and BIN](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1126-L1244),
[reference count](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1246-L1289).

| Input / check | `res=1`, 1280×720 | `res=2`, 1920×1080 | Status |
| --- | ---: | ---: | --- |
| RAW stride × Y rows | 1280×736 | 1920×1088 | `ours-OK` |
| UV rows / byte offset | 368 / 942,080 | 544 / 2,088,960 | `ours-OK` |
| HFI RAW active resolution | 1280×720 | 1920×1080 | `ours-OK`: active size is separate from stride/scanlines |
| Coded AVC size | 1280×720 | 1920×1088 | `ours-OK` |
| Coded crop, right / bottom | 0 / 0 | 0 / 8 | `ours-OK` |
| Linux padded RAW vs coded equality | Different heights | Equal dimensions | `ours-OK`: VPSS enabled only for 720p |
| RAW allocation per slot | 1,413,120 | 3,133,440 | `ours-OK` |
| BITSTREAM allocation per slot | 708,608 | 1,568,768 | `ours-OK` |

External RAW is 4 KiB-rounded Y plus UV, with 128-byte stride, 32 Y-row and
16 UV-row alignment. External BITSTREAM uses 32-aligned dimensions and the
VBR branch in `iris_bitstream_buffer_size`: both padded areas exceed
1280×720 and are below 4096×2176, so the initial three-bytes-per-pixel amount
is quartered, then 4 KiB-rounded. It is **not** BIN's size formula.
[RAW sizing](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L66-L83),
[BITSTREAM sizing](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L418-L444).

### Every internal allocation, independently recalculated

For this audit, the pin's original pure sizing helpers and their numeric
definitions were compiled in a temporary host harness outside the repository.
They were evaluated at coded 1280×720 and 1920×1088 with the inputs above.
A separate temporary C++ program included the unmodified native header and
printed `Geometry` and `buildLayout`. Thus equality below was checked against
the Linux calculations, not just copied from native tests. The original Linux
code is read as a specification; no GPL implementation is added to this repo.

Each size is a firmware-visible byte capacity **before** native 16 KiB arena
rounding and guards. Each cell is **Linux = ours**. Repeating the calculation
at 30 and 60 fps is deliberate: none of these seven helpers depends on fps
or target bitrate under the fixed configuration.

| Buffer, HFI type / port | 720p30 | 720p60 | 1080p30 | 1080p60 | Status |
| --- | ---: | ---: | ---: | ---: | --- |
| BIN, 7 / BITSTREAM=1 | 9,563,648 | 9,563,648 | 21,661,184 | 21,661,184 | `ours-OK` |
| COMV, 10 / BITSTREAM=1 | 436,224 | 436,224 | 983,040 | 983,040 | `ours-OK` |
| NON_COMV, 11 / BITSTREAM=1 | 392,448 | 392,448 | 542,720 | 542,720 | `ours-OK` |
| LINE, 8 / BITSTREAM=1 | 834,048 | 834,048 | 984,576 | 984,576 | `ours-OK` |
| DPB (`SCRATCH_2`), 6 / BITSTREAM=1 | 5,804,032 | 5,804,032 | 12,636,160 | 12,636,160 | `ours-OK` |
| VPSS, 13 / RAW=2 | 1,449,984 | 1,449,984 | 0, omitted | 0, omitted | `ours-OK` |
| ARP, 9 / NONE=0 | 204,800 | 204,800 | 204,800 | 204,800 | `ours-OK` |

The matching 640×480 control recalculates BIN 23,787,008, COMV 147,456,
NON_COMV 313,856, LINE 700,928, DPB 1,921,024, VPSS zero and ARP 204,800.
The smaller 720p BIN relative to this control is expected: BIN's macroblock
threshold switches branches at the padded 720p boundary, not monotonically
with active pixel count.

| Allocation | Pin calculation and audit details |
| --- | --- |
| BIN | `size_bin_bitstream_enc` → `hfi_buffer_bin_enc`; stage 2 uses three bit-bin groups, 1.7 multiplier, four-pipe split, SAO overhead and 256-byte alignment. Its size estimate includes the pin's 1.25 bit-depth allowance even for this NV12 session. `NUM_MBS_720P` is based on **1280×736**. Using active 1080 instead of coded 1088 gives the wrong BIN. [K:1126–1244](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1126-L1244), [thresholds](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.h#L144-L153). **`ours-OK`**. |
| COMV | `hfi_buffer_comv_enc`: one reference plus current picture gives two collocated-MV areas, plus six collocated-RC areas. **Do not multiply the allocation by the HFI COMV count of four.** [K:1300–1333](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1300-L1333). **`ours-OK`**. |
| NON_COMV | `hfi_buffer_non_comv_enc`: slice info/commands/headers, four-pipe frame RC, bit counts/bitmap, BSE, lambda, override and intra-refresh storage. Uses H.264 16×16 LCUs. [K:1335–1371](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1335-L1371), [K:1569–1614](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1569-L1614). **`ours-OK`**. |
| LINE | Base line storage plus **`size_vpss_line_buf_vpu33`**, four pipes, eight-bit. The VPSS *line component is always part of LINE*, independently of the standalone VPSS allocation. [VPU33 line component](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1488-L1508), [base and wrapper](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1616-L1722). **`ours-OK`**. |
| DPB | `iris_vpu_enc_scratch2_size` → `hfi_buffer_scratch2_enc`, eight-bit and one reference. One large allocation contains `(references + 3) = 4` reconstruction units, plus 4,096 bytes. Units are 1,449,984 / 3,158,016 bytes here, including Y/UV and both UBWC metadata areas. This is encoder `BUF_SCRATCH_2` mapped to HFI DPB=6, **not the decoder `BUF_DPB` count rule**. [K:1993–2053](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1993-L2053). **`ours-OK`**. |
| VPSS | `is_scaling_enabled` compares padded formats. When unequal, `hfi_buffer_vpss_enc` returns one `hfi_buffer_dpb_enc` surface; otherwise zero. At 720p the surface is 1,433,600 pixel bytes plus two 8,192-byte metadata areas. At 1080p the predicate is false. [K:1724–1804](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1724-L1804), [wrapper](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2055-L2064). **`ours-OK`**. |
| ARP | Session-persistent constant `HFI_BUFFER_ARP_ENC = 204800`; allocated separately from both port tables. The earlier stage-5 omission is fixed in these sources. [constant](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.h#L144), [persistent queue](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L750-L778). **`ours-OK`**. |

### Counts, ordering and arena accounting

Linux's encoder port tables are input **VPSS**, output
**BIN/COMV/NON_COMV/LINE/SCRATCH_2**. Count is one for each admitted internal,
with zero-size VPSS skipped; external minimum is four per queue.
[Tables](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L2057-L2067),
[counts](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2567-L2627),
[external minimum](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.h#L11-L14).

| Check | Native implementation | Status |
| --- | --- | --- |
| External counts | Four RAW + four BITSTREAM allocations/slots | `ours-OK` |
| Internal counts | One each BIN/COMV/NON_COMV/LINE/DPB/ARP; one VPSS at 720p, zero at 1080p | `ours-OK` |
| 720p fifteenth entry, raised by `code-review-batch8` | Already present. `buildLayout` adds VPSS conditionally. It is **15 total allocations**, not zero-based slot 15: VPSS itself is slot 9, DPB is slot 14. `kSlots=141` is ample. | `ours-OK` |
| Port and buffer IDs | Encoder RAW and VPSS on port 2; output internals on 1; ARP on 0; SCRATCH_2 → HFI DPB=6 | `ours-OK` |
| Input start | OPEN/CODEC → input config/capabilities → ARP → VPSS if nonzero → START RAW | `ours-OK` |
| Output start | Output config/capabilities → BIN/COMV/NON_COMV/LINE/DPB → START BITSTREAM → queue four BITSTREAM slots | `ours-OK` |
| COMV property | Four (`PropComvCount=0x03000193`) before COMV queueing, matching Linux output minimum absent a firmware override | `ours-OK` for this fixed session; `unknown` for future firmware-requested count changes |
| Cleanup | DRAIN RAW, require ACK/LAST and all frame returns, STOP BITSTREAM, STOP RAW, CLOSE; ARP backing retained until successful CLOSE | `ours-OK` |
| Device failure | Stop HFI on watchdog/system/SMMU failure, keep DMA and quarantine; exclusive core lease precedes E1 | `ours-OK` |

The native sequence is in `EncodeSelfTest::internals/transaction/shutdown`.
Linux equivalents are [stream-on](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_venc.c#L481-L549),
[port mapping](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L112-L151),
[buffer mapping / COMV count / submission](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1256-L1345).

`Layout::add` places each allocation at a 16 KiB boundary, rounds its capacity
up to 16 KiB and appends a **16 KiB invalid guard**. The E1 layout sets
`hevc=false`, so its limit and backing are exactly **268,435,456 bytes**.
The total below includes every external/internal buffer, rounding and guard;
it does not sum unrounded capacities and call that an arena requirement.

| Layout | RAW ×4 | BITSTREAM ×4 | Other allocations | Guarded total | Spare within 256 MiB | Status |
| --- | ---: | ---: | --- | ---: | ---: | --- |
| 640×480 control | 462,848 each | 921,600 each | ARP + five output internals, 14 entries total | 32,980,992 | 235,454,464 | `ours-OK` |
| 1280×720, either fps | 1,413,120 each | 708,608 each | ARP + VPSS + five output internals, 15 entries total | **27,557,888** | **240,877,568** | `ours-OK` |
| 1920×1080, either fps | 3,133,440 each | 1,568,768 each | ARP + five output internals, 14 entries total | **56,180,736** | **212,254,720** | `ours-OK` |

**`ours-OK`**: no region0/extra-RAM capacity is needed. Explicitly request
`piano-extra-ram=0 piano-video-extra-ram=0` and verify XNU backing in the run
receipt. The arena is shared exclusively with decode, not allocated in addition
to a concurrent decoder arena. Firmware/queue regions and page tables are
separate from this buffer-layout total. Runtime success of the XNU reservation
on a particular boot remains **`unknown`** until its ready/backing receipt.

### Level, bitrate, frame rate and evidence limits

| Check | Finding | Status |
| --- | --- | --- |
| H.264 level | Native `Level50=14` is the HFI enum for **level 5.0**; SPS `level_idc` is **50 decimal**. Do not send 50 as the HFI enum. Linux's selected AVC capability also defaults to 5.0. | `ours-OK` |
| Profile | HFI High=4; SPS profile identifier is 100. Apple profile strings/numbers require their own translation. | `ours-OK` for E1; `ours-missing` for E2 translation |
| Level budget | Coded macroblocks: 720p **3,600**, 1080p **8,160**; at 30/60 fps: **108,000 / 216,000** and **244,800 / 489,600 MB/s**. All fit level 5's 22,080 MB/frame and 589,824 MB/s. | `ours-OK` for these level limits; `unknown` for produced SPS/HRD compliance until larger streams are inspected |
| Bitrate | E1 defaults to **2,000,000 bit/s**; accepted range **100,000–40,000,000**. Linux selected gen2 range is 1–245,000,000, default 20,000,000; matching its default is unnecessary for a client-selected bitrate. E1's ceiling is below even level 5's 135,000,000 base MaxBR. | `ours-OK` for range/representation; `unknown` for achieved rate and PSNR at larger sizes |
| Buffer dependence | Changing 30 → 60 fps or admitted bitrate does not alter any audited allocation under VBR/stage 2/reference assumptions. RC-off/CQ, rotation, B frames, layers, ten-bit or other geometries require a new audit. | `ours-OK` within the fixed envelope |
| Frame-rate property / timestamps | Q16 `fps << 16`; portable settings admit 1–60; frame PTS is `floor(frame × 1e9 / fps)` nanoseconds. RAW returns correlate by slot, BITSTREAM returns by exact scheduled PTS. | `ours-OK` |
| Existing boot control | `PianoVideo.cpp` only applies res/frames/kbps/delay. It leaves `Settings::fps=30`. There is **no `piano-video-enc-fps` boot argument**. | `ours-OK` for 30 fps; `ours-missing` for a zero-code-change 60 fps selection |
| Performance | Frames are submitted as slots free, without real-time pacing. 314 ms for the supplied ten-frame 640×480 run includes session work. | `unknown` for sustained 720p/1080p 30/60 fps, power and thermal behavior |
| Export capacity | Four DMA bitstream slots are distinct from the worker's **256 KiB aggregate Annex-B export**. `truncated` fails the result if that export fills. Ten frames at 2 Mb/s have a nominal 83,333-byte budget at 30 fps, but VBR/IDR overhead is not bounded by that estimate. | `ours-OK` for bounded observation and truncation detection; `unknown` for actual larger-size output length |

Linux level/bitrate capabilities are [K:1212–1240](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1212-L1240),
[K:1288–1318](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1288-L1318)
and the `BITRATE_MAX` definition at that file's line 15. Level limits are from
[ITU-T H.264 (08/2024), Annex A, Table A-1](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-H.264-202408-I%21%21PDF-E&lang=e&type=items).
This level check establishes capacity, not a requirement to lower the existing
level before a RAM observation. Actual SPS crop, profile/level and software
decodability remain observation gates.

## Apple 27.3 end-to-end encoder path

```text
client / MediaToolbox
  VTCompressionSessionCreate -> CreateWithOptions
  -> VTSelectAndCreateVideoEncoderInstanceInternal
  -> encoder registry population -> vtLoadParavirtualizedVideoEncoders
  -> encs -> registry entry -> ParavirtualizedVideoEncoder_CreateInstanceWithSpecification
  -> +enc -> esta / properties / epfr
  -> VTCompressionSessionEncodeFrame -> efrm + IOSurface
  <- efrm reply (admission status/info)
  <- Epba / Efds as needed, Eemt (compressed sample), guest replies
  -> VTCompressionSessionCompleteFrames -> ecfr
  -> VTCompressionSessionInvalidate -> einv
  -> final object release -> -enc / UUID removal

All guest messages use VideoToolboxParavirtualizationSupport:
  IOServiceMatching -> IOServiceOpen(type 0)
  -> IOConnectCallScalarMethod / IOConnectCallStructMethod
  -> AppleVideoToolboxParavirtualizationUserClient
  -> native host -> future Iris encoder worker (currently absent)
```

The diagram shows the selected PV path, not an assertion that every VT session
must choose it or perform every optional operation. Public VT dispatches through
CMBaseObject/VTVideoEncoder method tables. E2 is a new native **host peer behind
Apple's existing userspace guest**, not an already implemented userspace encoder
unlocked by setting `piano-video-enc=1`.

### Discovery and registry

| Symbol / event, unslid address | Observed contract | Status |
| --- | --- | --- |
| `VTCompressionSessionCreate`, `0x196e04bbc` | Wrapper branches to `VTCompressionSessionCreateWithOptions`. | `ours-missing`: usable native compression endpoint |
| `VTCompressionSessionCreateWithOptions`, `0x196e01048` | Selects encoder, sets up session and calls methods through its class table. | `ours-missing` |
| `VTSelectAndCreateVideoEncoderInstanceInternal`, `0x197310b20` | Filters FigRegistry by codec/specification, including EncoderID, hardware preference/requirement and PV restrictions; instantiates the selected factory. | `ours-missing`: eligible native encoder entry/factory peer |
| `vtPopulateVideoEncoderRegistry`, `0x196ffceb4`; `vtInitializeVideoEncoderRegistry`, `0x197312dc4` | Populates registry once and loads PV inventory; public `VTCopyVideoEncoderList` also populates it. | `ours-OK`: Apple provides registry machinery; `ours-missing`: native encoder registration data |
| `vtLoadParavirtualizedVideoEncoders`, `0x1973131b8` | Installs inventory UUID handler; sends **`encs` request flag 1, `,ver` UInt32=6**; reads `,enc` plist array and `,ver`; registers each record. | `ours-OK`: native returns `,enc=[]`, `,ver=6`; `ours-missing`: a working record |
| `VTParavirtualizationHostCopyEncoderListReply`, `0x19730ffa8` | Apple reference host constructs inventory records from its encoder registry. This is the source to mirror, not the decoder inventory schema. | `ours-missing` |
| Proposed `com.unixnu.pianovideo.encoder` | No such encoder is published by current native code; `inventory()` adds entries only for `decs`. The proposed identifier is ours, not an Apple hardcoded discovery string. | `ours-missing` |
| ID wrapping | Loader forms **`paravirtualized:%@`** names/IDs, preserves host ID as `VTHostEncoderID`, and selection passes `kVTVideoEncoderSpecification_HostEncoderID` into the factory. `+enc` carries original host ID in `,cid`. | `ours-missing`: stable host/guest ID mapping |

Registry records consumed by the loader include `kVTVideoEncoder_CodecTypeKey`
(CFString **`VTCodecType`**, numeric codec), `kVTVideoEncoder_Rating`
(**`VTRating`**, integer), `kVTVideoEncoder_CodecNameKey` (**`VTCodecName`**),
class implementation name and ID, and optional **`Hide`**. The loader reads
codec/rating with `FigCFDictionaryGetInt32IfPresent`; the decoder's
`VTCodecType=["avc1"]` record must **not** be copied unchanged. These encoder
record fields are **`ours-missing`**. Their complete optional capability set,
selection policy across sandbox/XPC clients, and visibility in real applications
are **`unknown`** until an encoder inventory model and stock-VT round trip exist.

### IOKit / MIG transport and outer layout

No encoder-specific scalar selector or dedicated encoder MIG subsystem was
found on this path. The framework uses the standard IOKit external-method MIG
transport and the **same selectors as decode**; encoder operations are FourCC
messages inside its shared buffer. Mach message IDs and Apple kernel-private
dispatch layouts outside the inspected userspace/support path are **`unknown`**
and must not be invented as a new E2 ABI.

| Event / support symbol | Exact observed shape | Native status |
| --- | --- | --- |
| Setup block, `0x1a7f278f4` | Matches **`AppleVideoToolboxParavirtualizationDriver`**; `IOServiceOpen` type **0**. Notification type **0**; first send-buffer request **0x4000** bytes. | `ours-OK`: service/user-client names, type and notification |
| `vtParavirtualizationGuestSupportUpdateBuffersForSize`, `0x1a7f27cec`, selector **0** | One UInt64 input: requested size; exactly three UInt64 outputs: **capacity, task send-buffer address, task IODataQueue address**. Larger sends grow by VM-page rounding. | `ours-OK` within native 4 MiB message cap |
| `VTParavirtualizationGuestSupportRegisterGuestUUID`, `0x1a7f27d90`, selector **1** | `IOConnectCallStructMethod`, **16 input UUID bytes**; no output structure. | `ours-OK` within 32 registered UUIDs per client |
| `...DeregisterGuestUUID`, `0x1a7f27db0`, selector **2** | Same 16-byte shape; removes UUID ownership. | `ours-OK` |
| `VTParavirtualizationGuestSupportSendRawMessageToHost`, `0x1a7f27dd0`, selector **3** | Apple sends **six UInt64 scalars**: message byte length and five IOSurface-ID slots, zero padded; one scalar output carrying host status. Bytes themselves are copied into the selector-0 mapping. | `ours-OK`: native accepts 1–6, snapshots bytes, stops IDs at first zero and checks UInt32 range |
| Notification receive block, `0x1a7f27b54`, selector **4** | Dequeues host messages using 0x4000 receive capacity, dispatches them, then calls selector 4 with **zero inputs/outputs** to acknowledge queue draining. | `ours-OK` transport; `ours-missing` encoder-sized/fractured reply production |
| Completion threading | An Apple synchronous *guest API* waits for a later queued reply. Selector 3 must return promptly; no worker may wait for a guest response while holding selector/client locks. | `ours-OK` principle in decode transport; `ours-missing` encoder worker/reply-clerk model |
| IOSurface IDs | Sideband handles identifying task-owned surfaces, not IOVAs, user pointers, or in-band NV12 bytes. | `ours-OK` transport representation; `ours-missing` encode ownership/validation |

| Wire structure / field | Layout | Status |
| --- | --- | --- |
| Atom header | BE UInt32 size including header, BE FourCC; first atom `vtpv`, exactly 48 bytes | `ours-OK` |
| `vtpv` body | Message FourCC at +8, flags +12, 16-byte session/guest UUID +16, BE UInt64 request/reply ID +32, eight reserved zero bytes +40 | `ours-OK` |
| Direction flags | Guest request=1, host request=2, host reply=4, guest reply=8, one-way=0 | `ours-OK` transport; `ours-missing` encoder direction/state checks |
| Parameter | `para` atom: key FourCC +8, type FourCC +12, payload +16 | `ours-OK` |
| Scalar/plist/UUID types | `=s32`, `=u32` (4 BE bytes), `=s64` (8 BE bytes), `=pls` (binary plist), `=uui` (16 bytes), `=dat` (opaque bytes) | `ours-OK` for existing helpers; encoder field schema `ours-missing` |
| Dimensions | `,vdi` via `AppendCMVideoDimensions`; two 32-bit dimensions with explicit wire conversion | `ours-missing`: encoder reader |
| Top-level time | `=tim`, 24 bytes: BE signed64 value, signed32 timescale, UInt32 flags, signed64 epoch; `AppendCMTime` at `0x197309704` explicitly reverses fields | `ours-missing`: encoder time reader/validator |
| `=fds` | Big-endian QuickTime ImageDescription, 86-byte base plus extensions; width/height at +32/+34. `AppendCMFormatDescription` calls `CMVideoFormatDescriptionCopyAsBigEndianImageDescriptionBlockBuffer`. | `ours-OK`: decoder parser; `ours-missing`: encoder writer/format-description exchange |
| `=sbf` | Nested serialized sample-buffer atoms, described below; not a C pointer or an Annex-B blob | `ours-missing` |

Header construction was checked at `VTParavirtualizationCreateMessageAndByteStream`
`0x196ff7a64`. Native helper presence establishes only the listed generic
structures; it does not establish that encoder messages are dispatched. The
existing 4 MiB send bound, 256 KiB reply queue and 16 KiB Apple dequeue bound
are different limits. Advertising E2 before handling output fragmentation and
queue backpressure would be incorrect even if input messages are small.
Complete private CMBaseObject/VTVideoEncoder vtables and object-storage layouts
are **`unknown`** as a reusable ABI; the observed method calls establish the
path, not permission to copy private object offsets into the native peer.

### Session creation, properties and per-frame messages

All messages below use the session UUID. Synchronous replies echo the message
type and reply identifier with flag 4. An error-only selector status is separate
from a valid protocol reply containing `,err` and from a frame completion.
Every encoder-specific row is **`ours-missing`** unless stated otherwise.

| Operation / guest symbol and address | Message and typed parameters / result | Status |
| --- | --- | --- |
| Create instance, `ParavirtualizedVideoEncoder_CreateInstanceWithSpecification`, `0x197316fe8` | Creates UUID, installs handler, sends **`+enc`**: `,cod` SInt32 codec, `,ver` UInt32 **6**, optional `,cid` plist host ID, `,gin` dictionary guest info. Reply reads host `,ver` and `,hin`; Apple reference host appends `,err`, `,ver=6`, `,hin`. | `ours-missing` |
| Reference host creation, `VTParavirtualizationHostEncoderSessionCreate`, `0x197327590` | Extracts UUID/codec/version/ID/info, creates queue, timestamp queue, reply clerk and pending-frame groups. Prevents recursively selecting PV as its host encoder. | `ours-missing` |
| Start, `ParavirtualizedVideoEncoder_StartSession`, `0x197091208` | **`esta`**, `,vdi` dimensions; synchronous `,err`. | `ours-missing` |
| Prepare, `...PrepareToEncodeFrames`, `0x197091f5c` | **`epfr`**, synchronous `,err`. Does not submit a frame. | `ours-missing` |
| Copy property, `...CopyProperty`, `0x197317b48` | **`ecop`**, `,key` plist; reply `,err`, `,val` plist. | `ours-missing` |
| Set property, `...SetProperty`, `0x197090d10` | **`eset`**, `,key` / `,val` plists; reply `,err`. Special handling exists for multipass storage. | `ours-missing` |
| Set properties, `...SetProperties`, `0x197317d10` | **`emul`**, `,pro` dictionary; reply `,err`. | `ours-missing` |
| Supported properties, `...CopySupportedPropertyDictionary`, `0x197091cb8` | **`esup`**; reply `,err`, `,sup` dictionary, cached on guest. | `ours-missing` |
| Serializable properties, `...CopySerializableProperties`, `0x197317f1c` | **`esav`**; reply `,err`, `,pro` dictionary. | `ours-missing` |
| Public encode, `VTCompressionSessionEncodeFrame`, `0x196e07ef0`; `vtCompressionSessionEncodeFrameCommon`, `0x196e07fe8` | Validates/routes through encoder methods; guest `...EncodeFrame` at `0x19709144c` verifies dimensions, retains a per-frame pixel buffer under a UUID and obtains its IOSurface. | `ours-missing` |
| Frame message builder, `ParavirtualizedVideoEncoderCreateEncodeFrameMessage`, `0x197090648` | **`efrm`**: `,frm` SInt64 opaque frame token, `,pts` / `,dur` CMTime, optional `,fpr` dictionary, `,uui` pixel-buffer UUID, pixel-buffer/IOSurface attachments; one sideband input IOSurface for ordinary frames. | `ours-missing` |
| Frame admission reply | Guest sends with `GuestSendMessageWithIOSurfacesToHostAndCopyReplySync`; reads **`,err` SInt32 and `,inf` UInt32**. Submission success does not retire the pixel buffer or produce compressed output. | `ours-missing` |
| Pixel attributes, `VTParavirtualizationHostEncoderSessionSetPixelBufferAttributes`, `0x19711fad0` | Host **`Epba`**, `,pba` plist/dictionary; guest calls `VTEncoderSessionSetPixelBufferAttributes`. Host supports one-way or synchronous exchange; synchronous guest reply has `,err`. | `ours-missing` |
| Format, `...CreateVideoFormatDescription`, `0x19711fd24` | Host **`Efds`**, `,cod`, `,wdt`, `,hgt` SInt32 and `,ext` dictionary. Guest calls `VTEncoderSessionCreateVideoFormatDescription`; reply carries `,err` and format parameter **`=fds`** of type `=fds`. | `ours-missing` |
| Output, `...EmitEncodedFrame`, `0x19711ffd4` | Host **`Eemt`**, `,frm` SInt64, `,err` SInt32, `,efl` UInt32 encoding flags, `,uui` UUID or `,ui+` UUID array, optional `,sbf` serialized sample. Guest calls `VTEncoderSessionEmitEncodedFrame` and replies. | `ours-missing` |

Native HFI already returns bounded Annex-B output and frame PTS (**`ours-OK`**
as an E1 primitive). E2 still needs translations for average bitrate, expected
frame rate, profile/level, real-time mode, frame reordering, keyframe interval
and per-frame forced keyframes, plus truthful supported-property dictionaries
(**`ours-missing`**). Exact accepted property combinations and dynamic-update
behavior are **`unknown`**. Unsupported properties must receive an explicit
error rather than silently claim a capability the fixed E1 script cannot honor.

### IOSurface input: NV12 and BGRA are separate requirements

Apple's reference handler
`vtParavirtualizationHostEncoderSession_HandleEncoderEncodeFrame`
(`0x19711ce20`) extracts a single IOSurface, calls
`CVPixelBufferCreateWithIOSurface`, restores attachments/PTS/duration, and tracks
pixel-buffer UUIDs until output. The interface carries a surface and its metadata,
not an implicit promise of Iris-compatible pixel layout.

| Check | Finding / required E2 behavior | Status |
| --- | --- | --- |
| NV12 (`420v` / `420f`) | E1 generates native NV12 internally; VT input needs actual format, active size, plane count, row strides, offsets, allocation bounds, task ownership, lock/cache synchronization and lifetime validation before copying to guarded RAW. Full/video range and color attachments require an explicit policy. | `ours-missing` |
| BGRA | E1 HFI is configured NV12. A BGRA source requires tested color conversion to NV12 or an explicit unsupported result/negotiated source format. Existing decoder output copying supplies neither conversion nor encode input access. | `ours-missing` |
| Pixel attributes | `Epba` must negotiate honest formats and IOSurface-backed source buffers; arbitrary source buffers cannot be assumed to have IOSurfaces. | `ours-missing` |
| Attachments | `AppendPixelBufferAndIOSurfaceAttachments` (`0x196ff86c0`) copies CV attachments and bulk IOSurface attachments; inspected keys include `,bat`. Full attachment semantics, including color/range propagation, remain to be modeled. | `unknown` for complete schema; encoder support `ours-missing` |
| Ownership | Retain submitted surface/pixel-buffer UUID until exactly-once completion or proven cancellation/quiescence; EBD and compressed-output completion are separate. No DMA after release or invalidation. | `ours-missing` |
| Native surface machinery | Current host is a decoder writing output surfaces, including task mapping/cache publication/disposal. These routines are evidence for reusable mechanisms, not a read-side encoder ownership proof. | `ours-OK` for decode; encoder read/copy model `ours-missing` |

### Annex-B to AVCC and serialized CMSampleBuffer output

The guest's `paravirtualizedVideoEncoder_HandleMessageFromHost`
(`0x19708dab4`) reads `Eemt`, reconstructs a sample with
`VTParavirtualizationMessageCopyCMSampleBuffer` (`0x1973093c0`), forgets pending
pixel buffers by UUID/UUID array, then calls `VTEncoderSessionEmitEncodedFrame`.
The synchronous `efrm` reply and asynchronous `Eemt` must both be correct.
The token `,frm` is an opaque guest value echoed on completion, not an HFI
session ID or a kernel pointer to dereference.

**`ours-missing`**: build a valid compressed AVC sample from the Iris access
unit. Extract SPS/PPS, establish `avcC` and the NAL length-size convention,
replace Annex-B start codes with length prefixes, preserve NAL payloads and
access-unit boundaries, set PTS/DTS/duration, dimensions/crop, sync/dependency
attachments and frame status. NV12 input is not a compressed-output IOSurface.
The exact app-visible choice of sample-description extensions and handling of
format changes is **`unknown`** until stock-VT reconstruction is exercised.

There are **two distinct format layouts**. Outer `=fds` is a big-endian
ImageDescription. Inside `=sbf`, Apple's inspected modern serializer emits a
`sbuf` tree with **`fdsc` child atoms**, not that same flat ImageDescription.
`VTParavirtualizationCreateSerializedAtomDataBlockBufferForSampleBuffer`
(`0x197346740`) and its inverse (`0x1972d26f4`) establish:

| Nested atom / layout | Observed representation | Status |
| --- | --- | --- |
| `sbuf` | Parent containing timing, sample data/sizes, format and attachments; atom headers retain BE size/type | `ours-missing` |
| `opts` | Optional output PTS, **24 native-memory bytes** of CMTime | `ours-missing` |
| `stia` | Sample timing array, **72 bytes per entry**: duration, PTS and DTS, each 24-byte native CMTime | `ours-missing` |
| `nsmp` | Number of samples, **native 32-bit** value | `ours-missing` |
| `ssiz` | Sample sizes narrowed from host `size_t` to **native 32-bit entries**; payload is not BE-converted | `ours-missing` |
| `sdat` | Raw compressed sample bytes; AVC NAL length prefixes within these bytes follow the chosen AVC format convention | `ours-missing` |
| `fdsc` | `mdia` native UInt32 media type; video `vdim` native pair of 32-bit dimensions and `codc` native UInt32 codec; `extn` encodes format-extension dictionary | `ours-missing` |
| `satt`, `sat2`, `sary` | Serializer emits propagating/nonpropagating attachment dictionaries and sample-attachment array respectively; reader also recognizes `sat1`. Nested dictionary/array codecs must be modeled. | `ours-missing`; legacy `sat1` semantics and full optional value grammar `unknown` |
| Cached format | Serializer compares current/prior format and can omit unchanged format; receiver keeps prior format state | `ours-missing` |
| Protected/multipass/tile cases | Additional code exists, including `crpt` and other message families. No support is claimed for the bounded first AVC encoder. | `unknown` outside proposed E2 envelope |

The native-memory observation is instruction-backed: the `opts`/`stia` paths
pass CMTime/timing-array memory directly to
`vtParavirtualizationAtomWriterAppendAtomWithMemoryBlock` (`0x197347290`), which
appends bytes without swapping. `...AppendNumSamples` (`0x1972d3798`) stores
`w0` directly; `...AppendSampleSizeArray` (`0x197346f08`) stores 32-bit values
without reversal. On this arm64e cache these fields are little-endian. This is
distinct from the explicit reversals in **top-level** `AppendCMTime`.
Do not extend the existing wire header's “big endian throughout” comment to
these newly audited nested opaque payloads. No code comment is edited here.

**`ours-missing`**: output fragmentation and credit accounting. Apple's host
uses `AppendCMSampleBufferWithLimitAndCopyRemainingData` (`0x196ff85c0`) with
**0x4000** message limit, then emits remainder `,sbf` pieces. Nonfinal messages
replace direction bits with **flags 0x10** (continuation); the final message
has **host-request flag 2**. This follows the masked replacement in
`MessageChangeMessageFlags` (`0x1973081a8`), not an assumption that every piece
has flags `2|0x10`. The guest appends block-buffer references until complete.
Only the completed emission retires the guest frame/UUIDs and receives the normal
guest acknowledgment. A large IDR cannot be sent as one queue entry merely
because the native reply queue has 256 KiB capacity.

### Flush, invalidate, release and cancellation

| Event / symbol | Observed contract and required invariant | Status |
| --- | --- | --- |
| `VTCompressionSessionCompleteFrames` → `ParavirtualizedVideoEncoder_CompleteFrames`, `0x197091a24` | **`ecfr`**, `,pts` CMTime cutoff, synchronous `,err`. Reference host handler at `0x19711dadc` calls encoder CompleteFrames. Retire eligible pending frames and complete their emissions; do not equate this blindly with permanent Iris CLOSE. | `ours-missing` |
| `VTCompressionSessionInvalidate` → `ParavirtualizedVideoEncoder_Invalidate`, `0x1973176a8` | **`einv`**, synchronous reply; reference handler `0x19711f1f4` invokes encoder invalidation. Must prevent further submissions and account for outstanding work and callbacks. | `ours-missing` |
| `ParavirtualizedVideoEncoder_Finalize`, `0x197317870` | Sends **`-enc` one-way**, removes UUID handler and releases object state. Distinct from `einv`. | `ours-missing` |
| Host session removal, `vtParavirtualizationInvalidateHostEncoderSessionAndRemoveItFromHostSession`, `0x196ff71b0` | Calls **HostEncoderSessionInvalidate** and **CompleteInvalidate** before removing session from dictionary. | `ours-missing` |
| `VTParavirtualizationHostEncoderSessionInvalidate`, `0x1971211ac` | Marks invalid, invalidates reply clerk and serializes shutdown on encoder dispatch queue. | `ours-missing` |
| `...CompleteInvalidate`, `0x1971212e4` | Waits for pending frame/tile dispatch groups; contains diagnostics for pending emit calls. A scalar acknowledgment alone does not prove quiescence. | `ours-missing` |
| Frame/error completion | `Eemt` corresponds to decode's `Demt`; `einv` corresponds to `dinv`. Errors, dropped frames and teardown must retire each accepted token/UUID exactly once, even without sample data. | `ours-missing` |
| Per-frame cancel | No decoder-style **`dcan` → `ecan` equivalence** is established in the inspected encoder dispatcher. Decode `dcan` is not evidence for an encoder cancellation opcode. Exact independent cancel API/wire behavior is **not established**. | `unknown` |
| Client close / guest death / timeout | Must quiesce native session, discharge surfaces and credits, reject stale UUID/reply IDs, quarantine held DMA on unproven firmware shutdown. Current client close only destroys a decoder session. | `ours-missing` encoder model; behavior under real client death `unknown` |
| Optional passes / tiles | `BeginPass` / `EndPass` use `ebps` / `eeps`; tile and multi-image handlers also exist. Do not advertise these before implementing or explicitly rejecting them. | `ours-missing` for explicit bounded-E2 rejection policy; optional support `unknown` |

The observed Apple host uses separate queues, reply tracking and pending-frame
groups. E1's single bounded synthetic transaction cannot serve as E2's session
lifecycle without a new ownership/state model. In particular, Iris DRAIN/LAST,
STOP/CLOSE, VT flush cutoff, `einv`, `-enc`, `Eemt` acknowledgments and UUID
deregistration are different events. The mapping and timeout policy are
**`ours-missing`**, not implied by the successful resident-core E1/decode handoff.

## Required design and host model before an E2 image

These are acceptance criteria for subsequent work, not changes performed here.
All are currently **`ours-missing`** unless explicitly labeled otherwise above.

1. Define a narrow AVC envelope, inventory/host-ID mapping, property support and
   explicit rejects. Build an encoder message schema with typed/unique parameters,
   request IDs, UUID ownership and phase/direction validation. Keep `encs` empty
   until the peer is usable.
2. Model create/start/prepare/configure/submit/complete/invalidate/finalize and
   client death. Include the existing exclusive core lease, resident reuse,
   bounded pending RAW/output queues, admission backpressure and failure
   quarantine. No guest-dependent wait inside selector 3.
3. Prove IOSurface read ownership, NV12 plane validation/copy and color metadata;
   define BGRA conversion or rejection. Model out-of-order EBD/FBD, arbitrary
   CMTime/opaque tokens, duplicate returns and exactly-once pixel-buffer release.
4. Build an Annex-B→AVCC/access-unit model and both format serializers. Use
   generated AVC samples to exercise SPS/PPS/crop, sync/dropped/error attachments,
   PTS/DTS/duration, format caching, >16 KiB IDR fragmentation and reply-queue
   exhaustion. Feed the result through Apple's own 27.3 sample reconstruction
   and encoder callbacks; generic decoder `=sbf` parsing is insufficient.
5. Exercise `efrm` admission versus `Eemt` retirement separately, finite and invalid
   `ecfr` cutoffs, invalidation during DMA/output/guest-reply wait, final release,
   timeout and client loss. Resolve the **`unknown`** cancellation and optional
   attachment/format semantics with bounded guest/host experiments before
   depending on them. Do not create an encoder opcode by renaming `dcan`.

## Zero-code-change RAM observation plan

Only the **existing E1 30 fps** path is ready for this next step. Use the known
RAM boot procedure, preserve firmware/display/volatile-Data controls, and save
the payload/source identities and merged boot arguments. This analysis does not
build or boot that image. The supplied successful run does not specify its arena
backing here; independently confirm the XNU receipt for these observations.

For separate boots, select one resolution at a time:

```text
piano-video=6 piano-video-vt=1 piano-extra-ram=0 piano-video-extra-ram=0
piano-video-enc=1 piano-video-enc-delay=120 piano-video-enc-frames=10
piano-video-enc-kbps=2000 piano-video-enc-res=1
```

Then use the same configuration with **`piano-video-enc-res=2`** on a second boot,
after inspecting the first result. Leave media clients idle during E1's lease.
There is no new 60 fps flag to add. Proposed 60 fps observation remains HOLD
until a separate, authorized configuration path and its validation exist.

| Required observation | Pass criterion / status before the run |
| --- | --- |
| Arena and entries | XNU backing 268,435,456; 720p guarded 27,557,888 / 15 entries including VPSS 1,449,984 on RAW; 1080p guarded 56,180,736 / 14 entries and VPSS zero. Static `ours-OK`; runtime `unknown`. |
| Session / output | OPEN, both STARTs, ten ETB/EBD and nonempty FBD; SPS/PPS/IDR, correct PTS and active dimensions/crop; DRAIN ACK/LAST, both STOPs and CLOSE. `unknown` at larger sizes. |
| Export / software check | Save `piano-video-enc-result` and `piano-video-enc-stream` via IORegistry; require PASS, status 0, `truncated=0`, ten software-decoded frames at requested active size and PSNR-Y minimum 30 dB. `unknown` at larger sizes. |
| Safety / handoff | Zero watchdog/SMMU/system errors/quarantine. After a clean E1 pass, run the established `video-v6 --qualify` 13-case AVC control and require resident-core healthy reuse. On failure/quarantine, save evidence and end that boot's media work. `unknown` at larger sizes. |
| VT inventory | `encs` remains empty during E1 observation; no E2 app claim. `ours-OK`. |

The existing host checker accepts `--size 1280x720` or `--size 1920x1080`
and `--frames 10`; use the captured IORegistry file with
`package/PianoVideo/tests/enc_stream_check.py`. A truncated 256 KiB export is
a failed observation, not evidence of a DMA arena shortage and not grounds
to raise caps without analysis. High-bitrate/120-frame stress is outside this
bounded first scale-up observation.

## Repository verification

The temporary Linux/native comparison matched **42/42 capacities** (seven
allocations at 640×480, 720p and 1080p, each at 30/60 fps), and the unmodified
native layout program reproduced both guarded totals. All requested repository
checks passed: doc links under both `/usr/bin/python3` and
`/opt/homebrew/bin/python3` (**0 errors, 59/59 driver index entries**), repository
manifests under both interpreters (**15 tests each**),
`python3 scripts/ci/privacy-scan.py --diff origin/main` (**0 hits, 0 refused
files**), and `git diff --check origin/main`. Only this analysis, the driver
index and the E1 guide are changed.

## Fix-image gate

- **Path:** E1 `piano-video-enc-res=1/2` → existing synthetic NV12/HFI worker on
  the standard XNU 256 MiB arena: **READY for zero-driver-code-change RAM
  observe at 30 fps only**. E2 `VTCompressionSession` → encoder inventory →
  `+enc/esta/efrm/Eemt/ecfr/einv/-enc` → Iris: **HOLD**.
- **Checks:** All seven internal sizes, VPSS predicate, IDs/ports, counts,
  queue order and guarded-arena totals are **`ours-OK`** for both resolutions
  at 30/60 fps under the audited fixed settings. **No `ours-missing` VPSS or
  buffer gate remains.** Existing RAM fps selection beyond 30 is
  **`ours-missing`**; sustained rate, output size/quality and larger-size
  firmware behavior are **`unknown`**. E2 registry/session/input/output/
  fragmentation/lifecycle and its host model are **`ours-missing`**; unresolved
  cancellation and optional layouts remain **`unknown`**.
- **Root cause:** There is no reproduced scale-up failure to diagnose. The old
  ARP omission is fixed and the 720p fifteenth allocation already exists.
  E2's blocker is the absent encoder peer and ownership/serialization model,
  not a missing scalar selector or insufficient 256 MiB arena.
- **ONE change:** For the next E1 observation, change only the existing
  resolution selection to `res=1`, then separately `res=2`, retaining the
  explicit XNU-backing controls and bounded ten-frame/2 Mb/s settings above;
  **no driver change or fix image is justified by this audit**. The next E2
  engineering change must be a separately reviewed design and host model;
  keep its inventory empty and **do not build an E2 implementation image**
  until those gates close.
