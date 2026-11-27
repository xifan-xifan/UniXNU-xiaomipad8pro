# B3/B13: sensor calibration, real-lux ALS and desktop autorotate

Static analysis for Xiaomi Pad 8 Pro (`piano`, SM8750), 2026-11-27. **Status: HOLD.**
The unit calibration pack supports installed automatic brightness; direct persist-backed
calibration and physical desktop autorotate qualification remain open. This document
authorizes no device operation, image installation or daily-default change.

## Evidence boundary

Our source baseline is commit `028ce22d65dff140c63129d9b944a4631c643758`.
The Ubuntu kernel reference is **exactly** commit
`7f8d6bedde479a01f4a05d6d0581b43c7fd20a01`. Reference files were read from
that commit's git objects, independently of the reference checkout's current HEAD
and working-tree changes. Kernel paths below are relative to that kernel repository;
other paths are relative to this repository. Linux is a specification only; no Linux
implementation is copied here.

Evidence labels in the matrix mean:

- `ours-OK`: the named implementation/check exists in this baseline. Its evidence
  scope is stated: source, existing synthetic tests, or previously recorded hardware.
  Source or a host test does not establish a physical pass.
- `ours-missing`: a specified implementation or integration is absent from this baseline.
- `unknown`: the available source and public result records do not establish the
  physical behavior, per-unit equivalence or calibration accuracy.

No new tablet observation, private calibration inspection or host sensor simulation
was performed for this analysis. Existing test cases are cited as coverage, not as
newly executed tests. The staged qualification below is future work.

[sensors.md](sensors.md) combines a current daily-status section with historical
bring-up notes. Its November 11 pack record gives **338,020 bytes**, comprising 294
regular files: 228 registry files totaling 99,033 bytes and 66 ROM configuration
files. Directory entries add to the pack table's entry count; 294 is not that count.
These are recorded aggregates, not a new measurement or a hash of today's loaded pack.
The current page reports installed ALS checks on v13.1y12k/v13.1y12l and the
v13.1y12q daily line; real ambient-light transitions remain pending.
[November 25 results](../results-2026-11-25.md),
[November 26 results](../results-2026-11-26.md), and the
[B3/B13 backlog](../BACKLOG-hardware.md#2-backlog-b1b15) support that distinction.

The [November 27 RAM results](../results-2026-11-27.md#ram-qualification) record
v131y12t-qual1's accelerometer stream and long boot-argument read passing
`RO1/RO2/RO6/BA3`. They do not close physical four-edge, cover-detach or all-angle
touch qualification. [auto-rotate.md](auto-rotate.md) records earlier manual 0°/90°
display and native panel-axis touch evidence. Historical phrases such as discovery
“at S2” in sensors.md do not change the implemented levels: reverse file service
at `piano-sensors=2`, discovery at 3, streams at 4.

## Source map and pinned Ubuntu comparison

The source keys below are used throughout the Path & Check Matrix. Each names the
actual file and symbols, so an `ours-OK` row can be checked without relying on a
historical status paragraph.

| Key | Our files and exact symbols |
| --- | --- |
| P1 | [PianoSensors.cpp](../../package/PianoSensors/PianoSensors.cpp): `readArgs`, `loadPackFromAddon`, `loadPack`, `partitionRead`, `packOk`, `readConditions`, `publishStatus`, `publishTypes`, `openStreamPort`, `ssPublish`, `sendAmbient`, `fileServiceStopped`; [SensorsLoad.h](../../package/PianoSensors/SensorsLoad.h): `SL_PARTITION`, `SL_PATH`, `SL_MAX_BYTES` |
| P2 | [SensorsPack.h](../../package/PianoSensors/SensorsPack.h): format/constants; [SensorsPack.c](../../package/PianoSensors/SensorsPack.c): `ssp_header`, `ssp_open`, `ssp_path`, `ssp_find`; [SensorsLoad.c](../../package/PianoSensors/SensorsLoad.c): `sl_check`, `sl_load`, `sl_contents` |
| P3 | [make-sensor-pack.py](../../package/PianoSensors/tools/make-sensor-pack.py): `add_registry_tar`, `add_rom_config`, `add_soc0`, `pack`; [SensorsRpc.c](../../package/PianoSensors/SensorsRpc.c): `std_open`, `write_mode`, `apps_std`, `std_mkdir`, `refuse` |
| P4 | [PersistExt4Ro.h](../../package/PianoSensors/PersistExt4Ro.h): `persist_ext4::Reader::{open,inode,lookup,next,read,mapExtents,mapIndirect}`, `ReadBlocks`; [PersistExt4Verify.h](../../package/PianoSensors/PersistExt4Verify.h): `verifyRegistry`, `Equivalence`; [S6 contract](../../package/PianoSensors/PERSIST-EXT4.md) |
| P5 | [SensorsSsc.h](../../package/PianoSensors/SensorsSsc.h) and [SensorsSsc.c](../../package/PianoSensors/SensorsSsc.c): `SSC_QMI_SERVICE`, `ssc_qmi_parse`, `ssc_qmi_report`, `ssc_events_open`, `ssc_events_next`, `ssc_parse_sensor_event`, `ssc_parse_proximity_event`; [SensorsClient.c](../../package/PianoSensors/SensorsClient.c): `sc_type_name`, `send_request`, `request_failed`; [SensorsClient.h](../../package/PianoSensors/SensorsClient.h): `SC_TYPES`, `ScSensor` |
| P6 | [SensorsStream.c](../../package/PianoSensors/SensorsStream.c): `ss_accel_blocker`, `apply`, `restart`, `ss_from_list`, `build`, `sample`, `ss_receive`, `light_target`, `light_invalid`, `ss_tick`, `ss_force_off`; [SensorsStream.h](../../package/PianoSensors/SensorsStream.h): `SS_STREAMS`, `SS_LUX_MS` |
| P7 | [AutoStream.c](../../package/PianoAuto/AutoStream.c): `pas_action`, `pas_event`, `pas_response`, `pas_tick`; [AutoLight.c](../../package/PianoAuto/AutoLight.c): `pal_lux_valid`, `pal_event`, `pal_tick`, `pal_conditions`, `pal_select_curve` |
| P8 | [SensorsPolicy.h](../../package/PianoSensors/SensorsPolicy.h): `PS_ACCEL_AXES`, `PS_TYPE_*`, ALS constants; [SensorsPolicy.c](../../package/PianoSensors/SensorsPolicy.c): `ps_als_curve`, `ps_reference_curve`, `ps_als_target_permille`, `ps_als_step_curve`, `ps_accel_sample`, `ps_rotation_step`; [SensorsOrient.c](../../package/PianoSensors/SensorsOrient.c): `classify`, `sno_update`; [SensorsFixed.c](../../package/PianoSensors/SensorsFixed.c): `sfx_accel_ios`, `sfx_axes_apply` |
| P9 | [AmbientBacklight.h](../../package/PianoAuto/AmbientBacklight.h): `AmbientVersion::newer`, `Ambient::{target,adjust,step,nudge,manual,setAutomatic,invalidate,code,shift}`; [AndroidBrightness.h](../../package/PianoAuto/AndroidBrightness.h): `pa_sdr_code`, `pa_sdr_permille`, `PA_SDR_MIN_CODE`, `PA_SDR_MAX_CODE`, `PA_SDR_MAP_CRC` |
| P10 | [PianoBacklight.cpp](../../package/PianoBacklight/PianoBacklight.cpp): `readArgs`, `poll`, `serve`, `restartAmbient`, `applyAmbient`, `publishState`, `publishAndroidApply`; [Ktz8866.h](../../package/PianoBacklight/Ktz8866.h): `Controller::{step,userLevel,manualCode,ambient,ambientSnapshot,writeCode,applyLit}`, `Steps::build` |
| P11 | [AutoRotate.c](../../package/PianoAuto/AutoRotate.c): `par_defaults`, `par_cover`, `par_edge`, `par_observed`, `par_applied`, `par_step`, `par_deadline`; [piano-rotated.m](../../package/PianoAuto/daemon/piano-rotated.m): `main`, `configure`, `readInputs`, `sensorsTick`, `runPolicy`, `backendInit`, `applyRotation`, `degreesShown`, `pushTouch`, `reconfigured` |
| P12 | [PianoBootArgs.h](../../package/include/PianoBootArgs.h): `pianoBootArgs`, `pianoBootArgValue`; [LaunchAgent](../../package/PianoAuto/daemon/com.unixnu.piano-rotated.plist): `LimitLoadToSessionType`, `KeepAlive` |
| P13 | [PianoKeyboard.cpp](../../package/PianoKeyboard/PianoKeyboard.cpp): `PianoKeyboard::acceptHall`, `PianoKeyboard::coverAttached`, `piano-keyboard-attached`, `piano-keyboard-hall`; [TouchRotation.h](../../package/PianoAuto/TouchRotation.h): `pat_rotate`, `pat_unrotate`, `pat_request`, `pat_latch` |
| P14 | [PianoTouch.cpp](../../package/PianoTouch/PianoTouch.cpp): `PianoTouch::{gestures,setProperties,callPlatformFunction}`, `panelDisplayUuid`; [TouchMatrix.h](../../package/PianoTouch/TouchMatrix.h): `chooseOrientation`; [TouchMt.h](../../package/PianoTouch/TouchMt.h): `mtIdle`; [TouchNative.h](../../package/PianoTouch/TouchNative.h): `nativeUpdate`, `nativeIdle` |
| P15 | [SensorsGuard.c](../../package/PianoSensors/SensorsGuard.c): `sg_observe`, `sg_may_request`, `sg_result`, `sg_file_call`, `sg_strike`; [SensorsRpc.c](../../package/PianoSensors/SensorsRpc.c): `invoke`, `ssr_core_step`; [PianoAdspContract.h](../../package/include/PianoAdspContract.h): client API |
| P16 | Existing synthetic coverage: [persist_ext4_ro_host.cpp](../../package/PianoSensors/tests/persist_ext4_ro_host.cpp): `extraction`, `equivalenceFailures`; [streams.c](../../package/PianoSensors/tests/streams.c): `test_accel_sample_time`, `test_accel_wake_sample_time`; [auto_host.c](../../package/PianoAuto/tests/auto_host.c): `test_rotation_transitions`, `test_android_als`; [ambient_host.cpp](../../package/PianoAuto/tests/ambient_host.cpp): `test_trim`; [touch_native_host.cpp](../../package/PianoTouch/tests/touch_native_host.cpp): `testRotation` |

All K rows refer to kernel commit `7f8d6bedde479a01f4a05d6d0581b43c7fd20a01`.

| Key | Kernel-relative file/symbol | What it establishes and comparison |
| --- | --- | --- |
| K1 | `arch/arm64/boot/dts/qcom/sm8750.dtsi`: `remoteproc_adsp`, `glink-edge/fastrpc`, `compute-cb@3`; `arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-desktop.dts`: `&remoteproc_adsp` | ADSP on LPASS, `fastrpcglink-apps-dsp`, label `adsp`, coherent context bank 3, SIDs `0x1003/0x80` and `0x1043/0x20`. No nonsecure-domain property on this channel. The SoC node starts disabled; the desktop DTS enables it and selects the ADSP firmware. P1/P8 use the same channel and bank facts. |
| K2 | `drivers/misc/fastrpc.c`: `SENSORS_PD`, `fastrpc_init_attach`, `fastrpc_internal_invoke`, `fastrpc_get_args`, `fastrpc_invoke_send`, `fastrpc_release_current_dsp_process`; `include/uapi/misc/fastrpc.h`: `FASTRPC_IOCTL_INIT_ATTACH_SNS` | Sensor PD is 2; attach uses the init handle, one client-ID input and kernel client ID 0. Invoke constructs a payload even without buffers; message carries caller TID, context/PD and DMA metadata. Release belongs to teardown. P15 implements the reverse service on PianoPower's existing DSP; it is not a second remoteproc owner. |
| K3 | `arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts`: `gpio-keys/switch-lid`, `keyboard@4c`; `drivers/hid/hid-nanosic-wn8030.c`: `nanosic_wn8030_handle_vendor` | GPIO80 active-low is `SW_LID`; MCU command `0xa2`, byte 9 bit 0, supplies attach, inverted for `SW_TABLET_MODE`. These are distinct from each other and from the keyboard's own accelerometer report. P13 likewise separates attachment from hall state. |
| K4 | `arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts`, `arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-desktop.dts`, `arch/arm64/boot/dts/qcom/sm8750.dtsi` | These board/SoC descriptions contain the hub transport, not direct LSM6DSO/QMC6308/STK3BCX/SIP1328 sensor nodes. DTS absence does not establish physical absence behind SSC. Chip identities below are attributed to sensors.md's ROM/SSC evidence, not inferred from generic Linux IIO drivers. |
| K5 | `fs/ext4/super.c`: `ext4_superblock_csum_verify`, `ext4_group_desc_csum_verify`; `fs/ext4/inode.c`: `ext4_inode_csum_verify`; `fs/ext4/namei.c`: `ext4_dirblock_csum_verify`; `fs/ext4/extents.c`: `ext4_extent_block_csum_verify` | Linux has filesystem metadata checksum verification. P4 accepts some checksum-bearing layouts but performs structural validation only. A successful P4 read is not equivalent to Linux ext4 integrity verification. |
| K6 | `drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_12_0_sm8750.h`: VIG `.features`; `drivers/gpu/drm/msm/disp/dpu1/dpu_hw_catalog.c`: `VIG_SDM845_MASK_SDMA`; `drivers/gpu/drm/msm/disp/dpu1/dpu_hw_sspp.c`: `MDSS_MDP_OP_FLIP_LR`, `MDSS_MDP_OP_FLIP_UD` | The SM8750 catalog does not give these VIGs inline quarter-turn rotation. Horizontal/vertical flips do not establish a 90° scanout path. WindowServer composition, surface layout and native copy/flip eligibility must be checked separately. |

Ubuntu's libssc, sensorproxy, registry preparation and desktop brightness policy
are userspace, outside this kernel pin. Their historical descriptions in sensors.md
are useful context; this analysis does not claim to have revalidated those external
userspace revisions from the kernel tree.

## Calibration and transport: pack today, persist reader separately

The implemented chain is PianoPower's running ADSP → FastRPC sensor PD file
service → registry/configuration from the in-memory pack → SSC QMI service
400 (`0x190`) over QRTR → per-type SUID discovery → individual light/accel
clients. GLINK FastRPC serves files; SSC QMI carries discovery and samples.
They are different protocols with a common DSP dependency (P1/P5/P6/P15, K1/K2).
Calibration groups are supplied to the DSP, which applies them; the AP's axis map
and lux-to-brightness policy are separate from factory `fac_cal` data
([sensors.md, hardware facts](sensors.md#2-hardware-facts)).

`loadPackFromAddon` takes a carried, inflated and validated pack before the ESP
pack. An invalid carried pack fails rather than silently falling back. Without a
carried pack, `loadPack` reads `EFI/UniXNU/piano-sensors.pack` on GPT partition
`esp` through NativeUfs/FAT. `sl_check` completes before file service attach;
DSP file reads then use RAM, avoiding filesystem latency during callbacks. A pack
without registry content can pass format validation: `sl_contents` reports presence
bits and `packOk` explicitly reports missing registry/stock defaults. Format validity
alone does not prove unit calibration, completeness or current provenance (P1–P3).

The exact `SSCPACK1` invariants are:

| Part | Implemented invariant and boundary (P2/P3) |
| --- | --- |
| Header | Little endian, 32 bytes: magic at 0; u32 entry count at 8; u32 names-area size at 12; u64 total at 16; u32 sum at 24; zero u32 at 28. Count is 1–8192, names size 1–1,048,576, total at least header plus table/names. `sl_check` additionally requires total equal the actual supplied size, at most 4 MiB. |
| Table | Each 24-byte entry is u64 offset, u64 size, u32 name offset, u32 flags. Flags are exactly 0 (file) or 1 (directory). A directory has zero offset and size. File ranges start after table/names and fit within total, using subtraction bounds. |
| Names | Name offset is inside the area, its first byte is `/`, and the area's last byte is NUL, so each referenced string terminates within it. Names are strictly increasing by unsigned byte comparison, rejecting duplicates. The producer derives all parent directories and restricts each encoded path to fewer than 256 bytes. The parser itself does not enforce every producer canonical-path rule or disjoint file payloads. |
| Checksum | Sum of **table plus names bytes**, modulo 2^32, equals the header sum. Header and file payloads are not covered. This is not a CRC, cryptographic authentication, or a per-file integrity check. |
| Source checks | `add_rom_config` checks each ROM file against its manifest SHA-256. `add_registry_tar` selects one outer registry tree, refuses links/special files, and includes its regular files. There is no built-in factory-registry authenticity check. A separately retained whole-pack digest/provenance record is needed to identify a candidate pack. |
| Lookup aliases | `ssp_path` canonicalizes DSP requests, rejects parent traversal, maps `/persist` to `/mnt/vendor/persist` and `/system/vendor` to `/vendor`. These device paths are file-service aliases, not host storage locations. |

P4's `Reader` only receives a read callback and image length. It has no write,
mount, allocation or journal-replay API. Its initial superblock read is 1024 bytes
at partition byte 1024. `open` requires magic `0xef53`, revision 0/1, state exactly
1 and `s_last_orphan` at `0xe8` zero. Supported incompatibility mask is `0x26c2`;
supported read-only-compatibility mask is `0xb57b`; other bits are refused.
In particular, recovery-required, `meta_bg`, bigalloc, inline-data, encryption and
casefold layouts are not accepted. Clean filesystems may have a journal; it is ignored.

The subset supports 1/2/4 KiB blocks, power-of-two inode sizes from 128 bytes to
one block, contiguous primary descriptors of 32 bytes or 64 with the 64-bit feature,
and matching block/inode group counts. It checks filesystem/descriptor/inode-table
bounds and live regular-file/directory inode modes. Symlinks and special files are
refused. Extents have depth at most four external levels, ordered nonoverlapping
logical spans, valid physical ranges and no repeated block on the selected branch.
Legacy direct and single/double/triple indirect maps also work; holes/unwritten
extents read as zeros. Linear directory scanning skips unused/index/tail records;
record lengths, names, inode numbers, duplicate lookup matches and ancestor cycles
are checked. Limits are 255-byte components, 4096-byte paths, 32 components and
4 MiB per directory. Partial output on any read error must be discarded (P4).

These checks do **not** verify ext4 checksums, allocation ownership, all unvisited
branches, or semantic validity of calibration JSON. Checksum feature acceptance
does not imply checksum verification (K5). P4's immutable-image contract must also
be enforced by any future block adapter; a const callback signature cannot prove
the backing storage remains unchanged.

`verifyRegistry` validates the pack and compares image `/sensors/registry` against
pack `/mnt/vendor/persist/sensors/registry`, including the inner `registry` and
outer version/list files. It requires the same **nonempty set of regular-file
paths, sizes and bytes**, comparing 4 KiB chunks. Empty regular files count; empty
directories do not contribute files. Vendor/ODM files, metadata timestamps,
permissions and other persist content are outside this equivalence. Traversal is
bounded to 8192 live entries, 16 MiB of directory bytes, reader depth and the
pack's 255-byte usable path length. Results are `Match`, `Mismatch`, `InvalidPack`,
`ImageError` or `Limit`, carrying no calibration data (P4).

There is **no runtime persist-reader call site, boot switch or build integration**: only the
synthetic [persist reader test](../../package/PianoSensors/tests/persist_ext4_ro_host.cpp)
includes the verifier. The S6 stage cannot currently replace the pack's registry. Even after
registry integration, vendor/ODM configuration would still need a source. Real
image/pack equivalence, the unit's current filesystem features and calibration
quality remain unknown; the 338,020-byte record does not answer them.

**Zero writes to persist is invariant.** `SensorsRpc.c:std_open` rejects modes
containing `w`, `a` or `+`; `apps_std` refuses `fwrite`, remove, rename, truncate
and rmdir. `std_mkdir` only acknowledges an already packed directory, never creates
one; flush/fsync acknowledgments do not write. The current UFS path reads the ESP,
not persist. A future S6 adapter must expose only bounded partition reads, preload
and verify before DSP attach, and retain these RPC refusals. No mount/recovery,
registry repair, DSP write-through or writable fallback is part of this plan.

The guard observes charger/link/DSP health, paces requests, detects file-call
storms and latches sensors off after a fatal condition. The file service answers
declined calls and releases an attached session on failure where the DSP is healthy
enough to receive it. Sensors do not restart the ADSP; that DSP also supports
charging. A future persist read failure must not leave an attached sensor PD waiting
for storage or for an unanswered file request (P15, P1:`fileServiceStopped`).

## Real-lux ALS and the dark-room end

P5 discovers the default `ambient_light` sensor with update notifications. P6's
`ss_from_list` uses attributes to select request 514 on change, or 513 at the
lowest positive advertised rate if it is a streaming sensor. Light and accel each
have their own QRTR/QMI client. Responses to QMI request `0x20` acknowledge
receipt; reports `0x21`/`0x22` carry SNS events, with a matching SUID required.
Event 1025's first float is light. Request jumbo mode is not enabled even though
the decoder understands jumbo reports (P5/P6).

`pal_lux_valid` rejects NaN, infinity and negative nonzero values; either signed
zero is valid darkness. `pal_event` validates before the first-event gate, clamps
finite light to the selected curve endpoint, converts to milli-lux with integer
helpers and rounds to whole lux. Consequently `piano-sensors-lux` is a bounded,
rounded calibrated SSC reading, not an unrestricted raw photometer channel.
Accuracy status 0 is counted as unreliable but is not itself a rejection gate.
Numerically valid samples do not prove correct factory calibration or absolute
lux accuracy (P5–P8).

The chain then follows P8 → P9 → P10:

- `ps_als_step_curve` initializes immediately from a session's first sample.
  Subsequent goals need a 30-permille difference. Smoothing uses time constants
  of 1 s upward and 4 s downward, with updates at most every 250 ms, normally
  requiring 10 permille of published movement; the final goal can still publish.
  These are smoothing constants, not a promise to settle within 1/4 s.
- `pal_tick` continues toward the chosen goal between on-change events and stops
  scheduling smoothing when settled. Its no-sample `ps_als_step` call retains
  the already selected goal, including a reference-curve goal.
- `ssPublish` publishes an immutable `piano-sensors-light` dictionary containing
  target, lux, session, sequence, valid and fault, plus curve/revision on the
  versioned path. `sendAmbient` provides the matching direct platform-function
  transaction. Individual diagnostic properties are not an atomic snapshot.
- `PianoBacklight::applyAmbient` rejects old sequence or wrong curve/revision on
  the bounded SDR path. `Controller` applies codes only while controlling and
  lit. Both IC brightness registers must read back before common application is
  confirmed; a one-sided failure restores the last common level where possible.
  Requested code, verified IC code and optical brightness are separate observations.

The **reference** curve is selected through PianoBacklight's curve/revision/floor
snapshot when the Android-reference adapter is enabled, not merely because ALS
is enabled. `piano-backlight-reference=0` selects the legacy curve on that adapter;
reference 1 selects the reference curve. The adapter requires native brightness,
the explicit Android boot option and compatible IC configuration. Legacy policy
starts at 40 permille at zero lux; the reference policy interpolates Android-declared
nits and converts through `AndroidBrightness.h` into code permille (P1/P8–P10).

| Reference lux | 0 | 1 | 2 | 4 | 6 | 8 | 10 | 15 | 20 | 25 | 30 | 82 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Declared nits | 3.5 | 4.5 | 6 | 15 | 21 | 27 | 36 | 52 | 71 | 87 | 97 | 101 |

These explicit sub-82-lux anchors avoid a single straight line from zero to
82 lux. Above them, anchors continue to 11,165 lux / 600 declared nits. The shared
DBV/code table has CRC-32 `9673290e` over little-endian u16 pairs; that CRC is
unrelated to the sensor-pack sum or ext4 checksums. The reference lit code interval
is 178–1952, with the effective floor at least the configured floor; generic
`piano-backlight-min` defaults to 128 and clamps to 32–1024. Declared nits are
not a new measurement of physical luminance. See
[reference controls](android-reference-controls.md#brightness) and P8–P10.

`Ambient::step` preserves the keys' offset for the boot. Linear mapping follows
the controller's factor-of-two per three steps; exponential mapping shifts by
1/16 of the usable code interval. `nudge` skips offsets hidden by clamping and
does not accumulate invisible offset at an endpoint. `Ambient::adjust` makes a
user slider level a retained trim: additive codes for exponential mapping, a
level/curve ratio for linear mapping. `PianoBacklight::serve` routes a recognized
CoreBrightness user move through `Controller::userLevel`, keeping auto requested.
Before a fresh target, a pending user level and deferred key steps can be retained.
Explicit administrative `piano-backlight-percent`/`manualCode` or auto=0 instead
chooses manual; re-entry waits for a fresh valid target and preserves offset/trim
(P9/P10). The older reference-controls paragraph saying every native slider request
chooses manual is too broad for this baseline's intent classification.

Display/lid/system sleep gates retire validity; stream reopen starts from fresh
light. Stream failures and guard faults block automatic writes for the boot;
setting auto=1 cannot clear a latched fault. `AutoStream` serializes one outstanding
request, keeps an event arriving before the enable response, disables before
recovering an uncertain state, and bounds first-event re-arms. It does not enforce
a recurring fresh-sample deadline after a first valid on-change event. Unchanging
lux can be legitimate, but a stalled bright-dark-bright sequence cannot be ruled
out by an initial sample or an unchanged valid flag (P6/P7/P9/P10).

For that sequence, observe `piano-sensors-light-raw`, `-sample-valid`, `-status`,
`-valid-samples`, `-invalid-samples`, `-sample-ms`, `-valid-ms`, stream counters and
the immutable snapshot together. Lux-only changes can publish every `SS_LUX_MS`
(250 ms) even when hysteresis keeps the target. On the revisioned path, the light
transaction sequence advances for target/validity changes, not lux-only changes;
PianoBacklight can therefore retain the lux associated with its last accepted
target. Its `-ambient-lux` alone cannot prove SSC stalled. Above-endpoint clipping,
sub-lux rounding, hysteresis, user trim and floor/ceiling clamps likewise can hold
the code while raw light changes. Conversely, a changing code without fresh raw
samples may just be smoothing (P1:`ssPublish`, P6:`light_target`, P9:`newer`, P10).

## Desktop rotation, cover gates and touch

P8 converts SSC acceleration in m/s² to fixed-point down gravity with
`sfx_accel_ios`, then applies `PS_ACCEL_AXES="a+y+x+z"`. SSC +X, +Y, −X and −Y
map to top, left, bottom and right, respectively, under that convention.
`SensorsOrient.c:classify` accepts 0.6–1.4 g, needs a 1.3× leading-axis margin
for a changed orientation, and identifies flat poses when Z² is at least 72% of
total magnitude² (about 85% of magnitude). `sno_update` needs 250 ms of continuous
candidate dwell. Ambiguous diagonals, shocks/free fall and nonfinite components
break a candidate hold; flat states publish edge `none`, retaining the displayed
angle. This is accelerometer classification, not gyro/magnetometer fusion.

Receive time is the default dwell clock, so all events in one report have the
same time. Opt-in `piano-sensors-accel-sample-time=1` uses each event's SSC
19.2 MHz QTimer (`ticks/19200` milliseconds). Missing timestamps, backward/wrapped
time and gaps exceeding two requested sample periods reset candidate dwell;
duplicates do not update classification. The gap boundary is 200 ms at 10 Hz.
A discontinuity alone retains the last accepted edge. In sample-time mode a
closed acquisition gate or new session also clears dwell/edge, including a quick
gate bounce with the same SSC session. These cases have explicit coverage in
[streams.c](../../package/PianoSensors/tests/streams.c): `test_accel_sample_time`
and `test_accel_wake_sample_time`. The recorded RAM stream pass does not establish
the four physical axis signs or behavior through live suspend/wake (P6–P8).

The kernel normally requests accel at 10 Hz only with a rotation consumer, display
not explicitly off, and cover not attached. `readConditions` follows `piano-rotate`
unless test edges are selected. Diagnostic `piano-sensors-rotate=2` forces a consumer;
the separate accel-cover override is allowed only in that forced mode. Unknown
display state counts as on, and a missing attachment property defaults to detached.
Hall is used for the light gate, **not directly for the accel gate**. The daemon
also reads attach, not hall; closed-lid suppression of rotation depends on display
sleep/active-session state. A closed hall with a still-awake display is not a direct
rotation lock. These defaults need explicit qualification, including absent or
delayed properties (P1:`readConditions`, P6:`ss_accel_blocker`, P11:`readInputs`).

`ssPublish` writes edge before sequence and sends an edge-change notification.
The agent watches service changes and polls sequence at 250 ms when edges matter,
5 s when asleep. `par_step` adds a 600 ms edge settle and 1 s minimum between
rotations. Default cover mode 2 forces/holds 0° even under user rotation lock;
mode 1 forces only on attach; mode 0 ignores attachment in the agent, without
overriding the kernel acquisition gate. Detach waits 1.5 s, then a new agent settle.
Manual display rotation holds until a different valid edge arrives. An inactive or
sleeping display supplies no edge and cannot apply a rotation (P11).

There is a narrower sleep-dwell guarantee than auto-rotate.md's wording suggests.
`par_observed(-1)` invalidates the shown/touch angle but does not clear
`rot.edge`/`edge_since`; `par_step` returns on unknown shown angle before calling
`ps_rotation_step`, which is where a no-edge input normally clears dwell. Thus an
inactive pass alone does not guarantee a fresh 600 ms agent settle. **Static
inference:** if the same valid edge is already present at the first active pass,
the previous dwell may remain usable. An active pass observing edge `none`, a
changed edge, or a cover transition does reset it; sample-time acquisition has its
separate kernel reset. The existing `auto_host.c:test_rotation_transitions` panel-loss
case checks suppression/touch state, not a new post-wake settle interval. Qualify
this boundary rather than treating the historical reset claim as established.

The LoginWindow/Aqua LaunchAgent exits successfully before backend setup when
`piano-rotate` is absent/0. Level 1 observes, 2 changes display rotation, 3 also
tells touch. Explicit one-shot/diagnostic command-line options are separate from
that default launch gate. `PianoBootArgs.h` reads the complete merged
`kern.piano_bootargs` into a 4096-byte buffer, falling back to `kern.bootargs` only
on `ENOENT`, and selects the last exact key. The long-read RAM pass addresses this
gate, not physical orientation (P11/P12).

The implemented rotation call path is `runPolicy` → `applyRotation` → runtime
`MPDisplayMgr sharedMgr` / `displayWithID:` → `MPDisplay canChangeOrientation` /
`setOrientation:` in MonitorPanel → WindowServer, followed by public
`CGDisplayRotation` read-back. The internal panel must match vendor/model unless
an explicit diagnostic override is supplied. `backendInit` checks Objective-C
method encodings before calls; the live loop requires an awake console session.
Reconfiguration work is dispatched on the main queue, with inputs refreshed before
deciding. The policy allows 2 s for confirmation, backs off from 2 s up to 60 s,
and gives up after five failures. An already submitted request is not cancelled by
cover attach; it may appear before the follow-up return to 0° (P11).

DisplayServices is relevant to **brightness**, not an implemented rotation setter
in `piano-rotated`. For example,
[piano-brightness-control/main.cpp](../../tooling/native/piano-brightness-control/main.cpp)
resolves `DisplayServicesSetBrightness`/`GetBrightness`, while
[piano-brightness-sync.m](../../tooling/native/piano-brightness-sync/piano-brightness-sync.m)
watches `DisplayServicesClient` brightness intent. The lower rotation symbols
`SLSSetDisplayRotation`, `CGXSetDisplayRotation` and
`IOMobileFramebufferSetRenderingAngle` are documented in
[sensors.md's mechanism verdict](sensors.md#5a1-display-rotation-the-mechanism-verdict).
Their presence does not prove this full call stack executed on the candidate.
[NativeGopDisplay.cpp](../../package/NativeGopDisplay/NativeGopDisplay.cpp):
`NativeGopDisplay::runJob` owns native presentation; the exact angle hook calls,
swap dimensions and flip/copy choice across all angles remain unknown. K6 does
not supply a missing quarter-turn compositor. Historical fallback design text
in sensors.md §5a.6 is not evidence that a transpose fallback is implemented.

Touch first corrects raw positions into fixed 3200×2136 panel axes. Explicit
`piano-touch-flip` overrides the build's per-unit Y override, then calibration
orientation, then panel defaults (`TouchMatrix.h:chooseOrientation`). This per-unit
touch calibration is distinct from the SSC sensor registry. The optional desktop
transform in `TouchRotation.h:pat_rotate` is:

| CG angle | Edge up | Desktop coordinate from panel `(x,y)` | Desktop size | Panel TL, TR, BL, BR land at |
| --- | --- | --- | --- | --- |
| 0° | top | `(x, y)` | 3200×2136 | TL, TR, BL, BR |
| 90° | left | `(2135-y, x)` | 2136×3200 | TR, BR, TL, BL |
| 180° | bottom | `(3199-x, 2135-y)` | 3200×2136 | BR, BL, TR, TL |
| 270° | right | `(y, 3199-x)` | 2136×3200 | BL, TL, BR, TR |

Level 3 sends only a **read-back** angle through `piano-touch-rotation`, gated in
PianoTouch by `piano-touch-rotate=1` and administrator/console-user privilege.
`PianoTouch::gestures` latches `pat_request` only when `gestureIdle`, **`mtIdle`**
and `nativeIdle` are all true and a non-palm tracked contact starts. The `mtIdle`
guard prevents relatching while the multitouch router owns a gesture even if the
pointer engine is idle. A held gesture retains its starting coordinate map.
Property-write success acknowledges a request; the applied rotation property
updates when the frame thread latches it, not immediately (P13/P14).

Native digitizer mode defaults to panel axes and binds to the internal display's
`IOMFBUUID`; `nativeUpdate` uses the unrotated panel coordinates in that mode.
BackBoard supplies desktop rotation, supported by the recorded 90° evidence.
Use agent level 2 for that qualification path. The pointer/multitouch or explicit
native desktop-axis path instead needs the optional transform and level 3.
The two stages must be qualified with a declared coordinate owner. There is still
a display-change→touch-request interval in which a new desktop-axis gesture can
latch the previous angle. The daemon's `pushTouch` also stops retrying after three
refusals at one angle until angle/service changes; its return then suppresses
further retries, rather than proving touch accepted the angle. All-angle optical
and physical-corner checks remain necessary (P11/P14).

## Remaining SSC sensors

| Type | Presence evidence | Native discovery and missing behavior |
| --- | --- | --- |
| Accelerometer / gyroscope | sensors.md §2 attributes both to LSM6DSO from ROM configuration/SSC reports; K4 contains no direct board sensor node | `SC_ACCEL` and `SC_GYRO` are queried. Only accel has a stream and rotation consumer (P5/P6). Gyro samples, units/axes and calibration performance on this native baseline remain unknown. |
| Magnetometer | sensors.md §2 names QMC6308 from ROM configuration and a prior compass report; not established by a kernel IIO binding | `SC_MAG` lookup/attributes exist. No native mag stream or compass consumer exists (P5/P6). Native availability, accuracy and calibration remain unknown. |
| Front light / proximity | sensors.md §2 names STK3BCX and distinguishes front light from rear light | `SC_LIGHT` streams; `SC_PROX` only discovers. P5 contains `ssc_parse_proximity_event` and near/far event constants, but P6 has no proximity channel, policy or macOS consumer. Native near/far behavior remains unknown. |
| Rear light / flicker | sensors.md §2 names SIP1328 and types `ambient_light_back`/`flicker` | Neither type is in `sc_type_name`; no lookup or stream exists. Front ALS success says nothing about rear/flicker availability (P5/P6). |
| Hall / cover | K3 and PianoKeyboard provide GPIO80 lid and MCU attachment | These do not depend on SSC proximity discovery. An OEM hall name in historical sensor configuration does not replace the native cover signals (P13). |

`publishTypes` distinguishes `service not announced`, `pending`, `no answer`,
`none`, `found` without attributes, and attributes with `available` status. A
timed-out lookup is not an absence result; a found SUID is not a sample. Discovery
requests default-only with updates and retains the first SUID, so it is not a
complete inventory of all physical/virtual SSC sensors (P1/P5). No new native
gyro/mag/proximity sample record is established by this analysis.

## Path & Check Matrix

Keys resolve to exact file/symbol citations above. “Recorded” means the linked
existing result, not a check performed during this analysis. Each row assesses
one bounded claim; adjacent implementation and physical-check rows deliberately
have separate verdicts.

| ID | Path or check | Verdict | Evidence, boundary and closure condition |
| --- | --- | --- | --- |
| C1 | ADSP/GLINK FastRPC and QRTR SSC plumbing | ours-OK | Source P1:`openStreamPort`, P5:`SSC_QMI_SERVICE`, P8:`PS_GLINK_CHANNEL`, P15:`invoke`; K1/K2 agree on transport/PD/bank. |
| C2 | File callbacks served from checked RAM pack | ours-OK | P1:`loadPack`, `loadPackFromAddon`; P2:`sl_check`; P15:`ssr_core_step`. Existing installed ALS evidence in sensors.md. |
| C3 | Pack format, bounds and table/name sum | ours-OK | P2:`ssp_header`, `ssp_open`, `sl_check`; exact invariants above. Payload integrity is outside this sum. |
| C4 | Runtime pack payload authentication/checksum | ours-missing | P2:`ssp_open` sums only metadata; P3:`add_rom_config` hashes build inputs, not runtime payloads. Whole-pack provenance must be recorded separately. |
| C5 | Unit factory registry carried by pack | ours-OK | Recorded 338,020-byte pack in sensors.md §5b and installed unit-pack status; P3:`add_registry_tar`. No new contents or digest inspection. |
| C6 | Current pack equals this unit's current persist registry | unknown | P4:`verifyRegistry` exists, but no real-image `Match` is recorded here. Also identify the actually loaded pack and source (P1:`packOk`). |
| C7 | Read-only ext4 subset and work limits | ours-OK | Source P4:`Reader`; synthetic test `extraction`, `indirect`, `wideAddresses`, `partitionBounds`, `corruptMetadata`, `pathAndDirectoryLimits`. |
| C8 | ext4 metadata checksum/ownership verification | ours-missing | P4's structural checks omit these; K5 has distinct checksum verifiers. Treat trusted immutable-image provenance as a separate prerequisite. |
| C9 | Registry path-set/byte equivalence checker | ours-OK | P4:`verifyRegistry`; synthetic `equivalenceFailures`. Nonempty regular-file set only, not whole-filesystem equivalence. |
| C10 | Live persist adapter, registry replacement and opt-in | ours-missing | P1 loads only addon/ESP; P4 headers are included only by the host test. No runtime switch/build dependency. |
| C11 | Actual persist geometry, clean state and supported features | unknown | P4:`Reader::open` requirements are known; a compatible immutable unit image has not been inspected in this task. |
| C12 | Zero persist writes through current sensor service | ours-OK | P3:`std_open`, `apps_std`, `std_mkdir`; P1:`partitionRead` reads ESP; P4:`ReadBlocks` exposes no write API. |
| C13 | DSP/charger guard and answered failure teardown | ours-OK | Source P15:`sg_observe`, `sg_file_call`, `ssr_core_step`; P1:`fileServiceStopped`. No new shared-DSP stability run. |
| A1 | Default front ALS discovery and sample parsing | ours-OK | P5:`send_request`, `ssc_parse_sensor_event`; P6:`ss_from_list`, `ss_receive`, `sample`; installed ALS recorded. |
| A2 | NaN/negative rejection, zero darkness and endpoint saturation | ours-OK | P7:`pal_lux_valid`, `pal_event`; P8:`ps_als_end_lux`. Accuracy status is diagnostic, not a calibration gate. |
| A3 | Smoothing, hysteresis and independent lux reporting | ours-OK | P8:`ps_als_step_curve`; P6:`light_target`, `ss_tick`, `SS_LUX_MS`; P1:`publishStatus`. Last target lux need not be newest raw lux. |
| A4 | Bright→dark→bright response with fresh real samples | unknown | sensors.md daily status leaves it pending. Need raw/valid-time/sample-count evidence through both transitions and verified dual-IC application (P1/P10). |
| A5 | Reference curve below 82 lux | ours-OK | P8:`ps_reference_curve`, `ps_als_target_permille`; P9:`pa_sdr_code`; anchors are present, selected via P1:`selectLightCurve`. |
| A6 | Physical lux accuracy, dark-room readability and effective floor | unknown | P7:`pal_lux_valid`, P8:`ps_reference_curve`, P10:`readArgs` establish numeric policy, not real light or a comfortable floor. Need attended light ladder and optical/readability check. |
| A7 | Retained key offset, slider trim and explicit manual control | ours-OK | P9:`Ambient::{step,adjust,manual,invalidate}`; P10:`Controller::{userLevel,manualCode}`, `serve`; `ambient_host.cpp:test_trim` coverage. |
| A8 | Actual GUI/key adjustments across real light and wake | unknown | Backlog still requests manual slider/light check. P9:`Ambient::adjust`, P10:`Controller::userLevel` need observed auto intent, retained trim/offset and applied codes, including clamp ends. |
| A9 | Off/on serialization, fresh-session validity, fault latch | ours-OK | P7:`pas_*`, `pal_conditions`; P6:`light_invalid`, `ss_force_off`; P10:`restartAmbient`, `applyAmbient`. |
| A10 | Continuous stale-light detection after the first sample | ours-missing | P7:`pas_tick` only bounds first-event acquisition; later on-change silence has no recurring freshness deadline. Physical transitions must disambiguate silence. |
| A11 | Dual-controller brightness application/readback | ours-OK | P10:`writeCode`, `applyLit`, `publishState`, `publishAndroidApply`; actual optical output remains a separate check. |
| R1 | Accel 513/10 Hz acquisition and 1025 parsing | ours-OK | P6:`build`, `sample`; P5 event parser; recorded November 27 RAM stream pass. |
| R2 | Axis conversion, magnitude/diagonal/flat gates, 250 ms hold | ours-OK | P8:`sfx_accel_ios`, `PS_ACCEL_AXES`, `classify`, `sno_update`; host orientation/stream coverage. |
| R3 | SSC sample-time continuity and opt-in default | ours-OK | P1:`readArgs`, P6:`sample`, `reset_accel`; stream tests above and recorded RAM pass. Default remains receive time. |
| R4 | All four physical edges/signs, diagonal stability and live wake | unknown | P8:`PS_ACCEL_AXES`, P6:`sample` have only stand/manual/stream hardware evidence. Require attended edge poses plus separate live suspend/wake qualification. |
| R5 | Edge/property delivery and agent dwell/backoff/manual lock | ours-OK | P1:`ssPublish`; P11:`sensorsTick`, `par_step`, `par_observed`, `par_deadline`; host policy coverage. |
| R6 | Default-off LaunchAgent and merged boot-argument read | ours-OK | P11:`main`; P12:`pianoBootArgs`, `pianoBootArgValue`, plist; recorded long-read RAM pass. |
| R7 | MCU attach forces 0°, holds and delays after detach | ours-OK | P13:`PianoKeyboard::coverAttached`; P11:`par_cover`, `par_step`; P6:`ss_accel_blocker`; K3 comparison. |
| R8 | Hall distinct from attach, ALS/display gate plumbing | ours-OK | P13:`acceptHall`; P1:`readConditions`; P7:`pal_conditions`; K3:`switch-lid`. |
| R9 | Direct hall/lid check in accel/agent rotation decision | ours-missing | P6:`ss_accel_blocker` and P11:`readInputs` do not consume hall. Current suppression depends on display sleep; do not assume attach means lid closed. |
| R10 | Physical cover attach/detach and hall→display sequencing | unknown | P11:`par_cover`, P13:`coverAttached`, `acceptHall` need observed bits, stream disable/restart, edge reset and actual landscape/portrait behavior; include in-flight rotation and missing-property cases. |
| R11 | MonitorPanel setter and CG angle read-back | ours-OK | P11:`backendInit`, `applyRotation`, `degreesShown`, `runPolicy`; manual 0°/90° RAM evidence in auto-rotate.md. |
| R12 | Automatic all-angle WindowServer/scanout behavior | unknown | P11 call site exists; lower symbols in sensors.md are names, not runtime traces. Check 180°/270°, bounds, surface layout and copy/flip evidence; K6. |
| R13 | Unconditional agent dwell reset on display/session inactivity | ours-missing | P11:`par_observed`, `par_step` can retain `rot.edge`/`edge_since` while shown is unknown. An active no-edge pass clears it via P8:`ps_rotation_step`; same-edge return timing needs qualification. |
| T1 | Per-unit/panel correction and four-angle transform | ours-OK | P14:`chooseOrientation`; P13:`pat_rotate`, `pat_unrotate`; all-corner synthetic coverage in `touch_native_host.cpp`. |
| T2 | Gesture latch including multitouch ownership | ours-OK | P14:`PianoTouch::gestures` requires `mtIdle`, `gestureIdle`, `nativeIdle`; P13:`pat_latch`. No mid-gesture map update. |
| T3 | Native display binding and panel-coordinate ownership | ours-OK | P14:`panelDisplayUuid`, `nativeUpdate`; existing native 90° RAM evidence. Agent level 2 is the corresponding qualification profile. |
| T4 | Atomic display/touch transform handoff for desktop axes | ours-missing | P11:`pushTouch` follows angle read-back; P14 latches at gesture start. Neither forms an atomic transaction with WindowServer. |
| T5 | Sixteen physical corner targets and drag through rotation | unknown | P14:`PianoTouch::gestures`, P13:`pat_rotate` need all four corners at 0/90/180/270 with actual display angle, coordinate mode and visual evidence; property acceptance alone is insufficient. |
| D1 | Gyro/mag/proximity default SUID/attribute discovery | ours-OK | P5:`SC_GYRO`, `SC_MAG`, `SC_PROX`, `send_request`; P1:`publishTypes` distinguishes absence/timeouts. |
| D2 | Native gyro/mag/proximity sampling and consumers | ours-missing | P6:`SS_STREAMS` contains only light/accel; proximity parser in P5 is unused by it. No fusion/compass/near-far policy. |
| D3 | Actual native availability, units/axes and calibration of these types | unknown | P1:`publishTypes` and P6:`SS_STREAMS` delimit discovery versus sampling. ROM/SSC historical identities do not prove current native reports; record future sampling separately. |
| D4 | Rear ALS/flicker discovery and streaming | ours-missing | P5:`sc_type_name` omits both; P6 only has two channels. Rear sensor physical/runtime state remains unknown. |

## Staged read-only-first qualification plan

This is a plan, not a record of work executed. It advances from immutable host
inputs and output-only observation to bounded RAM experiments. No stage writes
persist. Stages that change brightness/display/input require a later authorized
candidate run; daily defaults remain untouched throughout this analysis.

1. **Host/source and immutable-image checks first.** Pin both revisions and record
   candidate pack identity/provenance without publishing its contents, paths or
   per-unit identifiers. Use the existing synthetic persist, PianoSensors,
   PianoAuto and native-touch suites as regression coverage. On an already
   available immutable, read-only unit partition image, call `Reader::open` and
   `verifyRegistry` against the exact candidate pack. Require `Match`, no I/O or
   limit errors, known supported features and an unchanged image digest before/
   after. A dirty/unsupported image stops this phase; do not replay or repair it.
   Preserve details only in owner-controlled evidence. This proves registry-file
   equivalence within P4's stated integrity boundary, not runtime S6 integration.

2. **Candidate RAM observation before control changes.** Use a timed return and
   the previously qualified display/touch/power configuration. Verify loaded pack
   source, sensor/agent revisions and merged boot keys. With rotate absent/0,
   confirm successful agent exit and no ordinary accel consumer; separately use
   agent level 1 for proposals only. A sensor level-1 survey is read-only on its
   sensor-facing path; levels 2–4 send RPC/QMI and are not passive observations.
   Record guard/charger/link generations, RPC/list/stream status and baseline
   counters. Stop on fatal/guard latch, file-call storm or charging regression;
   do not reattach/restart the DSP to continue a test. S6 cannot be enabled on
   this baseline; its later adapter needs separate review before a runtime trial.

3. **Attended real-light qualification.** Keep the tablet's pose fixed and expose
   the front ALS to repeatable bright→dark→bright changes, with at least 30 s
   per plateau and extensions until smoothing actually settles. Repeat three
   cycles. Record external light/reference observations, raw bits, valid/invalid
   counts and timestamps, reported lux, curve/revision/session, target, trim,
   offset and both IC codes/enables/errors. Require fresh samples at each physical
   transition, correct response direction outside hysteresis/clamps, and verified
   common code application. An on-change quiet plateau alone is acceptable;
   silence through a real transition is not. Do not use a stationary first sample
   or smoothing-only code movement as the pass criterion.

   In an **attended dark room**, check 0/near-zero, 1/2/4/6/8/10/15/20/25/30 and
   about 82 lux where the available reference can resolve them; record attainable
   levels rather than fabricating precision. Confirm readable login and desktop,
   no unwanted blanking/flicker and the selected/effective lit floor. Compare the
   configured curve against actual samples before proposing any floor/anchor
   change. In a separate controlled pass, exercise keys and the real Control Center
   slider, then repeat light changes and display off/on/system sleep. Require
   retained offset/trim and auto intent, a fresh target on re-entry, and no ALS brightness writes
   while gated dark. Explicit manual mode must hold until auto is requested again.
   Report physical luminance separately if it is actually measured.

4. **Display-path RAM checks, then attended physical rotation.** First probe the
   panel/backend and use test edges with rotation level 2 in an active console
   session and native panel-axis touch. Exercise 0→90→180→270→0, flat/no-edge,
   fast diagonal alternation, user lock and manual override. Require CG read-back,
   desktop bounds, upright optical content and stable WindowServer/agent operation.
   Injected proposals isolate display policy; they do not qualify sensor axes.
   Use the existing [auto-rotate checklist](auto-rotate.md#ram-qualification-checklist)
   for repetition and stability criteria, without enabling unrelated display
   experiments.

   Then perform **attended four-edge poses and cover attach/detach**, using live
   SSC with the sample-time option only in its explicit qualification profile.
   Correlate physical top/left/bottom/right with edge 0/1/2/3 and shown
   0°/90°/180°/270°. Check near-diagonal, shake, face-up/down and a held pose
   through sleep/wake. Check the first active agent pass with a same-edge return,
   including whether it observed edge `none`; require a fresh post-wake settle
   before claiming that guarantee. Attach from portrait and during a pending
   request: require
   attach=1, accel disabled, edge cleared, then 0° held. Detach on both side
   poses: require fresh dwell plus the 1.5 s detach hold and 600 ms agent settle.
   Observe hall close/open separately and prove display/rotation suppression;
   do not substitute simulated hall results for physical attach results. A delayed
   or missing property must be graded against the permissive defaults above.

5. **Attended touch at every angle.** Name the coordinate owner/profile before
   testing: native panel axes with level 2, or optional desktop axes with level 3
   and `piano-touch-rotate=1`. Tap four labeled inset physical corners at each
   shown angle and record all 16 outcomes against the transform table. Drag and
   hold one contact, then a multitouch gesture, through a rotation; verify the
   latch waits for all routers to become idle and the next gesture uses the new
   map. Check the display-change/request race and refused touch writes explicitly.
   Record optical evidence as well as HID/registry state. Restore 0° and verify
   touch before returning from RAM.

6. **Other sensors remain discovery-first.** Observe level-3 type/attribute
   results without new sample requests, distinguishing `none`, `no answer` and
   unavailable attributes. Gyro/mag/prox sampling needs a later explicit opt-in
   channel/consumer design, units/axes/validity rules, bounded lifecycle and power
   criteria. Near/far needs attended occlusion; gyro/compass need known motion/
   headings and calibration checks. Presence alone does not close D2/D3. Retain
   rear ALS/flicker as separate unimplemented types.

## Fix-image gate

```text
Status: HOLD
Scope: analysis only; B3/B13 qualification remains open.
Daily defaults: untouched; automatic desktop rotation stays off.
Persist: zero writes; no mount, recovery, repair or write-through.
S6: host reader/verifier only; no runtime registry replacement is enabled.
Required closure: exact-pack/immutable-registry equivalence and separately
reviewed runtime integration; attended bright-dark-bright and dark-room floor;
physical four-edge/cover/hall/wake behavior; all-angle display and touch checks.
Gyro/mag/proximity: discovery is implemented; native sampling is unimplemented
and physical availability/calibration remain unqualified.
Release action: none; no tablet access, installation or push in this task.
```
