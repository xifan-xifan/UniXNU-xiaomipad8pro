# Piano DPU M3 refresh modes

M3 keeps the 60, 90, 120 and 144 Hz hardware timing plans for the internal 3200 × 2136 display. The M3 boot path
publishes one fixed timing after verification. The 27.3 RAM boot captured for M1 was
scanning at 120 Hz with programmable fetch enabled on both interfaces. Each fetch start was `0x0013b6c1`, 24 lines before the
end of the frame. The DSI timing-shadow words at +0x1e4/+0x1e8/+0x1ec were idle (zero). Both command engines were configured for
software triggering with the in-frame DMA block clear.

| Reference path | Piano M3 path | Comparison |
| --- | --- | --- |
| Ubuntu `panel-novatek-nt36532.c:425–450`, four panel timings and fixed lane rate | `DpuHw.h` mode table and `DpuModes.h` per-link planner | Same porches. The compressed timing engine runs at 159,667,200 Hz at every rate. |
| Ubuntu `panel-novatek-nt36532.c:496–511,617–650`, module order and rate-group values | Rate-group writes on demand through the bonded DSI controllers | Same register values and module order. BOE sends B2 first. CSOT sends B3 first. |
| Ubuntu `dsi_host.c:1220–1260,1354–1391,1462–1494`, DSI video synchronization and DMA packet encoding | HS short writes in the common front porch with DMA completion checked on both controllers | Same short-packet layout. The driver uses the controller FIFO and checks the line counters before triggering. |
| Ubuntu `dpu_hw_intf.c:103–282,284–306`, timing setup and programmable fetch | Stage both interfaces together and move each enabled fetch start with the porch | Firmware already enabled fetch. The driver preserves its distance from the end of the frame. |
| Stock DFPS, master DSI host first with double buffering, followed by the HS timing-switch commands | Stage the DSI and INTF words together, verify the latch, send the new rate group when it changes | Different: stock also coordinates the SDE commit and clears the pen on exit from 60 Hz to a different rate group. The fixed 60 Hz path has no rate-group exit. |

The stock sequence is described in the [MiCode DSI display driver](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_display.c#L5433)
and [SDE encoder driver](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_encoder.c#L7075).

The three panel rate groups use page select `FF 10` and unlock `FB 01` before the two module-ordered writes.

| Refresh | Horizontal front porch per link | Vertical front porch | B2 | B3 |
| --- | ---: | ---: | --- | --- |
| 60 Hz | 117 | 2378 | `91` | `40` |
| 90 Hz | 309 | 68 | `00` | `80` |
| 120 Hz | 117 | 68 | `91` | `40` |
| 144 Hz | 21 | 68 | `00` | `00` |

## y12n-qual1 boot refusal

The 2026-11-26 RAM qualification classified M3a as a code failure. The console measured 119.878 Hz on both links, with both
frame and line counters changing (lines 3633–3634). M5 then recorded `count-en 0/0 -> 0/0` and verified motion on both links
(line 3685). The M3 service advertised selectable refresh, then refused 120 → 60 Hz before staging timing (line 3692). The
combined refusal message left the CTL flush value unknown. The zero enable readback would independently have blocked any switch
that reached the counter check.

The `M3 initial 60 Hz mode refused` message (line 3693) came from `NativeGopDisplay::createDisplayAttributes` after its boot-time
`PianoDpuMode` request reached the same gate that serves later mode selections. It consequently published only the boot profile's
60 Hz timing while scanout remained at 120 Hz. Qualification stopped the M3 phase at enumeration. The transitions, delivered
cadence, and M3 interactions with M2/M4/M5 remain unmeasured on this boot.

The original M3 analysis traced timing, fetch and DSI programming from M1, and the integration analysis marked M3 counter
availability unknown. Neither traced how `countersOn` turned the enable readback into an admission check. The timed M3 model
also coupled successful enable readback to counter motion in its switch cases. M5 had already separated the two after its
previous qualification, but M3 still used the readback contract.

Ubuntu v1.4.0 pins the display kernel to `83f0f89bed14`. The Piano line numbers below refer to the y12n-qual1 code
(`061fe59c`). The available stock MiCode DSI source also updates the clock master first, then its peer. Its DFPS path
enables timing double buffering and performs the async timing update without an INTF COUNT_EN readiness check in that
DSI step. The matching stock SDE INTF and CTL source was unavailable for this comparison. [MiCode DSI display driver](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_display.c#L5102)

| Ubuntu v1.4.0 path | Piano y12n-qual1 path | Comparison |
| --- | --- | --- |
| `dpu_encoder_phys_vid.c:463–478,316–332` programs the video INTF and fetch. `dpu_hw_intf.c:243–259` writes `INTF_FRAME_LINE_COUNT_EN = 3` during timing setup. | `DpuSlices.h:260–269` requests the enable bits when needed on the inherited scanout. `DpuModeSwitch.h:302–305` also requires the bits to read back. | **Same** enable request. **Different** readback requirement. M3 can drop that requirement once motion is measured on both inherited links. |
| `dpu_hw_intf.c:326–356` reads live frame and line counts, with engine status guarding the paired status read. | `DpuSlices.h:107–119,176–184` reads the live counts. `DpuPanel.h:54–77` checks for advancing frames and lines, but `DpuModeSwitch.h:302–305` skips that check. | **Same** live observations. **Missing** motion check in M3. Share M5's two-link check before depending on the counters for timing. |
| `dpu_hw_ctl.c:88–93` reads `CTL_FLUSH`. `dpu_encoder_phys_vid.c:532–550` waits for the register to clear and reports its value on timeout. | `DpuModeSwitch.h:302–305` refuses a nonzero value before staging, under the flip lock. `DpuSlices.cpp:594–596` prints one reason for both checks. | **Same** zero test. **Different** wait policy. The preflight refusal avoids staging a second transaction over a pending commit. **Missing** the raw flush mask in the refusal. |

## y12o-qual1 underrun and login failure

The 2026-11-26 RAM run reached ssh and captured the NativeGopDisplay start and multiple WindowServer launches. The login
gate timed out. Console lines 3697, 3812–3813 and 3819 show the BOE module, Night Shift replay, both links verified at
60.043 Hz after 120 → 60, and four timing elements published with 60 Hz active. At 4648 the 60 → 144 request and its
restore both reported `an INTF underran`. AMFI logged core-dump denials from successive WindowServer PIDs starting on the
next line. The denials establish repeated process failure. The specific WindowServer exception and its recovery policy
remain unknown.

The IOAV timing inventory is a contract. In y12o, the kernel equated every calculable timing with a supported selection.
IOAV carries the full uncompressed raster, a 16.16 rate and a nominal raster clock. The refresh dictionary and slot 423
describe the active interval. The saved display identity and four timing IDs allow WindowServer to request 144 Hz during
startup even when the boot request has already made 60 Hz active. The 27.3 shared-cache symbols show
`CA::WindowServer::IOMFBDisplay` and `AppleDisplay::update_display_limits` in QuartzCore and imports of
`IOMobileFramebufferGetSupportedDigitalOutModes`, `GetDigitalOutMode` and `SetDigitalOutMode`. The NativeGopDisplay
virtual method returns the worker's IOReturn to IOMFB. Bounded verification on the worker's flip lock completes before
NativeGopDisplay changes the active ID, refresh property or VBL pacing. An error therefore leaves 60 Hz in software even
if the hardware recovery cannot be verified. The trace has no user-space stack to prove what handling follows that return.

### Transport and fetch budget

Ubuntu v1.4.0 pins the comparison to Linux `83f0f89bed14`. Its panel sets full-width HFP 42/234/618/234 for
144/120/90/60 Hz. At 144, the 3306 × 2310 uncompressed raster gives the DRM clock of 1,099,707 kHz (integer truncation).
The per-link compressed active interval is ceil(1600 × 8 / 30) = 427 clocks. Each link adds 16 sync, 16 back and
21 front. Thus 480 × 2310 × 144 = 159,667,200 Hz. The 120/90 modes use htotal 576/768 with the same vtotal.
60 uses 576 × 4620. All four give the same nominal compressed clock. Four D-PHY lanes and RGB101010 yield
159.6672 MHz × 30 / 4 = 1,197.504 Mbit/s per lane and 149.688 MHz byte clock. The stock group is 1,197.5 Mbit/s,
149.6875 MHz byte clock and 159.666666 MHz pixel clock. The sub-5-ppm rounding difference is immaterial here.
144 Hz therefore needs neither a different DSI PHY PLL nor a different slice setup. It needs timely delivery of each line.

The P81 10-bpc, 8-bpp DSC 1.1 profile has two 800 × 24 slices per link, 800 compressed payload bytes per slice-line
and 89 slice rows. Ubuntu computes packetization and the panel-specific PPS at modeset. Native inherits DSC and the
DSI stream configuration. Its 144 plan keeps the active width and the payload per line, while cutting line time
from 3.608 to 3.006 µs. The Android display capture lists 144 in the same group as 120, 90 and 60. The stock
timing-group documentation gives equal bit clocks and HS rate-group commands. The captured Android ODM `*_mi.xml`
files contain display calibration, so the exact 144 porches above are from the pinned Linux table. Stock clock and
command evidence comes from SunUEFI's `oem-refresh.md`, the P81 boot panel descriptions and the MiCode DFPS path.
SunUEFI's stock note labels the variants oppositely to the P81 panel XML and pinned Linux driver. The latter two
agree on BOE B2 first and CSOT B3 first, which is also the qual1 boot log's order. [SunUEFI OEM refresh trace](https://github.com/slwyts/Project-SunUEFI/blob/4ef87402/docs/devel/hardware/oem-refresh.md)

The 144 fetch word is `(2310 − 24) × 480 + 1 = 0x00106da1` on each INTF. Leaving the 60 Hz word
`0x00280441` in place would put the start beyond the 144 frame. M3 actually stages the correct target word
and reads both back. In microseconds the 24-line advance shrinks from 86.58 to 72.15 and vsync + VBP from
382.40 to 318.66. Linux disables programmable fetch when those 106 lines already exceed the catalog's 24-line
worst case. SSPP bandwidth prefill is a separate 35-line budget in Linux. It fits within the 106 start-of-frame lines
at either rate, so it does not increase the Linux bandwidth formula here. It says nothing about VIG service during
active scanout, when each linear half must deliver 6,400 bytes per line every 3.006 µs at 144.

| Native linear BGRA, unity scale | 60 Hz | 90 Hz | 120 Hz | 144 Hz |
| --- | ---: | ---: | ---: | ---: |
| Active bytes per second, kB/s | 1,640,448 | 2,460,672 | 3,280,896 | 3,937,075 |
| One line's active-time throughput, both pipes, kB/s (`3200 × 4 × vtotal × Hz`) | 3,548,160 | 2,661,120 | 3,548,160 | 4,257,792 |
| Linux AB formula with 120% factor, total kB/s | 4,257,792 | 3,193,344 | 4,257,792 | 5,109,350 |
| Plane clock proxy, one 1600-wide half with 105% factor, MHz | 465.696 | 349.272 | 465.696 | 558.835 |
| Linux CRTC clock formula, full width before OPP cap, MHz | 931.392 | 698.544 | 931.392 | 1117.670 |

Linux splits AB across the ICC paths it acquires and passes the catalog's 800,000 kB/s minimum DRAM IB to each
path. IB is a separate peak argument with different aggregation from the splash OPP. Linux raises AB/IB before it
raises the MDP clock and caps the full-width clock calculation at the 575 MHz SM8750 OPP. The half-width figure shows
the 20% rate increase at each pipe and the 575 MHz OPP that would cover that estimate. It is a sizing proxy and does
not measure this firmware's VIG, DSC or clock-cycle requirements.

Piano's `kSplashAvgKBps` and `kSplashPeakKBps` are documentation constants for the Linux simplefb vote
(3,280,896/4,257,792 kB/s). PianoDpu issues no display interconnect vote and only votes the GCC register AHB
branch. The qual1 MDP RCG logged source 1 and divide-by-3, which gives no MHz without the PLL0 rate. The
PianoA830 idle DDR floor was level 5 (6,671 MB/s table entry, sized for 120 Hz). It provides shared DDR headroom,
with no per-display AB/IB, VBIF latency guarantee or MDP clock vote. The nominal splash AB is already 656,179
kB/s below the steady 144 payload before overhead, if the inherited display allocation was equivalent to that
reference. Qual1 contains no live display ICC, MDP PLL rate, VBIF pressure or per-link underrun timestamp. The exact
bottleneck remains unknown. Unchanged fetch/QoS resources and a shorter line deadline are the primary feed-side
suspects. A transition edge or panel/DSI sequencing issue remains possible.

### Line comparison at the y12o base (0ea7e24d)

The Sheng reference has counter and CTL observations in `DisplayTimingProbe.cpp:15–26` and a read-only survey contract
in `ShengDpuObservation.h:34–42` at `00ba909`. It supplies the same hardware observations for the admission check.
The P81 clock, DSC and mode-commit references come from MSM and stock P81. [Pinned Ubuntu kernel](https://github.com/yzddmr6/linux-sm8750-piano/tree/83f0f89bed14a0cb7d582a26e72bced0387870f4)

| Ubuntu file and lines, with stock where available | Piano file and lines | Comparison |
| --- | --- | --- |
| `panel-novatek-nt36532.c:420–450`, full raster and four porches. `dsi_host.c:608–655,658–687`, round compressed width per link and derive PCLK and byte rate. | `DpuHw.h:163–179`, `DpuModes.h:119–139`, `NativeGopDisplay.cpp:3052–3073`. | **Same** raster and compressed transport math. **Different** clock ownership. Native retains the running PHY and checks the timing family. Stock's equal clock groups support that choice. |
| `panel-novatek-nt36532.c:461–470,656–671,821–836`; `dsi_host.c:961–996,1042–1105`; `dpu_encoder.c:2067–2085,2149–2150`. | `DpuModes.h:119–139`; `DpuSlices.h:54–84` store allowlist. | **Same** slice geometry as the boot description. **Missing** live DSC/PPS and packetization verification. The retained VFP-only path uses the inherited payload format, H timing and rate group. |
| `dpu_encoder_phys_vid.c:176–212,225–250`; `dpu_hw_intf.c:284–306`. | `DpuModes.h:104–112,138`; `DpuModeSwitch.h:216–226,238–249`. | **Same** start formula. **Different** enable policy. Linux needs zero VFP fetch lines. The VFP-only change retains the active boot policy and moves the 24-line start with the frame. |
| `dpu_plane.c:116–184,1574–1583`; `dpu_core_perf.c:40–107,129–136,215–242,337–397`; `catalog/dpu_12_0_sm8750.h:439–465`; `sm8750.dtsi:4079–4081,4112–4142`. | `DpuHw.h:178–179`; `DpuAhbVote.h:15–22`; `DpuSlices.cpp:611–642`; `PianoA830.cpp:707–734`. | **Missing** per-mode MDP/ICC/MMCX vote. Keep the inherited 120-line-rate and measured 60 profile. Retain the firmware resources that delivered the same burst rate on qual1. Higher rates need measured clock and bandwidth ownership first. |
| `dpu_plane.c:196–310,354–409,1484–1533` drives SSPP FIFO fill, QoS LUT and VBIF outstanding limits. | `PianoDpu.cpp` survey; `DpuSlices.h:54–84`; qual1 console 258–265, VIG0/1 `fetch 0x87`, `qos 1`. | **Missing** a per-mode SSPP/VBIF program or deadline measurement. Preserve inherited VIG/flush configuration for the unchanged per-line rate. |
| `dsi_manager.c:208–335,358–411`; `dsi_host.c:387–417,2453–2455,2570`; `dpu_encoder_phys_vid.c:316–332,463–500`; `dpu_hw_ctl.c:144–180,339–344`. | `DpuModeSwitch.h:193–235,337–380`. | **Different** transaction. Linux programs on a full modeset, coordinates enable/flush, and stops the shared path before teardown. The retained M3 boot transaction uses SunUEFI's VFP shadow sequence. Simultaneity of both DSI and CTL latches during HFP changes is **unknown**. |
| `panel-novatek-nt36532.c:617–671`; stock clock-master-first DFPS and HS timing-video-switch with 60 Hz exit pen handling. | `DpuModeSwitch.h:89–190,337–343,363–377`. | **Same** group values and module order. **Different** timing of panel writes relative to SDE commit. The VFP-only path needs no rate-group DMA. |
| `dpu_encoder_phys_vid.c:532–550` reports CTL timeout; normal Linux enable/disable reinitializes timing and panel state. | `DpuModeSwitch.h:346–380`; `DpuSlices.cpp:251–259,623–632`. | **Different** live inverse transaction. **Missing** a fresh underrun observation boundary after restoring and reading back the old mode. |

### Why both verifications failed

The string at 4648 can come only from the final underrun predicate in `verifyRate`. The forward step therefore
got past timing readback, both HS DMA completions and the cadence and phase checks. The first sticky bit can have
been raised at any point from the pre-switch clear through the 144 verification. Neither DMA done nor register readback
proves that the first live frame latched atomically or that the panel accepted the new group on the expected frame.
The timed model idealized the DSI/CTL latch and checked matching totals and a fetch position inside blanking. It modeled
no bandwidth, clock margin, FIFO service deadline or mismatch between the panel's and controller's frame boundaries.
Its original one-shot underrun injection ended by the time recovery began.

The inverse operation cleared raw and IRQ-sticky underruns while 144 could still be scanning, waited for a safe
programming window, staged DSI0 then DSI1, INTF fetch/timing and the bonded CTL flush, and then sent the old group
into its front porch. A bad frame from the remaining 144 interval or from the timing/group handover could set the
bit again before the restored 60 interval. The restore's exact error shows the old timing, command-register readback,
cadence and phase passed. Persistent half-switched register state is therefore unnecessary to explain the log. Panel
acceptance and clean steady 60 scanout after the handover are both unknown from this capture. Record the transition
status up to old-mode restoration, then clear it and verify the restored rate and timing over complete frames. A
persisting underrun still fails recovery.

## Driver and boot arguments

`piano-dpu-modes=1` establishes the verified 60 Hz mode before NativeGopDisplay offers it as the single
`TimingElements` entry (ID 1000, color ID 100). `piano-dpu-modes=120` requests the same fixed contract at 120 Hz.
It verifies the inherited 120 Hz or the VFP-only return from 60 before publication.
`piano-dpu-modes=144` selects the experimental fixed 144 Hz profile. The worker waits for the MM0 vote before staging
timing. After a clean rollback it reports the restored 60 or 120 Hz to NativeGopDisplay for the single timing. A refusal
before staging or a failed rollback leaves timing registration unavailable on this profile. The switch is off by default.
See [the M3 bandwidth analysis](dpu-m3-bandwidth.md) and [the earlier high-refresh analysis](dpu-m3-144.md).
A stale higher timing ID returns `kIOReturnUnsupported` to IOMFB before a mode service call, property update or VBL change.
A request for the published timing remains idempotent. The 60 Hz VFP-only commit changes DSI TOTAL, the INTF
period and the enabled fetch starts. It retains the known 120 Hz line and link clocks and the inherited resources.
Omit `piano-dpu-refresh` on the M3 boot. With `=1`, a refusal before staging keeps the boot profile fallback.
With `=120` or `=144`, timing registration requires a verified mode. A verified rollback returns `kIOReturnAborted` to
startup with the verified 60 or 120 Hz rate for the single fixed timing. An unverified rollback returns
`kIOReturnOffline` and prevents timing registration.

| Refresh | Expected INTF clock | Frame interval | INTF programmable fetch start with M1's 24-line lead |
| --- | ---: | ---: | --- |
| 60 Hz | 159,667,200 Hz | 16.667 ms | `0x00280441` |
| 90 Hz | 159,667,200 Hz | 11.111 ms | `0x001a4901` |
| 120 Hz | 159,667,200 Hz | 8.333 ms | `0x0013b6c1` |
| 144 Hz | 159,667,200 Hz | 6.944 ms | `0x00106da1` |

The IOAV timing elements describe the uncompressed framebuffer raster. The fixed clock above is for the compressed, per-link
timing hardware. The default DSI shadow layout follows SunUEFI's 60/120 Hz path (+0x1ec DB, +0x1e8 flush).
`piano-dpu-refresh-db=2` selects +0x1e8 DB and +0x1e4 flush, the offsets in the downstream
[DSI register definitions](https://android.googlesource.com/kernel/msm/+/fb1612bae1aa0107928e17a1add77fa9a83f15c4/drivers/gpu/drm/msm/dsi-staging/dsi_ctrl_reg.h).
M1 read all three words at idle. Qual1 verified the VFP-only latch. The hardware timing of the ordered HS FIFO packets and
panel acceptance during rate-group changes remain open. [Qualcomm command path](https://android.googlesource.com/kernel/msm-extra/display-drivers/+/e7ab2aa8bd2f6f27b311ccada3722d20dee4bcef/msm/dsi/dsi_ctrl_hw_cmn.c)

## Interaction with M2/M4/M5

- **M2 verified-WC clean skip.** After any transaction that stages timing, the worker clears the clean verification for every
  retained flip slot while it still holds the flip lock. This includes attempts that roll back. The next eligible use of each
  backing runs the whole-backing clean and WC recheck again. A same-rate request or a refusal before timing stores keeps the
  existing verification.
- **M4 Night Shift.** With `native-gop-display-night-shift=1`, the worker replays the retained location-9 matrix and brightness
  correction on DSPP0 and DSPP1 after the new timing latch, or after a verified rollback. It holds the same lock through the
  PCC latch. M5 wake still replays Night Shift before the synchronous panel call returns to NativeGopDisplay.
- **M5 panel sleep and fetch.** The slices worker processes panel power before a queued mode request. A request while the panel
  is asleep, scanout is stopped, or panel recovery is unsafe returns `kIOReturnNotReady` before timing or DSI stores. At an
  awake switch both enabled fetch starts move with the new timing and keep their lead from frame end. Panel sleep saves the
  current mode's starts and wake restores them before restarting scanout.

## RAM-boot read list

1. With M3 armed from boot, verify 120 → 60 on both links, one 3200 × 2136 timing at ID 1000, matching
   `IOMFBDisplayRefresh` and stable login. Compare the snapshot's 60 Hz totals, both fetch starts and underrun status.
2. Restart the GUI session and enumerate again. A saved higher rate must resolve to the offered 60 Hz timing. Check
   flip cadence, Night Shift, panel sleep/wake and touch on the fixed mode. Confirm that wake restores both 60 Hz fetch starts.
3. On a control boot with the M3 argument absent, check the existing single timing and zero M3 stores.
4. For `piano-dpu-modes=144`, capture the completion status and encoded X/Y of the MM0 command before the mode latch.
   Compare to a fixed-120 control with the same workload. Verify applied MM0/LLCC/DDR and MDP core/MMCX, then check
   per-INTF underruns through the transition, sustained idle and motion, and panel wake. Confirm a single fixed timing
   and matching delivered cadence after verification. If the transition fails, identify the failing interval and the
   rate that passed rollback verification.

The following Swift probe enumerates modes and can exercise a sequence through CoreGraphics. Run it in the logged-in GUI session.
It selects the internal panel by the Xiaomi vendor and product IDs, then keeps the active logical resolution when switching.

```swift
import CoreGraphics
import Foundation

var displays = [CGDirectDisplayID](repeating: 0, count: 16)
var count: UInt32 = 0
precondition(CGGetOnlineDisplayList(UInt32(displays.count), &displays, &count) == .success)
guard let panel = displays.prefix(Int(count)).first(where: {
    CGDisplayVendorNumber($0) == 0x61a9 && CGDisplayModelNumber($0) == 0x8750
}), let initial = CGDisplayCopyDisplayMode(panel),
   let modes = CGDisplayCopyAllDisplayModes(panel, nil) as? [CGDisplayMode] else {
    fatalError("Piano panel or modes unavailable")
}
func hz(_ mode: CGDisplayMode) -> Int { Int(mode.refreshRate.rounded()) }

for mode in modes {
    print("\(mode.width)x\(mode.height) pixels=\(mode.pixelWidth)x\(mode.pixelHeight) \(mode.refreshRate) Hz")
}
print("active \(initial.refreshRate) Hz")
for rate in CommandLine.arguments.dropFirst() {
    guard let wanted = Int(rate), let mode = modes.first(where: {
        $0.width == initial.width && $0.height == initial.height &&
        $0.pixelWidth == initial.pixelWidth && $0.pixelHeight == initial.pixelHeight && hz($0) == wanted
    }) else { fatalError("mode unavailable: \(rate)") }
    let before = CGDisplayCopyDisplayMode(panel).map(hz)
    let result = CGDisplaySetDisplayMode(panel, mode, nil)
    Thread.sleep(forTimeInterval: 0.75)
    let after = CGDisplayCopyDisplayMode(panel).map(hz)
    print("\(before ?? 0) -> \(wanted) Hz: result \(result.rawValue), active \(after ?? 0) Hz")
    precondition(result == .success && after == wanted)
}
```

## Fix-image gate

Path: QuartzCore WindowServer selects an IOAV color/timing pair through IOMFB. NativeGopDisplay enumerates the timings
and validates the IDs. Its startup calls the synchronous PianoDpu worker under the flip lock to establish 60 Hz.
The worker checks the bonded CTL0/INTF1+2/DSI0+1 path carrying VIG0/1 through LM0/1 and DSC0/1 to the P81 panel.
Ubuntu uses full MSM modesets. SunUEFI has the live same-group VFP path and MiCode shows the separate SDE/DSI DFPS path.
NativeGopDisplay publishes the timing, slot 423, refresh dictionary and VBL period from the verified result.

Checks: **ours-OK** on qual1 for boot counter motion, the 120 → 60 commit, timing and cadence readback and Night Shift.
**ours-missing** for qualification at enumeration, per-mode MDP/ICC/VBIF sizing and the recovery underrun observation
boundary. **Unknown** are the precise failing interface and resource, the clock/urgency state at 144, a live clean 60 Hz
restore, power-state interactions after the fix, and the exact WindowServer failure after IOMFB returned the error.

Root cause: Unqualified 144 Hz was exposed as settable. The native VIG/DSC feed needs 20% higher per-line throughput
with unchanged resource programming. The live HFP and rate-group transition also lacks stock's coupled commit sequence.
The sticky underrun status leaves the failing frame unresolved. The restore used an observation window that included
the failed mode, so its error could also have come from 144 Hz frames.

ONE change: Make M3 a qualified fixed-mode contract. Publish 60 Hz only after verifying it or publish the confirmed
inherited 120 Hz as a single fallback after a verified rollback. Refuse unqualified requests before writes. Keep the
validated line rate, DSC configuration and inherited 120 Hz resources for 60 Hz. After a failed boot timing commit,
verify the previous timing and count underruns from a fresh baseline on restored scanout. Surface a persistent
failure as offline. Exercise the qual1 sequence and verify both the refusal and the recovery window in the timed model.
