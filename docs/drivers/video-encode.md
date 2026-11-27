# B10: Iris hardware encode, end-to-end static analysis

Status: **HOLD — analysis only**, 2026-11-27. Baseline: `cc795085`.
Hardware AVC/HEVC decode is established by [video.md](video.md); encode is
still open in [B10](../BACKLOG-hardware.md#2-backlog-b1b15). This is the
analysis prerequisite for the historical V8 encode item in video.md, not
the later section also named V8 for High AVC **decode**.

For the later E1 implementation and its first tablet START failure, see
[E1 stage-5 analysis](video-encode-e1.md#stage-5-static-analysis) and its
[current fix-image gate](video-encode-e1.md#fix-image-gate). The implementation
gap matrix below describes the historical baseline named above.

The missing work is an encode session, its memory/ownership model, and its
VideoToolbox protocol implementation. Changing the empty encoder inventory
alone would expose an unusable endpoint. A bounded 1080p NV12 → AVC session
fits the existing arena's **capacity**, but the existing decode layout and
state machine cannot encode. E1 must first supply a host-tested native
self-test; E2 can then add PV registration. Neither exists at this baseline.
No production source, image, installed profile, or device state is changed
by this document.

## Evidence, pins, and classification

`ours-OK` means the named source check or reusable mechanism exists. It
does **not** qualify it for encode on hardware. `ours-missing` means the
required encode implementation/check/test is absent. `unknown` means static
evidence does not establish the behavior; each such item needs a named
follow-up. Linux implementation facts, Apple wire observations, calculated
sizes, proposed behavior, and hardware observations are distinguished below.

Linux was read locally, read-only, at **yzddmr6/linux-sm8750-piano
`b5100d5f2e0bb80e1191d521a48596674608eb3d`** (K). The older
`1c21cb9895d69c1c5e83ceae68952fedf522644a` in PianoVideo's provenance is
not substituted for this pin. All Linux file names/line numbers below are
relative to `drivers/media/platform/qcom/iris/` at K:

| Reference | Pinned source and relevant scope |
| --- | --- |
| platform | [iris_platform_vpu3x.c:47–50,198–227](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_vpu3x.c#L47): VPU35 uses `iris_vpu33_buf_size`, four pipes, HFI gen2 |
| enc | [iris_venc.c:223–343,481–628](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_venc.c#L223): formats, properties, allocations, starts, drain/resume |
| cmd | [iris_hfi_gen2_command.c:91–168,170–754,761–891,914–1377](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L91): ports, properties, OPEN, queueing, lifecycle |
| caps | [iris_hfi_gen2.c:1185–1950,2001–2132](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1185): encoder capability and buffer tables actually selected by SM8750 |
| ctrl | [iris_ctrls.c:514–700,753–864,1436–1614,1710–1730](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L514): port selection and control setters |
| sizes | [iris_vpu_buffer.c:1134–1288,1300–1794,1971–2063,2445–2470,2596–2623](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1134): encoder sizing and counts |
| constants | [iris_vpu_buffer.h:11–152](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.h#L11), [iris_platform_common.h:22–31](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_common.h#L22) |
| buffers | [iris_buffer.c:66–83,418–544](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L66): external sizes and internal allocation selection |
| response | [iris_hfi_gen2_response.c:32–94,196–233,308–521,910–969](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_response.c#L32): completion direction, errors, LAST, four-pass dispatch |
| common | [iris_common.c:49–155,201–234](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_common.c#L49), [iris_vb2.c:102–150](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vb2.c#L102), [iris_vidc.c:253–327](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vidc.c#L253) |
| wire | [iris_hfi_gen2_defines.h:13–254](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_defines.h#L13), [iris_hfi_gen2_packet.h:13–103](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_packet.h#L13), [iris_hfi_common.h:17–37](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_common.h#L17), [iris_hfi_gen2_packet.c:232–271](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_packet.c#L232) |

Apple evidence is the decoded **macOS 27.3 arm64e VideoToolbox 3395.5.1**,
Mach-O UUID `DE6D299C-3D76-3F97-9A24-9F9C78F779C7`; LC_BUILD_VERSION
reports macOS MinOS/SDK 27.3. Inspected with `ipsw dyld macho`, `disass`, and
bounded `dump` reads. Addresses in the Apple sections are **unslid virtual
addresses in that build**, not file offsets or patch instructions. Symbols
and independently expressed protocol facts are the evidence record. No
Apple binary, listing, extracted payload, or host location belongs here.
Temporary inspection/calculation files are disposable and removed after use.

Our source references are repository-relative at `cc795085`. In particular,
[vt/UPSTREAM](../../package/PianoVideo/vt/UPSTREAM) pins the imported frontend
to UniXNU-droidvm-piano `706338e309b410a1f04d87eb3ae009612577aedf` and
explicitly records the native port's lack of an encode device. Its original
host history is not proof that this native backend implements that encoder.

## Linux encoder: session identity and sequence

The Linux `ENCODER` domain is a driver-side session type. **Gen2 OPEN does
not carry a decoder/encoder domain word.** `cmd:817–855` sends OPEN
(`0x01000003`) on NONE=0 with header session 0, payload U32=new session ID,
host flags 3 (interrupt and response). The next property is CODEC
(`0x03000100`), port NONE, U32_ENUM: AVC encode **2**, HEVC encode **4**;
decode is AVC **1**, HEVC **3** (`cmd:761–798`, `wire:201–207`). The
DEC_DEFAULT_HEADER property is sent only for decode. Reusing our decode
codec word or first-input PSC sequence would open the wrong kind of session.

Linux opens once during VB2 queue setup (`iris_vb2.c:133–145`), after the
format is selected. For encode, V4L2 OUTPUT is **input RAW**, HFI port 2;
V4L2 CAPTURE is **output BITSTREAM**, HFI port 1 (`cmd:91–149`). The names
OUTPUT/CAPTURE refer to the application queue, not HFI RAW/BITSTREAM.

1. Configure active raw dimensions, padded storage, crop, destination codec
   and coded dimensions, frame rate and controls. NV12 is the linear
   eight-bit format; HEVC is not automatic Main10 support. The input format
   list is NV12/QC08C, not P010 (`enc:83–95`).
2. Input STREAMON calls `iris_set_properties(INPUT)`, allocates/queues the
   once-per-session ARP, determines and queues VPSS if nonzero, then
   START(RAW). Output STREAMON likewise sends output properties, ensures
   ARP, queues BIN/COMV/NON_COMV/LINE/SCRATCH_2, then START(BITSTREAM)
   (`enc:481–545`, `caps:2057–2067`). These are independently started VB2
   queues; this describes input-then-output invocation, not an assertion
   that the API prohibits the reverse order. An E1 test should fix one order.
3. START is `0x01000005`, flags 3, no payload, addressed to the session and
   port. Both subscription helpers return immediately for ENCODER
   (`cmd:926–927,1080–1081`): no decode SETTINGS_CHANGE subscription/wait is
   required to discover the encoder's output geometry. Linux enqueues OPEN
   and START rather than synchronously waiting for each acknowledgement;
   STOP and CLOSE have explicit completion waits.
4. Queue empty compressed-output buffers and populated RAW inputs; before
   STREAMON QBUF is deferred (`enc:548–570`). Keep output credit available
   while consuming input. Buffer completion and final drain are below.
5. Encoder STOP command means **drain**, not immediate STREAMOFF:
   `iris_venc_stop_cmd` sends DRAIN(RAW), flags 7, and sets DRAIN state.
   Accept output until DRAIN acknowledgement and terminal LAST, including
   zero-payload LAST. Resume is explicit `RESUME` with U32=DRAIN on paused
   ports after LAST (`enc:573–628`, `cmd:1215–1253`).
6. STREAMOFF sends STOP (`0x01000006`, flags 7) to the selected port and
   waits, updates state and flushes **deferred** buffers. Stop both ports,
   then CLOSE (`0x01000004`, NONE, flags 7), wait, and retire internal
   storage (`common:201–234`, `iris_vidc.c:253–327`). No separate gen2
   `HFI_CMD_FLUSH` appears in this pin. Linux does not impose one universal
   two-port STREAMOFF order on callers. Proposed E1 uses stop input RAW,
   then output BITSTREAM, then CLOSE after draining; that is a testable
   policy, not an observed firmware requirement.

### Properties and their exact scope

The table describes **what this reference sends**, not an independently
proven smallest set accepted by firmware. Config parameters run first, then
the capability setters (`ctrl:1710–1728`). The latter loop does not propagate
each setter's return value. E1 must check each chosen write; absence of a
Linux error does not prove that firmware accepted a property.

Payload types: U32=1, Q16=8, U32_ENUM=9, PACKED32=10, U32_ARRAY=11,
PACKED64=13. HFI fields/payload words are native little-endian on this
platform, unlike the outer Apple atom protocol.

| Property / ID | Port, encoding and interpretation | Source at K |
| --- | --- | --- |
| CODEC `03000100` | NONE, ENUM 2/4, selects encoder | cmd:761–798 |
| COLOR_FORMAT `03000101` | RAW, ENUM **1**=NV12; QC08C maps to 2 | cmd:475–509 |
| RAW_RESOLUTION `03000178` | RAW, PACKED32 `(activeWidth<<16)\|activeHeight` | cmd:170–181 |
| CROP_OFFSETS `03000105` | Both ports, PACKED64 as two words: `(left<<16)\|top`, `(right<<16)\|bottom`. RAW subtracts crop from active raw size; BITSTREAM removes codec padding. At 1920×1080 output coded height 1088, bottom=8 | cmd:227–282 |
| LINEAR_STRIDE_SCANLINE `03000104` | RAW, PACKED64, Y then UV `(stride<<16)\|scanlines`; stride aligned 128, Y height aligned 32, UV=Y/2 | cmd:512–544 |
| SIGNAL_COLOR_INFO `03000155` | RAW, PACKED32 colour/range flags from V4L2 values; use explicit BT.709/video range for E1 | cmd:355–395 |
| BITSTREAM_RESOLUTION `03000103` | BITSTREAM, PACKED32; active output rounded to 16 for AVC, 32 for HEVC, with rotation considered | cmd:184–224 |
| FRAME_RATE `0300010c` | BITSTREAM, Q16, integer fps shifted 16 (30 → `001e0000`) | cmd:620–631 |
| PROFILE `03000107`, LEVEL `03000108` | BITSTREAM, ENUM values from V4L2 profile/level enums, **not SPS profile_idc/level_idc bytes**. Defaults High/5.0 for AVC, Main/5 for HEVC; E1 chooses AVC High/4.0 explicitly | caps:1185–1259; ctrl:601–638 |
| TOTAL_BITRATE `0300013b` | RAW for capability flags containing INPUT; U32 bits/s, default 20,000,000, bounds depend on entropy/codec. This pin's spelling of the requested `HFI_PROP_BITRATE` concept is `HFI_PROP_TOTAL_BITRATE` | caps:1288–1297; ctrl:514–530,753–785 |
| TOTAL_PEAK_BITRATE `0300017c` | RAW, U32; setter acts for CBR and clamps peak to at least target | ctrl:788–809 |
| RATE_CONTROL `0300012a` | BITSTREAM, ENUM: VBR_CFR=0, CBR_CFR=1, CQ=2, OFF=3, CBR_VFR=4, LOSSLESS=5. Selected by bitrate mode, frame RC and frame-skip controls | ctrl:839–863; wire:68–78 |
| TIME_DELTA_BASED_RATE_CONTROL `0300012b` | BITSTREAM, U32 default 0, explicitly disables firmware timing-derived RC | caps:1321–1329; ctrl:1616–1632 |
| STAGE `0300010a` | NONE, U32: CBR chooses stage 1, other encode modes stage 2; allocation must use the same value | ctrl:559–582 |
| BUFFER_HOST_MAX_COUNT `03000123` | Both RAW and BITSTREAM via separate caps, U32, default **64**, advertised control range 64–256. It is a host ceiling, not a request to allocate 64 buffers; external queue minimum is 4 | caps:1619–1636; constants:22–23; sizes:2596–2605 |
| SEQ_HEADER_MODE `03000149` | BITSTREAM, ENUM: separate=1, joined-first=2, prefix-sync=4. PREPEND_SPSPPS_TO_IDR selects 4 | ctrl:681–700 |
| MAX_GOP_FRAMES `03000146` | RAW, U32; reference default 59 at 30 fps. `HFI_PROP_GOP_CLOSURE` is **absent** from this pin: no invented ID or closed-GOP guarantee | caps:1349–1357; wire:110–122 |
| REQUEST_SYNC_FRAME `03000145` | RAW, ENUM **1** without sequence header or **2** with prefix header; a dynamic request before the target RAW submission, not a Boolean toggle | caps:1911–1919; ctrl:1598–1613 |
| COMV_BUFFER_COUNT `03000193` | BITSTREAM, U32, set before queuing COMV from output minimum count (or firmware minimum); reference minimum is 4, distinct from reconstruction count | cmd:1293–1339 |

The task's `HFI_PROP_SYNC_FRAME_REQUEST` is called
**`HFI_PROP_REQUEST_SYNC_FRAME`** here. Ubuntu fix
[K:1cb49011cf28](https://github.com/yzddmr6/linux-sm8750-piano/commit/1cb49011cf289fb24c392f0339f71fb4d5ed79ec)
adds `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` → REQUEST_SYNC_FRAME, the dynamic
input-port capability, and the 1/2 enum setter. K contains the fix. This
requests a sync picture; checking the resulting AVC IDR NAL and SPS/PPS is
still required. It is not evidence of an Apple `efin` operation.

Other capability setters cover CABAC, deblocking, QP limits and optional
frame QP, rotation/flip, intra refresh, LTR, temporal layers and slice
limits (`caps:1360–1950`). E1 fixes CABAC, no rotation/flip/scaling, no LTR,
no temporal layers/B pictures, one slice and frame RC enabled. Neither
`PIPE=4` from the decode path nor decode-only OPB/default-header/RAP settings
should be imported without an encoder capability source. HEVC uses the
same lifecycle with codec 4, 32-pixel coding units and different scratch
formulas; the offered input formats do not establish ten-bit encode.

### Buffer wire layout, ownership and completion

HFI header and packet headers are 32 bytes each. The 64-byte buffer payload
has offsets: type 0, index 4, base IOVA u64 at 8, address offset 16,
capacity 20, data offset 24, data length 28, PTS ns u64 at 32, flags 40,
reserved 44–63 (`iris_hfi_gen2_packet.h:92–103`). Base is 4 KiB aligned;
RAW data offset is zero. `HFI_CMD_BUFFER=01000009` uses STRUCTURE=5 and
interrupt flag 1; the normal host flag is nonsecure `0x200`, not the
READONLY ownership flag (`cmd:1308–1353`). Read-only IOSurface access is a
separate concept from HFI `READONLY=0x10` retention.

| Driver object | HFI type / port | Ownership rule |
| --- | --- | --- |
| Input NV12 | RAW=2 / RAW=2 | Copy/prepare fully before enqueue; source slot stays firmware-owned until matching RAW input-done. Input-done can precede bitstream output; it is not encoded-frame completion |
| Output compressed | BITSTREAM=1 / BITSTREAM=1 | Submit capacity with empty data; on completion validate returned session, type, port, index, base and offsets/length, then copy only returned bytes before requeue |
| BIN / COMV / NON_COMV / LINE | 7 / 10 / 11 / 8, all BITSTREAM | One of each; hold through session use and release acknowledgement or confirmed close |
| Reconstruction aggregate `BUF_SCRATCH_2` | **DPB=6 / BITSTREAM=1** | One aggregate buffer, contains several reconstructed images; not the decoder's separate RAW-port DPB pool |
| ARP | 9 / NONE=0 | One persistent 204,800-byte allocation; shared by both stream-on paths within the session |
| VPSS | 13 / RAW=2 | One only when sizing returns nonzero; see padded-format caveat below |

`cmd:114–149,1256–1285` gives those mappings. Response dispatch is
domain-sensitive for external buffers (`response:490–503`): RAW goes to
input-done for encode, BITSTREAM to output-done. Output lookup compares
index, base and data offset; it stores returned size/timestamp/flags
(`response:348–399`). Our future implementation must add explicit checked
`data_offset + data_size <= capacity` arithmetic and reject stale epochs,
duplicate done and mismatched ports before exposing data.

The reference response helper still has decode-shaped details: internal
type conversion always maps external BITSTREAM to BUF_INPUT, STOP's pause
mapping is RAW→CAPTURE, and port validation does not admit NONE/ARP
(`response:32–94,455–465`). The external encode done branch is explicit,
but these helpers are not an encode ownership proof. Preserve the actual
wire mappings and test ARP release/STOP state independently. Do not infer
that a missing ARP return licenses freeing DMA.

Process session/system errors before information, properties, and commands
in each received message (`response:910–969`). Information includes
unsupported `06000001`, corrupt `06000002`, overflow `06000004`, drain-LAST
`06000006`; buffer LAST is `10000000`. A zero-data terminal buffer is a
drain fence, not an extra sample. DRAIN ack alone does not prove all output
has arrived. On cancellation stop admission, settle every accepted token,
STOP both started ports, CLOSE, then release DMA only after proven quiesce.
Never wait for output while withholding all output buffers.

## Exact encoder memory model

These are **calculated reference sizes**, not hardware measurements or a
new Darwin allocator. Arithmetic was evaluated from the pinned pure sizing
routines in a disposable host calculation, with source dispatch and counts
checked separately. That calculation is not an E1 HFI state-machine test.
All numbers below are bytes unless marked MiB (1,048,576 bytes).

Assumptions: four pipes; eight-bit NV12; active 1920×1080, 3840×2160 UHD,
or 4096×2160 DCI; no requested scale/rotate/blur; `num_ref=1` (no B/LTR/
hierarchy); VBR_CFR, frame RC on, stage 2; four inputs and four outputs.
`W,H` below are `fmt_dst` coded dimensions: width as listed, height 1088
for 1080p, 2160 for AVC 4K, 2176 for HEVC 4K. Raw storage is aligned
128×32. Choosing active crop explicitly is essential: `enc:272–318`
maintains both padded formats and separate active `enc_raw`/`enc_scale`
fields. Do not subtract a padded crop from an active dimension unsigned.

### Reproducible formulas

Let `A(x,n)` round up to a multiple of n, `C(x,n)=ceil(x/n)`, `P=4`,
`L=16` for AVC and 32 for HEVC, `X=C(W,L)`, `Y=C(H,L)`, `N=X*Y`,
`w=X*L`, `h=Y*L`, and `R=num_ref=1`.

- **RAW input** (`buffers:66–83`): stride `A(rawWidth,128)`, Y rows
  `A(rawHeight,32)`, UV rows `A(ceil(paddedHeight/2),16)` after the format
  setter has padded height; total `A(stride*(Yrows+UVrows),4096)`.
  At 1080p: stride 1920, Y rows 1088, UV rows 544, UV offset 2,088,960,
  active bytes 3,110,400, capacity **3,133,440**.
- **External bitstream** (`buffers:418–444`): begin with
  `T=3*A(W,32)*A(H,32)`; use `T/8` above 4096×2176 pixels, `T/4` above
  1280×720, otherwise T. With frame RC disabled/CQ, double when below
  uncompressed YUV size; then `A(T,4096)`. This is a capacity bound, not
  bitrate/fps divided into bytes per frame.
- **BIN** (`sizes:1150–1243`): internal bitstream estimate S starts at
  `3*W*H`; double below 3680 macroblocks (1280×736), divide by 4 through
  36,864 macroblocks (4096×2304), divide by 8 above that. OFF/CQ doubles
  it; multiply by 5 then integer-divide by 4; `S=A(S,4096)`.
  Stage 2: `B=A(floor(17*S/10),256)`, three bit-bin groups.
  Stage 1: `B=A(3*A(W,L)*A(H,L),256)`, one group.
  For four pipes `Q=A(floor(B/2),256)`;
  `SAO=64*floor((W+32)*(H+32)/1024)+384`;
  `BIN=A(Q+SAO,256)*4*groups+512`.
- **COMV** (`sizes:1300–1332`): AVC collocated motion
  `A(48*(N+32),256)*(R+1)`; HEVC `A(16*(4*N+32),256)*(R+1)`.
  Add `6*A(32*C(C(W,16),8)*C(H,16),256)` for collocated RC.
  R changes with B pictures, LTR and hierarchy (`sizes:1246–1288`);
  neither external count=4 nor host ceiling=64 substitutes for R.
- **NON_COMV** (`sizes:1335–1375,1572–1613`):
  `F=256+16*(14+C(h/L,8))`;
  `F=6*A(4*A(11*F,256),512)` for four pipes;
  `M=(h/L)*floor((w+8*L)/L)`.
  Sum `A(256+16*N,256) + 20480 + 6144 + F + A(256+4*N,256)`
  `+ A(256+floor(N/8),256) + 98304 + 2816`
  `+ 2*A(16*C(M,8),256) + A(3*A(2*N,8),256)`.
  HEVC adds 256 bytes for one slice.
- **LINE** uses **VPU33's** VPSS line formula on VPU35, not the older
  `size_vpss_line_buf` (`sizes:1379–1512,1617–1687,1707–1721`). For 8-bit,
  its base is the sum of:
  `2*A(w,256)`;
  `A(8*w+1024,256)+2*A((8*w+1024)/2,256)`;
  `A(4*A(K+512,512),256)` where AVC `K=80*C(h,16)`, HEVC
  `K=64*floor((h+32)/32)`;
  `A(2*h+256,2048)`;
  AVC `A(256+16*(w/16),256)` or HEVC `A(64*(w/32),256)`;
  `4*A(256+64*floor(h/64),256)`;
  `4*A(256+64*floor(h/32),256)`;
  `A(2*w,256)`;
  `4*A(256+64*(h/16),2048)`;
  `A(256+16*(w/16),256)`;
  `A(16*(w/32),256)` with integer division.
  Add VPU33 VPSS line storage: for `D=max(w,h)`,
  `t4=16*C(2*D,4)+256`, `l4=65536+64`, `t2=16*C(D,4)+256`,
  `l2=32*C(2*D,4)+64`,
  `V=A(2*P*(l4+l2)+2*(t4+t2)+8*(w+1)+8*h*P,256)`.
- **Reconstruction aggregate / SCRATCH_2** (`sizes:1993–2052`):
  `ms=A(C(W,32),64)`, `mh=A(C(H,8),16)`,
  `meta=A(ms*mh,4096)`;
  `D=A(W,128)*(A(H,32)+A(floor(H/2),32))+2*meta`;
  aggregate **`D*(R+3)+4096`**. The `+3` is essential.
  `hfi_buffer_dpb_enc` is a different per-picture helper, used for VPSS;
  it is not the aggregate allocation.
- **ARP** is 204,800 bytes (`sizes:1773–1775`). **VPSS** is zero if source
  and destination format dimensions are equal and blur is off; otherwise
  `A(W,32)*(A(H,32)+A(floor(H/2),32))+2*meta` (`sizes:1516–1522,
  1725–1743,1778–1794,2055–2062`). One allocation of each nonzero internal
  type; no decoder PERSIST or gen1 SCRATCH_1 in the gen2 encode table.

### Sizes, counts, arena comparison

| Allocation | Count | AVC 1080p | HEVC 1080p | AVC UHD | HEVC UHD | AVC DCI | HEVC DCI |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| RAW capacity, each | 4 | 3,133,440 | 3,133,440 | 12,533,760 | 12,533,760 | 13,369,344 | 13,369,344 |
| BITSTREAM capacity, each | 4 | 1,568,768 | 1,568,768 | 6,266,880 | 6,266,880 | 6,684,672 | 6,684,672 |
| BIN | 1 | 21,661,184 | 21,661,184 | 85,712,384 | 86,342,144 | 91,395,584 | 92,071,424 |
| COMV | 1 | 983,040 | 458,752 | 3,892,224 | 1,828,864 | 4,150,272 | 1,950,720 |
| NON_COMV | 1 | 542,720 | 345,600 | 1,312,256 | 542,976 | 1,377,024 | 559,616 |
| LINE (VPU33 path) | 1 | 984,576 | 973,824 | 1,422,080 | 1,399,552 | 1,468,672 | 1,446,400 |
| SCRATCH_2 aggregate | 1 | 12,636,160 | 12,636,160 | 50,434,048 | 50,434,048 | 53,776,384 | 53,776,384 |
| ARP | 1 | 204,800 | 204,800 | 204,800 | 204,800 | 204,800 | 204,800 |
| VPSS if format dimensions differ | 1 | 3,158,016 | 3,158,016 | 12,607,488 | 12,607,488 | 13,443,072 | 13,443,072 |
| Total without VPSS | — | 55,821,312 | 55,089,152 | 218,180,352 | 215,954,944 | 232,588,800 | 230,225,408 |
| Same, each region rounded to 16 KiB plus 16 KiB guard | 14 regions | 56,180,736 | 55,443,456 | 218,497,024 | 216,268,800 | 232,882,176 | 230,522,880 |
| Guarded MiB without VPSS | — | **53.578125** | 52.875 | 208.375 | 206.25 | 222.09375 | 219.84375 |

**Padding caveat:** the actual Linux `is_scaling_enabled` compares padded
`fmt_src` and `fmt_dst`, not active `enc_raw` and `enc_scale`. Setting AVC
RAW 3840×2160 stores 3840×2176; setting compressed output to 3840×2160
stores height 2160. This triggers VPSS even with equal requested active
dimensions. Including this fifteenth region gives AVC UHD **231,129,088
bytes = 220.421875 MiB**, DCI **246,349,824 bytes = 234.9375 MiB** with
guards. HEVC's 32-row output alignment makes both formats 2176 and avoids
this particular mismatch. At 1080p both formats are 1088, so VPSS is zero
under the stated setup. Any differing format/crop/scale setup must recompute
the predicate, rather than unconditionally allocating or omitting VPSS.

CBR changes stage and BIN even at the same bitrate: AVC 1080p BIN is
13,083,136 and guarded total **45.390625 MiB**; AVC UHD BIN is 51,890,688,
guarded total including the above VPSS **188.171875 MiB**. OFF/CQ, larger
reconstruction counts, extra credits and scratch generations require fresh
sizing. These totals do not establish arbitrary 4K configurations fitting.

Our [VideoBuffers.h](../../package/PianoVideo/VideoBuffers.h):16–59 reserves
**256 MiB = 268,435,456 bytes** for AVC, versus up to 768 MiB for HEVC
decode. Its `Layout::populate` builds two compressed inputs, decoder
PERSIST, two scratch generations and DPB/RAW pairs; those are unrelated to
the encode counts above. `VideoGeometry.h:15–52` admits bounded AVC
decode geometry and computes decoder scratch. An encode-specific sizing
and layout model is required even when retaining the 256 MiB backing.

For proposed 1080p E1, one 14-region layout consumes 56,180,736 bytes,
leaving **212,254,720 bytes = 202.421875 MiB** of that arena. Thus capacity
does not require a larger arena for this fixed slice. It does not permit
sharing the active decoder's arena or changing its layout in place.
[VideoArena.h](../../package/PianoVideo/VideoArena.h):11–43 currently takes
the concrete decode `Layout`, maps only owned pages and leaves guards
invalid; it needs an independently tested encode layout consumer later.

Backing overhead is separate: [VideoHw.h](../../package/PianoVideo/VideoHw.h):256–258
has `0x302000` UC bytes (3,153,920), declared in a 4 MiB IOVA region;
[VideoSmmu.h](../../package/PianoVideo/VideoSmmu.h):153–163 uses 132×4096
=540,672 table bytes for the 256 MiB arena. The current
[PianoVideo.cpp](../../package/PianoVideo/PianoVideo.cpp):590–598 uniqueness
ledger is `(131072+65536)*8=1,572,864` bytes. Rounded UC allocation,
tables and ledger total **5,275,648 bytes = 5.03125 MiB**, outside the
arena, plus descriptors/CPU session objects and the existing firmware
reservation. The present allocator reserves the full 256 MiB regardless
of used encode bytes: that backing plus these metadata allocations is
**261.03125 MiB**, before those other owners. PV input surfaces, copied
sample storage and reply queues are additional E2 CPU costs. The 256 KiB
PV receive queue cannot hold an unfragmented worst-case 1080p sample.

## Apple 27.3 PV encoder wire contract

### Envelope, inventory and creation

`vtpv` is a 48-byte first atom: BE32 size/type, BE32 message 4CC and flags,
16-byte guest/session UUID, BE64 request ID, eight reserved bytes.
`para` atoms then contain BE32 key and type and typed payload. Flags:
guest request 1, host request 2, host reply 4, guest reply 8, one-way 0;
`0x10` is the observed continuation/more-data bit. Session UUID and frame
token are distinct. [PianoVideoVTWire.h](../../package/PianoVideo/vt/PianoVideoVTWire.h):3–28,63–102
already provides the common header and scalar parsing/writing.

Scalars `=s32`, `=u32`, `=s64` and `=tim` are big-endian; `=tim` is 24
bytes: value i64, timescale i32, flags u32, epoch i64
(`VTParavirtualizationMessageAppendCMTime`, `0x197309704`, swaps at
`0x197309774–794`). `=uui` is 16 opaque UUID bytes; `=pls` holds a binary
property list; `=dat` holds bytes; CMVideoDimensions uses `=wxh`, two
big-endian signed 32-bit dimensions (`1973095fc`). **Nested sample-buffer memory blocks
are not uniformly big-endian**; see the sample section.

`vtLoadParavirtualizedVideoEncoders` (`0x1973131b8`) sends **`encs`**
with `,ver` U32=6, reads `,enc` as a CFArray and `,ver` as U32, then checks
each element is a dictionary (`0x197313228–3a4`). The host list writer
`VTParavirtualizationHostCopyEncoderListReply` (`0x19730ffa8`) selects
hardware-accelerated registry entries and serializes these fields:

| Descriptor field | Observed representation / use |
| --- | --- |
| `VTCodecType` (`kVTVideoEncoder_CodecTypeKey`) | CFNumber Int32 containing the codec OSType, e.g. `avc1` or `hvc1`; not a four-character CFString in the wire descriptor |
| `VTRating` (`kVTVideoEncoder_Rating`) | Int32 ranking; no encode rating is established for PianoVideo |
| `VTCodecName` (`kVTVideoEncoder_CodecNameKey`) | Display name from the host registry |
| `CMClassImplementationName` | Implementation display name |
| `CMClassImplementationID` | Stable host implementation ID, used later as HostEncoderID; not a surface ID |
| `Hide` (`kVTVideoEncoder_Hide`) | Optional CFBoolean; affects registration flags, not HFI capabilities |

Field construction is at `0x197310478–504`, lookup and registration at
`0x197313324–470`. Key literals were also checked through the constants'
CFString references. The guest prefixes implementation name and ID with
`paravirtualized:` and carries the original ID as
`kVTVideoEncoder_HostEncoderID`; creation sends that as `,cid`. Do not copy
the decoder's `VTDecoderCapabilities` descriptor into an encoder entry.

`ParavirtualizedVideoEncoder_CreateInstanceWithSpecification`
(`0x197316fe8`, writes at `0x197317188–254`) creates a fresh UUID,
installs a guest handler and sends **`+enc`**, not `ecre`:
`,cod` S32 codec, `,ver` U32=6, optional `,cid` plist HostEncoderID,
`,gin` dictionary including guest build information. Reply is same message
type with flag 4, `,err` S32, `,ver` U32=6, `,hin` dictionary. The guest
reads version and host-info **before** reading status (`0x19731728c–2d4`);
an error reply cannot safely be assumed to consist solely of `,err`.
`VTParavirtualizationHostEncoderSessionCreate` (`0x197327590`) reads the
same keys and constructs that reply. No create/start implies a frame was
encoded; codec availability is independently checked.

### Complete message inventory

G→H means Apple's guest encoder calling our future host; H→G means a
callback to Apple's guest. Replies retain the request type/ID/UUID with
the reply flag. All request/reply operations carry `,err` S32 unless a
more specific reply payload is listed. These are observed message types,
not guessed abbreviations; compiler comparison-tree boundary constants
that differ by one from a real tag are not messages.

| Direction / message | Meaning and principal parameters | Apple symbol / unslid entry |
| --- | --- | --- |
| G→H `encs` | Inventory, `,ver`; reply `,enc` plist array and `,ver` | `vtLoadParavirtualizedVideoEncoders` `1973131b8` |
| G→H `+enc` | Create, fields above | `ParavirtualizedVideoEncoder_CreateInstanceWithSpecification` `197316fe8` |
| G→H `esta` | Start, `,vdi` video dimensions | `ParavirtualizedVideoEncoder_StartSession` `197091208` |
| G→H `epfr` | Prepare to encode frames | `ParavirtualizedVideoEncoder_PrepareToEncodeFrames` `197091f5c` |
| G→H `efrm` | Encode: `,frm` S64 token, `,pts`/`,dur` CMTime, optional `,fpr` dictionary, `,uui` source UUID, attachments and one transported IOSurface. Reply has `,err` and `,inf` U32 encode-info flags | `ParavirtualizedVideoEncoderCreateEncodeFrameMessage` `197090648`; `ParavirtualizedVideoEncoder_EncodeFrame` `19709144c` |
| G→H `ecfr` | Complete frames through `,pts` CMTime | `ParavirtualizedVideoEncoder_CompleteFrames` `197091a24` |
| G→H `einv` | Invalidate synchronously; guest clears its session pointers after the reply | `ParavirtualizedVideoEncoder_Invalidate` `1973176a8` |
| G→H `-enc` | Finalize/remove, one-way; distinct from invalidate | `ParavirtualizedVideoEncoder_Finalize` `197317870` |
| G→H `ecop` / `eset` | Copy/set property: `,key` plist, `,val` plist | `ParavirtualizedVideoEncoder_CopyProperty` `197317b48`; `SetProperty` `197090d10` |
| G→H `emul` / `esav` | Set-properties `,pro` dictionary / copy serializable properties → `,pro` dictionary | `ParavirtualizedVideoEncoder_SetProperties` `197317d10`; `CopySerializableProperties` `197317f1c` |
| G→H `esup` | Supported property dictionary → `,sup` plist dictionary | `ParavirtualizedVideoEncoder_CopySupportedPropertyDictionary` `197091cb8` |
| H→G `Epba` | Set source pixel-buffer attributes, `,pba` plist dictionary; can be one-way or synchronous depending on host context | `VTParavirtualizationHostEncoderSessionSetPixelBufferAttributes` `19711fad0` |
| H→G `Efds` | Create video format: `,cod`, `,wdt`, `,hgt` S32; `,ext` dictionary. Reply `,err` and **key** `=fds` containing a typed format description | `VTParavirtualizationHostEncoderSessionCreateVideoFormatDescription` `19711fd24` |
| H→G `Eemt` | Encoded-frame completion, described below; guest reply flag 8 | `VTParavirtualizationHostEncoderSessionEmitEncodedFrame` `19711ffd4` |
| G→H `ebps` / `eeps` | Begin/end pass: `,bfl` U32 on begin; end reply `,fpr` S32 further-pass result | `ParavirtualizedVideoEncoder_BeginPass` `19709215c`; `EndPass` `197092380` |
| H→G `Esps` | Next-pass time ranges, `,trd` CFData | `VTParavirtualizationHostEncoderSessionSetTimeRangesForNextPass` `197121778` |
| G→H `ests` | Start tile session: `,esp` dictionary, `,twh` Int32 size | `ParavirtualizedVideoEncoder_StartTileSession` `1970925c0` |
| G→H `epti` | Prepare tiles: `,ept` S32 | `ParavirtualizedVideoEncoder_PrepareToEncodeTiles` `19709282c` |
| G→H `etil` | Encode tile: `,til` S64, `,uui`, `,tio` point, `,tia` size, `,tip` dictionary, IOSurface/attachments; reply `,err`, `,efl` U32 | `ParavirtualizedVideoEncoderCreateEncodeTileMessage` `1970907cc`; `EncodeTile` `197092a50` |
| G→H `ecti` | Complete tiles | `ParavirtualizedVideoEncoder_CompleteTiles` `197093170` |
| H→G `TEsa` / `TEcd` | Tile attributes (`,twh`, `,pba`) / tile format (`,cod`, `,wdt`, `,hgt`, `,ext`; reply key `=fds`) | `VTParavirtualizationHostEncoderSessionSetTileAttributes` `197120578`; `CreateTileVideoFormatDescription` `1971207d8` |
| H→G `TEem` / `TEsr` | Tile completion (`,til`, `,err`, `,efl`, `,sbf`, `,uui`), supports fragmentation / tile requirements (`,cba`, `,ter` plists), one-way | `VTParavirtualizationHostEncoderSessionEmitEncodedTile` `197120a88`; `SetTileEncodeRequirements` `197120f94` |
| G→H `emif` | Multi-image frame: `,frm`, `,pts`, `,dur`, `,fpr`, `,ui+` UUID array, `,tc+` tag collections, surface array; reply `,err`, `,inf` | `ParavirtualizedVideoEncoder_EncodeMultiImageFrame` `197093370` |
| H→G `mpsi` / `mpid` | Multipass storage set/copy identifier: `,vid` plist (copy returns it) | `VTParavirtualizationHostEncoderSessionMultipassStorageSetIdentifier` `1971219dc`; `CopyIdentifier` `197121bfc` |
| H→G `mpst` / `mpdt` | Timestamp / timestamp+duration: request `,fts` CMTime and `,mps` plist; reply `,tsp` CMTime, also `,dur` for mpdt | `...MultipassStorageGetTimeStamp` `197121df0`; `...GetTimeStampAndDuration` `1971220e4` |
| H→G `mpcd` / `mpsd` | Read/write storage data: `,tsp`, `,ind` S32, `,mpd` data; supports fragmentation | `...MultipassStorageCopyDataAtTimeStamp` `197122434`; `...SetDataAtTimeStamp` `19712270c` |
| H→G `mpin` | Invalidate multipass storage | `VTParavirtualizationHostEncoderSessionMultipassStorageInvalidate` `197122a64` |
| H→G `Bye!` | Generic termination understood by encoder guest; posts compression SessionTerminated notification | `paravirtualizedVideoEncoder_HandleMessageFromHost` `19708dab4`, branch `19708e68c` |

The host's per-encoder dispatcher at `0x19711bce4` confirms the G→H
session operations above. The generic host-session dispatcher at
`0x197306f2c` handles `encs`, `+enc`, and `-enc` outside that switch.
There is **no `efin` in these encode methods**: complete-frames is `ecfr`.
E2 should implement the normal single-image path and explicitly reject
tiles, multipass, and multi-image operations before DMA, with support
properties that do not advertise them. Their observation here does not
make them part of E1 or prove the Iris firmware implements them.

### Source IOSurface, read-only access, and admission

Apple's guest `EncodeFrame` checks CVPixelBuffer width/height against the
started dimensions (`0x1970914e8–510`), generates a per-source UUID,
remembers the pending pixel buffer, obtains its IOSurface, and refuses a
missing surface (`0x197091514–550`). The serializer adds token, timing,
frame options and UUID; `AppendPixelBufferAndIOSurfaceAttachments`
(`0x196ff86c0`) adds `,bat`/`,dp1` CFData and `,pat` plist as applicable.
`GuestSendMessageWithIOSurfacesToHostAndCopyReplySync` (`0x197307dc0`)
sends the surface separately from the atom bytes. It waits for a control
reply; this is not the eventual compressed sample callback.

Apple's host reference has an additional surface-mapping lifetime:
`VTParavirtualizationHostSessionDeliverMessageFromGuest` (`0x197306f2c`)
associates mapping IDs with child IOSurfaces, reuses live weak references,
and later relinquishes unused mappings. The generic release transport box
is a **`rels` atom**, not a `vtpv` encoder command: BE32 size/type followed
by BE64 mapping IDs (`vtParavirtualizationCreateMessageBoxToRelinquishSurfaceMappingIDs`,
`0x197309e8c`). Mapping IDs are not interchangeable with the local
IOSurface IDs received by our kext. `HostEncoderSessionDeliverMessageFromGuest`
(`0x197327c30`) snapshots up to five surfaces/mapping IDs onto a dispatch
channel. Its encode handler (`0x19711ce20`, surface section
`0x19711cf20–fd0`) requires exactly one surface, creates a CVPixelBuffer
with it, restores attachments/timing and retains source UUID/pixel-buffer
records keyed by frame token before calling the underlying encoder.

That reference wraps surfaces for a host userspace encoder. It does **not**
establish that our kext can directly DMA arbitrary IOSurface pages or that
Apple's guest method itself takes a read-only CPU map. Our native transport
currently receives **IDs** in selector 3, not Apple userspace CVPixelBuffers
or trusted physical addresses
([PianoVideoVTParavirt.cpp](../../package/PianoVideo/vt/PianoVideoVTParavirt.cpp):254–304).

The required E2 input path is therefore an explicit new contract:

1. Check exactly one nonzero surface ID, client/session ownership, token,
   dimensions, NV12 `420v` range/colour and both plane extents before admission.
   Decode's output-surface lookup provides useful checks, but encoder input
   authorization must be tested separately, including sandboxed clients.
2. Retain a legitimate IOSurface/descriptor reference, acquire read access,
   and map only the validated allocation range **read-only**. Public
   `IOSurfaceTypes.h` defines `kIOSurfaceLockReadOnly=1`; existing native
   external lock/unlock selectors are 2/3 with `{id,options,reserved}`
   ([PianoVideoVTSurface.h](../../package/PianoVideo/vt/PianoVideoVTSurface.h):34–42).
   Applying options=1 and `kIOMapReadOnly` to a new kernel input path needs
   its own pinned ABI/model checks; it is **unknown** on this endpoint.
3. Copy active Y/UV rows **from** that map **into** a private bounded RAW
   slot, initialize padding, order writes before HFI enqueue, then unmap,
   unlock and balance references. Never reuse decode's writable destination
   copy, cache the mapped address by ID, or assume client rowBytes equals
   Iris stride. A held source must not mutate during the snapshot.
4. Retain the guest frame/UUID completion obligation until the encoded
   result/error retires it. RAW input-done releases a private DMA slot;
   `Eemt` retires the guest frame. These fences can occur in either order.

Decode lookup uses IOSurfaceRoot method 4 with a `0xc68` result, validates
layout, and accounts for invalidate56/release1; it also contains a
version-specific kernel-surface fallback
([PianoVideoVTHost.cpp](../../package/PianoVideo/vt/PianoVideoVTHost.cpp):250–325).
That fallback is not evidence of encode authorization. Avoid importing it
into E2 without a separate ownership proof. Rejected inputs need bounded
cleanup and exactly one public error disposition.

### Encoded sample delivery, fragmentation, and errors

`HostEncoderSessionEmitEncodedFrame` (`0x19711ffd4`) constructs **`Eemt`**
with host-request flag 2, `,frm` S64 token, `,err` S32 status,
`,efl` U32 encode flags, optional returned `,uui` source UUID or `,ui+`
UUID array, and `,sbf` typed **`=sbf`** sample data when present
(`0x197120068–17c`). No output IOSurface is required for compressed data.
An encode error can complete a frame without a sample. Control `efrm`
status/`,inf` and later `Eemt` status/`,efl` are different messages and must
not double-complete a rejected frame.

The serializer `VTParavirtualizationCreateSerializedAtomDataBlockBufferForSampleBuffer`
(`0x197346740`, writes at `0x197346850–ae0`) builds an `sbuf` container.
Its ordinary video content includes:

| Atom | Payload observation |
| --- | --- |
| `opts` | Optional output presentation CMTime, **24 native-memory bytes** |
| `stia` | Timing array, **72 native-memory bytes per entry**: duration, PTS, DTS CMTime structures |
| `sdat` | Encoded bytes from the CMBlockBuffer |
| `nsmp` | Nonzero sample count, native 32-bit memory block (`1972d3798`) |
| `ssiz` | Sample sizes converted from size_t to a native 32-bit array (`197346f08`) |
| `fdsc` | Format description when changed; contains `mdia` native OSType, `vdim` two native Int32 values, `codc` native codec OSType, and `extn` serialized dictionary (`1973470fc`) |
| `satt` / `sat2` / `sary` | Propagating/non-propagating buffer dictionaries / sample attachment array through the private serializer; indexed keys depend on negotiated protocol version. Reader also recognizes the older `sat1` form |

Atom sizes/types still use the atom framing; the above raw memory payloads
are little-endian for the pinned arm64 host. They differ from outer
`=tim` and from the separate `=fds` ImageDescription parameter used by
`Efds`. Inspecting a valid decoder ImageDescription alone does not supply
an encoder `=sbf` serializer. Protected sample variants also exist in the
Apple reader; they are outside the proposed unprotected E2 envelope.
The full indexed attachment dictionary codec and every protected variant
remain **unknown** for our implementation and must be exercised against
Apple's reader before accepting such samples.

For AVC, the proposed bridge must bound-parse the Iris output NALs, retain
SPS/PPS, supply the `avcC` format extension (including NAL-length width),
and convert Annex-B start codes to length-prefixed AVCC for CoreMedia.
IDR is determined by NAL semantics, not merely a requested keyframe or an
I-picture indication. Preserve sample PTS/duration, set DTS consistently
with no reordering, and provide sample `NotSync`/dependency attachments.
For HEVC the analogous format needs VPS/SPS/PPS and `hvcC`; it is a later
slice. **The first firmware output's actual NAL framing/header placement
is unknown until E1 captures and independently parses it.** Linux's H264/
HEVC format and header-mode settings do not by themselves prove AVCC.

The Apple writer limits message size to **`0x4000`**
(`0x197120174,208`); when data remains it changes flags using mask `0x12`
and value `0x10`, sends intermediate `Eemt` fragments, then a final
host request. The guest accumulates fragments, parses the assembled sample
and emits exactly once; it replies with flag 8. At
`paravirtualizedVideoEncoder_HandleMessageFromHost` `0x19708db88–dcf4`
the returned UUIDs retire pending source buffers, and the token is passed
to `VTEncoderSessionEmitEncodedFrame`. A successful admission does not
replace this callback or its acknowledgement. Our 256 KiB queue and
decode-only writer do not implement that stream; E2 needs bounded
fragment storage, backpressure and final-ACK ownership tests.

Keyframe is a frame-property dictionary request such as ForceKeyFrame,
translated by our proposed encoder into HFI REQUEST_SYNC_FRAME before the
target RAW buffer. `ecfr` carries an upper PTS completion barrier; it is
not a firmware cancellation or an instruction to discard pending frames.
For E1 a terminal HFI drain is sufficient. Repeated nonterminal VT
CompleteFrames calls need a distinct E2 policy: track accepted tokens
through the requested PTS and their `Eemt`/ACKs; drain/resume only when
needed and tested. Do not copy the decoder's `dfin`/software-finish logic
or assume public flush equals HFI STOP.

Invalidation has separate guest and native obligations. Apple's reference
`HostEncoderSessionCompleteInvalidate` (`0x1971212e4`) waits for outstanding
frame/tile dispatch groups; its initial wait is two seconds and warnings
can repeat with increasing waits, not a guaranteed two-second shutdown.
The guest `einv` path clears encoder-session pointers after the synchronous
reply (`0x197317748–758`). Settle/cancel accepted frames, join workers,
prove STOP/CLOSE safety and retire surface obligations **before** replying;
`-enc` later removes the object. Never equate a queued reply with a public
VT completion. The parallel [decode cancellation analysis](video-hevc-cancel.md)
remains independent of this work.

| Status space | Observed fact / proposed mapping |
| --- | --- |
| Apple `,err` | Signed OSStatus, 0 success; serialization/transport errors are separate return values. Frame and control replies each need their own status |
| `-12900`, `-12901` | Public VT property unsupported / read-only; use for unsupported/read-only encoder properties |
| `-12902`, `-12903`, `-12904` | VT parameter / invalid session / allocation failed. Guest width/height/no-IOSurface checks use parameter error via `197318398–448` and helper `197093a84`; create allocation failures use `197317620–694` |
| `-12908`, `-12912`, `-12915` | Public VT could-not-find-encoder / encoder malfunction / encoder-not-available-now. Candidate E2 mappings for unavailable implementation, native failure, busy core; not observed successful PianoVideo encoder behavior |
| `-19093` | Private PV validation error observed in inventory element/type checks (`1973134dc,500`) and host create (`197327c10`); no public semantic name claimed |
| Our current constants | VTHost:31 uses decoder statuses `-12909`, `-12910`, `-12911`, plus `-12903/-12904`; no encoder-specific mapping exists |
| HFI session errors | `04000001` unknown session, `02` max sessions, `03` fatal, `04` invalid state, `05` insufficient resources, `06` buffer not set, `08` unsupported stream; system watchdog `05000001`. Preserve exact HFI evidence and map once to a VT terminal result; never expose a positive HFI type as OSStatus |

Public status names above were checked in `VTErrors.h`; private values
were checked at the cited 27.3 call sites. IOKit selectors also have an
IOReturn plus a scalar host status (`PianoVideoVTParavirt.cpp:300–305`);
an IOKit success is not encode admission. Timeout or uncertain DMA quiesce
must retain backing and block reuse, not synthesize success or power-cycle
the installed decoder.

## Our architecture and complete path/gap table

Source anchors:
[VideoSession.h](../../package/PianoVideo/VideoSession.h):268–281,386–626,733–963;
[VideoBuffers.h](../../package/PianoVideo/VideoBuffers.h):16–109;
[VideoGeometry.h](../../package/PianoVideo/VideoGeometry.h):10–116;
[VideoArena.h](../../package/PianoVideo/VideoArena.h):11–43;
[VideoHfi.h](../../package/PianoVideo/VideoHfi.h):41–133;
[PianoVideo.cpp](../../package/PianoVideo/PianoVideo.cpp):584–706,837–1055,1086–1124;
[PianoVideoNative.h](../../package/PianoVideo/vt/PianoVideoNative.h):16–47;
[PianoVideoVTHost.cpp](../../package/PianoVideo/vt/PianoVideoVTHost.cpp):250–393,630–843;
[PianoVideoVTContract.h](../../package/PianoVideo/vt/PianoVideoVTContract.h):17–45.

The table follows caller → transport → firmware → output → teardown. A row
marked OK for shared infrastructure deliberately does not promote the
corresponding encode-specific row.

| Hop / layout / event | Check or missing disposition at baseline | Classification |
| --- | --- | --- |
| Public VT discovers our service/user client | Existing PV classes, open gate and shared message/notification plumbing; described in video.md V0b/V5a | `ours-OK` |
| Encode default-off boot gate | No `piano-video-enc` parsing or encode self-test level; highest current survey level is 6 | `ours-missing` |
| `encs` reply | inventory():686–698 returns typed empty `,enc` array and version 6; log says `encoders0` | `ours-OK` |
| Qualified encoder descriptor | No descriptor, profile/dimension/property envelope, rating or native readiness predicate for encode | `ours-missing` |
| Transport snapshot and atom bounds | Selector3:254–304 snapshots shared bytes, bounds message, validates header/atoms and client UUID | `ours-OK` |
| Encoder flags, duplicate parameters and surface count | No per-message encode schema, required fields, fragment policy or exact-one-input-surface validation | `ours-missing` |
| `+enc` create/`,ver`/`,hin`/`,cid` | Only `+dec` is implemented at VTHost:771–787; generic bad-state/unknown responses are not create compliance | `ours-missing` |
| Per-client UUID and exclusive native core | Existing client UUID checks; native acquire/owns/closing and quarantine in PianoVideo.cpp | `ours-OK` |
| Decode/encode arbitration | No encode domain in the lease or way to safely rebuild an exclusive encode layout on the resident core | `ours-missing` |
| `esta`, `epfr`, properties and Epba | Decoder-only configuration/attributes; no width/rate/bitrate/profile/format setup for encode | `ours-missing` |
| Source surface lookup, read-lock/map | Existing decode destination validation is reusable evidence only; input read-only map and authorization not exercised | `unknown` |
| Per-plane NV12 snapshot and padding | Existing copy moves native RAW into a guest destination; no checked reverse copy into private encode RAW | `ours-missing` |
| Bounded credits, frame token and PTS | Decode ledger exists, but input-done/encoded-output/guest-ACK distinctions need an encode ledger | `ours-missing` |
| Rails/clocks/firmware/PAS/SMMU/queues | Existing VideoBringup/Ops/Hfi and backing ownership checks for the same VPU35 core | `ours-OK` |
| Encode bandwidth, power and firmware behavior | Decode operation does not measure encoder votes, throughput or thermal behavior; needs later RAM qualification | `unknown` |
| HFI packet/ring checks and ordering | Checked queue bounds, wrap, producer barrier and doorbell, response envelope validation | `ours-OK` |
| HFI OPEN and codec selection | Session:744 hardcodes decode codec 1/3; absent encode domain/codec 2/4 and encode start order | `ours-missing` |
| RAW/bitstream geometry and crop | Decode geometry rejects above its envelope and sizes decode scratch; no encode crop/padding arithmetic model | `ours-missing` |
| BIN/COMV/NON_COMV/LINE/reconstruction/ARP/VPSS | No encode allocator/count/size model; enum lacks ARP and VPSS; decode DPB port is 2 | `ours-missing` |
| Arena capacity and guards | 256 MiB AVC bound, 16 KiB guards and checked page walk exist; calculated fixed E1 fits | `ours-OK` |
| Encode arena page layout and simultaneous owners | buildArena takes decode Layout; no encode-specific guarded map, overlap/high-water or overflow tests | `ours-missing` |
| Encoder control properties and first-error checking | No RC/GOP/header/keyframe implementation; decoder properties cannot replace them | `ours-missing` |
| Both STARTs and output prequeue | Current code starts BITSTREAM input, awaits decode PSC then RAW output; wrong directions for encode | `ours-missing` |
| RAW submission / input-done | Existing Bitstream input retire path is decode-specific; no encode RAW producer with per-slot completion fence | `ours-missing` |
| BITSTREAM done / data bounds | Existing RAW frame/hash path cannot return compressed bytes or validate encode data extents | `ours-missing` |
| Encode error/info/picture/header association | Four-pass dispatcher exists; encode frame correlation, overflow/no-output/codec-config handling is absent | `ours-missing` |
| IDR and SPS/PPS, Annex-B/AVCC, timestamps | Decode AVC parser is not an encoder output normalizer/format builder | `ours-missing` |
| Efds / `=sbf` / Eemt / fragment stream | Only decoder callbacks are emitted; no encoder serializer, attachment codec, fragment/ACK state | `ours-missing` |
| Private serialized sample accepted by Apple | Direct inspection establishes structure, not successful round-trip or public callback behavior | `unknown` |
| Keyframe request from `,fpr` | No ForceKeyFrame → HFI request ordering or returned-IDR verification | `ours-missing` |
| `ecfr` repeated completion barriers | No encode finish policy, token fence or HFI drain/resume implementation | `ours-missing` |
| Terminal DRAIN/ACK/LAST | Decode has drain and empty-LAST checks; encode changes input/output ports and retirement conditions | `ours-missing` |
| STOP/CLOSE and DMA quarantine mechanism | Session:905–963 and native_close retain failed DMA dependencies and refuse reuse | `ours-OK` |
| Encode STOP/CLOSE/ARP release coverage | No encode two-port teardown model, including partial start and late responses | `ours-missing` |
| `einv`, worker join, pending Eemt and `-enc` | Decode-only lifecycle; no encoder cancellation disposition or all-frames/public wait proof | `ours-missing` |
| Client death and sandboxed app recording | Native encode has no public-client qualification or IOSurface lifetime witness | `unknown` |
| E1 tests and emitted bitstream | Existing generated software encodes are decoder fixtures, not an Iris encode state machine or output witness | `ours-missing` |

## Minimal vertical slices and host verification plan

Everything in this section is **proposed future work**. No encode driver
or image is authorized by the existence of this analysis. Keep decode
cancel changes separate and rebase/review integration after that work.

### E1: default-off native 1080p AVC self-test

Introduce an explicit opt-in such as `piano-video-enc=1`, default 0, only
after its portable host model/tests exist. The proposed gate chooses an
exclusive native encode self-test; **encoder inventory remains empty**.
Do not reinterpret `piano-video=7` from the historical plan as implemented.
No decode session may own the core when the test begins; failed encode
leaves the native core quarantined under the existing ownership policy.

Fix one configuration first: 1920×1080 active NV12 video range, storage
1920×1088, 30 fps, AVC High level 4.0, CABAC, VBR_CFR 20 Mbit/s, stage 2,
joined-first sequence header, no B/LTR/hierarchy/scaling/rotation. Four
RAW/four compressed buffers, six nonzero internal regions, Linux's
host-max ceiling 64 with actual admission limited to the four owned slots.
Whether firmware accepts a smaller host-max property is unknown; E1 does
not depend on changing that reference value. Arena and output-copy sizes
must be checked before any HFI/DMA side effect.

The native self-test generates owned NV12 patterns with frame numbers,
known plane colours, and changing spatial blocks. It opens codec=2,
sets all selected properties, queues ARP and output internals, starts RAW
then BITSTREAM, prequeues all compressed destinations, and submits a
bounded sequence. Record independent RAW input-done and compressed-output
counts, returned PTS, byte bounds and buffer reuse. Request a midstream
sync frame with enum 2 before its RAW packet, then verify an actual IDR
and sequence headers. Finish with DRAIN(RAW), ACK+LAST including empty
LAST, STOP(RAW), STOP(BITSTREAM), CLOSE; verify no live firmware buffer or
CPU loan before healthy reuse. Include a zero-frame close and resident
reopen case. Record bounded milestones, first error, queued/releasing
objects and arena high-water; no tablet identifiers or private locations.

The first E1 deliverable must be a portable, **scripted HFI gen2 firmware
model**, using the actual ring/packet representation and independently
specified expected events, not a test which only repeats its own setter
code. Required test matrix:

| Host test group | Required evidence / pass condition |
| --- | --- |
| Protocol transcript | OPEN header session 0, session payload, CODEC=2/4; exact ports, property IDs/types/values, scratch type/count and both STARTs; no decode PSC dependency |
| Geometry and budget | Independent arithmetic agrees with all size-table columns; packed crop/stride offsets match active/padded layout; guards unmapped; overflow, odd/zero dimensions, slot/capacity excess rejected before allocation/queue writes |
| Ownership and reorder | Model writes compressed data through mapped IOVAs; RAW done before/after output, output buffer reuse only after snapshot, token-to-PTS association, output starvation/backpressure, all four slots wrap safely |
| Keyframe/header control | Request immediately precedes chosen RAW; enum 1/2 and prefix mode checked; returned stream parser verifies SPS/PPS/IDR, not just a successful property enqueue |
| Drain and repeated lifecycle | ACK before LAST and LAST before ACK; payload then empty-LAST; no duplicate sample; zero-frame close, partial-start failure, both STOP orders as negative/explicit-policy cases, reopen with fresh session epoch |
| Adversarial response | Bad packet length/payload/flags/session/port/type/index/IOVA/offset/size, integer overflow, duplicate/stale/late done, corrupt/overflow/error info, watchdog, missing STOP/CLOSE response; no unchecked copy, double retirement or unsafe DMA release |
| Cancellation and release | Cancel before start/queued/inflight/output-held/drain; ARP/internal release, accepted-frame disposition and worker join are independent fences; timeout retains pages, SMMU, firmware/power dependencies and blocks another bootstrap |
| Regression boundaries | Default-off path and existing decode contract unchanged; no encode descriptor before successful native qualification; pre-existing decode host checks pass after any future shared-infrastructure edits |

The model must not pretend to generate hardware H.264. It can return owned
synthetic/canned compressed payloads to test copying, framing and lifecycle;
that proves host behavior only. **Actual encoded bitstream correctness
remains unknown until a later, separately gated RAM run produces bytes.**

For that later output, preserve an owned `.h264` stream plus per-sample
metadata and input manifest; keep untrusted firmware addresses/logs out of
published artifacts. Independently parse SPS cropping, profile, level,
timing, NAL lengths and IDR positions. Convert Annex-B to AVCC with retained
SPS/PPS and remux; round-trip both representations. Example host checks
(planned, not results of this analysis):

```sh
ffprobe -v error -f h264 -show_streams -show_frames -of json E1.h264
ffmpeg -v error -f h264 -i E1.h264 -f null -
ffmpeg -v error -f h264 -i E1.h264 -pix_fmt nv12 -f framemd5 E1-decoded.framemd5
```

Require the expected active 1920×1080 crop, valid High/4.0 stream, complete
frame count, monotonic 30 fps sample timing in the remuxed container, no
decode errors and a verified requested IDR. Encoding is lossy: do not
require byte-exact input/output hashes. Compare software-decoded patterns,
plane/range checks, motion/frame identity and a predeclared PSNR/SSIM
threshold against generated input. A separate AVFoundation reader must
decode the AVCC/MP4 samples successfully and agree on frame count,
dimensions, timing and keyframe metadata. Pin the tool versions in the
eventual test manifest; none of these output checks has run for Iris here.

### E2: opt-in PV encoder inventory after E1 qualification

Add exactly one bounded AVC descriptor and a separate encoder session/
worker/domain. Keep the opt-in gate default off until qualification; do
not add HEVC or 4K inventory merely because the size calculations exist.
Public selection must identify the proposed implementation ID and report
`UsingHardwareAcceleratedVideoEncoder` only for a real Iris-backed session.
Publish only supported properties and explicit maxima; reject unsupported
range/format, frame reordering, protected content, tiles, multi-image and
multipass requests before DMA.

Before registration, host tests need: complete +enc success/error envelopes;
supported-property/copy/set negotiation; 420v source-surface schema,
read-only map/copy and reference balance; frame admission/error exactly
once; SPS/PPS/avcC and `Efds` round-trip; `Eemt` native-memory sample fields
and indexed attachments; messages exceeding 16 KiB and queue capacity;
final guest ACK failure, stale UUID/request/token, repeated `ecfr`,
`einv` while pending, client death and healthy recreate. Use Apple's own
27.3 reader/public compression callback as an independent wire oracle;
successful parsing by our writer's matching parser is insufficient.

Then a future AVFoundation/VideoToolbox probe should select the explicit
encoder ID with RequireHardwareAcceleratedVideoEncoder, submit owned
CVPixelBuffer/IOSurface NV12 frames, request a midstream keyframe, complete
frames multiple times, invalidate with pending work, and recreate.
Require one callback per accepted frame, hardware-use property, balanced
surfaces/maps, bounded waits and clean native STOP/CLOSE. AVAssetWriter
records a short movie; AVAssetReader and software FFmpeg both decode and
inspect it. Run unsupported-format/busy-core/default-off fallback cases
too, so software recording cannot be mistaken for hardware success.
4K, HEVC, concurrency, rate/quality tuning and sustained power/thermal
qualification are later work with separately recomputed memory bounds.

## Licence and publication boundary

Linux GPL sources were specifications only. Equations, values, state
observations and source citations above are independently expressed facts;
no Linux implementation is copied into Darwin production code. Apple
inspection contributes factual protocol/symbol observations only. Test
patterns and eventual public media must be project-generated. No firmware,
Apple binary, host location, credentials, network details or per-device
identifier is part of this analysis.

## Fix-image gate

**HOLD — analysis only.**

Path: **HOLD.** Owned NV12 → bounded private RAW slot → HFI encoder
OPEN/CODEC/properties/internals → STARTs → RAW/bitstream completion →
NAL/AVCC sample → Eemt/guest ACK → drain/STOP/CLOSE. Shared decode
infrastructure exists; encode-specific hops are missing or unknown as
classified above. No encode image is eligible.

Checks: **HOLD.** Static Linux/Apple/ours inspection and arithmetic are
recorded. E1 scripted HFI model, independent geometry/ownership/error tests,
actual output validation, and E2 wire round-trip tests do not exist at this
baseline. Documentation/link/privacy checks cannot replace those tests.

Root cause: **HOLD.** The established architectural gap is the missing
native encoder/session/buffer/output path and PV encoder implementation;
`encoders0` accurately avoids advertising it. No failed encode hardware
experiment exists here, so no electrical/firmware failure root cause is
claimed. Capacity alone is not the blocker for the fixed 1080p slice.

ONE change: **HOLD.** The next reviewable unit is the portable E1 encode
geometry/state/ownership model and its scripted host tests, with no image
and no encoder inventory. Only after that evidence exists may a separate
review consider one default-off native self-test change; VT registration
remains E2. This document does not release the fix-image gate.
