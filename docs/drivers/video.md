# Video: the Iris video core natively (PianoVideo) and VideoToolbox through the VM-era paravirtual decoder

Status, 2026-11-18: V2 power/PAS and V3 INIT/version/lifecycle are tablet-confirmed. V4 (`vid-v4a`) decoded
our real 320×240 AVC AU and matched the exact active NV12 golden hash, with safe DMA teardown (§13).
V5c is tablet-confirmed: real caller input traverses VT and Iris into an IOSurface-backed callback with the original golden
hash and safe teardown. V6p completed one 60-frame Main AVC stream with matching pixels and balanced surface references.
Stock QuickTime displays the first hardware frame, but V7d drops the following 59 frames: the bridge drains during preroll
and subsequently exhausts its admission credits. V7e keeps the eight-credit limit, defers successful replies beyond four
published frames without blocking the receive path, and makes finish a completion barrier for this decode-order endpoint.
Rejected requests use Apple's error completion once. V7e2 and V8a subsequently qualified stock QuickTime playback of the
owned320×240 Main and1920×1080 High AVC fixtures:60 native/client frames,0 dropped and optically confirmed motion.
V9 implements bounded HEVC Main/Main10/P010 for qualification; no HEVC tablet result is claimed.
V9c contains failed native sessions for the rest of a boot: a firmware/session or STOP/CLOSE error retains
the existing DMA, firmware and power dependencies and blocks another core bootstrap. DMA release alone
does not establish hardware health. Owning-task PID/name and lifecycle reason logs support qualification;
this containment does not establish the electrical cause of a reset or safe repeated cold initialization.
The earlier ladder and historical test recipes below do not establish macOS app playback. Backlog item B10.

## 1. Purpose and gate

The native Iris bridge is an opt-in qualification endpoint; stock QuickTime continuous playback is confirmed for the two
owned AVC fixtures. Throughput for other streams needs measurement. The SoC's video core decodes H.264, HEVC and VP9 (§4.5), and encodes H.264
and HEVC. Gate: battery life (owner priority) and daily use (QuickTime, Safari, wallpapers, thumbnails);
not a login gate. Ubuntu uses it: H.264/HEVC/VP9 decode and H.264/H.265 encode verified at 1080p and 4K, AV1 in software
(`xiaomipad-8pro-mainline/docs/STATUS.md`, `README.en.md`; the core lists AV1, but a third-party piano report has AV1 decode failing
at submit: `unixnu-research/hackios-redmi-k90-annibale/docs/analysis/2026-11-05-video-decode.md` §4.1). The macOS side is proven:
in the VM, VideoToolbox decoded H.264 and HEVC (including the 4096x2160 P010 Aerial) on Ubuntu's Iris through a paravirtual decoder
(the VM-era progress notes, local, not published: issue 4, 2026-11-10).

## 2. Hardware facts

Sources: the piano boot DT (`out/inputs/piano-boot.dtb`, "DT"); the Ubuntu piano kernel (`unixnu-research/ubuntu-ref/kernel`, piano/dev
7f8d6bed, GPL: facts only): iris (`drivers/media/platform/qcom/iris`), videocc/gcc (`drivers/clk/qcom/*-sm8750.c`), qcom_scm,
interconnect `sm8750.c`; the firmware pin (`xiaomipad-8pro-mainline/firmware/piano-iris.json`). Every offset is in
`package/PianoVideo/VideoHw.h`.

| Item | Value | Source | State |
|---|---|---|---|
| Core | `aa00000` (0xf0000), `qcom,sm8750-iris`, VPU 3.5, HFI gen2, 4 VPP pipes, ≤ 16 sessions, ≤ 2x 8K macroblocks per frame; decode H.264/HEVC/VP9/AV1 (`iris_fmts_vpu3x_dec`), encode H.264/HEVC | DT, `iris_platform_vpu3x.c` | verified |
| Regions | VCODEC +0 (codec domain; idle words +0x70+4·pipe); CPU_CS +0xa0000 (CTRL_INIT +0x48, CTRL_STATUS +0x4c, QTBL_INFO/ADDR +0x50/54, SFR +0x5c, UC_REGION +0x64/68, H2X int +0x148, IC_SOFTINT +0x150 doorbell, X2RPMH +0x168); WRAPPER +0xb0000 (INTR status/mask +0xc/+0x10, LPI +0x54..0x60, SPARE_0 +0x78, CORE_POWER_STATUS +0x80); WRAPPER_TZ +0xc0000 (CPU status +0x10 WFI); AON +0xe0000 (NOC LPI) | iris register defines, `iris_vpu3x.c` | verified (source) |
| Interrupt | SPI 174 level (INTID 206): A2H message bit 2, watchdog bit 3 | DT, `iris_vpu_common.c` | verified |
| Clocks | GCC video AHB `0x132004` + XO `0x132038` (Linux keeps both on), AXI0 `0x132018` (codec), AXI1 `0x132028` (controller); VIDEO_CC `aaf0000`: PLL0 Taycan ELU (L 0x25 + alpha 0x8000 = 720 MHz), `mvs0_clk_src` +0x8000 (720..1890 MHz from PLL0) and divider +0x809c (/3) = the OPP clock; branches MVS0C +0x804c, MVS0C free-run +0x805c, MVS0 +0x807c, MVS0 free-run +0x808c; AHB/XO/sleep +0x80a4/d4/f8 (kept on by Linux) | DT, videocc, gcc | verified (source) |
| GDSCs | MVS0C +0x8034 (controller, "venus"), MVS0 +0x8068 (codec, "vcodec0", child of MVS0C, hardware-controlled once running) | videocc | verified |
| Rails, OPPs | MMCX + MXC (VIDEO_CC itself sits on both, at low_svs); OPPs 240/338/420/444/533/570/630 MHz, 240 = MXC svs + MMCX low_svs_d1 | DT | verified |
| Bus | `cpu-cfg` (APPSS → VENUS_CFG, Linux 1 MB/s) and `video-mem` (`qnm_video_mvp` → EBI1: BCM **MM1** + SH0/MC0/ACV); Linux's DDR table: 1080p30 294, 1080p60 567, 4K30 826, 4K60 1608 MB/s. The display rides MM0, so nothing keeps MM1 up for video | DT, interconnect, iris | verified (source); MM1 at XNU: needs-boot |
| SMMU | apps SMMU `15000000` (DT `dma-coherent`), streams **0x1940, 0x1947** mask 0 (one Linux domain); the core is `dma-coherent`; device addresses below **0xe0000000** (`dma_mask`) | DT, iris | verified |
| Content protection | after authentication, SCM MP_VIDEO_VAR (`0x42000c08`) with CP `[0, 0x25800000)`, non-pixel `[0x01000000, 0x25800000)` | `iris_firmware.c`, scm | verified (source) |
| Firmware | `qcom/sm8750/vpu35_4v.mbn` (DT `firmware-name`), 2 409 160 bytes, signed ELF32, the stock odm_a `firmware/vpu35_4v.mbn` (OS3.0.303.0); Linux's generic `qcom/vpu/vpu35_p4.mbn` is not piano's (vendor-signed) | DT, pin, K90 note | verified (pin); generic one refused: inferred |
| Carveout | `video@9c300000`, 8 MiB, no-map (the node's memory-region), outside XNU's DRAM window | DT | verified |
| PAS | ID **9**: init_image, mem_setup, segments, auth_and_reset; suspend/resume SET_REMOTE_STATE(0/1, 0) (already in Sm8750Scm); unload PAS shutdown 9 | `iris_firmware.c` | verified (source) |
| Thermal | zone `video` = TSENS2 (`c22a000`) sensor 15: hot 120 °C, critical 125 °C (PianoThermal reads it) | DT | verified |
| State at XNU | expected cold (nothing before XNU runs video); SMMU entries for 0x1940/0x1947, free groups and banks, IDR0.CTTW | level 1 | **needs-boot** |

## 3. Ubuntu as the specification

- **Core init** (`iris_core.c:iris_core_init`): queues, power, firmware, boot, HFI. *Queues* (`iris_hfi_queues_init`): a 456-byte
  table (288-byte header, three 56-byte queue headers: command, message, debug), three 1 MiB queues of variable-size packets (indices
  in 32-bit words, first word = size), a 4 KiB SFR whose first word is its size; write-combined, device-addressed. *Power*
  (`iris_vpu_power_on`): bandwidth votes, controller (MVS0C GDSC, AXI1, MVS0C free-run, MVS0C), codec (MVS0 GDSC, AXI0, MVS0
  free-run, MVS0), the OPP rate, WRAPPER +0x88 = 0, A2H/watchdog unmasked. *Firmware* (`iris_fw_load`): the MDT into the carveout,
  PAS 9, MP_VIDEO_VAR. *Boot* (`iris_vpu_boot_firmware`): UC_REGION address and size (1 MiB aligned), QTBL address + enable, SFR,
  SPARE_0 = 1 (VPU 3.5), CTRL_INIT = 1, SCIACMDARG3 = 1, poll CTRL_STATUS (≤ 1000 x 50–100 µs; error 0x4 = bad UC region),
  H2X interrupt on, X2RPMH = 0; then the codec GDSC in hardware mode.
- **HFI gen2** (`iris_hfi_gen2_packet.h`, `_defines.h`): a 32-byte header {size, session, header id, 4 reserved, packet count} and
  32-byte packets {size, type, flags, payload info, port, packet id, 2 reserved} + payload. Host flags: interrupt 1, response 2,
  non-discardable 4, get-property 8; firmware flags: success 1, information 2, session error 4, system error 8. Ports: none 0,
  bitstream 1, raw 2. Commands 0x0100000x (INIT 1, OPEN 3, CLOSE 4, START 5, STOP 6, DRAIN 7, RESUME 8, BUFFER 9, SUBSCRIBE_MODE b,
  SETTINGS_CHANGE c, PAUSE 0x11), properties 0x03000xxx, session errors 0x0400000x, watchdog 0x05000001, information (DRAIN_LAST,
  PSC_LAST) 0x0600000x. A buffer is 64 bytes {type, index, IOVA (4 KiB aligned), offset, size, data offset, data size, timestamp ns,
  flags}. A command is written to the command queue, then IC_SOFTINT rings the core; answers raise SPI 174.
- **Handshake** (`iris_hfi_gen2_command.c`): INIT (payload `HFI_VIDEO_ARCH_LX`, plus the UBWC properties of `ubwc_config.c`'s
  SM8750 entry: 8 channels, HBB 16, swizzle levels 2 and 3, bank spreading), then IMAGE_VERSION as a get-property (the answer is
  the firmware's version string), then inter-frame power collapse; the reply within 1 s.
- **A decode session** (`iris_vdec.c`, `iris_hfi_gen2_command.c`): OPEN (session id) → CODEC → default header → the input
  properties (decode-order output, resolution, crop, bit depth, coded frames, minimum output count, POC type, profile, level,
  colour; per-codec lists in `iris_hfi_gen2.c`) → PERSIST, then BIN/COMV/NON_COMV/LINE/PARTIAL internal buffers (sizes:
  `iris_vpu_buffer.c:iris_vpu33_buf_size`, per codec and resolution) → SUBSCRIBE_MODE + START(bitstream) → bitstream buffers →
  the firmware's SETTINGS_CHANGE (the port-settings change = V4L2 SOURCE_CHANGE) → the output properties (OPB, NV12/P010, linear
  stride/scanline, width aligned to 128 and height to 32) → DPB buffers → START(raw) → raw buffers; buffer-done packets return each
  (LAST/PSC_LAST flags); DRAIN for the end of stream; STOP, CLOSE.
- **Power** (`iris_hfi_common.c`): runtime suspend 1.5 s after the last use: PC prep, wait PC_READY + WFI, SET_REMOTE_STATE(0),
  power off (codec: NoC LPI handshake retried up to 1000 times, bridge reset; controller: X2RPMH masked, CPU NoC / video-ctl NoC /
  debug-bridge LPI, clocks, GDSC, resets). Resume: power on, SET_REMOTE_STATE(1), boot again: **no re-authentication**.
- **Fixes Ubuntu carries** (`ubuntu-history/kernel.md` §1.10), behaviours PianoVideo needs from the start: v0.3.0 unmatched
  timestamp metadata discarded, no interrupt work after removal or during power-off; v0.6.0 decode-order output and larger capture
  pools on gen2 (K:1c507b3d51b9, K:e6537c292a51); v0.10.0 a drain finishes when the last buffer has no payload (K:4d00f8297193),
  encoder key-frame requests (K:1cb49011cf28).
- **Acceptance** (`device/media/iris-check.py`): one bounded H.264 decode, every frame's MD5 against a host `framemd5`: level 5's
  criterion.

## 4. UniXNU template to build on, and the design

| Piece | Base (licence) | Use |
|---|---|---|
| VideoToolbox side | the VM-era VideoToolbox paravirtual files, source UniXNU-droidvm `706338e`: `package/DroidVMVTParavirt.cpp` (the service), `DroidVMVTHost.cpp` (the host), `DroidVMVTWire.h` (the wire format) + its patch series 0001–0017 (BSD-2-Clause-Patent) | vendored as `package/PianoVideo/vt/`; native renames, boot control and the V0b stub are listed in UPSTREAM (level 6) |
| Its transport | its virtio-media transport file | **replaced** by PianoVideo's Iris backend, the same 14 functions |
| PAS, MDT, firmware file | PianoPower `Sm8750Scm.c` (MIT, copied; MP_VIDEO_VAR and PAS shutdown through its generic call), `QcomMdt.c`, `FatVolume.c` (copied); the shared run-time firmware loader (`package/common`, [kc-space.md](../kc-space.md) §6: a pin in the kext, the file from a RAM image's addon or the ESP) | level 3: built (`VideoFirmware.h`, §12.2) |
| Rails, bandwidth | PianoA830 `Sm8750Rpmh` (MIT): its cmd-db layout, corner rule and TCS protocol re-expressed in `VideoRpmh.h` under its DRV-2 owner word: `mxc.lvl`, `mmcx.lvl`, MM1 (not SH0/MC0/ACV: §12.1) | level 2: built (§12.1) |
| SMMU | PianoUsbHost `SmmuIdentity.h` (plan/apply, bank search, TLB maintenance) and PianoDpu `DpuSmmu.h` (the 32-bit-input table, SEP 7, faults), re-expressed in `VideoSmmu.h` | level 4: built (§12.3) |
| IRQ, power | `package/common/PianoIrq.h`, `package/include/PianoPowerContract.h` | levels 4, 7 |

The ADSP is not involved (the core runs its own firmware); PianoPower's DSP contract does not apply.

**4.1 The VideoToolbox verdict: reuse it, unchanged above the transport.** The VM's frontend is Apple's own: VideoToolbox's
ParavirtualizedVideoDecoder/Encoder and `VideoToolboxParavirtualizationSupport.framework` in userspace, which open the kernel service
`AppleVideoToolboxParavirtualizationDriver`. The service file *is* that service: the user client the framework expects
(selector 0: send buffer + IODataQueue; 1/2: register/deregister a session UUID; 3: send a message with ≤ 5 IOSurface IDs; 4:
drained; notification port 0). The host file is the "Mac host" half: it answers VideoToolbox's `vtpv` messages (decs/encs,
+dec, dsta/dsts, dfrm, dfin, dinv, Dbuf/Demt; big-endian atoms, the wire header) and runs each session as a client of a
**stateful V4L2 codec**. Nothing in either file is virtio-specific: the device side is 14 `extern "C"` functions
(`droidvm_media_start/devices/caps/card/is_up/open/close/ioctl/ioctl_sg/mmap/munmap/set_sink/set_fast`). So **no new user client and
no new userspace**: the native kernel provides those functions with V4L2 stateful-codec semantics over Iris, which is what Linux's
iris driver gave the VM through crosvm's proxy, and the VM's 16 patches (0001–0017) come along (HEVC rated 1000, `dsts`, SEI skipping, HEIC tiles
refused, the 4K P010 direct pool, frame thinning, session lifecycle). The subset the host file uses: ENUM_FMT, ENUM_FRAMESIZES,
G_FMT/S_FMT (MPLANE), REQBUFS/QUERYBUF/QBUF (USERPTR only; QBUF carries the physical page list), STREAMON/OFF, G_CTRL
MIN_BUFFERS_FOR_CAPTURE, S_CTRL display delay, SUBSCRIBE_EVENT (EOS, SOURCE_CHANGE), DECODER_CMD START/STOP (ENCODER_CMD later);
events DQBUF/EVENT/ERROR through the sink in virtio-media's layout; card "Iris Decoder"/"Iris Encoder" (0013 matches the role);
payloads in the 64-bit Linux layouts it already builds. Natively the page lists become SMMU entries (no crosvm copy), and VTHost
copies each decoded frame into the player's IOSurface as in the VM, or decodes in place (its direct mode).
Rejected: an AppleAVD-compatible service (AVD's userspace drives Apple's own hardware), a private VideoToolbox decoder plug-in (its
method tables would need Apple binary reverse engineering), MediaExtension decoders (public, but for formats the system lacks, in
apps that opt in; inferred from Apple's documentation). The 27.3 guest gate is established from code: `VTParavirtualizationIsRunningInGuest`
dispatches once to `VTParavirtualizationGuestSupportSetUpWithHandlers`; the support framework matches the registry class
`AppleVideoToolboxParavirtualizationDriver`, then opens its `IOUserClientClass` `AppleVideoToolboxParavirtualizationUserClient`
with `IOServiceOpen` type 0. This path does not read `kern.hv_vmm_present` or a guest boolean property, so a kext can satisfy it
truthfully while leaving `kern.hv_vmm_present=0`.

**4.2 Memory: a translating bank of our own.** The core addresses 32 bits below 0xe0000000, and every XNU page is above 4 GiB
(0x8c0000000..0xc00000000), so bypass cannot work: the streams need a stage-1 bank with XNU's tables. It is the same root problem as
audio, sensors (32-bit DSP addresses) and the DPU (32-bit fetch); here it is the easy case: Linux under the same Gunyah programs
translating banks for 0x1940/0x1947 itself and decodes (Ubuntu ships it), and the core is cold, so a fresh group per stream and a
fresh bank replace nothing (the DPU must reprogram its live bank in place). Window: IOVA `[0x80000000, 0xe0000000)` (above the CP
range MP_VIDEO_VAR declares, below the limit): the UC region (queues + SFR, 1 MiB aligned) first, then per-session internal buffers,
then client buffers mapped at QBUF (16 KiB XNU pages = four 4 KiB entries) and kept per buffer index until REQBUFS 0 or close. A 4K
P010 session (6 capture buffers of 26.7 MB, its DPB and internal buffers) needs a few hundred MiB: two fit. Tables: 4 KiB granule,
T0SZ 32, in XNU memory (TCR2 PASize ≥ 36 bits, as dpu.md); the core and this SMMU are `dma-coherent`, so entries are normal
write-back inner-shareable and, if IDR0.CTTW reads 1 (level 1 prints it), walks are coherent; otherwise tables are written uncached
(dpu.md's rule). A coherent data path also means VTHost's copy reads a cached mapping (the DPU's copy reads uncached: 34 ms a frame).
**Coordination:** one portable `package/common/SmmuWindow.h` (table builder, window allocator, map/unmap of page lists, TLBI by VA +
sync, fault readout) for PianoDpu (B1b) and PianoVideo, and one registry with a lock word for free stream-match groups and banks
(PianoWifi already takes a free group for 0x1401: two kexts must never take the same one).

**4.3 Firmware at run time** (built: §12.2). The owner's own `vpu35_4v.mbn` (the stock odm_a file, the copy the Ubuntu project
pins) is pinned in the kext as `PianoVideo/vpu35_4v.mbn` and loaded at run time by the shared firmware loader
([kc-space.md](../kc-space.md) §6): from a RAM image's addon, or the installed ESP's `EFI/UniXNU/firmware/PianoVideo/vpu35_4v.mbn`,
size and SHA-256 checked against the pin. The build stages the file into the ignored `out/firmware/PianoVideo/` and the image
builders place it; never in this repository or in the kext; never disassembled (data for the secure world's loader). Later, an
odm_a reader would serve video, audio and sensors alike. The load is PianoPower's sequence with PAS 9, holding the **kernel-wide
secure-call lock** (`../scm-lock.md`, tag `VIDE`) from init_image through auth_and_reset and MP_VIDEO_VAR, and around every other
request (no two secure calls at once: the K90's rule, `charging.md` §5a's stall); PianoVideo.json then names PianoA830 as a library.

**4.4 The backend's states.** Per instance: the V4L2 state machine (Linux's `iris_state.c/.h` as the specification: deinit, init,
input streaming, output streaming, streaming, error; sub-states first source change, resolution change and its last buffer, drain
and its last buffer, input/output pause) over one HFI session; a core thread owns the
queues (message queue drained on SPI 174, polled while starting), debug queue drained to the log (rate-limited), SFR printed on a
watchdog or system error; a system error closes every session with ERROR events (VideoToolbox then falls back to software) and the
core is recovered by power off + boot (the firmware stays in the carveout).

**4.5 Power** (*inferred* until measured). Linux's own formula (`iris_vpu3x_vpu4x_calculate_frequency`, SM8550-class caps: 200 VPP
cycles per macroblock over 4 pipes, 489 583 firmware cycles per frame) asks ~16 MHz for 1080p30 H.264 and ~120 MHz for 4K60: every
common stream runs at the **lowest OPP, 240 MHz** (MXC svs, MMCX low_svs_d1), with Linux's DDR votes of 294 MB/s (1080p30) to
1608 MB/s (4K60). Software decoding costs, by the K90 analysis's estimate (§3.3 there), 15–60 % of an Oryon core for 1080p30 H.264,
1.5–2x that for HEVC/VP9, ~4x for 4K (4K60 HEVC is not real-time in software).

| Case | Software (CPU) | Iris |
|---|---|---|
| 1080p30 H.264 | ~0.3–0.8 W | core at 240 MHz + 0.3 GB/s DDR: ~0.05–0.15 W; the NV12 copy (3.1 MB a frame, cached) ≈ 1 % of a core |
| 1080p30 HEVC / VP9 | ~0.5–1.5 W | the same as H.264 |
| 4K HEVC, 30–60 fps (the Aerials) | several W, frames dropped at 60 | ~0.15–0.4 W; the P010 copy (26.7 MB a frame) is 0.8–1.6 GB/s each way: the direct mode matters |
| Nothing playing | 0 | 0: collapsed 1.5 s after the last frame, the firmware kept; resume is power on + SET_REMOTE_STATE(1) + boot, no PAS |

Measured as `power.md` §6.4: one 1080p30 H.264 and one HEVC clip looped in QuickTime, fixed brightness, no cable,
`piano-video=0` against `=6`, the mean of PianoPower's 10 s battery lines from minute 2 to 9, and the decoding process's CPU in `top`.

## 5. IOKit family / ABI

Level 1: `PianoVideo : IOService` on IOPlatformExpertDevice, registers nothing; properties `piano-video`, `piano-video-survey` (one
line). Levels 2-4 add `piano-video-bringup` (one line: §12.8's summaries). Level 6: the vendored transport registers `AppleVideoToolboxParavirtualizationDriver` on IOResources (IOUserClientClass
`AppleVideoToolboxParavirtualizationUserClient`: Apple's names, because the sandbox lets a process open only the user clients it
names) and the descriptor gains `com.apple.iokit.IOSurface`. The vendored files keep their `droidvm_*` symbol names (the native
kext rule checks bundle, name and IOClass, not symbols). PianoVideo joins the power contract: off = every session paused and the core
collapsed, votes dropped; on = nothing until the next session.

## 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-video` | 0 | 1 the read-only survey; 2 + V1 power on/off; 3 + V2 the firmware (PAS 9); 4 + V3 the SMMU bank and the HFI handshake (all built, §12); later 5 + the embedded H.264 self-test, 6 + the VideoToolbox service (§8) |
| `piano-video-cc` | 1 | 0: VIDEO_CC and the core are not read by the survey (the fallback if a load there stalls) |
| `piano-video-smmu` | 2 | 0 skip the SMMU survey; 1 its global pages; 2 also the banks of matching groups |
| `piano-video-carveout` | 0 | 1: also load the first 32 bytes of the carveout `9c300000` (memory XNU does not own) |
| `piano-video-delay=S`, `-repeat=S`, `-samples=N` | 0, 0, 3 | wait first; read the clock words again every S s, N times |
| `piano-video-cycles=N` | 1 | levels 2 and 3: power cycles (the test plan: 20 for V1, 3 for V2); at most 1000 |
| `piano-video-hold-ms=MS` | 50 | levels 2 and 3: how long each cycle keeps the core powered (with the firmware loaded at 3) |
| `piano-video-idle-ms=MS` | 1500 | level 4: the collapse delay after the last activity (Linux's autosuspend: 1000 + 500 ms) |
| `piano-video-resumes=N` | 0 | level 4: resume / ping / idle / collapse cycles after the first collapse (the test plan: 10) |
| `piano-video-unload=1` | 0 | level 4: end with Linux's deinit (resume, PAS shutdown, off, the bank and UC region released); 0 leaves the steady state (collapsed, firmware kept) |
| `piano-video-stop=STEP` | 0 | levels 2-4: power everything down after step STEP (§12.6's numbers): the bisecting aid |
| `piano-video-mmcx-floor=VLVL` | 0x140 | the MMCX vote is never below this and never lowered under it (NOM_L1: the boot display's need, §12.1); 0 only once a native display votes its own |
| later: `piano-video-codecs=MASK`, `piano-video-direct` | 3 (H.264, HEVC), 0 | which codecs `decs` offers (4 = VP9); decode into IOSurfaces |

## 7. Kernel collection

`tooling/k27/build-native-piano-video.sh`: the secure-call lock's own tests (`package/include/tests/run.sh`), the shared firmware
loader's tests (`package/common/tests/run-firmware-tests.py`), the copies checked byte for byte against their originals
(FatVolume, QcomMdt, Sm8750Scm: PianoPower's), host tests `tests/video_host.cpp` (level 1: the gate at each step, the allowlist of
loads, the two registers never loaded, no word loaded twice, the PLL/RCG/divider/OPP decoders, CTRL_STATUS and the core states,
the SMMU scan with free groups and banks in use) and `tests/bringup_host.cpp` (levels 2-4, §12.7; with the owner's
`vpu35_4v.mbn` when it is on this Mac, its pin and headers too), then the pin (`tooling/persist/firmware.py stage --kext
PianoVideo`: the generated pins header and source; with the owner's copy here, the file staged into the ignored
`out/firmware/PianoVideo/`), the objects (`PianoVideo.cpp`, `FatVolume.c`, `QcomMdt.c`, `Sm8750Scm.c`, the shared loader's
`PianoFirmware.cpp`/`PianoFirmwareCore.c`/`PianoFirmwareZlib.c`, the generated pins, the glue) and the import check: 340 kernel
imports, 4 from the libraries (`_native_ufs_part_find`, `_native_ufs_part_read` from NativeUfs for the loader's ESP source;
`_sm8750_rpmh_drv2_owner`, `_sm8750_scm_owner` from PianoA830), 0 unresolved. `PianoVideo.json` names
`com.unixnu.driver.NativeUfs` and `com.unixnu.driver.PianoA830` as libraries; both are linked before PianoVideo in
`configs/k27-native-kexts.txt`, so every `build-native-image.sh` links it. The link with the whole list and its result: §12.7.

## 8. Hardware test plan and the ladder (RAM boots; V0 unattended, the rest attended)

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-video=1" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-video1 && cp -R out/ram out/ram-video1
python3 tooling/native/usb-console.py --capture 1000 &   # then: tooling/native/native-loop.sh out/ram-video1/boot-piano-macos-ram-A-bundled.img
grep -a "PianoVideo" logs/usb-console/<newest>-port0.txt
```

**V0 expected** (nothing changes; `...` = values to record):
```
PianoVideo: start: piano-video=1 cc=1 smmu=2 carveout=0 delay=0 repeat=0 samples=3 (Iris 0xaa00000, VIDEO_CC 0xaaf0000, SPI 174, streams 0x1940/0x1947, carveout 0x9c300000)
PianoVideo: level 1: loads only, nothing written
PianoVideo: stage 1: GCC video branches (read only)
PianoVideo: gcc: video ahb ........ (on|off) xo ........ (..) axi0 ........ (off) axi1 ........ (off)
PianoVideo: stage 2: VIDEO_CC behind GCC's video AHB (read only)
PianoVideo: videocc: pll0 mode ........ (not locked, not running) ... L 0 alpha 0000 -> 0 MHz; mvs0 rcg ... (root off, source xo, ...) ...
PianoVideo: videocc: gdsc mvs0c ........ (off) cfg ..., mvs0 ........ (off...) cfg ...; branches mvs0c ........ (off) ...
PianoVideo: gate: controller GDSC (MVS0C) collapsed: the core is cold and not read (as expected before any video driver)
PianoVideo: stage 3: the core skipped: its gate is closed
PianoVideo: core: not read (controller GDSC (MVS0C) collapsed: ...)
PianoVideo: core: cold: the video core is off (GDSC collapsed or its AHB gated): level 2 powers it from cold, ...
PianoVideo: stage 4: SMMU streams 0x1940-0x194f and their banks (read only)
PianoVideo: SMMU scr0 ... idr0 ... idr1 60000053: N stream-match groups (F free), 83 banks (U in use by valid groups), 128 pages, coherent table walks yes|no
PianoVideo: SMMU streams 0x1940-0x194f: no stream-match entry          (or: group G smr ... s2cr ... (translate, bank B) ...)
PianoVideo: SMMU route: no stream-match entry for 0x1940/0x1947 (...): level 4 takes a free group per stream and a free bank ...
PianoVideo: summary: gcc ahb .. axi0 off axi1 off; videocc read; mvs0c off mvs0 off; pll0 off 0 MHz; core cold; smmu 0x1940 none 0x1947 none free F
PianoVideo: stage 6: done
PianoVideo: done: N MMIO loads, 0 stores; P pages mapped, 0 map failures
```
A hang after `stage 2` (no `videocc:` lines) means VIDEO_CC is not readable at its rails' boot levels: rerun with
`piano-video-cc=0` (GCC alone) and give level 2 the MMCX/MXC votes before the first VIDEO_CC load; a hang after `stage 4` is the
SMMU (`piano-video-smmu=0`; PianoDpu and PianoWifi read the same pages). A `core:` line other than `cold`
(a firmware already running) changes level 3 (shut it down first). Then once more with `piano-video-carveout=1`.

| Level | Arguments | Step | Pass | Effort |
|---|---|---|---|---|
| V0 | `piano-video=1` | survey (built) | the lines above, no hang, `0 stores` | done offline; 1 boot |
| V0b | `piano-video-vt=1` | the vendored VT frontend + VTHost with a capability-only stub backend listing H.264/HEVC and refusing `dsta` | **built, not yet run**; with `kern.hv_vmm_present=0`, the support probe should open `AppleVideoToolboxParavirtualizationDriver` / `AppleVideoToolboxParavirtualizationUserClient`, report guest=true, emit `decs`, then refuse stream start without affecting the desktop | 1 boot |
| V1 | `=2` | power (built offline, §12.1): MXC svs + MMCX at the display floor + BCM MM1, GCC AXI0/1, PLL0 720 MHz, MVS0C then MVS0 GDSC + clocks at 240 MHz; the survey's core readout with the gate open; off in Linux's order | CORE_POWER_STATUS bit 1, sane words, CTRL_STATUS 0; GDSCs off again; 20 cycles; the panel unaffected (§12.8 T1, T2) | 1–2 boots |
| V2 | `=3` | firmware (built offline, §12.2): the pinned file (the RAM image's addon, else the ESP), MDT, PAS 9 init/mem_setup/segments/auth_and_reset, MP_VIDEO_VAR under one hold of the secure-call lock; PAS shutdown | every SMC `ok`, no stall, 3 repeats (§12.8 T3) | 1–2 boots |
| V3 | `=4` | SMMU groups + bank, UC region, boot words, CTRL_INIT; INIT + IMAGE_VERSION; PC prep, collapse, resume (built offline, §12.3) | init without error, INIT answered with success, the version string, FSR 0; 10 collapse/resume cycles (§12.8 T4, T5) | 2–4 boots |
| V4 | `=5` | one H.264 clip in the kernel (our own 320x240, 10 frames, from ffmpeg's test source), the whole session of §3 | every NV12 frame's MD5 = the Mac's `framemd5`; no SMMU fault; close + collapse | 2–3 weeks (buffer sizes, the state machine); 5–10 boots |
| V5 | `=6` | the V4L2-semantics backend under the vendored VT files | `vtread.js`: `dinv: in 300 out 300 dropped 0`, frames equal to the source, UsingHardwareAcceleratedVideoDecoder true; QuickTime/Safari H.264; HEIC still Apple's; 30 min, no leak | 1–2 weeks; 3–6 boots |
| V6 | `=6` | HEVC Main/Main10 (P010, the Aerial), VP9 (`decs` entry `vp09`) | the VM's HEVC tests on Iris; VP9 if VideoToolbox takes it | 1–2 weeks |
| V7 | — | collapse after 1.5 s, bandwidth by resolution, the s2idle off state, the thermal hook; the A/B of §4.5 | collapse between clips; a measured saving; s2idle with a paused video | ~1 week |
| V8 | `=7` | encode H.264/HEVC through VTHost's encoder half (VPSS buffers, key-frame request) | 1080p and 4K AVAssetWriter output that decodes | 2–3 weeks |
| — | `piano-video-direct=1` | decode into IOSurfaces (NV12, P010) | no copy time at 4K60 | ~1 week |

Total about 10–16 weeks attended; H.264 in VideoToolbox (the first battery payoff) after V5, about 6–9 weeks in.

## 9. Fallback

Without `piano-video` and `piano-video-vt` nothing is mapped and VideoToolbox keeps Apple's software decoders. `piano-video-vt=1` publishes only the V0b service; the stub advertises H.264/HEVC and refuses stream start, so no Iris register or HFI work occurs. From level 6, a format the core cannot
take is refused at `dsta` (VideoToolbox then decodes in software); a session error or a core watchdog ends the sessions with ERROR
events (software again) and powers the core off. A hang or reset returns to the previous system (RAM boots, `piano-reset-after`).

## 10. Open questions

1. **Guest gate (resolved in V0b analysis):** 27.3 dispatches once to `VTParavirtualizationGuestSupportSetUpWithHandlers`; the
   support framework calls `IOServiceMatching("AppleVideoToolboxParavirtualizationDriver")`, `IOServiceGetMatchingService`, and
   `IOServiceOpen(type=0)`, using the service's `IOUserClientClass` `AppleVideoToolboxParavirtualizationUserClient`. It does not
   read `kern.hv_vmm_present` or a guest boolean property. V0b tests this with `kern.hv_vmm_present=0`; setting that sysctl remains
   out of scope because it changes other subsystems and costs the 'RGhA' framebuffer (`soft-metal.md`).
2. The state at XNU time (GCC video AHB, GDSCs, PLL0) and whether VIDEO_CC is readable at the boot rail levels → V0.
3. SMMU: entries for 0x1940..0x194f, free groups and banks, CTTW → V0; a fresh bank accepted under Gunyah → V3.
4. PAS 9 from XNU with the owner's file, under the secure-call lock (`../scm-lock.md`, built 2026-11-11) → V2 (built: §12.2;
   the tablet answers it).
5. Is the MM1 vote needed (the display uses MM0)? Built with it (§12.1: on while powered, off after). Whether DDR's own votes
   (SH0/MC0/ACV, left to PianoA830 and the boot loader) are enough for the handshake → V3; for decoding → V4/V7.
6. The internal buffer sizes (`iris_vpu33_buf_size`) re-derived exactly → V4 (INSUFFICIENT_RESOURCES or an SMMU fault otherwise).
7. VP9 through the paravirtual protocol (the VM never offered `vp09`) → V6.
8. Direct decode: does CoreVideo's pool give the core's layout (128-byte rows, 32-line scanlines, chroma right after)? → after V5.
9. VTHost's encoder half was only run against the phone's MediaCodec, never Iris → V8.
10. Protected content (secure sessions, CP buffers): not supported; such players fall back or refuse.
11. VIDEO_CC readable at the boot rails? V1 votes MXC/MMCX before its first VIDEO_CC access; the level 1 survey that runs first
    does not (V0's question 2 decides whether `piano-video-cc=0` belongs in V1's arguments).
12. A second kext taking a free stream-match group or bank at the same moment: each kext plans and writes in one go on its own
    thread; PianoVideo takes the highest free groups and the first free bank below the top. No registry yet (§4.2's plan) →
    before two of them run at once.
13. The firmware's answer times (CTRL_STATUS, INIT, the version, PC_READY) on piano: Linux's bounds are used (100 ms, 1 s,
    2.5 ms) → V3's lines give them.

## 11. Licence note

The Linux iris, Venus, clock, SCM, interconnect and SMMU drivers (GPL-2.0) are read as the specification only (offsets, fields,
sequences, sizes); nothing is copied. PianoVideo is BSD-2-Clause-Patent; its shape comes from PianoDpu (this repository); levels
2-4 copy PianoPower's FatVolume and QcomMdt (BSD-2-Clause-Patent) and Sm8750Scm (MIT, Adreno830's; the notice is in
`package/PianoVideo/LICENSE`) byte for byte, and re-express PianoA830's RPMh protocol (MIT), PianoUsbHost's SmmuIdentity.h and
PianoDpu's DpuSmmu.h (this repository) in their own headers. The VT
files to vendor are BSD-2-Clause-Patent (source in §4). `vpu35_4v.mbn` is the owner's data, read at run time, never committed or
disassembled; no Apple binary is reverse engineered (public headers, class and symbol names only). Provenance:
`package/PianoVideo/UPSTREAM`.

## 12. Levels 2-4 as built: V1 power, V2 firmware, V3 SMMU bank and HFI (2026-11-11, offline)

| File | What |
|---|---|
| `VideoOps.h` | the one access path: each (address, value) pair written is logged the first time (`write <block>+<offset> = <value> (<what>)`), later repeats only counted; read-modify-writes as Linux's `regmap_update_bits` (no write when nothing changes); every poll bounded (at most timeout / step + 1 reads); the step numbers and error names |
| `VideoRpmh.h` | cmd-db lookups (ARC level tables, BCM addresses), the corner rule, one ACTIVE-TCS transaction on apps DRV-2 under PianoA830's DRV-2 owner word (tag `VIDE`), the video's votes |
| `VideoPower.h` | V1: on and off in Linux's VPU 3.5 order at 240 MHz; the readout's verdict |
| `VideoFirmware.h` | V2: the pin, the image's place in the carveout, PAS 9 + MP_VIDEO_VAR under one hold of the secure-call lock, PAS shutdown |
| `VideoSmmu.h` | V3: two stream-match groups and a fresh stage-1 bank, our table for the UC region, the teardown |
| `VideoHfi.h` | V3: the queue table and queues, HFI gen2 packets, answers, the boot words |
| `VideoBringup.h` | the levels as one sequence over the owner's Hw (§12.5's rules) |
| `PianoVideo.cpp` | the kernel's Hw: MMIO pages on demand (explicit `ldr`/`str`), the cmd-db copy, the DRV-2 word, the firmware through `piano_fw_load` (the generated pin), SHA-256, the metadata buffer, the carveout and UC-region mappings, the secure calls (`Sm8750Scm`) |

The gate: the kext runs levels 2-4 only for `piano-video=2..4`, after level 1's survey (unchanged); `Bringup::run` refuses any
other level before its first access, so nothing is read or written by them below 2 (host-tested).

### 12.1 V1: power on and off at 240 MHz (`piano-video=2`)

The votes (apps DRV-2 under PianoA830's DRV-2 owner word: compare-exchange 0 → `VIDE`, 10 µs steps, at most 2 s; one ACTIVE TCS per
request in PianoA830's `Sm8750RpmhWriteActive` protocol, completion polled every 1 ms for at most 100 ms; a TCS that does not
complete is left as evidence and never reused):

| Resource | While powered | After the power-off | Why |
|---|---|---|---|
| `mxc.lvl` (ARC) | the first hlvl ≥ svs (0x80) | hlvl 0 | VIDEO_CC needs MXC low_svs, the 240 MHz OPP svs; MXC's other users (camera, CDSP) do not run natively |
| `mmcx.lvl` (ARC) | the first hlvl ≥ max(low_svs 0x40, the floor 0x140) | never lowered under the floor: with the default floor nothing is sent (the vote is sent once per boot) | the boot framebuffer's MDP at 575 MHz needs MMCX NOM_L1 (DT mdss OPP); nothing natively votes MMCX and a DRV-2 vote replaces the DRV's earlier one, so a lower vote could take the panel's rail away. MMCX's parent CX is not voted: it stays ≥ NOM_L1 for the display (gpu.md: PianoA830's floor), above MMCX's NOM_L1 |
| BCM `MM1` (enable mask) | `0x60000001` (commit, valid, x 0, y 1) | `0x40000000` (commit, valid 0) | the video path `qnm_video_mvp` → `qns_mem_noc_sf` is MM1's; the display rides MM0 |
| SH1, SH0, MC0, ACV, CN0 | not voted | — | SH1 (`qnm_mnoc_sf`) also carries `chm_apps`, the CPUs' own traffic: necessarily on. SH0/MC0/ACV are the DDR vote, one per DRV: PianoA830 (or the boot loader) holds it, at or above the display's 3.28 GB/s, far above what V1-V3 move (no frames); a shared DDR aggregator with PianoA830 is V7's (Linux's decode table: 294 MB/s for 1080p30). CN0 (`cpu-cfg`) is keepalive |

**Shared with the camera (2026-11-12).** MXC and MM1 are PianoCamera's too (its levels 2-3: MXC low_svs for CAMCC and its PLLs,
MM1 for `qnm_camnoc_hf`; it relies on the boot display's MMCX and only declares the floor). Both drivers' needs go through `package/include/PianoMmVotes.h` (words in PianoA830,
[camera.md](camera.md) §4.3.1): each records what it needs, the vote is the maximum of the needs with the highest MMCX floor any
client declared, only what changes is sent, under the helper's own lock (then the DRV-2 lock). With PianoVideo alone the commands
are the table's exactly (host-tested against the rules above over every sequence of up to six operations); with the camera
streaming, the video core's power-off gives MXC back to the camera's low_svs instead of 0 and sends nothing for MM1
(`tests/bringup_host.cpp` testSharedVotes).

The sequence (Linux's `iris_vpu_power_on`/`_off` with VPU 3.5's ops, and the clock framework underneath; bounds in brackets):

| Step | On |
|---|---|
| 10, 11 | the rails, then MM1 |
| 12 | GCC `gcc_video_ahb_clk` and `gcc_video_xo_clk` get CLK_EN if not set, CLK_OFF checked [200 µs]; never turned off (Linux's probe leaves them on) |
| 13 | once per boot, VIDEO_CC as its probe leaves it: PLL0's configuration (L 0x25 + CAL_L 0x44, alpha 0x8000, CONFIG_CTL/_U/_U1, USER_CTL_U; OUTCTRL off, standby, RESET_N), only when PLL0 is not running; the critical AHB/sleep/XO branches; DLY_ACCU_RED_SHIFTER_DONE (0x8074, 0x8040) and 0x9f24 bit 0; both GDSCs' wait values with HW control and SW override off (a GDSC found requested on is collapsed by the power-off) |
| 14 | MVS0C GDSC: SW_COLLAPSE cleared, CFG POWER_UP_COMPLETE [2 ms], RETAIN_FF |
| 15 | GCC AXI1; PLL0 (calibration once while PCAL_NOT_DONE, RESET_N, RUN, LOCK_DET [1.5 ms], outputs, OUTCTRL; a PLL0 found running must hold Linux's 720 MHz configuration, else refused); `mvs0_clk_src` on PLL0 / 1 (ROOT_EN forced [0.5 ms], CFG, UPDATE [0.5 ms], released); the read-only mvs0 divider must be /3 (else refused: the rails are voted for 240 MHz only); MVS0C free-running, MVS0C [200 µs each] |
| 16, 17 | MVS0 GDSC as 14; GCC AXI0; MVS0 free-running (memory enable cleared, its ack [200 µs], then the branch); MVS0 (voted halt, skipped in hardware gating) |
| 18 | WRAPPER_CORE_CLOCK_CONFIG = 0 |
| 19 | the readout: level 1's survey with the gate open; the verdict: both domains read, CORE_POWER_STATUS bit 1, no word all ones (a bus error's pattern; a 0 version word is recorded, not refused: its layout is only inferred for VPU 3.5), CTRL_STATUS 0, PLL0 locked, mvs0 at the 240 MHz OPP |
| 20 | codec off (only if CORE_POWER_STATUS says powered: clock config, the four pipes idle [20 ms], the MVP NoC LPI handshake [1000 tries] and ack [2 ms], the AHB bridge reset 3/2/0); MVS0 out of hardware mode, collapsed [2 ms], MVS0 off, free-running off (memory first), AXI0 off |
| 21 | controller off: X2RPMH masked, CPU NoC LPI [2 ms], video-ctl NoC LPI [1000 tries, 2 ms], debug bridge; MVS0C off, free-running off, the RCG parked on XO, PLL0 standby, AXI1 off, MVS0C collapsed [2 ms], the resets bus0/bus1/core/vcodec0_core (assert, 1 µs, deassert) |
| 22, 23 | both GDSCs read back collapsed (before the rails go: VIDEO_CC is not read without them); MM1 off, then the rails |

A wait that fails while powering on powers everything down at once; one that fails while powering off is reported (`power-off:
... (going on, as Linux does)`) and the rest still runs. Deviations from Linux, on purpose: it powers on at the highest OPP and
drops to the session's rate, V1 stays at 240 MHz; it votes `video-mem` at INT_MAX (clamped to 15 GB/s), V1 votes MM1 only; it
unmasks the A2H/watchdog interrupt, V1-V3 poll; GCC's critical branches get a CLK_OFF check here.

Repeated cycles: step 13 runs once; PLL0's calibration once; RETAIN_FF stays set after the first collapse, so cycle 2 writes the
two GDSC words with bit 11 set once more and from cycle 3 on no new pair is logged.

### 12.2 V2: the firmware (`piano-video=3`)

**Where the file comes from: the shared run-time loader** ([kc-space.md](../kc-space.md) §6). The kext carries only the pin
`PianoVideo/vpu35_4v.mbn` (2 409 160 bytes, SHA-256 `95c5882f9b66b93ad2635604b7db563cc310481e017fa6b82e0ce6cbb9eae01b`, the
Ubuntu project's `firmware/piano-iris.json`; committed in the build script and in `VideoHw.h`). `build-native-piano-video.sh`
stages the owner's copy (`PIANO_VIDEO_FW`, default `../xiaomipad-8pro-mainline/firmware/qcom/sm8750/vpu35_4v.mbn`) into the
ignored `out/firmware/PianoVideo/`; `build-ram-images.py` finds the pin in the collection and puts the checked file into the RAM
image's addon (`Payloads/kernel.macho.gpuaddon`, entry `EFI/UniXNU/firmware/PianoVideo/vpu35_4v.mbn`), and `build-esp.py` places
it at the same path on an installed ESP. At stage 30 PianoVideo calls `piano_fw_load` (the boot's addon first, else GPT `esp`
through NativeUfs and FatVolume, waiting up to 60 s for it), which returns the file only when its size and SHA-256 match the pin;
stage 31 checks the pin again with the kext's own SHA-256 (the two facts of `VideoHw.h`), and the memory is released after the
load. Not the vendor/odm partition: `odm_a` is a logical partition inside `super` (LP metadata) holding an EROFS image, which
nothing native reads (an LP parser plus an EROFS reader, compression included, for one file); the FAT `modem_a` does not hold the
video firmware on piano (the pin's source is odm). PianoVideo is the loader's first user in the collection (kc-space.md §7 plans
the same for PianoTouch, PianoA830 and PianoKeyboard); the loader's own tests (`package/common/tests/run-firmware-tests.py`, run
by the build) cover the pins, the addon, an mtools ESP image and the damaged cases.

**The image** (checked offline against the owner's file by the build when it is on this Mac): ELF32, 22 program headers, 15
loadable, the hash segment last, relocatable, span 0x800000 from 0xf500000 (exactly the carveout), metadata 5564 bytes, 1 354 984
bytes to store.

**The sequence** (Linux `iris_fw_load` + `qcom_mdt_load` as the specification): 30 the file; 31 the pin (before any secure call);
32 the headers (a hash segment, every loadable segment inside the carveout); 33 the metadata (headers + hash segment) in a
contiguous buffer, cleaned to the PoC; then **one hold of the kernel-wide secure-call lock** (`PianoScmLock.h`, tag `VIDE`) across
34 PAS init_image(9), 35 PAS mem_setup(9, 0x9c300000, span), 36 the segments (an 8 MiB write-combined mapping of the carveout,
8-byte stores, the memory-only tails zeroed, a barrier, unmapped), 37 PAS auth_and_reset(9), 38 MP_VIDEO_VAR (`0x42000c08`; CP
`[0, 0x25800000)`, non-pixel `[0x01000000, 0x25800000)`), each request also taking the lock through `Sm8750Scm`'s ops (lock order:
SCM mutex → hold mutex). The SCM budget is 30 s per yielding request (PianoPower's, for an authentication). A failure after
init_image is followed by PAS shutdown(9) inside the same hold (Linux does that after MP_VIDEO_VAR only; doing it after mem_setup,
the load or the authentication too lets the next try start from a clean PAS state). 39 PAS shutdown(9) at the end of a V2 cycle,
while the core is still powered (Linux unloads before it powers off). Every secure call has a line before it (with 20 ms for the
console) and one after, so a call that does not return is the last line.

### 12.3 V3: the streams' own SMMU bank and the HFI handshake (`piano-video=4`)

**V3 S1 lifecycle (vid-v3b, awaiting tablet verification).** The prior `vid-v3a` mailbox confirmed stage 42 rejected
SCTLR `0x1027` reading `0x27`; teardown then ended at an unsupported as-found CBAR TYPE0 store (the inferred SError cause).
FSR `0x400` has Format=2 and no valid fault bits. This revision uses the existing Wi-Fi and camera SMMU contracts.

**Evidence transaction.** Level 4 keeps the `vid-v2c5` power/PAS contract, with ten
resume/ping/collapse cycles and final unload by default; levels 2/3 retain their defaults. Explicit `piano-video-resumes=0
piano-video-unload=0` keeps the earlier steady state. The transaction adds no IRQ handler or IRQ-clear writes. The plan is
rechecked immediately before apply; a changed group, bank or competing SID route is refused before writing. This is a conflict
check, not a cross-kext atomic reservation: combined images must still ensure other bank allocators have finished.
A fingerprint of other routes and active S1 banks must remain stable through apply; both TTBR ASIDs are checked for conflicts.
Level 4 defers the initial device survey. It prepares queues/tables in local memory, completes c5 power stages 10–18, then
performs video bank/TLB operations and publishes both routes. `V3 lifecycle` records numbered phase results, `verify` records
want/got/mask/mismatch, and `store completed` is emitted after each SMMU store returns.

The `V3 bank` lines record plan/apply/release routes and SCTLR/CBAR/CBA2R/TCR/TCR2/TTBR0/TTBR1/MAIR. `V3 memory` records physical page
ends, the 1024-page software walk and scratch aliases; `V3 layout` records the table, all three queue headers and SFR bounds.
`CTRL_STATUS sample/final` records actual reads and elapsed microseconds (at most 1001 reads / 100 ms of requested delay).
`V3 TX/RX` records headers, packet fields and queue indices before parsing; `V3 doorbell` counts every event independently of
the deduplicated MMIO log. `V3 wait`, `DBG summary`, core/fault/SFR snapshots distinguish INIT-only, missing version, malformed
messages, command consumption, watchdog and translation faults. A missing response does not establish whether the CPU was released.

`V3 first-HFI PASS` requires INIT success, version and consumed commands, with no bank fault or watchdog. Subsequent PC/WFI
and every resume ping have their own result. `V3 teardown` reports PAS, power and SMMU outcomes; failed shutdown, power-off or
TLB restoration cannot produce an unloaded/ok summary. Failed stages snapshot readable MMIO and shared memory before teardown.
The traces are evidence hooks; only a tablet result can establish DMA reachability, HFI readiness or decoding support.

**The bank.** Plan (reads only): stage 1 with 4 KiB AArch64 tables (IDR0/IDR2), no extended stream IDs (sCR0.EXIDENABLE), the
geometry, an output size reaching XNU's RAM; refused if any valid group covers 0x1940 or 0x1947 (level 1 lists it: never
replaced); the two highest free groups (other kexts take the lowest); the first bank from banks − 2 down that no valid group
translates through and whose SCTLR.M is off. Table: four contiguous 4 KiB pages (one 16 KiB XNU page): L1[2] → L2, L2[0..1] → two
L3 tables mapping IOVA `[0x80000000, +4 MiB)`: the UC region's 770 pages (any XNU pages, read-write, Normal non-cacheable, AF), the
rest of the window on one zeroed scratch page; everything else faults. Walks non-cacheable over tables cleaned to the PoC (whatever
IDR0.CTTW says); the table is walked in software first (every page of the window must translate to its page). The bank: SCTLR 0,
CBA2R VA64, CBAR stage 1 / stage 2 bypass (Linux's MemAttr WB, BPSHCFG NSH), TCR2 PASize + **SEP 7** (SEP 0 faults every IOVA with
bit 31 set under this hypervisor family: dpu.md), TCR T0SZ 32, TTBR0 our table, TTBR1 disabled by EPD1; both TTBRs carry
ASID=bank, preserving all PA high bits. MAIR0 is 0x44. Clear valid stale faults, bank TLBIALL + TLBSYNC, then SCTLR
M/TRE/AFE/CFRE/ASIDPNE (no CFIE: FSR is polled). Require SCTLR bits `0x27`, allowing ASIDPNE to read zero; compare TCR2 fields
with mask `0x3801f`, explicitly recording tolerated TBI bits `0x60`. Verify both S2CR TYPE/CBNDX values, record their attributes,
then make both exact SMRs valid; global TLBIALLNSNH + sTLBGSYNC. TLB register stores are never 0.

Release first stops PAS while powered, invalidates and verifies both SMRs, restores only legal S2CRs (an unsupported baseline
such as `0x0300006e` is replaced with FAULT), clears SCTLR.M, and completes bank/global TLB maintenance before dropping the
video power/NoC dependencies. If trapped FAULT keeps the already accepted translation, release requires SMR invalid plus M=0
and completed TLBs. Unsupported CBAR TYPE0 is never restored on this S1-only SMMU; leave accepted CBAR `0x1f300`/CBA2R=1.
Only a legal as-found VA64 context can be restored. Unknown quiesce, detach or TLB completion retains DMA and available power
instead of reporting unloaded. FSR raw, Format and `FSR & 0xc00001fe` are recorded separately; only the masked bits are faults.
`tooling/native/video-v3-verdict.py RUN_FOLDER` checks console evidence or the recovered mailbox when capture failed, and keeps
the first firmware result (including its exact failing stage) separate from the lifecycle verdict.

**The UC region** (0x302000 bytes, write-combined CPU view, non-cacheable device view: no cache maintenance; barriers before a
write index and before the doorbell): the 456-byte queue table (version 0, size 456, qhdr0 288, qhdr size 56, three queues, name
`iris-hfi-queues`), three 1 MiB queues from +456 (headers: status 1, start IOVA, type q | 0x0101 << 16, size in words, variable
packets, watermarks 1, rx_req 1 except the debug queue), the SFR at +0x301000 (its size word 4096). Boot words (Linux's order):
UC_REGION_ADDR = QTBL_ADDR = 0x80000000, UC_REGION_SIZE 4 MiB, QTBL_INFO 1, SFR_ADDR = SFR + 1 (VIDEO_ARCH_LX), WRAPPER_SPARE_0 1,
CTRL_INIT 1, SCIACMDARG3 1; CTRL_STATUS polled [100 ms, 100 µs between reads] until non-zero: init done without an error code, else refused
(0x4: the UC region); H2XSOFTINTEN 1, X2RPMH 0; MVS0 GDSC to hardware mode.

**The handshake**: SYS_INIT (320 bytes: INIT with HFI_VIDEO_ARCH_LX and flags response | interrupt | non-discardable, then
SM8750's UBWC properties: 8 channels, MAL 32, HBB 16, swizzle levels 1/2/3 = 0/1/1, bank spreading 1), IMAGE_VERSION (a
get-property, 64 bytes), INTRA_FRAME_POWER_COLLAPSE 1 (68 bytes); header and packet ids from 0, one more each; IC_SOFTINT after
each. The message queue is polled every 1 ms [1 s]: a malformed message (sizes outside the 4 KiB response or the message), a
system error (flag or type range) or INIT without success fails; the version string (128 bytes, NULs as spaces, trailing spaces
cut) is logged; the debug queue's lines are logged (`fw: ...`, the first 32, then every 64th).

**Then the battery rule**: the core stays powered only while a session runs. After the handshake (V3's session) it collapses
`piano-video-idle-ms` (1500) after the last activity: prepare-PC (PC_READY already, or WFI + idle then SYS_PC_PREP and PC_READY
[2.5 ms] and WFI [2.5 ms]; not idle: tried again every 100 ms, ten times), SET_REMOTE_STATE(0, 0) alone, the power-off (the
firmware stays in the carveout; MXC back to 0, MM1 off). Resume (`piano-video-resumes`): power on, SET_REMOTE_STATE(1, 0), the boot
words again (no PAS), hardware mode, INTRA_FRAME_POWER_COLLAPSE, IMAGE_VERSION as a ping [1 s], FSR 0, then idle and collapse.
`piano-video-unload=1` ends with resume, PAS shutdown, bank/routes released while the buses remain on, power-off, then DMA free. Without it the
steady state is: both GDSCs collapsed, the rails at the boot's levels, the firmware in the carveout under PAS 9, the bank and the
3 MiB UC region kept for the next session (V5's).

### 12.4 Failures

V1/V2 retain their established power/PAS cleanup. V3 captures readable core/queue/SFR/fault evidence, resumes a collapsed core
before PAS shutdown, releases the S1 domain while dependencies remain on, then powers off and frees memory. A failed secure
shutdown retains firmware ownership flags. An unconfirmed shutdown, detach, TLB sync or power-off retains DMA and reports a
cleanup error alongside the original failure. The stage word ends
negative at the failed step (`PianoVideo: FAILED at stage N: <reason>`), and `piano-video-bringup` says the same in one line.

### 12.5 What every level keeps

Every register write logged once; every wait bounded (the brackets above; the DRV-2 lock 2 s; the secure-call lock its 30 s
bound; a TCS 100 ms; the loader's wait for the ESP 60 s, only when the boot has no addon); no store from the zero register; nothing written below `piano-video=2`; `piano-video-stop=STEP`
powers everything down after that step (§12.6).

### 12.6 Step numbers

10 rails · 11 MM1 · 12 GCC · 13 VIDEO_CC set-up · 14 controller GDSC · 15 controller clocks · 16 codec GDSC · 17 codec clocks · 18
preset · 19 readout · 20 codec off · 21 controller off · 22 GDSCs read collapsed · 23 votes off · 30 the file · 31 the pin · 32 the
headers · 33 the metadata · 34 init_image · 35 mem_setup · 36 segments · 37 auth_and_reset · 38 MP_VIDEO_VAR · 39 PAS shutdown · 40
SMMU plan · 41 UC memory and table · 42 bank and routes · 43 queues · 44 boot words · 45 CTRL_STATUS · 46 hardware mode · 47 the
three packets · 48 the answers · 49 idle · 50 prepare-PC · 51 SET_REMOTE_STATE(0) · 52 collapsed · 53 resume · 54
SET_REMOTE_STATE(1) · 55 boot words again · 56 the ping · 57 unload · 58 SMMU teardown · 59 done. `piano-video-stop` takes 10-19,
31, 33, 34, 36, 37, 41, 42, 46, 48 and 50 (other numbers are ignored).

### 12.7 Host tests and the link

`tests/bringup_host.cpp` runs the levels against one SoC model: GCC's branches; VIDEO_CC (a PLL0 that locks only when configured
and running, a shared RCG whose update completes only on a running source, GDSCs reporting power-up/down in CFG, branches that run
only inside a powered GDSC on a running RCG, the free-running clock's memory enable); the core's controller and codec registers,
refused (a violation) whenever their GDSC or clocks are off; VIDEO_CC refused while its rails or GCC's video AHB are off; apps
DRV-2 (TCSes that complete or not, any access without the DRV-2 lock a violation) with a cmd-db image; the boot display's MMCX
vote, which a lower DRV-2 vote would take away; the secure world (the hold at each call, the metadata blob and the carveout's
contents checked at auth_and_reset, the core powered); the apps SMMU and a firmware that reaches the queues only by walking our
bank's table (SEP 0 faulting bit-31 IOVAs), boots on CTRL_INIT when the UC region is right and answers in the message and debug
queues. Checked: nothing touched below level 2; 20 V1 cycles with the votes (MXC up/down 20 times, MM1 on/off 20 times, MMCX sent
once and never lowered), the order (rails before VIDEO_CC, MVS0C before its clocks, PLL0 before the RCG switch, MVS0C before MVS0,
the codec off before the controller, the RCG parked between them, the resets after MVS0C's collapse, MM1 off before the rails),
the write log (twenty cycles log what two log; one line per logged pair); GCC's AHB found off; ten injected faults (rails not
completed, the DRV-2 lock busy, an RSC write not read back, GCC stuck, either GDSC stuck, no PLL lock, the RCG stuck, a wrong
divider, a branch stuck), each failing at its step with everything off and no violation; MVS0 found requested on; PLL0 found
running at another rate (refused); the readout's verdict (a 0 version word recorded, all ones refused); the stop
argument at every V1 step; the off-side warnings (LPI denied, pipes busy, CPU NoC stuck) with the power-off completing; the
MMCX floor at 0; no or incomplete cmd-db; V2's three cycles (the four calls under one hold, the shutdown alone, PAS shutdown before
the codec's GDSC goes), the pin refusing another file and a one-byte change, images without a hash, outside the carveout or not
ELF, no file, each secure call refused (shutdown inside the hold where it belongs); V3 with ten collapse/resume cycles and the
unload (groups 99/98 and bank 80 chosen around a translating bank, the UBWC values, the version string, ids in order, eleven
collapses, the SMMU exactly as found afterwards, the memory freed), the steady state (the collapse 1.5 s after the handshake,
not before), thirteen V3 faults (an existing entry, no bank, no memory, a stuck TLB sync, no boot, the UC region refused, no
answer, INIT failed, a system error, no version, never idle, SET_REMOTE_STATE refused, the authentication refused); the ring
buffers across the queue's end, a full queue, oversized and zero-size packets, bad indices; the packets' exact words; parse() on
malformed, session, error and version messages; cmd-db lookups, the corner rule, the TCS layout, a busy and a stuck TCS; the SMMU
plan's refusals and the table's walk. With the owner's file on this Mac, also its pin and headers (§12.2).

Built 2026-11-12 (k27 kernel 0059, on main 45493f4): `check-kext-imports: 340 imports from the kernel, 4 from the libraries
(_native_ufs_part_find, _native_ufs_part_read, _sm8750_rpmh_drv2_owner, _sm8750_scm_owner), 0 unresolved`; `build-native-image.sh`
with the whole list: `com.unixnu.driver.PianoVideo: linked, ... 414 pointers`, `native kext rule: 23 native kexts linked, none
from outside this repository`, `firmware: 1 of 1 pinned file(s) in the addon` (the container 899 728 bytes);
`verify-ram-images.py`: the addon present and recorded, the ADT's `GpuAddon` slot, the entry
`EFI/UniXNU/firmware/PianoVideo/vpu35_4v.mbn` matching its pin in the payload's own collection, `PASS: 0 failure(s)`.

### 12.8 The batched tablet test list (RAM boots only; V0 first)

Prerequisites: V0 (§8) passed: VIDEO_CC was readable, the core cold, no SMMU entry for 0x1940-0x194f (if V0 needed
`piano-video-cc=0`, add it to every line below). No ESP update: the firmware travels in each RAM image's addon
([kc-space.md](../kc-space.md) §6). `build-native-image.sh` runs `build-native-piano-video.sh`, which stages the owner's copy into
`out/firmware/PianoVideo/` (`PIANO_VIDEO_FW` when it is not next to this repository); `build-ram-images.py` then prints
`firmware: 1 of 1 pinned file(s) in the addon` (about 0.9 MB compressed) and gives the payload's ADT the `GpuAddon` slot. If it
prints `pinned firmware not staged`, the copy was not found and stage 30 fails with `addon: none in this boot; esp: not found`.

Each test is one RAM boot (`tooling/native/native-loop.sh`, the USB console captured as in §8), built as
`PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 <args>"
tooling/k27/build-native-image.sh && cp -R out/ram out/ram-video<T>`; read with `grep -a PianoVideo logs/usb-console/<newest>-port0.txt`.
`...` stands for values to record.

| T | `<args>` | Pass |
|---|---|---|
| T0 | (none) | the image with the addon boots as before (the first firmware addon on piano: DarwinBoot refusing it stops before XNU with `EFI_COMPROMISED_DATA`; then `PIANO_FW_ADDON=0` and the ESP, §12.9); PianoVideo inert: no `PianoVideo:` line (it declines in `probe()`) and no `PianoFirmware:` line |
| T1 | `piano-video=2` | V1, one cycle: the lines below, the panel undisturbed, `stage 59: done` |
| T2 | `piano-video=2 piano-video-cycles=20` | V1, twenty cycles: `V1: cycle 20/20 ok`, the write count grows, the logged count stops after cycle 2 |
| T3 | `piano-video=3 piano-video-cycles=3` | V2: every `smc: ... ok`, three cycles, no stall |
| T4 | `piano-video=4` | V3: INIT answered, the version string, `fsr 00000000`, collapsed 1.5 s later, steady state |
| T5 | `piano-video=4 piano-video-resumes=10 piano-video-unload=1` | V3: ten collapse/resume cycles with a ping each, then unloaded |

**T1 expected** (after level 1's lines, which end `PianoVideo: done: N MMIO loads, 0 stores; ...`):
```
PianoVideo: levels 2-4: cycles=1 hold-ms=50 idle-ms=1500 resumes=0 unload=0 stop=0 mmcx-floor=0x140
PianoVideo: level 2: writes start here; every register write is logged the first time it is written
PianoVideo: rpmh: mxc.lvl 0x3.... (N levels, top vlvl 0x...), mmcx.lvl 0x3.... (N levels), MM1 0x5....; MMCX floor 0x140
PianoVideo: V1: 1 power cycle(s) at 240 MHz, 50 ms powered each
PianoVideo: stage 10: rails: MXC svs, MMCX at the display floor (RPMh, DRV-2 lock)
PianoVideo: write rsc+0xd00 = ........ (rsc IRQ_ENABLE: this TCS masked)              (... the TCS's words, each once)
PianoVideo: rpmh: rails up: mxc.lvl hlvl H (vlvl 0x80), mmcx.lvl hlvl H (vlvl 0x140, floor 0x140): 2 command(s), TCS T, completed after 1 ms
PianoVideo: stage 11: bus: BCM MM1 on
PianoVideo: rpmh: MM1 on (60000001): TCS T, completed after 1 ms
PianoVideo: stage 12: GCC video AHB and XO branches
PianoVideo: stage 13: VIDEO_CC set-up (PLL0 configuration, critical branches, GDSC wait values)
PianoVideo: write videocc+0x10 = 00440025 (pll0 L_VAL: 0x25 (720 MHz), CAL_L 0x44)    (when PLL0 is not running; ... about 15 more)
PianoVideo: stage 14: controller GDSC MVS0C on
PianoVideo: write videocc+0x8034 = ........ (mvs0c GDSC: out of collapse)            (the wait fields 0x226 at 23:12, bit 0 clear)
PianoVideo: stage 15: controller clocks: GCC AXI1, PLL0 720 MHz, mvs0_clk_src, MVS0C free-running, MVS0C
PianoVideo: write videocc+0x8004 = ........ (mvs0_clk_src CFG: PLL0 / 1 (720 MHz))
PianoVideo: stage 16: codec GDSC MVS0 on
PianoVideo: stage 17: codec clocks: GCC AXI0, MVS0 free-running, MVS0 (240 MHz)
PianoVideo: stage 18: WRAPPER_CORE_CLOCK_CONFIG = 0 (the core clock runs)
PianoVideo: stage 19: the readout with the gate open
PianoVideo: readout: pll0 locked 720 MHz, mvs0 240 MHz (OPP 0); gdsc mvs0c 8....... mvs0 8.......; core version ........ ctrl status 00000000 power status .......2 (codec powered) intr mask ........ tz cpu ........
PianoVideo: stage 20: codec power-off (Linux iris_vpu35_power_off_hw)
PianoVideo: stage 21: controller power-off (Linux iris_vpu35_vpu4x_power_off_controller)
PianoVideo: stage 22: both GDSCs collapsed
PianoVideo: stage 23: MM1 off, then the rails
PianoVideo: rpmh: MM1 off (40000000): TCS T, completed after 1 ms
PianoVideo: rpmh: rails down: mxc.lvl hlvl 0, mmcx.lvl stays at hlvl H: 1 command(s), TCS T, completed after 1 ms
PianoVideo: V1: cycle 1/1 ok (on max ... us, off max ... us; W writes, L logged)
PianoVideo: stage 59: done
PianoVideo: bring-up: V1 ok: 1/1 cycles; on max ... us, off max ... us; rpmh requests 4; writes W (L logged)
PianoVideo: bring-up done: ... MMIO loads, ... stores (L distinct writes logged, R repeats); P pages mapped, 0 map failures; DRV-2 requests 4
```
**T2**: the same, then `V1: cycle 5/20 ok`, `10/20`, `15/20`, `20/20`; `rpmh requests 80` (four a cycle: rails up, MM1 on, MM1
off, rails down; MMCX rides only the first); in cycle 2 the two GDSC words once more (RETAIN_FF kept: `write videocc+0x8034 = ...`,
`write videocc+0x8068 = ...`), no new `write` line after that.

**T3 expected** (V1's power-on lines, then per cycle):
```
PianoVideo: V2: 3 cycle(s): power on, PAS 9 with the pinned file (the boot's addon or the ESP), MP_VIDEO_VAR, 50 ms, PAS shutdown, power off
PianoVideo: stage 30: the firmware file (the boot's addon or the ESP)
PianoFirmware: EFI/UniXNU/firmware/PianoVideo/vpu35_4v.mbn: the boot's addon, 2409160 bytes (addon: ok)
PianoVideo: firmware: EFI/UniXNU/firmware/PianoVideo/vpu35_4v.mbn: ok from the boot's addon (addon: ok)
PianoVideo: stage 31: the pin: size and SHA-256
PianoVideo: firmware: 2409160 bytes, sha256 95c5882f9b66b93a...: the pinned vpu35_4v.mbn (OS3.0.303.0)
PianoVideo: firmware: ELF32, 22 program headers, 15 loadable, hash at 21, relocatable, span 0x800000 at 0xf500000, metadata 5564 bytes
PianoVideo: firmware: metadata 5564 bytes at 0x........
PianoVideo: smc: PAS init_image(9): calling
PianoVideo: smc: PAS init_image(9): ok (status 0, x0 0, x1 0, ... SMCs: ...; ... us)
PianoVideo: smc: PAS mem_setup(9): ok (...)
PianoVideo: firmware: 15 segments, 1354984 bytes stored at 0x9c300000 (+ 0x800000 mapped write-combined), unmapped
PianoVideo: smc: PAS auth_and_reset(9): ok (...)
PianoVideo: smc: MP_VIDEO_VAR (0x42000c08): ok (...)
PianoVideo: V2: cycle 1: the firmware is authenticated; CTRL_STATUS 00000000 (no HFI yet), tz cpu status ........
PianoVideo: smc: PAS shutdown(9): ok (...)
...                                                          (cycles 2 and 3 alike)
PianoVideo: bring-up: V2 ok: 3/3 cycles; on max ... us, off max ... us; rpmh requests 12; writes ... (... logged)
```
No `secure-call lock ... held by` line is expected (nothing else makes secure calls then); one with `free again` is fine, `TAKEN
OVER` is not.

**T4 expected**:
```
PianoVideo: V3: SMMU bank + UC region, power on, PAS 9, HFI boot and handshake; collapse after 1500 ms idle; 0 resume(s)
PianoVideo: smmu: idr0 ........ idr1 60000053 idr2 ........ scr0 ........: N groups (F free), 83 banks; groups G/G bank B PASize P
PianoVideo: dma: UC region 3153920 bytes in 770 pages (first 0x........), write-combined at 0x...; table 0x........
PianoVideo: smmu: table at 0x........ maps IOVA 0x80000000 + 0x400000 (770 pages of the UC region, the rest on scratch 0x........)
PianoVideo: write smmu+0x... = 00000000 (smmu bank SCTLR: translation off while programmed)           (... the bank and the two routes)
PianoVideo: stage 43: the queue table, three queues and the SFR
...                                                          (V1's power-on and V2's load, without the shutdown)
PianoVideo: write iris+0xa0064 = 80000000 (UC_REGION_ADDR: the queue table)                         (... the boot words)
PianoVideo: hfi: CTRL_STATUS ........: init done
PianoVideo: hfi: SYS_INIT (+7 UBWC properties): 320 bytes, header 0, 8 packet(s)
PianoVideo: hfi: IMAGE_VERSION: 64 bytes, header 1, 1 packet(s)
PianoVideo: hfi: INTRA_FRAME_POWER_COLLAPSE: 68 bytes, header 2, 1 packet(s)
PianoVideo: fw: ...                                          (the firmware's debug lines, if it writes any)
PianoVideo: hfi: INIT answered with success after ... us (... messages, ... packets)
PianoVideo: hfi: firmware version: ...                       (to record)
PianoVideo: stage 49: idle: the collapse delay after the last activity
PianoVideo: hfi: SYS_PC_PREP: 64 bytes, header 3, 1 packet(s)        (or none when PC_READY is already set)
PianoVideo: smc: SET_REMOTE_STATE(0, 0): ok (...)
PianoVideo: V3: collapsed after 1500 ms idle (collapse 1); the firmware kept
PianoVideo: V3: steady state: the core collapsed, the firmware in the carveout, the bank and the UC region kept
PianoVideo: bring-up: V3 ok: firmware ...; handshake ... us; collapse/resume 0/0; fsr 00000000; fw lines ...; writes ... (... logged)
```
**T5**: T4's lines, then ten times `smc: SET_REMOTE_STATE(1, 0): ok`, `hfi: CTRL_STATUS ........: init done`, `hfi:
INTRA_FRAME_POWER_COLLAPSE ...`, `hfi: IMAGE_VERSION ...`, `smc: SET_REMOTE_STATE(0, 0): ok` (`V3: collapse/resume 1/10 ok`, 5,
10), then `stage 57: unload ...`, `smc: PAS shutdown(9): ok`, `stage 58: ...`, `V3: unloaded: PAS 9 shut down, the core off, the
video streams released`, `bring-up: V3 ok: ...; collapse/resume 10/10; fsr 00000000; ...`.

What a failure line means and the next boot: `FAILED at stage 10/11` (`rpmh: ...` before it): DRV-2 busy or a TCS that never
completed: nothing was powered; record the TCS words. A hang right after a `stage 13`/`14` line: VIDEO_CC or the GDSC at these
rails: rerun with `piano-video-stop=13` and then `=14`. `FAILED at stage 15` with `mvs0 divider`: the divider is not /3: record
it (the rails must then be voted for that OPP). `FAILED at stage 30`: the loader's trail says which source failed (`addon: none in this boot`: the image was built without the staged copy; `another SHA-256 than the pin's`: a different file was staged); `FAILED at stage 31`: the loader returned a file the kext's own SHA-256 does not match (record both lines). A hang
after `smc: ...: calling`: the secure world did not return (the Gunyah watchdog resets the board; the RAM boot returns); rerun T3
with `piano-video-stop=33` to separate the file from the calls. `FAILED at stage 45` with `error 0x4`: the UC region refused
(record UC/QTBL/SFR words: the alignment or the SMMU); `no answer after 1000 polls`: the firmware did not start its HFI.
`FAILED at stage 48` with `smmu: FAULT ... fsr ...`: the bank or the table (record FAR); without a fault: the firmware's packets
(record `fw:` lines and the SFR). `FAILED at stage 50`: the firmware never idles (record CTRL_STATUS and the tz cpu word).

### 12.9 Risks

1. **The display's MMCX**: if the boot loader's MMCX vote sat on another DRV and was higher than NOM_L1, our NOM_L1 vote on DRV-2
   changes nothing; if it sat on DRV-2 and was higher, ours lowers it to NOM_L1, which the MDP at 575 MHz still has. T1's "panel
   undisturbed" checks it; `piano-video-mmcx-floor` raises the floor.
2. **DDR not voted for video**: the handshake moves a few kilobytes; whether the firmware's own boot needs more than the display's
   DDR level is unknown (V3 answers). Decoding needs the V7 aggregator with PianoA830 (one DDR vote per DRV).
3. **VIDEO_CC at the boot rails**: the level 1 survey reads VIDEO_CC before V1's votes (V0's open question 2).
4. **PAS 9 from EL1 under Gunyah**: the first PAS of this image from XNU; Linux under the same hypervisor does it (Ubuntu).
   MP_VIDEO_VAR is new to this project. Both run under the secure-call lock; a stall is the Gunyah watchdog's.
5. **A fresh SMMU bank for 0x1940/0x1947 under Gunyah**: Linux does it under the same hypervisor; PianoUsbHost's identity bank is
   the same shape (not yet run on the tablet either). SEP 7 is DpuSmmu's finding. A concurrent kext taking the same free group or
   bank is excluded only by timing (open question 12).
6. **Memory types**: the UC region is non-cacheable on both sides (no maintenance); the core is `dma-coherent`, so a cacheable
   device view (as Linux's) would also work; ours is the conservative choice.
7. **Bounds**: Linux's (100 ms CTRL_STATUS, 1 s answers, 2.5 ms PC_READY) may be short for a cold first boot of the firmware;
   V3's lines record the real times (open question 13).
8. **The steady state** keeps 3 MiB of wired memory and the bank after V3 when `piano-video-unload=0`, with nobody using them
   until V5: deliberate (the first session's resume costs no PAS), released by `piano-video-unload=1`.
9. **The first pin in the collection**: PianoVideo is the first kext that pins a file, so from now on every RAM image
   `build-native-image.sh` makes carries a firmware addon (about 0.9 MB) and its ADT the `GpuAddon` slot, whatever the boot
   arguments; no addon has booted on piano yet (kc-space.md §9). T0 checks it first; `PIANO_FW_ADDON=0` builds images without it
   (V2/V3 then read the ESP). `build-esp.py` with this collection needs the staged copy (or `--allow-missing-firmware`, after which
   stage 30 reports `not found` on that system).


## 13. V4 kernel single-frame self-test (tablet-confirmed)

`piano-video=5` adds one bounded session inside V3's verified lifecycle; the autonomous self-test and native VT mode are mutually exclusive.
The committed fixture is an own encode of constant Y/Cb/Cr=64/96/160 at 320×240: SPS/PPS/one Baseline IDR, 286 bytes,
SHA-256 `ed63e8ee3deeb819b059f5c8346f66da0592b1922eec0198a47a6efa33f96923`. Host reproduction and independent software decode
are checked by `package/PianoVideo/tests/fixture_host.py`; no external media or firmware is embedded.

OPEN/config/subscriptions/START BITSTREAM lead to one ETB and first PSC. Asynchronous OPEN/START/RESUME are accepted by
PSC/downstream evidence, with every error and packet ID logged. STOP/CLOSE and DRAIN retain their real response requirements.
The PSC minimum controls DPB count and aggregate COMV size (`43008*N+512`, N=1..64). Pre/post scratch uses separate storage,
including PARTIAL; release requests precede replacements and RESUME, while old storage remains owned until matching RELEASE_DONE.
The frame gate requires all five releases. RAW starts before DPB/FTB submission; at least max(N,2) RAW buffers accommodate LAST.

A separate 64 MiB IOVA arena follows the UC window, using the same bank/ASID and both video SIDs. Every backing page is wired,
checked for uniqueness and mapped at 4 KiB granularity; guards stay invalid. Thirty-six table pages cover both windows. The
fixture-sized HFI buffers use the pinned VPU35 size facts, not a general codec calculator. CPU data access uses one NC/WC view
following cached zero/clean; no live table edits or cached data writes occur during the session.

Success needs one nonempty, correlated, error-free RAW completion and 115200 active NV12 bytes hashing to
`bd230ec4a9cd22c274831d1b526c43d3ceb39a089257c3cb4114fbea0a8102dc`, excluding row/bottom padding (stride384, UV offset98304).
The pre-submit sentinel must differ. EBD, internal release and an empty LAST are not decoded pictures. DRAIN ACK/LAST, STOP RAW,
STOP BITSTREAM, CLOSE, idle/collapse and V3 PAS/quiesce/route/TLB/power teardown all remain required. The five-second transaction
budget and one-second command/cleanup waits preserve first-error evidence; unconfirmed PAS or domain release keeps all DMA.

The host model covers counts 1/4/7/64, command-first PSC/FBD headers, delayed scratch release, malformed packets, incorrect pixels,
addresses/timestamps, corruption, session/system errors, missing completions, READONLY and late replies. Expanded-arena lifetime
checks exercise the real portable V3 teardown on a nonresponsive session, including PAS/TLB failures. These are host checks;
the valid `vid-v4a` tablet capture confirms rel0039 session acceptance and pixel DMA with this exact golden.
QuickTime, Safari, IOSurface and VT selection remain separate consumer work.

## 14. V5a bounded native AVC VT bridge (tablet qualification pending)

The vid-v5a tablet run confirmed PV selection, client mappings and dsta delivery, then our driver returned -12910 before
native session open. The 27.3 QuickTime ImageDescription serializer includes a zero-u32 after its extension atoms;
the old validator rejected that valid four-byte tail. V5b recognizes it only at an atom boundary and at the exact end.
The exact CoreMedia tool descriptor and metadata-only query pass the shared dsta/dcan validator on the host; a Main-profile
candidate and malformed/hidden/duplicate/protected extensions remain rejected. Every dcan request and answer records its
format, session state, decision and transport result. The full VT-to-IOSurface decode still needs tablet qualification.

The vid-v5b tablet run confirms hardware session creation and positive/negative dcan. Its first malformed six-byte sample
reaches selector3 and is correctly refused before DMA, but the driver replies with an error without emitting that frame.
The 27.3 PV codec and decompression duct do not synthesize a callback for that codec error; the public asynchronous wait
then waits for the outstanding frame. The valid AU has not been submitted. V5c completes rejected tokens through the same
worker-owned Demt/ack path, independently of the single accepted AU. An empty finish remains nonterminal because public
WaitForAsynchronousFrames calls FinishDelayedFrames even after a rejection. Bounded rejection backpressure fails the transport,
whose guest error path emits its original frame. Every shared-send event and outbound queue decision is logged. No firmware,
pixel format, capability or native decode sequence changes; the same qualification tool and acceptance criteria remain.

`piano-video=6 piano-video-vt=1` publishes version6 avc1 only, with the explicit qualification name
`PianoVideo native AVC V5a qualification`. Apple's built-in ParavirtualizedVideoDecoder opens our type0 Apple-named
user client. The frontend consumes the actual length-prefixed sample (252 bytes for the own fixture), prepends the exact
proven SPS/PPS, and sends 287 caller bytes to Iris. Frontend/native input SHA-256 and the full caller frame token are logged;
no embedded input is substituted by the native transaction. HFI timestamps retain both token words.

A demand session reserves the exclusive core before firmware/DMA preparation. One worker drives V3 initialization and
V4 first PSC, internal releases, DPB/RAW, EBD and FBD. Terminal DRAIN ACK and LAST release a delayed first picture; dfin
completes that same terminal transaction and never restarts it. The returned retained arena subdescriptor names the same
scatter-backed ledger entry as its WC view and IOVA. RAW is held until the IOSurface copy finishes, separately from firmware
READONLY/internal references. Source layout is active320×240, stride384, scanlines256, UV98304, allocation147456.

The guest Dpba/Dbuf pool supplies a 420v IOSurface. The frontend opens macOS IOSurfaceRoot for the decoder's task,
checks the native lock result, exactly two uncompressed planes, dimensions, strides and allocation extents, maps that task's
surface into the kernel, and copies only active rows. Demt success follows successful copy. Every accepted input completes
once; request waits run on the worker so selector3 remains free to deliver replies. Invalidate cancels, joins the worker,
settles pending work and releases surfaces before native close. Unconfirmed PAS/routes/TLB cleanup retains all DMA and
fences subsequent sessions.

Only the proven Baseline SPS/PPS, 320×240 progressive8-bit/no crop, one IDR are accepted. dcan returns true only for the
same supported configuration; tiles, HEVC, changed format, P/B, malformed/oversized input, a second AU and concurrent native
sessions are refused. Host checks cover external input/token correlation, descriptor lifetime, parser/layout overflow,
exactly-once cancellation, and failed-quiesce retention. `tooling/native/build-video-v5a.sh` builds the ordinary VT tool;
its fresh hardware+PV-required session must return one IOSurface-backed420v callback with the §13 golden, reject unsupported
inputs, and finish/invalidate safely. Selection, input/frame correlation and native completions are required alongside the
hardware property. This endpoint is opt-in RAM qualification, not general AVC/HEVC, QuickTime, Safari or wallpaper rollout.

## 15. Bounded persistent AVC qualification (V6; hardware result pending)

The one-AU bridge is now a persistent exclusive session. The supported input is one complete picture per sample in a real
avcC configuration: 320×240, 8-bit 4:2:0 progressive, no crop, Baseline/Main, POC0/2 and bounded references/reordering.
Input remains in demux/DTS order. Firmware timestamps carry opaque frame identities, independent of PTS; callbacks recover
Apple's original source reference and OutputPTS. Decode-order output is explicitly enabled. No default app rollout is implied.

Eight frame credits and at most 8 MiB of software AU storage feed two independently owned 7,077,888-byte DMA inputs.
The guarded 141-entry aperture is 56,623,104 bytes. EBD releases an input slot; only output/error/no-output/cancel retires a
frame. First PSC supplies actual N for split DPB and COMV. RAW FBD holds a separate loan until the checked IOSurface copy
finishes, then FTB recycles that slot. Each output opens and closes its own owner-task IOSurfaceRoot client and mapping.
A guest Dbuf/Demt wait never stops HFI pumping. Unexpected internal returns, changed SPS/second PSC, unknown tokens,
READONLY reuse, unsupported wire flags or unsafe quiesce fail closed.

Finish submits one drain barrier after earlier queued AUs, requires ACK plus LAST and all prior dispositions, and remains
nonterminal. The next input resumes BITSTREAM and RAW with DRAIN cause. Seek qualification invalidates and rebuilds from an
IDR with a fresh native SID; no old output/reply may enter the next epoch. The V3/V4 PAS/routes/TLB retention contract is kept.

The signed `video-v6` tool and owned fixtures live under `tooling/native/` and `package/PianoVideo/tests/fixtures/v6`.
Its self-test checks both clips and, where private PV helpers are exported, actual Apple serialization through the production
contract parser. The decoded 27.3 helpers are internal and absent from its export trie; an unavailable offline audit reports
SKIP, while qualification exercises the actual serializer through public VT decode. A public-only self-test covers this
preflight path without a VT session. Named phases stop after the first failed phase and cleanup. Its single target
qualification run includes original golden/rejection regressions, 60-frame Main IBP with flags1 and flags9, the IP control,
closed-GOP drain/resume, DoNotOutput on a reference, retained-surface rehash, cancel/recreate and clean close. Every callback
is compared by token and OutputPTS with independent software hashes; callback order is not assumed to be display order.
Performance is measured separately and is not called real-time playback merely because all pixels match. In-place DRC,
synchronous flags0, HEVC, new geometries, protected content and QuickTime/Safari integration need separate qualification.

The first V6 hardware session (`vid-v6a2`) passed INIT, persistent OPEN, first PSC and EBD, then returned RAW0 with the
correct address, extent and token but picture flags `0x81`. Our streaming validator rejected ancillary bit `0x80` before
output delivery. V6c follows the pinned Iris/sheng bitmask handling: require a known AVC kind and reject NOSHOW (`0x40`),
while preserving address, extent, token and ownership validation. A failed session also keeps the same malfunction result
for repeated finish requests until invalidation. Actual-parser replays and finish-state host regressions pass; V6c hardware
qualification subsequently delivered the original golden frame; its second session stalled after PSC, before DPB/RAW
submission ([results](../results-2026-11-18.md)). Continuous decoding remains unqualified.

V6f adds passive first-timeout evidence to the native path. Before cleanup at the first `-20`, it samples the existing
readable domains' core, clock, power and interrupt registers, queue indices and pending heads. It retains the last 40 TX
packets and 16 RX packets (header fields and up to 16 payload words), the current session's PSC/ownership/ledger, and the
previous session's final state. Codec idle words additionally require the live `CORE_POWER_STATUS` power bit, since firmware
can collapse that domain while the host's provisioned-state flags remain set. Sampling finishes before formatting; timestamps delimit the sequential sample. Registers
with interrupt side effects are excluded. The snapshot changes no waits, timeouts, HFI bytes or cleanup decisions.

`PianoVideo: V6f snapshot begin/end` bounds the console/IOLog record. `piano-video-first-error` on the PianoVideo IORegistry
service preserves the same text independently of owner teardown; `V6f snapshot exported IORegistry 1` confirms publication.
Absent domains are marked unreadable rather than read speculatively. The unchanged V6 tool and fixtures reproduce the
original phase order. This is diagnostic instrumentation, not a firmware-stall fix or a continuous-decoding qualification.


## 16. Stock playback qualification and bounded High AVC (V8)

The V7e2 RAM test qualified the owned 320×240 Main AVC fixture in unmodified QuickTime: 60 native frames, client lifetime
60 decoded/0 dropped, rate1 and optically confirmed fixture motion. The asynchronous reply window stays four with eight
credits and two input slots; VT finish preserves decode-order references instead of issuing HFI EOS. Exhaustive exclusion
of all software-factory attempts is still unknown. This result does not qualify other resolutions, codecs or clients.

The V8 implementation admits progressive limited-range 8-bit420 Baseline/Main/High AVC through active1920×1080,
POC0/2, references≤4, reorder≤4, one SPS/PPS, four-byte NAL lengths and one complete AU≤1MiB per sample. It validates
High syntax, scaling lists/8×8 transforms, avcC extensions, coded/active dimensions, crop and level mappings through4.2.
Field coding, POC1, FMO, transform bypass, high bit depths, other chroma formats, full range, protected samples and DRC
remain rejected. Exact same-configuration dcan is unchanged.

The native path derives FIRST_IPSC geometry and pixel crop from the stream, checks active dimensions against the SPS,
and adopts bounded firmware-returned coded geometry. Initial COMV usesN4; PSC's realN controls COMV count and split
DPB/RAW supply. General AVC uses at most16 pictures and four scratch types (AV1-only PARTIAL is omitted), with two
disjoint scratch generations. At coded1920×1088 it reserves186,449,920 bytes including guards within a256MiB aperture;
132 SMMU table pages cover that aperture. Legacy self-tests retain their prior layout. RAW uses linearNV12, references
useQC08C, and only crop-adjusted active rows reach a checked420v IOSurface. Pool size remains bounded to4MiB.

The owned High1080 and Main720 clips, compressed identities, PTS/DTS and independent software NV12 hashes are in
`package/PianoVideo/tests/fixtures/v8`. `tooling/native/build-video-avc-audit.sh` checks actual CoreMedia formats and all
compressed packets without creating a VT session. The next image must prove native/client counts, returned geometry,
safe close and optical motion in stock QuickTime. V8a subsequently passed these checks. HEVC/P010,
aerial playback, Safari/MSE and concurrent sessions remain separate gates; cinematic boot is a frozen-surface/Metal
handoff, with no established HEVC movie dependency.

## 17. Bounded HEVC implementation (V9; tablet qualification pending)

The first visible HEVC target is Aerial wallpaper: the27.3 default asset is Main10,3840×2160, High tier6.1 with temporal
layers, WPP and B pictures. Its consumer can submit synchronous flags0/2. Official cinematic boot instead hands RGB
surfaces to WindowServer/Metal; no HEVC movie dependency has been established there.

V9 adds hvc1 inventory and capability queries, immutable hvcC/VPS/SPS/PPS admission, Main/Main10 progressive limited-range
420, four-byte NAL lengths, one complete AU≤1MiB and an IDR session start. Protected streams, other profiles/chroma,
parameter-set changes and concurrent native owners fail closed. Codec3 uses HEVC PSC fields including tier, requires
returned resolution/crop/depth/minimum/profile/level/tier and sizes COMV, split references and RAW from the returned N.
The HEVC aperture has a768MiB ceiling and N≤10. V9e backs the complete guarded layout rounded to2MiB:
1080p Main10 uses188MiB and Main8 uses148MiB; tables and page metadata follow that span. AVC retains256MiB/N16.
Two disjoint scratch generations and16KiB invalid guards remain mandatory. Allocation failure frees partial preparation;
unconfirmed DMA teardown retains backing and poisons subsequent opens.

Main8 uses linearNV12/420v and QC08C references. Main10 uses linearP010/x420 and QC10C references; HFI stride is in
pixels while CPU copy stride is in bytes. Each active plane is checked independently against its own IOSurface offsets
and row bytes, with a64MiB surface bound. Synchronous admission returns selector3 immediately, progresses one request
without waiting for a future admission, and answers the original request only after Demt acknowledgement. Cancellation,
send failure and the eight-second request deadline have one completion owner. Async AVC retains its four published/eight
credit window and completion-barrier finish.

The V9i presentation candidate brackets each CPU destination write with the native IOSurface writable lock/unlock
protocol, validates the lock's current layout, and cleans the written plane extents to PoC before unlocking and emitting
Demt. A DSB and temporary memory-descriptor completion alone do not publish dirty CPU cache lines to a retained
noncoherent GPU import. The same transaction covers NV12 and P010, preserving the guest pool's display lifetime.
Host gradient/reuse checks cover byte visibility and padding; zero flicker or transparent frames in a valid continuous
camera recording remains a separate tablet acceptance gate. Decode completion alone does not qualify presentation.

V9d limits native HEVC admission to1920×1080, coded height≤1088 and Apple level≤120 (Level4), consistently in inventory,
dcap, dsta/dcan and the native-submit boundary. The wider parser and bounded4K/N10 layout remain available for analysis;
4K/Level6.1 is declined with UnsupportedDataFormat before a native reservation, PAS or HFI. This is a conservative
experimental envelope, not1080p HEVC tablet qualification. The single V9 RAM
qualification measures stock QuickTime AVC, Main8 and then Main10 with native/client identities, geometry, teardown
and optical motion, stopping on quarantine or unsafe handoff. Aerial selection, TSA/retiming, sustained cadence,
colour and looping remain separate runtime acceptance checks; the installed static wallpaper stays in place.

In the saved V9c firmware-addon run, WallpaperAerialsExtension's first4K Main10 session delivered one native frame and
closed successfully. Its next session returned the same valid PSC (depth10/profile2/level11/tier1/N8), then stopped
consuming commands before OPB_ENABLE. The Output-stage RAW START wait returned host timeout−20; no firmware error packet
or code was returned. N8 and the actual buffer sizes fit the768MiB/N10 layout. The physical cause of the firmware stall
remains unknown. V9c quarantine retained dependencies and prevented another bootstrap without a tablet reset; V9d keeps
that containment. Its early decline is intended to leave ordinary VT fallback available. Wallpaper motion and the actual
fallback factory must be measured separately; a zero native frame count does not qualify aerial playback.

V9d's1080p Main10 attempt failed earlier: host stage41 returned no-memory−15 while requesting the fixed768MiB backing.
No HFI packet was sent and ETB/EBD/PSC/decoded were all zero. The aggregate refusal did not identify which descriptor
failed. Its wallpaper capture showed no resolved motion across26.45seconds; the actual fallback factory was not recorded.
V9e removes the unnecessary maximum-size reservation while preserving every buffer capacity, both scratch generations,
Ncap10, invalid guards and quarantine. This allocation correction is host-checked;1080p HEVC remains tablet-unqualified.
