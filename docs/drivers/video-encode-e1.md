# PianoVideo E1 integration

RAM `v131y12u-e1fix1` (2026-11-27) recorded the first real-hardware Iris VPU 3.5
H.264 encode PASS for the opt-in E1 kernel self-test (105/0, `rc=0`, 640×480
10/10 frames, PSNR-Y min 51.2 / avg 51.6 dB, 314 ms, 22,439 bytes, followed by
13/13 AVC decode on the same resident core). RAM `v131y12u-e1-1080p` (2026-11-27)
then passed Full-HD 1920×1080@30 (113/0, `rc=0`, 14 entries 56,180,736 B, `VPSS 0`,
10/10 frames, PSNR-Y min 41.0 / avg 44.3 dB, 358 ms, 88,882 bytes, followed by
13/13 AVC decode on the same resident core). The preceding tablet capture,
`v131y12u-m5obs1`, failed at stage 5 when START(BITSTREAM=1) returned a session
error; the retest verified all five `b10-e1-stage5-fix` conformance gates from
the [stage-5 analysis](#stage-5-static-analysis). See the
[720p/1080p scaling and Apple 27.3 E2 contract analysis](video-encode-e2-analysis.md)
for the next gate: E2 VideoToolbox encoder registration remains HOLD and daily
installed `v13.1y12q` keeps `piano-video-enc=0` and `encs=0`.

- E1 starts only with one literal `piano-video-enc=1` token, level 6 and a successful boot reservation. It waits for the decoder's exclusive lease and reuses the arena with its current backing. On firmware system error, watchdog or SMMU fault, the worker stops issuing HFI commands and quarantines the core with its DMA backing held for the rest of the boot.
- AVC and HEVC continue to use the existing decode path, VT inventory and borrow policy. The RAM16G reservation and XNU fallback still serve decode. See the [driver guide](video.md#18-e1-bounded-native-h264-encode-self-test).

## Host results

- The ASan/UBSan suite checks the exact ARP and capability packets, four
  simultaneous RAW and BITSTREAM slots, and out-of-order returns. EBD uses
  the queued RAW slot even when the returned timestamp is zero. FBD uses the
  returned nanosecond PTS at 30 and 60 fps. It verifies default-off bring-up,
  ARP quarantine and exact AVC page table restoration after
  AVC → E1 → Main10 → AVC on both XNU and region0 backings. The AVC, HEVC
  and VT suites pass. The FFmpeg checker accepts its reference stream and
  rejects truncated and wrong-size samples. The CI runner reports 11
  passed and zero failed. The two existing suite entries run inside
  `run-bringup-host.sh`.
- The native build compiled all 12 arm64e objects with zero warnings.
  The import check against `kernel-k273-0070-s0001-native.macho` found 431
  kernel imports, six declared library imports and zero unresolved imports.

All admitted E1 layouts fit the 256 MiB reservation. The pinned VPU33
sizing code compares padded RAW and coded AVC dimensions for standalone
VPSS. At 1080p both formats are 1920×1088, so VPSS is zero and BIN is
21,661,184 bytes at the padded coded height. The 640×480 scratch and
reconstruction capacities are the same as in the original capture.

| Active size | Padded RAW | Coded AVC | VPSS bytes | Entries | Guarded bytes |
| --- | --- | --- | ---: | ---: | ---: |
| 640×480 | 640×480 | 640×480 | 0 | 14 | 32,980,992 |
| 1280×720 | 1280×736 | 1280×720 | 1,449,984 | 15 | 27,557,888 |
| 1920×1080 | 1920×1088 | 1920×1088 | 0 | 14 | 56,180,736 |
| 320×240 | 384×256 | 320×240 | 131,072 | 15 | 9,732,096 |

## RAM batch E qualification

This matrix covers the RAM16G backing. The `v131y12u-e1fix1` PASS used the
standard 256 MiB XNU backing. [Fix-image result](#fix-image-gate)

Use the same 27.3 RAM payload, pinned Iris firmware addon, fixed display profile and volatile Data across the four boots below. Use the RAM16G qualification FD with DarwinBoot patch 0002 and stage the ADT placeholder with `PIANO_EXTRA_RAM=1`. Keep `piano-video=6 piano-video-vt=1 piano-extra-ram=1` in every boot and keep the normal VT grant and borrow defaults. Save the commit, FD/KC identities, boot UUID, `kern.boottime`, merged `kern.piano_bootargs` and a fresh serial capture per boot.

| Order | `piano-video-extra-ram` | `piano-video-enc` | Required observation |
| --- | --- | --- | --- |
| 1 | `0` | Omitted | XNU-backed 256 MiB V9h reservation, zero video slabs allocated. |
| 2 | `0` | `0` | Same reservation, decode results and E1 silence as boot 1. |
| 3 | `1` | `0` | Region0-backed V9h reservation with the same decode results. |
| 4 | `1` | `1` | E1 only, after all decode and RAM16G evidence is saved. |

1. On boots 1–3, save `sudo sysctl machdep.piano_extra_ram_video`, the `/chosen/piano-extra-ram` property and the V9h owner/ready lines. For boot 3 require `state=ours`, `self-test=pass pages=70144 bad=0`, `used=128 free=9` and the console's 128 write-combined slabs starting at `0x89e00000`. On each boot, check serial and the PianoVideo IORegistry after the 120-second default E1 delay for zero E1 lines and absent `piano-video-enc-result` and `piano-video-enc-stream`.
2. On each control boot, run `video-v6 --qualify package/PianoVideo/tests/fixtures/v6` and save the per-frame NV12/PTS results and native session receipts. In stock QuickTime play the owned `fixtures/v8/high1080.mp4` AVC clip, then `fixtures/v9/main8-1080.mp4` and `fixtures/v9/main10-1080.mp4`. Capture the opening transition and playback, including V9j input-ready before the start reply, V9j3 first real input, matching native/client frame counts, NV12 or P010 as appropriate, and a healthy STOP/CLOSE for each. Require zero flicker, transparent frames, watchdog, SMMU faults or quarantine. Verify Safari still selects native AVC, WebKit's HEVC goes to the existing fallback, and VT `encs` still lists zero encoders. Stop the batch on a failed control and save its evidence.
3. **Run E1 last.** On boot 4 set `piano-video-enc-delay=120 piano-video-enc-res=0 piano-video-enc-frames=10 piano-video-enc-kbps=2000` with the table's flags. Confirm the extra-RAM reservation and leave media clients idle so E1 takes the lease. Save serial from the arm line through OPEN, ten input returns and ten nonempty bitstream completions, DRAIN ACK/LAST, both STOP acknowledgments and CLOSE. Require `E1 encode end PASS` with status 0, `quarantined 0`, zero watchdog or SMMU faults and `truncated 0` in the result log. Capture both properties using `ioreg -r -c PianoVideo -l -w0 > enc-ioreg.txt`.
4. On the host, run `python3 package/PianoVideo/tests/enc_stream_check.py --ioreg enc-ioreg.txt --size 640x480 --frames 10`. Require the IORegistry PASS, a complete Annex B stream within 256 KiB with SPS, PPS and IDR, ten software-decoded frames and minimum luma PSNR of 30 dB. If E1 stalls, fails, or never starts, save serial and any available IORegistry data and end the boot. E1 can lock the video core, so finish the batch without starting another video session on that boot.


## Stage-5 static analysis

Historical analysis, recorded 2026-11-27 against native base `411825dd4` and Linux
**K = `yzddmr6/linux-sm8750-piano b5100d5f2e0bb80e1191d521a48596674608eb3d`**.
The inspected Linux Iris directory matches K (no local Iris changes). The
native header's older `7f8d6bedde479` comment is provenance, not the pin used
for this comparison. No firmware disassembly, driver changes, fix image,
new hardware run or encode-success claim is part of this analysis.
K's generic descriptor names `qcom/vpu/vpu35_p4.mbn`; the capture's actual
filename/version below is the firmware identity under investigation, not
a claim of byte identity with an uninspected Linux firmware file.

**Finding:** E1 omits the encoder's once-per-session **ARP buffer, HFI type
9, port NONE=0, 204,800 bytes**. Linux queues it before the first port START;
it is separate from both internal-buffer tables. This is the strongest
static explanation for a failure when the second START activates the
encoder. E1 also omits two unconditional Linux control writes: deblocking
and disabling time-delta-based rate control. All three omissions must be
closed together. Two external buffers per queue and microsecond frame PTS
are additional conformance defects to close in the same implementation.
The firmware's exact first failing predicate remains **unknown**: the
capture contains a generic START session-error flag, not a diagnostic
identifying ARP or a particular property.

### Hardware evidence and its limits

The supplied `v131y12u-m5obs1` RAM capture is the evidence, not a reproduced
run. Firmware file `vpu35_4v.mbn` reported:

```text
14:vfw-3.5:rel0039-cb08be438d47230207aef1e5d0622f9f5dc4de54 PROD
V3 INIT PASS (cmd-consumed 113 doorbells 3 fsr 00000000 watchdog 0)
E1 first-error -19 stage 5 session 56360001 type 01000005 flags 00000004 port 1
```

The workload was H.264 High/profile enum 4, level enum 14 (**5.0**, not 1.4
or 4.0), NV12 640×480, stride 640, Y rows 480, UV offset 307,200, 30 fps,
2,000,000 bit/s, GOP setting 9, ten frames. Coded size was also 640×480.
The arena used 29,868,032 guarded bytes and nine allocations:

| HFI type / index | Port | Capacity (bytes) | Captured IOVA interval |
| --- | ---: | ---: | --- |
| RAW 2 / 0 | 2 | 462,848 | `[80400000,80471000)` |
| RAW 2 / 1 | 2 | 462,848 | `[80478000,804e9000)` |
| BITSTREAM 1 / 0 | 1 | 921,600 | `[804f0000,805d1000)` |
| BITSTREAM 1 / 1 | 1 | 921,600 | `[805d8000,806b9000)` |
| BIN 7 / 0 | 1 | 23,787,008 | `[806c0000,81d6f600)` |
| COMV 10 / 0 | 1 | 147,456 | `[81d74000,81d98000)` |
| NON_COMV 11 / 0 | 1 | 313,856 | `[81d9c000,81de8a00)` |
| LINE 8 / 0 | 1 | 700,928 | `[81df0000,81e9b200)` |
| DPB 6 / 0 | 1 | 1,921,024 | `[81ea0000,82075000)` |

OPEN and START(RAW=2) each returned success flag `00000001`. START(1),
header 48 / packet 55, returned `00000004` immediately. STOP(1) returned
all five queued internals (7, 8, 10, 11, 6), then succeeded; STOP(2) and
CLOSE succeeded, with `close-error 0`, `unknown-props 0`. Stage 6 was never
reached, so there were no external HFI buffer submissions, input frames or
encoded bytes in this attempt.

Two corrections matter when interpreting this record:

- The enumerated stage-1..4 sequence has **45 E1 packets**, headers 3..47 /
  packets 10..54: `2 + 19 + 1 + 23`. START(1) is the 46th E1 packet. The
  reported “47 accepted packets” must not be inferred from header 47;
  global header/packet counters include earlier system traffic.
- Submission/command-ring consumption is not per-property acceptance.
  Property writes have host flags 0; internal queues request interrupt
  only. Only OPEN and START(2) have explicit success replies here.
  `consumed()` waits for the command read pointer, while pumping replies
  ([VideoEncodeSession.h:273–276](../../package/PianoVideo/VideoEncodeSession.h#L273-L276)); it does not require an acknowledgement for every packet.
  `unknown-props` counts received unhandled property packets
  ([VideoEncodeSession.h:216–219](../../package/PianoVideo/VideoEncodeSession.h#L216-L219)), not unknown outbound properties.

In pinned Linux, flag 4 in the **packet header** is
`HFI_FW_FLAGS_SESSION_ERROR` ([iris_hfi_gen2_defines.h:250–255](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_defines.h#L250-L255)). It is not a buffer-payload
flag named `HFI_BUF_FW_FLAG_ERROR`; the buffer firmware flags are a
separate enum ([iris_hfi_gen2_defines.h:243–248](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_defines.h#L243-L248)). Native [VideoEncodeSession.h:183–202](../../package/PianoVideo/VideoEncodeSession.h#L183-L202) converts any such flag
to `kErrHfi`, whose local value is **-19** ([VideoOps.h:139–143](../../package/PianoVideo/VideoOps.h#L139-L143)). This is not Linux
`-ENODEV`, nor a firmware numeric reason of -19. Linux also dispatches
session errors by the flag ([iris_hfi_gen2_response.c:950–962](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_response.c#L950-L962)); a failed START command
lands in its generic/unknown error case unless a separate reason event is
present ([iris_hfi_gen2_response.c:235–272](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_response.c#L235-L272)). No `HFI_ERROR_BUFFER_NOT_SET` event is supplied.

### References and end-to-end path

All Linux citations below are immutable links at K, relative to
`drivers/media/platform/qcom/iris/`. Every source citation names the file
and its exact line range; those ranges refer to source, not this document:

| Area | File and role |
| --- | --- |
| Platform/power | [iris_platform_vpu3x.c:47–50](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_vpu3x.c#L47-L50) / [iris_platform_vpu3x.c:198–228](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_vpu3x.c#L198-L228): SM8750 selects gen2, VPU35, **four pipes**, and **`iris_vpu33_buf_size`**. [iris_platform_sm8750.h:9–20](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_sm8750.h#L9-L20) and [iris_platform_sm8650.h:1–13](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_sm8650.h#L1-L13) are platform clock/reset data, not separate encoder property tables; [iris_vpu3x.c:279–286](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu3x.c#L279-L286) selects power operations. |
| Capabilities/controls | [iris_hfi_gen2.c:1185–1950](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1185-L1950): selected encoder capabilities; [iris_hfi_gen2.c:2001–2020](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L2001-L2020) and [iris_hfi_gen2.c:2057–2132](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L2057-L2132): config/internal tables and `iris_hfi_gen2_data`; [iris_platform_common.h:143–219](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_common.h#L143-L219): capability traversal order; [iris_ctrls.c:1710–1730](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1710-L1730): config then every populated setter. |
| Lifecycle | [iris_venc.c:223–343](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_venc.c#L223-L343): format geometry; [iris_venc.c:481–570](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_venc.c#L481-L570): stream-on and QBUF; [iris_vb2.c:102–150](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vb2.c#L102-L150) / [iris_vb2.c:157–214](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vb2.c#L157-L214): OPEN and stream dispatch; [iris_common.c:49–155](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_common.c#L49-L155): START calls; [iris_vidc.c:120–142](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vidc.c#L120-L142): queue allocation minimums. |
| HFI | [iris_hfi_gen2_command.c:91–168](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L91-L168): ports/property writes; [iris_hfi_gen2_packet.c:232–269](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_packet.c#L232-L269): packet construction; [iris_hfi_gen2_packet.h:23–103](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_packet.h#L23-L103): wire layouts; [iris_hfi_common.h:17–45](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_common.h#L17-L45): payloads, ports, host flags; [iris_hfi_gen2_defines.h:13–159](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_defines.h#L13-L159): IDs; [iris_hfi_gen2_response.c:910–969](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_response.c#L910-L969): reply dispatch. |
| Allocation | [iris_buffer.c:486–580](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L486-L580) / [iris_buffer.c:750–778](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L750-L778): allocation and separate persistent path; [iris_vpu_buffer.c:2445–2470](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2445-L2470): VPU33 sizing selected on VPU35; [iris_vpu_buffer.c:2567–2627](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2567-L2627) / [iris_vpu_buffer.h:11–14](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.h#L11-L14): counts/alignment. |
| Native | Native [PianoVideo.cpp](../../package/PianoVideo/PianoVideo.cpp), [VideoEncode.h](../../package/PianoVideo/VideoEncode.h), [VideoEncodeSession.h](../../package/PianoVideo/VideoEncodeSession.h), [VideoHfi.h](../../package/PianoVideo/VideoHfi.h), [VideoBuffers.h](../../package/PianoVideo/VideoBuffers.h), [VideoGeometry.h](../../package/PianoVideo/VideoGeometry.h), [VideoSession.h](../../package/PianoVideo/VideoSession.h), [VideoOps.h](../../package/PianoVideo/VideoOps.h), [VideoBringup.h](../../package/PianoVideo/VideoBringup.h), [enc_host.cpp](../../package/PianoVideo/tests/enc_host.cpp). |

1. **Prepare and map.** Native [PianoVideo.cpp:856–910](../../package/PianoVideo/PianoVideo.cpp#L856-L910) acquires the exclusive core,
   configures `enc::Geometry`, selects encoder layout and invokes the
   transaction. [VideoEncode.h:82–108](../../package/PianoVideo/VideoEncode.h#L82-L108) computes external geometry; [VideoEncode.h:181–196](../../package/PianoVideo/VideoEncode.h#L181-L196) builds
   the nine-entry layout; [VideoBringup.h:523–539](../../package/PianoVideo/VideoBringup.h#L523-L539) selects and maps it through
   [VideoBuffers.h:55–70](../../package/PianoVideo/VideoBuffers.h#L55-L70). Linux [iris_venc.c:223–323](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_venc.c#L223-L323) maintains active and padded geometry,
   sets external counts, and sizes external buffers with
   [iris_buffer.c:66–83](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L66-L83) / [iris_buffer.c:418–444](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L418-L444). The decoder formulas in
   [VideoGeometry.h:9–32](../../package/PianoVideo/VideoGeometry.h#L9-L32) and decoder `SelfTest` setup [VideoSession.h:728–756](../../package/PianoVideo/VideoSession.h#L728-L756) are not E1's startup
   path: shared arena infrastructure does not supply decoder PERSIST or
   any implicit encoder ARP.
2. **OPEN and codec.** Native [VideoEncodeSession.h:310–315](../../package/PianoVideo/VideoEncodeSession.h#L310-L315) sends system-header OPEN with
   U32=session ID, waits for consumption, then ENUM codec 2. Linux
   [iris_vb2.c:133–145](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vb2.c#L133-L145) → [iris_hfi_gen2_command.c:817–855](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L817-L855) → [iris_hfi_gen2_command.c:762–798](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L762-L798) sends the same OPEN and
   AVC encoder codec. No extra encode-domain word or decode default
   header belongs in OPEN. Native [VideoHfi.h:143–166](../../package/PianoVideo/VideoHfi.h#L143-L166) and [VideoEncodeSession.h:69–97](../../package/PianoVideo/VideoEncodeSession.h#L69-L97) match Linux
   [iris_hfi_gen2_packet.c:232–269](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_packet.c#L232-L269) and [iris_hfi_gen2_packet.h:23–103](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_packet.h#L23-L103) for envelope/payload layout.
3. **RAW stream-on (stages 2–3).** Native [VideoEncodeSession.h:316–320](../../package/PianoVideo/VideoEncodeSession.h#L316-L320) sends five input
   config properties and 14 capability writes, optional VPSS, then
   START(2). Linux OUTPUT STREAMON dispatches to
   [iris_venc.c:481–507](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_venc.c#L481-L507): `iris_set_properties` → **allocate/queue ARP** →
   size/allocate/queue VPSS → [iris_common.c:49–59](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_common.c#L49-L59) → START. Config selection
   is [iris_hfi_gen2_command.c:685–759](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L685-L759) / [iris_hfi_gen2.c:2001–2007](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L2001-L2007). Native omits ARP and two control
   writes, detailed below; VPSS size zero at 640×480 is correct.
4. **BITSTREAM stream-on (stages 4–5).** Native [VideoEncodeSession.h:321–325](../../package/PianoVideo/VideoEncodeSession.h#L321-L325) sends three
   output properties, repeats capabilities, queues BIN, COMV-count=4,
   COMV, NON_COMV, LINE, DPB, then START(1). Linux
   [iris_venc.c:510–538](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_venc.c#L510-L538) repeats config/capabilities, ensures ARP exists, queues
   the five output internals from [iris_hfi_gen2.c:2061–2067](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L2061-L2067), then
   [iris_common.c:142–144](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_common.c#L142-L144) → [iris_hfi_gen2_command.c:1122–1146](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1122-L1146). ARP is queued **once**: its helper
   returns early for a nonempty list ([iris_buffer.c:750–778](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L750-L778)). The COMV property
   is injected immediately before COMV ([iris_hfi_gen2_command.c:1293–1353](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1293-L1353)), just as in E1.
5. **Subscriptions and queue order.** Both Linux subscribe helpers
   explicitly return for ENCODER ([iris_hfi_gen2_command.c:926–927](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L926-L927) / [iris_hfi_gen2_command.c:1080–1081](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1080-L1081)).
   `iris_vb2_start_streaming` dispatches the queue requested by userspace
   ([iris_vb2.c:183–193](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vb2.c#L183-L193)); it does not prescribe one universal first queue.
   For the input-first E1 invocation, RAW START precedes output config,
   output internals and BITSTREAM START. Internals precede their START;
   Linux defers external input/output buffers until both ports stream
   ([iris_vb2.c:208–213](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vb2.c#L208-L213), [iris_venc.c:562–570](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_venc.c#L562-L570)). E1 queues empty BITSTREAM buffers then
   RAW frames after both START submissions ([VideoEncodeSession.h:325–340](../../package/PianoVideo/VideoEncodeSession.h#L325-L340)); there is no
   missing pre-START external queue requirement in this pin.
6. **Failure and close.** Native [VideoEncodeSession.h:183–236](../../package/PianoVideo/VideoEncodeSession.h#L183-L236) records the first error;
   [VideoEncodeSession.h:352–394](../../package/PianoVideo/VideoEncodeSession.h#L352-L394) attempts STOP(1), STOP(2), CLOSE even after this session
   failure, while device faults have a separate no-more-HFI path. The
   supplied returns support ownership recovery of the five submitted
   internals. [PianoVideo.cpp:912–942](../../package/PianoVideo/PianoVideo.cpp#L912-L942) still treats the failed attempt as a failed
   reservation. A healthy close does not prove that encoder activation or
   the omitted resources were valid.

### Check matrix: every captured TX packet

`ours-OK` means the transmitted fields agree with the pinned Linux path
for the chosen settings; it does **not** assert individual firmware
acceptance. `ours-missing` identifies an absent expected operation;
`ours-mismatch` identifies a different contract; `unknown` marks evidence
that cannot close a firmware check. Further matrices below cover missing
operations and buffer contracts, so an OK packet row is not an OK session.

Types, flags and values are hexadecimal; counters, ports, payload-tag
numbers and word counts are decimal.
Payload abbreviations: `N=0` NONE, `U=1` U32, `S=5` STRUCTURE, `Q=8` Q16,
`E=9` ENUM, `P32=10` PACKED32, `P64=13` PACKED64. There is no distinct
BITMASK payload tag in this enum: **12 is S32_ARRAY**, and U32_ARRAY is 11 ([iris_hfi_common.h:23–37](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_common.h#L23-L37)). No encoder subscription
array is required. Every property has flags 0; command flags are shown
explicitly. `v0` is the captured first word. For P64, only `v0` was logged:
parentheses give the second word reconstructed from native code, not an
additional hardware observation. Buffer payload details are checked in
the next matrix.

| Stage | Header / packet | Type / meaning | Port | Flags | Payload / words | v0 (expected v1) | Status; native / Linux evidence |
| --- | --- | --- | ---: | --- | --- | --- | --- |
| 1 | 3 / 10 | `01000003` OPEN | 0 | `00000003` | U/1 | `56360001` | `ours-OK`; [VideoEncodeSession.h:312–315](../../package/PianoVideo/VideoEncodeSession.h#L312-L315); [iris_hfi_gen2_command.c:832–855](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L832-L855) |
| 1 | 4 / 11 | `03000100` CODEC | 0 | `00000000` | E/1 | `00000002` | `ours-OK`; [VideoEncode.h:52](../../package/PianoVideo/VideoEncode.h#L52); [iris_hfi_gen2_command.c:762–798](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L762-L798) |
| 2 | 5 / 12 | `03000101` COLOR_FORMAT | 2 | `00000000` | E/1 | `00000001` | `ours-OK`; [VideoEncode.h:133](../../package/PianoVideo/VideoEncode.h#L133); [iris_hfi_gen2_command.c:476–509](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L476-L509) |
| 2 | 6 / 13 | `03000178` RAW_RESOLUTION | 2 | `00000000` | P32/1 | `028001e0` | `ours-OK`; [VideoEncode.h:134](../../package/PianoVideo/VideoEncode.h#L134); [iris_hfi_gen2_command.c:170–181](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L170-L181) |
| 2 | 7 / 14 | `03000105` CROP_OFFSETS | 2 | `00000000` | P64/2 | `00000000 (00000000)` | `ours-OK`; [VideoEncode.h:135](../../package/PianoVideo/VideoEncode.h#L135); [iris_hfi_gen2_command.c:227–282](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L227-L282) |
| 2 | 8 / 15 | `03000104` LINEAR_STRIDE_SCANLINE | 2 | `00000000` | P64/2 | `028001e0 (028000f0)` | `ours-OK`; [VideoEncode.h:136](../../package/PianoVideo/VideoEncode.h#L136); [iris_hfi_gen2_command.c:512–544](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L512-L544) |
| 2 | 9 / 16 | `03000155` SIGNAL_COLOR_INFO | 2 | `00000000` | P32/1 | `14000003` | `ours-OK`; [VideoEncode.h:137](../../package/PianoVideo/VideoEncode.h#L137); [iris_hfi_gen2_command.c:355–395](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L355-L395) |
| 2 | 10 / 17 | `03000107` PROFILE | 1 | `00000000` | E/1 | `00000004` | `ours-OK`; [VideoEncode.h:155](../../package/PianoVideo/VideoEncode.h#L155); [iris_ctrls.c:601–619](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L601-L619) |
| 2 | 11 / 18 | `03000108` LEVEL | 1 | `00000000` | E/1 | `0000000e` | `ours-OK`; [VideoEncode.h:156](../../package/PianoVideo/VideoEncode.h#L156); [iris_ctrls.c:621–639](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L621-L639) |
| 2 | 12 / 19 | `03000123` BUFFER_HOST_MAX_COUNT | 2 | `00000000` | U/1 | `00000040` | `ours-OK`; [VideoEncode.h:157](../../package/PianoVideo/VideoEncode.h#L157); [iris_hfi_gen2.c:1619–1627](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1619-L1627) |
| 2 | 13 / 20 | `03000123` BUFFER_HOST_MAX_COUNT | 1 | `00000000` | U/1 | `00000040` | `ours-OK`; [VideoEncode.h:158](../../package/PianoVideo/VideoEncode.h#L158); [iris_hfi_gen2.c:1629–1637](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1629-L1637) |
| 2 | 14 / 21 | `0300010a` STAGE | 0 | `00000000` | U/1 | `00000002` | `ours-OK`; [VideoEncode.h:159](../../package/PianoVideo/VideoEncode.h#L159); [iris_ctrls.c:559–582](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L559-L582) |
| 2 | 15 / 22 | `03000149` SEQ_HEADER_MODE | 1 | `00000000` | E/1 | `00000002` | `ours-OK`; [VideoEncode.h:160](../../package/PianoVideo/VideoEncode.h#L160); [iris_ctrls.c:681–700](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L681-L700) |
| 2 | 16 / 23 | `0300013b` TOTAL_BITRATE | 2 | `00000000` | U/1 | `001e8480` | `ours-OK`; [VideoEncode.h:161](../../package/PianoVideo/VideoEncode.h#L161); [iris_ctrls.c:760–782](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L760-L782) |
| 2 | 17 / 24 | `0300012a` RATE_CONTROL | 1 | `00000000` | E/1 | `00000000` | `ours-OK`; [VideoEncode.h:162](../../package/PianoVideo/VideoEncode.h#L162); [iris_ctrls.c:839–863](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L839-L863) |
| 2 | 18 / 25 | `03000146` MAX_GOP_FRAMES | 2 | `00000000` | U/1 | `00000009` | `ours-OK`; [VideoEncode.h:163](../../package/PianoVideo/VideoEncode.h#L163); [iris_hfi_gen2.c:1349–1357](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1349-L1357) |
| 2 | 19 / 26 | `03000121` CABAC_SESSION | 1 | `00000000` | U/1 | `00000001` | `ours-OK`; [VideoEncode.h:164](../../package/PianoVideo/VideoEncode.h#L164); [iris_ctrls.c:886–908](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L886-L908) |
| 2 | 20 / 27 | `0300014c` FLIP | 2 | `00000000` | E/1 | `00000000` | `ours-OK`; [VideoEncode.h:165](../../package/PianoVideo/VideoEncode.h#L165); [iris_ctrls.c:1213–1229](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1213-L1229) |
| 2 | 21 / 28 | `03000131` IR_RANDOM_PERIOD | 1 | `00000000` | U/1 | `00000000` | `ours-OK`; [VideoEncode.h:166](../../package/PianoVideo/VideoEncode.h#L166); [iris_ctrls.c:1269–1292](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1269-L1292) |
| 2 | 22 / 29 | `03000147` MAX_B_FRAMES | 1 | `00000000` | U/1 | `00000000` | `ours-OK`; [VideoEncode.h:167](../../package/PianoVideo/VideoEncode.h#L167); [iris_hfi_gen2.c:1720–1727](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1720-L1727) |
| 2 | 23 / 30 | `03000145` REQUEST_SYNC_FRAME | 2 | `00000000` | E/1 | `00000001` | `ours-OK`; [VideoEncode.h:168](../../package/PianoVideo/VideoEncode.h#L168); [iris_ctrls.c:1598–1613](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1598-L1613) |
| 3 | 24 / 31 | `01000005` START RAW; RX success=1 | 2 | `00000003` | N/0 | `—` | `ours-OK`; [VideoEncodeSession.h:318–320](../../package/PianoVideo/VideoEncodeSession.h#L318-L320); [iris_hfi_gen2_command.c:1122–1146](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1122-L1146) |
| 4 | 25 / 32 | `03000103` BITSTREAM_RESOLUTION | 1 | `00000000` | P32/1 | `028001e0` | `ours-OK`; [VideoEncode.h:144](../../package/PianoVideo/VideoEncode.h#L144); [iris_hfi_gen2_command.c:191–224](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L191-L224) |
| 4 | 26 / 33 | `03000105` CROP_OFFSETS | 1 | `00000000` | P64/2 | `00000000 (00000000)` | `ours-OK`; [VideoEncode.h:145](../../package/PianoVideo/VideoEncode.h#L145); [iris_hfi_gen2_command.c:247–282](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L247-L282) |
| 4 | 27 / 34 | `0300010c` FRAME_RATE | 1 | `00000000` | Q/1 | `001e0000` | `ours-OK`; [VideoEncode.h:146](../../package/PianoVideo/VideoEncode.h#L146); [iris_hfi_gen2_command.c:620–631](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L620-L631) |
| 4 | 28 / 35 | `03000107` PROFILE | 1 | `00000000` | E/1 | `00000004` | `ours-OK`; [VideoEncode.h:155](../../package/PianoVideo/VideoEncode.h#L155); [iris_ctrls.c:601–619](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L601-L619) |
| 4 | 29 / 36 | `03000108` LEVEL | 1 | `00000000` | E/1 | `0000000e` | `ours-OK`; [VideoEncode.h:156](../../package/PianoVideo/VideoEncode.h#L156); [iris_ctrls.c:621–639](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L621-L639) |
| 4 | 30 / 37 | `03000123` BUFFER_HOST_MAX_COUNT | 2 | `00000000` | U/1 | `00000040` | `ours-OK`; [VideoEncode.h:157](../../package/PianoVideo/VideoEncode.h#L157); [iris_hfi_gen2.c:1619–1627](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1619-L1627) |
| 4 | 31 / 38 | `03000123` BUFFER_HOST_MAX_COUNT | 1 | `00000000` | U/1 | `00000040` | `ours-OK`; [VideoEncode.h:158](../../package/PianoVideo/VideoEncode.h#L158); [iris_hfi_gen2.c:1629–1637](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1629-L1637) |
| 4 | 32 / 39 | `0300010a` STAGE | 0 | `00000000` | U/1 | `00000002` | `ours-OK`; [VideoEncode.h:159](../../package/PianoVideo/VideoEncode.h#L159); [iris_ctrls.c:559–582](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L559-L582) |
| 4 | 33 / 40 | `03000149` SEQ_HEADER_MODE | 1 | `00000000` | E/1 | `00000002` | `ours-OK`; [VideoEncode.h:160](../../package/PianoVideo/VideoEncode.h#L160); [iris_ctrls.c:681–700](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L681-L700) |
| 4 | 34 / 41 | `0300013b` TOTAL_BITRATE | 2 | `00000000` | U/1 | `001e8480` | `ours-OK`; [VideoEncode.h:161](../../package/PianoVideo/VideoEncode.h#L161); [iris_ctrls.c:760–782](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L760-L782) |
| 4 | 35 / 42 | `0300012a` RATE_CONTROL | 1 | `00000000` | E/1 | `00000000` | `ours-OK`; [VideoEncode.h:162](../../package/PianoVideo/VideoEncode.h#L162); [iris_ctrls.c:839–863](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L839-L863) |
| 4 | 36 / 43 | `03000146` MAX_GOP_FRAMES | 2 | `00000000` | U/1 | `00000009` | `ours-OK`; [VideoEncode.h:163](../../package/PianoVideo/VideoEncode.h#L163); [iris_hfi_gen2.c:1349–1357](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1349-L1357) |
| 4 | 37 / 44 | `03000121` CABAC_SESSION | 1 | `00000000` | U/1 | `00000001` | `ours-OK`; [VideoEncode.h:164](../../package/PianoVideo/VideoEncode.h#L164); [iris_ctrls.c:886–908](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L886-L908) |
| 4 | 38 / 45 | `0300014c` FLIP | 2 | `00000000` | E/1 | `00000000` | `ours-OK`; [VideoEncode.h:165](../../package/PianoVideo/VideoEncode.h#L165); [iris_ctrls.c:1213–1229](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1213-L1229) |
| 4 | 39 / 46 | `03000131` IR_RANDOM_PERIOD | 1 | `00000000` | U/1 | `00000000` | `ours-OK`; [VideoEncode.h:166](../../package/PianoVideo/VideoEncode.h#L166); [iris_ctrls.c:1269–1292](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1269-L1292) |
| 4 | 40 / 47 | `03000147` MAX_B_FRAMES | 1 | `00000000` | U/1 | `00000000` | `ours-OK`; [VideoEncode.h:167](../../package/PianoVideo/VideoEncode.h#L167); [iris_hfi_gen2.c:1720–1727](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1720-L1727) |
| 4 | 41 / 48 | `03000145` REQUEST_SYNC_FRAME | 2 | `00000000` | E/1 | `00000001` | `ours-OK`; [VideoEncode.h:168](../../package/PianoVideo/VideoEncode.h#L168); [iris_ctrls.c:1598–1613](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1598-L1613) |
| 4 | 42 / 49 | `01000009` BIN | 1 | `00000001` | S/16 | `00000007` | `ours-OK`; [VideoEncodeSession.h:88–97](../../package/PianoVideo/VideoEncodeSession.h#L88-L97); [iris_hfi_gen2_command.c:1328–1353](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1328-L1353) |
| 4 | 43 / 50 | `03000193` COMV_BUFFER_COUNT | 1 | `00000000` | U/1 | `00000004` | `ours-OK`; [VideoEncodeSession.h:296–301](../../package/PianoVideo/VideoEncodeSession.h#L296-L301); [iris_hfi_gen2_command.c:1293–1305](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1293-L1305) |
| 4 | 44 / 51 | `01000009` COMV | 1 | `00000001` | S/16 | `0000000a` | `ours-OK`; [VideoEncodeSession.h:88–97](../../package/PianoVideo/VideoEncodeSession.h#L88-L97); [iris_hfi_gen2_command.c:1256–1353](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1256-L1353) |
| 4 | 45 / 52 | `01000009` NON_COMV | 1 | `00000001` | S/16 | `0000000b` | `ours-OK`; [VideoEncodeSession.h:88–97](../../package/PianoVideo/VideoEncodeSession.h#L88-L97); [iris_hfi_gen2_command.c:1256–1353](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1256-L1353) |
| 4 | 46 / 53 | `01000009` LINE | 1 | `00000001` | S/16 | `00000008` | `ours-OK`; [VideoEncodeSession.h:88–97](../../package/PianoVideo/VideoEncodeSession.h#L88-L97); [iris_hfi_gen2_command.c:1256–1353](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1256-L1353) |
| 4 | 47 / 54 | `01000009` DPB (SCRATCH_2) | 1 | `00000001` | S/16 | `00000006` | `ours-OK`; [VideoEncodeSession.h:88–97](../../package/PianoVideo/VideoEncodeSession.h#L88-L97); [iris_hfi_gen2_command.c:1256–1353](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1256-L1353) |
| 5 | 48 / 55 | `01000005` START BITSTREAM; RX session error=4 | 1 | `00000003` | N/0 | `—` | `unknown`; [VideoEncodeSession.h:323–325](../../package/PianoVideo/VideoEncodeSession.h#L323-L325); [iris_hfi_gen2_command.c:1122–1146](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1122-L1146) |

All 22 property IDs in the supplied sequence have been resolved above.
In particular, **`03000193` is `HFI_PROP_COMV_BUFFER_COUNT`**
([iris_hfi_gen2_defines.h:159](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_defines.h#L159)); **`03000155` is `HFI_PROP_SIGNAL_COLOR_INFO`**
([iris_hfi_gen2_defines.h:140](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_defines.h#L140)). Neither is an unknown extra property. Port selection for
capabilities gives INPUT flags precedence over OUTPUT
([iris_ctrls.c:515–530](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L515-L530)), so bitrate, GOP, flip and sync-request on port 2 are
intentional. Profile/level and frame rate on port 1 are correct. ENUM
profile 4/level 14 matches Linux High/5.0 defaults ([iris_hfi_gen2.c:1185–1237](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1185-L1237)).
Color `14000003` matches Linux's all-default V4L2 color selection:
reserved matrix 3, reserved transfer/primaries 0, video format 5,
presence flags/range 0 ([iris_hfi_gen2_command.c:355–385](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L355-L385), [iris_hfi_gen2_packet.c:79–89](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_packet.c#L79-L89)). This is an
unspecified color declaration, not an explicit BT.709 declaration.

| Stage-8/9 transmitted cleanup operation (TX counters not supplied) | Port / flags / payload | Check |
| --- | --- | --- |
| STOP `01000006` | 1 / 7 / NONE, zero words | `ours-OK`: [VideoEncodeSession.h:358–367](../../package/PianoVideo/VideoEncodeSession.h#L358-L367) vs [iris_hfi_gen2_command.c:1149–1175](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1149-L1175); five internal returns and success observed. |
| STOP `01000006` | 2 / 7 / NONE, zero words | `ours-OK`: same source; success observed. |
| CLOSE `01000004` | 0 / 7 / NONE, zero words | `ours-OK`: [VideoEncodeSession.h:368–373](../../package/PianoVideo/VideoEncodeSession.h#L368-L373) vs [iris_hfi_gen2_command.c:866–885](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L866-L885); success observed. |

Cleanup flags/payload are reconstructed from native source; the supplied
cleanup summary establishes the responses, not full TX hex dumps.


### Check matrix: ports, required setup and conditional properties

“Required” here means an operation the pinned Linux encoder performs for
these settings. The opaque firmware's minimal accepted subset is not
published by this source. Missing unconditional writes are confirmed
conformance defects; whether each is independently fatal at START is
unknown. Decoder-only helpers and unselected definitions are not evidence
of a mandatory encoder write.

| Check | Pinned Linux behavior / native comparison | Status |
| --- | --- | --- |
| NONE=0 / RAW=2 / BITSTREAM=1 | [iris_hfi_common.h:17–21](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_common.h#L17-L21) and [VideoEncode.h:51](../../package/PianoVideo/VideoEncode.h#L51) agree. V4L2 OUTPUT=encoder input=RAW=2; CAPTURE=encoder output=BITSTREAM=1 ([iris_hfi_gen2_command.c:91–111](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L91-L111)). | `ours-OK` |
| External RAW and optional VPSS ports | HFI type 2 and 13 → RAW=2 ([iris_hfi_gen2_command.c:134–137](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L134-L137), [iris_hfi_gen2_command.c:1256–1285](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1256-L1285)); [VideoEncode.h:186–190](../../package/PianoVideo/VideoEncode.h#L186-L190). | `ours-OK` |
| Output scratch / reconstruction ports | BIN 7, COMV 10, NON_COMV 11, LINE 8, SCRATCH_2→DPB 6 → BITSTREAM=1 ([iris_hfi_gen2_command.c:138–144](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L138-L144)); [VideoEncode.h:191–193](../../package/PianoVideo/VideoEncode.h#L191-L193). DPB is not on RAW in encode. | `ours-OK` |
| Persistent ARP port and ordering | Driver BUF_ARP → HFI 9 on NONE=0 ([iris_hfi_gen2_command.c:145–147](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L145-L147), [iris_hfi_gen2_command.c:1282–1283](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1282-L1283)), once before first START ([iris_venc.c:489–507](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_venc.c#L489-L507), [iris_buffer.c:750–778](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L750-L778)). No ARP in [VideoEncode.h:54–55](../../package/PianoVideo/VideoEncode.h#L54-L55) or [VideoEncode.h:181–196](../../package/PianoVideo/VideoEncode.h#L181-L196), no queue path in [VideoEncodeSession.h:287–325](../../package/PianoVideo/VideoEncodeSession.h#L287-L325). | `ours-missing` |
| Five RAW config writes | COLOR_FORMAT, RAW_RESOLUTION, CROP, LINEAR_STRIDE_SCANLINE, SIGNAL_COLOR_INFO in that order ([iris_hfi_gen2.c:2001–2007](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L2001-L2007)); native [VideoEncode.h:130–138](../../package/PianoVideo/VideoEncode.h#L130-L138) matches. | `ours-OK` |
| Three BITSTREAM config writes | BITSTREAM_RESOLUTION, CROP, FRAME_RATE ([iris_hfi_gen2.c:2016–2020](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L2016-L2020)); native [VideoEncode.h:141–147](../../package/PianoVideo/VideoEncode.h#L141-L147) matches. | `ours-OK` |
| Populated capability setters on both stream-ons | Config then capability enum traversal, not array initializer order ([iris_ctrls.c:1710–1728](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1710-L1728), [iris_platform_common.h:143–219](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_common.h#L143-L219)). Native [VideoEncode.h:152–169](../../package/PianoVideo/VideoEncode.h#L152-L169) preserves its 14 distinct effective writes but omits the next two rows. | `ours-mismatch` |
| H.264 DEBLOCKING_MODE `03000129` | Port 1, flags 0, PACKED32=10, one word **`00060600`**: enabled all boundaries=0, alpha=0 biased +6 in bits 23:16, beta=0 biased +6 in bits 15:8 ([iris_hfi_gen2.c:1371–1398](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1371-L1398), [iris_ctrls.c:911–969](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L911-L969), [iris_hfi_gen2_defines.h:60–66](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_defines.h#L60-L66)). Sent each stream-on after CABAC in capability order. Entirely absent from E1. | `ours-missing` |
| TIME_DELTA_BASED_RATE_CONTROL `0300012b` | Port 1, flags 0, U32=1, one word **0**, unconditionally each stream-on ([iris_hfi_gen2.c:1321–1329](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1321-L1329), [iris_ctrls.c:1616–1632](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1616-L1632)). Enum order is after REQUEST_SYNC_FRAME ([iris_platform_common.h:215–217](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_common.h#L215-L217)). Linux explicitly overrides firmware default enabled; E1 does not override the documented default. | `ours-missing` |
| Duplicate effective setters | Linux has H.264 and HEVC profile/level entries whose setters select the active codec ([iris_ctrls.c:601–638](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L601-L638)); HFLIP/VFLIP setters both pack the combined value ([iris_ctrls.c:1213–1229](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1213-L1229)). E1 emits each distinct resulting property once. Repeating identical values is unnecessary for state equivalence; any undocumented dependency on duplicate writes is unobservable. | `ours-OK` (effective state) |
| HFI_CMD_SUBSCRIBE_MODE `0100000b` / settings change | Both encoder helpers return before sending any subscription ([iris_hfi_gen2_command.c:926–927](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L926-L927), [iris_hfi_gen2_command.c:1080–1081](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1080-L1081)). No encoder PSC wait is required by this pin. Native absence matches. | `ours-OK` |
| BUFFER_FW_MIN_OUTPUT_COUNT `03000124` | Helper exists ([iris_hfi_gen2_command.c:321–335](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L321-L335)), but neither selected encoder config list nor encoder caps calls it ([iris_hfi_gen2.c:1185–1950](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1185-L1950), [iris_hfi_gen2.c:2001–2020](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L2001-L2020)). E1 must not add decoder negotiation based only on the helper's existence. | `ours-OK` (absent by design) |
| LUMA_CHROMA_BIT_DEPTH `0300010f`, CODED_FRAMES `03000120`, PIC_ORDER_CNT_TYPE `03000128`, TIER `03000109` | Not selected for AVC encoder config/caps above. NV12 selects eight-bit input; explicit decoder bit-depth/POC/tier setup is not an encoder requirement here. | `ours-OK` (absent by design) |
| PIPE `0300010b`, WORK_MODE | PIPE is defined ([iris_hfi_gen2_defines.h:50–51](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_defines.h#L50-L51)) but absent from the selected encoder capability table. Four pipes come from SM8750 platform sizing; encoder work mode uses STAGE=2 ([iris_ctrls.c:559–582](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L559-L582)). No separate gen2 WORK_MODE write is selected. | `ours-OK` (no extra write) |
| MIN_QUALITY, COMPLEXITY, ENC_RING_BUFFER_COUNT | No such encoder properties/capability setters appear in K's gen2 definitions and selected table ([iris_hfi_gen2_defines.h:42–161](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_defines.h#L42-L161), [iris_hfi_gen2.c:1185–1950](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1185-L1950)). Firmware support for names from other interfaces is not established; no numeric IDs should be invented. | `unknown` (ABI); no demonstrated omission |
| PICTURE_TYPE `03000162`, BUFFER_MARK `0300016c` | Defined ([iris_hfi_gen2_defines.h:142–146](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_defines.h#L142-L146)), but not encoder config/subscription/setter requirements. PICTURE_TYPE is received metadata in [VideoEncodeSession.h:216–219](../../package/PianoVideo/VideoEncodeSession.h#L216-L219). A receiver does not imply a mandatory outbound setter or subscription. | `ours-OK` (absent by design) |
| H.264 entropy/profile/level/GOP/B frames | High=4, level 5.0=14, CABAC=1, chosen GOP=9 and B=0 all match active encoder enums/controls ([iris_hfi_gen2.c:1185–1237](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1185-L1237), [iris_hfi_gen2.c:1349–1368](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1349-L1368), [iris_hfi_gen2.c:1720–1727](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1720-L1727)). No separate `HFI_PROP_H264_*` startup family is selected. | `ours-OK` |
| Min/max/frame QP packed properties | Setters return without sending when no client QP is set under VBR ([iris_ctrls.c:972–1009](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L972-L1009), [iris_ctrls.c:1036–1073](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1036-L1073), [iris_ctrls.c:1100–1140](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1100-L1140)). Native absence is appropriate for these settings. | `ours-OK` |
| Peak bitrate | Only CBR sends it ([iris_ctrls.c:785–809](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L785-L809)); E1 VBR does not. | `ours-OK` |
| Rotation, LTR count/use/mark, layers and layer bitrates | Zero rotation sends nothing ([iris_ctrls.c:1183–1193](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1183-L1193)); LTR=0 and layers disabled/zero count send nothing ([iris_ctrls.c:1366–1408](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1366-L1408), [iris_ctrls.c:1437–1450](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1437-L1450), [iris_ctrls.c:1529–1541](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1529-L1541), [iris_ctrls.c:1573–1589](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1573-L1589)). No client layer bitrate is set. | `ours-OK` |
| Single slice / multi-slice properties | Default single-slice setter returns without a packet ([iris_hfi_gen2.c:1922–1949](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L1922-L1949), [iris_ctrls.c:1649–1652](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_ctrls.c#L1649-L1652)). No missing multi-slice count for E1. | `ours-OK` |
| RAW START then BITSTREAM START | Matches input-first invocation; Linux independently dispatches each queue, with internals before its START. START type/flags/port/payload all match [iris_hfi_gen2_command.c:1122–1146](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1122-L1146). The firmware implementation of activation-time validation is unavailable. | `ours-OK` (order/wire); `unknown` (FW predicate) |

### Check matrix: buffers, sizes, counts and ownership

This is the **VPU35 → VPU33 sizing route** selected by
[iris_platform_vpu3x.c:47–50](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_platform_vpu3x.c#L47-L50), not an assumed `iris_vpu35_buf_size` function (none exists
in this pin). [iris_vpu_buffer.c:2445–2458](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2445-L2458) selects the encoder size helpers;
[iris_hfi_gen2.c:2057–2067](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L2057-L2067) selects VPSS plus five output types. ARP is added through
its separate persistent helper. Count is one for each internal object
([iris_vpu_buffer.c:2567–2578](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2567-L2578), [iris_vpu_buffer.c:2606–2623](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2606-L2623)). Encoder reconstruction is
**BUF_SCRATCH_2 mapped to HFI DPB**, not decoder BUF_DPB/min_count pictures.
There is no additional queued `ENC_REF` type in [iris_hfi_gen2_defines.h:220–233](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_defines.h#L220-L233); reference
pictures are storage inside the aggregate reconstruction allocation.

For the formulas, `A(x,a)` rounds up to alignment a, `C(x,a)=ceil(x/a)`;
`W=640`, `H=480`, `P=4`, `L=16`, `N=C(W,L)*C(H,L)=1200`,
`R=num_ref=1` (no B, LTR or hierarchy). Internal sizes use coded destination
dimensions ([iris_vpu_buffer.c:1134–1147](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1134-L1147)). Re-evaluating these equations gives every
captured internal capacity exactly; no shrink, additional DPB pool or
VPU generation substitution is justified by this capture.

| Object: driver → HFI / port | Linux count and size formula / evaluated bytes | Ours and status |
| --- | --- | --- |
| INPUT → RAW 2 / 2 | **4**, `A(A(W,128)*(A(H,32)+A(C(H,2),16)),4096)` = **462,848 each** ([iris_buffer.c:66–83](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L66-L83), [iris_vpu_buffer.c:2596–2603](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2596-L2603)). | Two at correct capacity and port ([VideoEncode.h:180–187](../../package/PianoVideo/VideoEncode.h#L180-L187)). Size/port `ours-OK`; count `ours-mismatch`. |
| OUTPUT → BITSTREAM 1 / 1 | **4**, `A(3*A(W,32)*A(H,32),4096)` below the 720p threshold = **921,600 each** ([iris_buffer.c:418–444](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L418-L444), [iris_vpu_buffer.c:2601–2603](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2601-L2603)). | Two at correct capacity/port ([VideoEncode.h:180–189](../../package/PianoVideo/VideoEncode.h#L180-L189)). Size/port `ours-OK`; count `ours-mismatch`. |
| ARP → 9 / **0** | **1**, constant `HFI_BUFFER_ARP_ENC` = **204,800** ([iris_vpu_buffer.c:1773–1775](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1773-L1775), [iris_vpu_buffer.h:144](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.h#L144), [iris_vpu_buffer.c:2616–2621](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2616-L2621)). | No allocation, mapping, size, queue or ownership entry. `ours-missing`. |
| BIN → 7 / 1 | **1**, stage-2 formula B1 below, **23,787,008** ([iris_vpu_buffer.c:1150–1243](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1150-L1243)). | One, exact bytes/port, header 42; [VideoEncode.h:67](../../package/PianoVideo/VideoEncode.h#L67) / [VideoEncode.h:191](../../package/PianoVideo/VideoEncode.h#L191). `ours-OK`. |
| COMV → 10 / 1 | **1**, `A(48*(N+32),256)*(R+1) + 6*A(32*C(C(W,16),8)*C(H,16),256)` = **147,456** ([iris_vpu_buffer.c:1300–1332](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1300-L1332)). | One, exact bytes/port, header 44; [VideoEncode.h:67](../../package/PianoVideo/VideoEncode.h#L67) / [VideoEncode.h:191](../../package/PianoVideo/VideoEncode.h#L191). `ours-OK`. |
| NON_COMV → 11 / 1 | **1**, formula B2 below = **313,856** ([iris_vpu_buffer.c:1335–1375](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1335-L1375), [iris_vpu_buffer.c:1572–1613](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1572-L1613)). | One, exact bytes/port, header 45; [VideoEncode.h:67](../../package/PianoVideo/VideoEncode.h#L67) / [VideoEncode.h:192](../../package/PianoVideo/VideoEncode.h#L192). `ours-OK`. |
| LINE → 8 / 1 | **1**, base **56,576** + VPU33 VPSS-line **644,352** = **700,928**, formula B3 below ([iris_vpu_buffer.c:1379–1512](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1379-L1512), [iris_vpu_buffer.c:1617–1721](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1617-L1721)). | One, exact bytes/port, header 46; [VideoEncode.h:67](../../package/PianoVideo/VideoEncode.h#L67) / [VideoEncode.h:192](../../package/PianoVideo/VideoEncode.h#L192). `ours-OK`. |
| SCRATCH_2 → DPB 6 / 1 | **1 aggregate**, `D*(R+3)+4096`, `D=A(W,128)*(A(H,32)+A(floor(H/2),32))+2*A(A(C(W,32),64)*A(C(H,8),16),4096)`. `D=479,232`; total **1,921,024** ([iris_vpu_buffer.c:1993–2052](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1993-L2052)). | One aggregate, exact bytes/port, header 47; [VideoEncode.h:67](../../package/PianoVideo/VideoEncode.h#L67) / [VideoEncode.h:193](../../package/PianoVideo/VideoEncode.h#L193). `ours-OK`; four reconstructed images are inside this buffer, not four HFI DPB commands. |
| VPSS → 13 / 2 | Count **1 if nonzero**; `hfi_buffer_dpb_enc` only if padded source/destination sizes differ or blur is enabled, otherwise **0 bytes / no allocation** ([iris_vpu_buffer.c:1725–1761](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1725-L1761), [iris_vpu_buffer.c:1778–1794](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L1778-L1794), [iris_vpu_buffer.c:2055–2062](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vpu_buffer.c#L2055-L2062), [iris_buffer.c:537–538](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L537-L538)). | Source/destination both 640×480, blur off; zero and omitted ([VideoEncode.h:67](../../package/PianoVideo/VideoEncode.h#L67) / [VideoEncode.h:190](../../package/PianoVideo/VideoEncode.h#L190)). `ours-OK`. Other E1 resolutions require their own padded-dimension check. |
| Extra PERSIST, PARTIAL, SCRATCH_1, separate ENC_REF | Not in this gen2 encoder's selected queue tables; ARP is the persistent allocation. A scratch sizing helper alone does not select that buffer for queueing. | Absent. `ours-OK`; firmware interfaces beyond K are `unknown`. |
| Buffer packet structure and host flags | Command `01000009`, interrupt flag 1, STRUCTURE=5, 16 words; zero index for these internals, 4 KiB-aligned base, size in bytes, offsets/data length/PTS/reserved zero, nonsecure payload flag `00000200` ([iris_hfi_gen2_command.c:1308–1353](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1308-L1353), [iris_hfi_gen2_packet.h:64–103](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_packet.h#L64-L103)). | [VideoEncodeSession.h:88–97](../../package/PianoVideo/VideoEncodeSession.h#L88-L97) constructs exactly this. Captured mappings are aligned and disjoint with guards ([VideoBuffers.h:55–70](../../package/PianoVideo/VideoBuffers.h#L55-L70)). `ours-OK` statically; actual words 1..15 were not printed, so full wire-byte equality is `unknown`. |
| COMV_BUFFER_COUNT versus memory count | Property port 1, U32=4 from external output minimum (or firmware override), immediately before COMV ([iris_hfi_gen2_command.c:1293–1339](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1293-L1339)). One COMV allocation sized by `R`, not four allocations. Host ceiling 64 is a separate property. | Header 43 / packet 50 exactly matches; `ours-OK`. Does not cure the external pool count mismatch. |
| Internal queue timing and returns | ARP before first START; VPSS before RAW START if needed; BIN→COMV-count→COMV→NON_COMV→LINE→DPB before BITSTREAM START ([iris_venc.c:481–538](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_venc.c#L481-L538), [iris_hfi_gen2.c:2057–2067](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2.c#L2057-L2067)). | Five output queues ordered correctly and returned on STOP; `ours-OK`. ARP is the omitted earlier operation. |

**B1 — BIN.** Below 3680 macroblocks, internal estimate
`S=A(floor((3*W*H*2)*5/4),4096)=2,306,048` for VBR. Then
`B=A(floor(17*S/10),256)=3,920,384`, `Q=A(floor(B/2),256)=1,960,192`,
`SAO=64*floor((W+32)*(H+32)/1024)+384=21,888`;
`BIN=A(Q+SAO,256)*P*3+512=23,787,008`. This large allocation is expected
for this low-resolution branch; it is not evidence of using decoder BIN.

**B2 — NON_COMV.** Set
`F=6*A(4*A(11*(256+16*(14+C(H/16,8))),256),512)=147,456`,
`M=(H/16)*floor((W+128)/16)=1440`.
Sum `A(256+16*N,256)+20480+6144+F+A(256+4*N,256)`
`+A(256+floor(N/8),256)+98304+2816`
`+2*A(16*C(M,8),256)+A(3*A(2*N,8),256)=313,856`.

**B3 — LINE.** The eight-bit AVC base is the sum of
`2*A(W,256)`, `A(8*W+1024,256)+2*A((8*W+1024)/2,256)`,
`A(4*A(80*C(H,16)+512,512),256)`, `A(2*H+256,2048)`,
`A(256+16*(W/16),256)`, `4*A(256+64*floor(H/64),256)`,
`4*A(256+64*floor(H/32),256)`, `A(2*W,256)`,
`4*A(256+64*(H/16),2048)`, `A(256+16*(W/16),256)`, and
`A(16*floor(W/32),256)` = **56,576**.
For `d=max(W,H)`, VPU33 adds
`A(2*P*(65536+64+32*C(2*d,4)+64)`
`+2*(16*C(2*d,4)+256+16*C(d,4)+256)`
`+8*(W+1)+8*H*P,256)` = **644,352**.
This VPSS **line storage is always included in LINE**; it is distinct from
the optional standalone VPSS image buffer whose size is zero here.

The existing nine-entry guarded span recomputes as
`sum(A(capacity,16384)+16384)=29,868,032` bytes. Four RAW + four BITSTREAM
+ ARP + the same five scratch/reconstruction buffers would be **14 entries,
32,980,992 guarded bytes**, using [VideoBuffers.h:55–70](../../package/PianoVideo/VideoBuffers.h#L55-L70). This fits the existing
256 MiB arena; no arena enlargement is needed for this workload. All new
allocations still need mapped pages and invalid guards; borrowing a
previous decoder PERSIST allocation is not an ARP submission.

The external count mismatch is confirmed against both size/count functions
and V4L2's `min_reqbufs_allocation=4` on each queue
([iris_vidc.c:120–142](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vidc.c#L120-L142)). However, firmware had received **no external buffers** at
stage 5, and host-max=64 advertises only a ceiling. It cannot inspect the
host's unsubmitted two-entry pool from the logged START packet. Thus the
count mismatch is a later qualification gate, not a demonstrated direct
cause of this START rejection.

### Additional gate after START: timestamp units

`ours-mismatch`: [VideoEncodeSession.h:278](../../package/PianoVideo/VideoEncodeSession.h#L278) computes `frameUs()=1000000/fps`, then
[VideoEncodeSession.h:340](../../package/PianoVideo/VideoEncodeSession.h#L340) passes `f*frameUs()` directly as HFI timestamp;
[VideoEncodeSession.h:130–132](../../package/PianoVideo/VideoEncodeSession.h#L130-L132) divides returned PTS by the same microsecond interval.
The pinned gen2 buffer contract explicitly says **nanoseconds**
([iris_hfi_gen2_packet.h:85–100](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_packet.h#L85-L100)). Linux copies `vb2->timestamp` unchanged into the driver
buffer ([iris_common.c:13–25](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_common.c#L13-L25)), then unchanged into HFI ([iris_hfi_gen2_command.c:1320–1325](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_hfi_gen2_command.c#L1320-L1325)).
At 30 fps, E1's second frame would carry **33,333**, while nanosecond PTS
should be approximately **33,333,333**. Frame-index recovery must use the
same corrected rational timestamp schedule; changing only TX units leaves
EBD indexing wrong. Wall-clock latency measurements may remain in
microseconds. Internal-buffer timestamps are zero and correct.

This defect cannot cause the captured stage-5 failure: no RAW packet was
sent. It compounds the missing time-delta-RC override if startup is fixed,
so both belong to the same next implementation. Existing host tests check
microsecond PTS and 14 capability entries / nine layout entries
([enc_host.cpp:190–192](../../package/PianoVideo/tests/enc_host.cpp#L190-L192), [enc_host.cpp:370–380](../../package/PianoVideo/tests/enc_host.cpp#L370-L380), [enc_host.cpp:404–406](../../package/PianoVideo/tests/enc_host.cpp#L404-L406)). Those checks encode the
current assumptions and do not independently establish Linux conformance.

### Closed findings and ranked causal explanation

| Rank / scope | Finding and causal limit | Status |
| --- | --- | --- |
| 1 — activation resource | ARP is required by both Linux stream-on paths and completely absent from E1. Input START success can coexist with deferred resource checks when BITSTREAM START activates encoding. Missing ARP best fits the complete trace, including healthy buffer returns/close. That timing is an inference; firmware check order is unavailable. | `ours-missing`; strongest stage-5 candidate |
| 2 — startup control | H.264 deblock PACKED32 `00060600` is sent unconditionally by K but omitted. Firmware's default and whether activation requires an explicit write are not established. It remains a candidate independent of ARP. | `ours-missing`; exact fatality `unknown` |
| 3 — startup control / later quality | K unconditionally disables time-delta RC with U32 0; E1 does not. Linux describes enabled as the firmware default. Missing it is a known state difference, especially with E1 timestamps, but there is no proof it alone makes START fail. | `ours-missing`; stage-5 fatality `unknown` |
| Later allocation/credit gate | Two RAW / two BITSTREAM allocations versus K's four each. No external buffer payload was visible to firmware yet, so not a direct count check demonstrated at stage 5. | `ours-mismatch` |
| Later frame-timing gate | Microsecond-valued HFI PTS versus K's nanosecond contract, with coupled EBD indexing. Unreached in the supplied attempt. | `ours-mismatch` |
| Excluded static hypotheses | Port swap, wrong AVC codec/domain, wrong START type/flags/order, `03000193` misidentification, missing encoder subscription, five existing scratch sizes/counts/ports, and needing four separate DPB queues do not match the source comparison. No watchdog/SMMU failure is indicated in the supplied evidence. | `ours-OK` for checked fields; not proof against all future faults |

The closed **host-side root cause** is an incomplete transcription of the
pinned encoder startup contract: only the per-port internal tables were
implemented, omitting the independent ARP path, and the capability script
omits two active setters. The pool and timestamp defects would remain
behind that gate. Static evidence closes these five implementation gaps;
it does **not** close which opaque firmware predicate produced flag 4.
Do not relabel the generic -19 as an observed ARP-specific error, and do
not treat an ARP-only fix as closure of all known gates.

## Fix-image gate

**E1 640×480 gate closed on RAM.** `v131y12u-e1fix1` (image `43ea26f225fc88cb`,
`b10-e1-stage5-fix` commits `2bd83f8ad` / `721bb6011`) passed all five
conformance gates live at 640×480, 10 frames and 2 Mbps.

| Gate | Hardware result |
| --- | --- |
| Session ARP | `HFI_BUFFER_ARP` (type 9, port 0, 204,800 B) was mapped and queued exactly once before `START(2)`. |
| H.264 deblocking | `HFI_PROP_DEBLOCKING_MODE` carried `00060600` on both RAW and BITSTREAM stream-ons. |
| Time-delta rate control | `HFI_PROP_TIME_DELTA_BASED_RATE_CONTROL` carried `0` on both stream-ons. |
| External buffer pools | The 14-entry guarded layout (32,980,992 B) mapped, with four RAW and four BITSTREAM slots. |
| Frame timing and completion | With the nanosecond HFI timestamp fix in place, the frame gate passed with 10 EBD and 10 nonempty FBD. |

Both `HFI_CMD_START` replies (RAW port 2, BITSTREAM port 1) and `DRAIN ACK`
returned `flags 00000001`. `STOP(1)`, `STOP(2)` and `CLOSE` completed with
`close-error 0` and `queued-at-close 0`. The kernel finished in **314 ms**
with **22,439 B** of H.264 and zero watchdog events, faults or quarantine.
The host FFmpeg check decoded **10/10** frames and measured **51.6 dB average**
and **51.2 dB minimum** PSNR-Y against a 30.0 dB threshold. On the same boot,
`video-v6 --qualify` passed **13/13** AVC decode phases after switching the
resident Iris arena from E1 to AVC. [Full RAM results](../results-2026-11-27.md#v131y12u-e1fix1-2026-11-27)

Both STOPs preserve ARP ownership. A successful CLOSE fences the session's
DMA, while a failed CLOSE or a device fault leaves the backing in quarantine.
Linux keeps ARP outside the port tables and destroys it after session CLOSE.
[Iris persistent buffer lifecycle](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_buffer.c#L661-L706)
[Iris session CLOSE](https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/media/platform/qcom/iris/iris_vidc.c#L312-L325)

E1 stays RAM-only and opt-in. Installed daily `v13.1y12q` defaults to
`piano-video-enc=0` with an empty VT encoder inventory (`encs=0`). The
inventory stayed empty with `piano-video-enc=1` in the qualifying RAM run.
**HOLD** pending analysis for 720p/1080p hardware runs and E2+ userspace
VideoToolbox encoder registration (`VTCompressionSession`). Implementation
and hardware qualification of those paths remain open.
