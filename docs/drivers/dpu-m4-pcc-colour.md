# DPU M4 Night Shift colour on SM8750

The original M4 investigation below covers PCC register encoding. The
[location-9 boot-reference fix](#location-9-boot-reference) covers the remaining warm
Night Shift OFF state and records the 2026-11-27 RAM qualification. **Owner decision
(2026-11-27, `OWNER-0749`):** `y12t` install with Night Shift daily routing ON is
APPROVED (combinable with DPU M2 promotion if safe, with backup, SHA-256, read-back
verify, and a ready rollback).

## References and register layout

The Ubuntu v1.4.0 kernel pin is `83f0f89bed14a`. Its PCC setup, CRTC colour
path and SM8750 catalog match the piano reference `b5100d5f2e0b`.
`catalog/dpu_12_0_sm8750.h:142–158,206–224` pairs LM0/1 with DSPP0/1 at
MDP + `0x54000/0x56000`. `dpu_hw_catalog.c:428–432` selects PCC v6
(`0x60000`) at DSPP + `0x1700`, length `0x90`. The piano path uses
CTL0 and PCC bases MDP + `0x55700/0x57700`.
[Ubuntu release pin](https://github.com/yzddmr6/xiaomipad-8pro-mainline/blob/v1.4.0/kernel/source.json)
[Linux SM8750 catalog](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_12_0_sm8750.h)
[Linux sub-block catalog](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_catalog.c)

Read the Linux CRTC conversion and writer together. DRM CTM defines
`M[out][in]` in RGB. `_dpu_crtc_get_pcc_coeff` puts CTM element 1
(`R←G`) into `cfg->g.r`, and `dpu_setup_dspp_pcc` writes that
field to `+0x14`. The Qualcomm downstream SDE PCC writer puts the
**red input's contribution to green output** at `+0x14`. Its hardware
layout has a group for each input, and within each group the three output
registers are R, G, B. The downstream v1.7 writer spells out the offsets,
and the v4 writer uses the same per-output-plane stride. Therefore Linux's
CTM mapping transposes off-diagonals against the downstream register
layout. The v6 catalog in the piano kernel keeps the same offsets and
writer. Using the downstream layout for v6 is an inference from those
sources and the captured symptom.
[DRM CTM ABI](https://github.com/torvalds/linux/blob/master/include/uapi/drm/drm_mode.h)
[CRTC conversion](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14a0cb7d582a26e72bced0387870f4/drivers/gpu/drm/msm/disp/dpu1/dpu_crtc.c#L794-L817)
[Linux writer](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_dspp.c)
[Qualcomm SDE v1.7](https://android.googlesource.com/kernel/msm/+/e1e85fa160463d8c5e55c58c1806668e9740a117/drivers/gpu/drm/msm/sde/sde_hw_color_processing_v1_7.c)
[Qualcomm SDE v4](https://android.googlesource.com/kernel/msm-extra/display-drivers/+/ea5b69a0327130b9553a3ad6ede3efb2734be756/msm/sde/sde_hw_color_proc_v4.c)

Matrix entries below use `output←input`. Linux line numbers refer to
`dpu_hw_dspp.c` and `dpu_crtc.c` at the Ubuntu pin. y12q used
`DpuHw.h:kPccCoef` and `DpuPcc.h:applyPrograms`.

| PCC offset | Linux writer and CTM entry | Qualcomm SDE meaning | y12q → corrected |
| --- | --- | --- | --- |
| `+0x00` | 58–61, 76, bypass 0 / enable bit 0 | bypass / enable | Same, with captured enable on neutral |
| `+0x04/08/0c` | untouched | R/G/B constant terms | Same. Startup refuses a bypassed base with nonzero constants |
| `+0x10` | 64, 806, `cfg->r.r`, CTM[0] | R←R | R←R |
| `+0x14` | 68, 807, `cfg->g.r`, CTM[1] | G←R | R←G → G←R |
| `+0x18` | 72, 808, `cfg->b.r`, CTM[2] | B←R | R←B → B←R |
| `+0x1c` | 65, 810, `cfg->r.g`, CTM[3] | R←G | G←R → R←G |
| `+0x20` | 69, 811, `cfg->g.g`, CTM[4] | G←G | G←G |
| `+0x24` | 73, 812, `cfg->b.g`, CTM[5] | B←G | G←B → B←G |
| `+0x28` | 66, 814, `cfg->r.b`, CTM[6] | R←B | B←R → R←B |
| `+0x2c` | 70, 815, `cfg->g.b`, CTM[7] | G←B | B←G → G←B |
| `+0x30` | 74, 816, `cfg->b.b`, CTM[8] | B←B | B←B |
| `+0x34` onward | untouched | higher-order terms on downstream SDE | Same. Relevant v6 state beyond the linear words was outside the capture |
| CTL `+0x13c/+0x140`, then `+0x18` | `dpu_hw_ctl.c:144–180,293–295,394–413` | stage PCC (bit 4) for each DSPP, latch DSPPs (bit 29) | Same masks. Native waits for clear and verifies both blocks |

## Native path and the capture

- **ours-OK, ABI and routing.** The 27.3 ABI trace recorded in
  `GopColorLog.h` goes from CoreBrightness through WindowServer and
  QuartzCore's `update_color_matrix` to IOMobileFramebuffer user-client
  selectors `0x37` (location 9, slot 309) and `0x32` (slot 314).
  `NativeGopDisplay.cpp:3430–3488` copies the nine words in row order
  and routes location 9 and the scalar correction independently to
  PianoDpu. The source trace and runtime callback establish the handoff.
  The location-9 trace below covers CoreBrightness's matrix semantics.
- **ours-OK, input units.** On callback #2, `0x00000000e91ad900 / 2^32`
  is about 0.911 and `0xffffffffff1301aa` as a signed word divided by
  `2^32` is about −0.004. This is two's-complement S31.32. Linux DRM
  receives sign-magnitude S31.32. Its `CONVERT_S3_15` removes bit 63,
  takes bits 34:17 and writes them as the 18-bit coefficient. That
  discards the DRM sign and does not specify the hardware sign encoding.
  Both paths have 15 fractional bits at the register, so `0x8000`
  is one. PianoDpu rounds via Q16.16, retaining its signed arithmetic.
- **ours-OK, state and correction.** `DpuSlices.cpp:1098–1129` copies the
  payload synchronously, updates desired state under the worker lock
  and queues the work. `DpuNightShift.h` rejects invalid words, keeps
  the last colour across zero matrices, and restores the captured base
  on identity. Before boot-reference cancellation it applied
  `gain × (firmware base × Apple matrix)`, using
  one unsigned Q16.16 gain for all nine terms. A bypassed firmware base
  acts as the identity during active colour. The capture reports unity
  (`0x10000`) as the correction, so it cannot explain the hue shift.
- **ours-OK, pixel unpack.** Both VIG pipes reported BGRA and
  `SRC_UNPACK=0x03020001` (console lines 254/258). The Linux ARGB8888
  format uses the same B,G,R,A-to-component mapping in
  `disp/mdp_format.c:350–352`. `&BGA` offers its linear BGRA plane 0
  to the same pipes, or copies it to the same boot layout. VIG unpacks
  the bytes into named channels before LM and DSPP. SDE's PCC labels
  refer to the named RGB channels. The sheng GOP handoff
  (`ShengGopLogDxe.c:105–126`, `66421cb`) similarly carries the GOP
  format and pitch, with no reference matrix or PCC sign setting.
- **ours-missing in y12q, coefficient serialization.** At startup
  both PCCs and GC were disabled and the captured constants were zero.
  The #1 snapshot at 29279–29280 gives PCC register order
  `074a0 009af 001b2 | 20076 06c84 00039 | 200ec 20359 05ba9`
  on both halves. It agrees with the old output-major map and its
  bit-17 sign-magnitude writes. For example, −0.0036 became `20076`.
  Interpreted as signed 18-bit two's-complement, that word is −3.9964.
  The post-login 74111–74112 snapshot has
  `05349 027d9 004de | 201eb 030d5 00067 | 20250 2073d 01125`.
  Under the downstream layout and two's-complement decoding its
  unit-grey response is approximately `[-7.316, -3.251, 0.175]`,
  which clips to blue. The same coefficients with the proposed
  serialization yield `[1.000, 0.370, 0.059]`. The transposition
  places the misencoded negatives in *red and green output*, which is
  why interpreting the old words in y12q's software order misses the
  failure. A transpose alone would only move the small cross terms.
- **ours-OK, commit, restore and replay.** `DpuSlices.cpp` holds the flip
  transaction lock, checks the awake and clock/path gates, and skips
  repeated programs. It stages both DSPPs before one CTL flush.
  `applyPrograms` waits up to 60 ms and compares enable and all nine
  words per half. A failure restores the last verified program and
  latches the runtime fault after a failed restore or repeated failures.
  Wake and modeset replay the retained state. The old read-back checked
  equality with the words it wrote and therefore accepted the wrong
  encoding. The capture had successful latches during the login ramp.

The signed hardware interpretation is the inference that explains the
captured words and the reported blue output. A same-content, time-aligned
optical capture is still needed. The first warm matrix was already in the
registers before the reported visible change at login, so the console
alone cannot explain why the lock screen appeared correct.

## Host verification

`dpu_pcc_host.cpp` drives the actual matrix decode, correction, PCC
programmer and read-back against the register model. The #1 raw log
ends partway through the final row, so the last two inputs are
reconstructed at PCC precision from its matching snapshot. The
post-login input is reconstructed at the same precision from the
74111–74112 snapshot. The expected words below are ordered by
increasing offset from `+0x10` through `+0x30` on both halves.

| Input | Expected PCC words in register order |
| --- | --- |
| Identity, enabled unity base | `08000 00000 00000 \| 00000 08000 00000 \| 00000 00000 08000` |
| First captured matrix, unity correction | `074a0 3ff8a 3ff14 \| 009af 06c84 3fca7 \| 001b2 00039 05ba9` |
| Stronger post-login matrix, unity correction | `05349 3fe15 3fdb0 \| 027d9 030d5 3f8c3 \| 004de 00067 01125` |

The test checks that red gain is at least blue gain and that equal RGB
inputs yield R ≥ G ≥ B, with all three channels present. The existing
state test also exercises scalar changes, firmware base composition,
bypass restoration, rollback and replay.

## Fix-image gate

Path: CoreBrightness → WindowServer/QuartzCore 27.3 → IOMobileFramebuffer selectors 0x37/0x32 → NativeGopDisplay → PianoDpu worker → DSPP0/1 PCC v6 → CTL0 latch → both panel halves. The Linux CTM and Qualcomm downstream paths and the sheng handoff are compared above.

Checks: **ours-OK** for callback units, gates, RGB unpack, scalar correction, firmware base restore and CTL latch. **ours-missing** in y12q for PCC orientation, sign serialization and per-change coefficient evidence. **unknown** for optical signed behaviour, the transfer domain and the lock-to-desktop timing. Future qualification uses the same neutral-grey and RGB patches before and after login with M4, a changed warm matrix, identity/bypass, and a wake replay. Compare both halves and the logged PCC read-back in ascending register order. Keep the image gated on that evidence.

Root cause: y12q serialized a row-major RGB matrix with the Linux CTM's transposed PCC mapping and encoded negatives as sign-magnitude in an 18-bit field. Applying the downstream register order and the inferred two's-complement sign to the old read-back reproduces red/green clipping and surviving blue.

ONE change: Serialize the existing RGB matrix to input-grouped PCC words with signed 18-bit two's-complement coefficients. Use the same layout and sign for the firmware base and register view. Record the actual verified DSPP0/1 words once per changed program. Pin the regression to the captured callback and read-back, and check identity and the warm response through the host MMIO model.

## Location-9 boot reference

Native Night Shift routing is opt-in with `native-gop-display-night-shift=1`.
The daily profile keeps routing off. The RAM qualification starts with Night
Shift OFF so the first valid location-9 callback can establish the reference.

### 2026-11-27 RAM qualification

The unlocked `y12t-obs1` run passed. Both DSPPs returned the following gray
responses with zero PCC apply failures.

| State | R | G | B |
| --- | ---: | ---: | ---: |
| Base | 1.000 | 1.000 | 1.000 |
| Half | 1.032 | 0.714 | 0.398 |
| Warm2 | 1.059 | 0.441 | 0.110 |
| Off again | 1.000 | 1.000 | 0.999 |

The result qualifies this boot sequence with fixed adaptation. Persisted-ON
startup and changes in ambient adaptation are outside the qualification.
The measurements give the register response to unit gray. A general matrix
applied to encoded scanout approximates its linear-light response, and the
red gain above unity can clip bright inputs. Daily routing requires an owner
decision.

### Why the OFF state was warm

The earlier `y12s-qual1` capture showed both firmware PCCs bypassed at boot
and unity Q16.16 brightness correction. Its first location-9 callback already
carried a warm matrix, approximately `[911 76 13 | -4 848 2 | -7 -26 716]`
in thousandths. The last two raw words were incomplete in the interleaved
log, so the replay reconstructs them at the precision of the matching S3.15
register words. The second OFF request returned within 17 register LSBs of
the boot transform. Both GC blocks were disabled in the snapshots.

| Earlier snapshot | R | G | B | Grader result |
| --- | ---: | ---: | ---: | --- |
| Base | 1.000 | 0.846 | 0.683 | OFF fails |
| Off | 1.000 | 0.845 | 0.682 | OFF fails |
| Half | 1.000 | 0.602 | 0.258 | Warm passes |
| Full warm | 1.000 | 0.370 | 0.059 | Warm passes |

CoreBrightness sends a composed white-point transform. Switching Night Shift
off clears blue-light reduction while the independent ambient and native-white
contributions can persist. Routing the absolute transform applies the boot
adaptation to GOP scanout. Unity brightness correction preserves its color
cast. A gamma-2.2 approximation still gives the earlier boot matrix gray
gains of about 1.000 / 0.927 / 0.841.

#### macOS 27.3 trace

The addresses are unslid for macOS 27.3 (26C5035r).

| Symbol and address | Observed operation |
| --- | --- |
| CoreBrightness `_CFXSetWhitePointType`, `0x1be7e64e4` | Type 1 installs the D65 RGB↔LMS basis and D65 reference white. |
| CoreBrightness `__CFXGetMatrix`, `0x1be7e98dc` | Normalizes XYZ by Y, applies the Bradford basis, conjugates LMS gains back into RGB, and normalizes by the largest RGB row sum. These operations define a linear-light matrix. |
| CoreBrightness `_CFXSetNativeWhitePoint`, `0x1be7eb61c` | Uses the supplied native xy for the native-white correction and its ramp. |
| CoreBrightness `_CFXEnableAmbientAdaptation`, `0x1be7e74ec` and `_CFXEnableBlueLightReduction`, `0x1be7e8894` | Control ambient adaptation and blue-light reduction independently. |
| CoreBrightness `__CFXUpdateBLRTarget`, `0x1be7e8a44` and `_CFXUpdateColorFade`, `0x1be7e9e48` | The blue-light reduction target starts at identity. The fade combines it with the ambient ramp and optional corrections, including native white. |
| QuartzCore `IOMFBDisplay::update_color_matrix`, `0x18e1e54cc` | Ramps the matrix and sends nine row-major S31.32 words to location 9. Sends brightness correction independently as Q16.16. The D50 XYZ path uses ICC location 6. |
| `_IOMobileFramebufferSetMatrix`, `0x1b5ed52c4` | Dispatches the 72-byte payload through user-client selector `0x37` to slot 309. Correction uses selector `0x32` and slot 314. |
| Kernel `IOMobileFramebufferAP::set_matrix`, `0xfffffe000abfddf8` | Packs the location and matrix into DCP request `A437`. `setBrightnessCorrection` sends the scalar as `A438`. |

Location 9 carries the result of the composition with no enable bit or
contributor metadata. The trace establishes the transform's linear-light
semantics and the transport. The live policy behind the captured warm
baseline and the DCP firmware's transfer-block order remain unknown.
QuartzCore has separate gamma and ICC paths. NativeGopDisplay publishes
synthetic sRGB primaries, D65 xy and gamma 2.2.

PianoDpu retains the input-grouped, signed PCC words and single CTL latch
from the M4 encoding fix. The Qualcomm catalog places PCC v6 and GC v2
in separate blocks at DSPP + `0x1700` and `0x17c0`. Native routing programs
PCC only. [M4 register layout and source references](#references-and-register-layout)

### Boot-reference cancellation and replay

[`DpuNightShift.h`](../../package/PianoDpu/DpuNightShift.h) captures the first
valid nonzero matrix `B`. For later callbacks it computes the relative
transform `M × inverse(B)`. The boot reference is on the input side
because `M = relative × B`. PianoDpu then composes that relative transform
with the captured firmware PCC and applies the independent brightness gain.
An exact reference yields identity for the matrix factor. An explicit identity
also resets the gain to unity, restoring the firmware words and bypass state.
A nearby OFF callback retains the small difference. Blanking, invalid
callbacks, modesets and wake preserve the reference.

Inversion uses 64-bit integer Q32 cofactors and a Q48 determinant. The first
matrix is accepted as a reference only if its determinant has magnitude at
least 1/64 and the inverse coefficients fit within ±4. Relative coefficients
use the same ±4 bound.

The [PCC host test](../../package/PianoDpu/tests/dpu_pcc_host.cpp) exercises
both halves through decode, programming, latching, read-back and replay. It
also checks multiplication order, integer limits, correction, rollback and
restoration. It can emit snapshots for grading.

```sh
NS_TEST_DIR=$(mktemp -d)
c++ -std=c++20 -Wall -Wextra -Werror -Ipackage/PianoDpu \
  package/PianoDpu/tests/dpu_pcc_host.cpp -o "$NS_TEST_DIR/pcc-host"
"$NS_TEST_DIR/pcc-host" "$NS_TEST_DIR"
python3 -m unittest discover -s tooling/native/tests -p test_pcc_gray.py
python3 tooling/native/pcc-gray.py --expect off "$NS_TEST_DIR/ns-off-dpu.txt"
python3 tooling/native/pcc-gray.py --expect warm "$NS_TEST_DIR/ns-warm-dpu.txt"
```

The [gray grader](../../tooling/native/pcc-gray.py) checks both DSPPs.
OFF accepts unity gray within 2%, including bypass. Warm requires both
PCCs enabled and `R >= G >= B` with `R >= 0.9`. The earlier grader's
minimum for every channel rejected full warmth when blue reached 0.059.
