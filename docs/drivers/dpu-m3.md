# Piano DPU M3 refresh modes

M3 adds selectable 60, 90, 120 and 144 Hz modes for the internal 3200 × 2136 display. The 27.3 RAM boot captured for M1 was
scanning at 120 Hz with programmable fetch enabled on both interfaces. Each fetch start was `0x0013b6c1`, 24 lines before the
end of the frame. The DSI timing-shadow words at +0x1e4/+0x1e8/+0x1ec were idle (zero). Both command engines were configured for
software triggering with the in-frame DMA block clear.

| Reference path | Piano M3 path | Comparison |
| --- | --- | --- |
| Ubuntu `panel-novatek-nt36532.c:425–450`, four panel timings and fixed lane rate | `DpuHw.h` mode table and `DpuModes.h` per-link planner | Same porches. The compressed timing engine runs at 159,667,200 Hz at every rate. |
| Ubuntu `panel-novatek-nt36532.c:496–511,617–650`, module order and rate-group values | Rate-group writes on demand through the bonded DSI controllers | Same register values and module order. BOE sends B2 first. CSOT sends B3 first. |
| Ubuntu `dsi_host.c:1220–1260,1354–1391,1462–1494`, DSI video synchronization and DMA packet encoding | HS short writes in the common front porch with DMA completion checked on both controllers | Same short-packet layout. The driver uses the controller FIFO and checks the line counters before triggering. |
| Ubuntu `dpu_hw_intf.c:103–282,284–306`, timing setup and programmable fetch | Stage both interfaces together and move each enabled fetch start with the porch | Firmware already enabled fetch. The driver preserves its distance from the end of the frame. |
| Stock DFPS, master DSI host first with double buffering, followed by the HS timing-switch commands | Stage the DSI and INTF words together, verify the latch, send the new rate group when it changes | Same ordering. The stock 60 Hz exit path also has a pen clear command. Its relevance to this port remains to be checked on hardware. |

The stock sequence is described in the [MiCode DSI display driver](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_display.c#L5433)
and [SDE encoder driver](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_encoder.c#L7075).

The three panel rate groups use page select `FF 10` and unlock `FB 01` before the two module-ordered writes.

| Refresh | Horizontal front porch per link | Vertical front porch | B2 | B3 |
| --- | ---: | ---: | --- | --- |
| 60 Hz | 117 | 2378 | `91` | `40` |
| 90 Hz | 309 | 68 | `00` | `80` |
| 120 Hz | 117 | 68 | `91` | `40` |
| 144 Hz | 21 | 68 | `00` | `00` |

## Fix-image gate

**Path** WindowServer and CoreGraphics mode selection → IOMobileFramebuffer → NativeGopDisplay timing ID → PianoDpuMode →
CTL and INTF1/2 → bonded DSI0/1 → NT36532. The same NativeGopDisplay mode supplies slot 423, the refresh property and VBL pacing.

**Checks** M1 established the 120 Hz register baseline, fetch state and command-path setup. The Ubuntu and stock references supply
the timing and command sequence. Establish CoreGraphics enumeration, the effective DSI latch, panel acceptance, visible quality
and touch on the M3 RAM boot.

**Root cause** NativeGopDisplay publishes one timing. The earlier VFP-only writer refuses the inherited programmable fetch
configuration and does not handle a horizontal porch change or a different panel rate group.

**One change** Add the opt-in M3 mode list backed by a verified, reversible timing and panel transaction.

## Driver and boot arguments

`piano-dpu-modes=1` enables M3. On the internal display, startup asks PianoDpu to establish 60 Hz before publishing the four
timing elements. If startup cannot establish that mode, NativeGopDisplay publishes the boot profile's single timing.
Without the boot argument the daily 60 Hz display contract remains in place. The existing B1c refresh policy and M3 are
alternative users of the timing hardware. Omit `piano-dpu-refresh` on the M3 boot.

NativeGopDisplay's `set_digital_out_mode` validates the color and timing IDs, then calls the PianoDpu worker's synchronous
`PianoDpuMode` service. The worker shares the flip engine's transaction lock. It classifies and checks both links, stages
the time-critical words with local interrupts masked, and waits for the CTL latch. It reads back the INTF and DSI timing words
and checks cadence, phase and underruns. A rate-group change uses four HS short writes in the common front porch on both links
and checks command-DMA completion and the restored command registers. At 60 ↔ 120 Hz only the porches change.
An anomaly after staging triggers a second transaction with the preceding timing and rate group and disables further
switches for that boot. A still-busy DMA remains under the controller's ownership, and the log reports incomplete recovery.
On success NativeGopDisplay updates its active Hz, slot 423, `IOMFBDisplayRefresh`, and the next armed VBL deadline.

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
M1 read all three words at idle. The live latch and the panel's handling of the ordered HS FIFO packets still need RAM-boot
qualification. The [Qualcomm command path](https://android.googlesource.com/kernel/msm-extra/display-drivers/+/e7ab2aa8bd2f6f27b311ccada3722d20dee4bcef/msm/dsi/dsi_ctrl_hw_cmn.c)
documents the FIFO, software trigger, and DMA completion.

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

Run the **M3 phase last** in a combined qualification, after completing and saving the M2, M4, and M5 results. Keep the display
at 60 Hz through the earlier phases on an M3-enabled boot. Use the 27.3 M1 flip profile with volatile Data and a boot guard,
`piano-dpu=2 piano-dpu-flip=4 piano-dpu-ahb-vote=1`, and
`piano-dpu-modes=1 piano-dpu-snapshot=1 piano-dpu-cadence=2`. Keep `piano-dpu-clean-skip=1`,
`native-gop-display-night-shift=1`, and `piano-dpu-panel=1` with the M5 backlight and touch settings for the integration reads.

1. After the GUI session is ready, record `system_profiler SPDisplaysDataType`, the CoreGraphics mode list, and both driver
   services in IORegistry. Expect the same panel resolution at 60, 90, 120 and 144 Hz in the list. The first active mode should
   be 60 Hz. Take a baseline with `sudo sysctl -n machdep.piano_dpu_snapshot`. Save the two timing totals, both fetch starts,
   the DSI shadow and command path, `DPTimingModeId`, and `IOMFBDisplayRefresh`.
2. In the same desktop session, switch along
   `60 → 90 → 120 → 144 → 60 → 120 → 90 → 144 → 90 → 60 → 144 → 120 → 60`.
   This covers each of the 12 directed pairs once. After each switch, compare the active CoreGraphics rate, the refresh property,
   the snapshot's classified mode and both fetch starts. Save the corresponding `PianoDpu: mode: … verified` and
   `NativeGopDisplay: M3: timing …` lines. A refusal, underrun, failed revert, or disagreement fails this step.
3. With M2's conditional policy active, warm a steady flip workload until its `clean:` `skip` counter increases. Read
   `machdep.piano_dpu_snapshot` before the switch, on the first eligible shows after it, and after repeated shows. Check that
   `init` increases again for reused WC slots before they resume skipping. Record `maps` and `evictions` to distinguish a reused
   slot from a newly mapped backing. With Night Shift active, capture the PCC enable and coefficients for both halves before
   and after the switch, and the `night shift: M3 mode switch` replay line. Confirm the warm white point and the retained correction.
4. At 90 Hz, take a snapshot before sleep and another when `piano-dpu-panel` reports `asleep`. Check both INTF engine and fetch
   enable bits and verify the counters stop. Request 144 Hz through CoreGraphics in the same login session. If it reaches the
   driver, expect `mode: 144 Hz refused` and no verified switch. Read the snapshot again while asleep to confirm the engines and
   fetch remain off and the timing stays at 90 Hz. On wake, verify both 90 Hz fetch starts, the Night Shift wake replay, and
   advancing frame counters before retrying 144 Hz. This retry must succeed and update both fetch starts. Return to 60 Hz.
5. At every rate, run the same continuously animating workload. Save at least two complete `piano-dpu-cadence` windows with
   latched frames and compare their presented intervals with the table. Keep the mode verification's frame-counter cadence
   alongside the flip cadence so a renderer that misses frames can be distinguished from incorrect scanout timing.
6. Record the C920 across all 12 transitions with moving high-contrast content. Watch the panel and inspect the recordings for a grey or
   black frame, a split between the halves, or tearing. At each steady rate exercise taps, continuous scrolling and two-finger
   gestures. Save a positive touch event count and note any loss, stalls or coordinate shift.
7. Run 100 successful switches starting at 60 Hz. Use eight complete directed-pair loops and the first four edges of the next
   loop. Record the requested and returned rate for every step, the switch count, both links' timing and cadence at the end,
   and any display or touch errors. Finish at 60 Hz and confirm the original mode, fetch starts and working touch.
8. On a separate control boot from the same kexts with the M3 argument absent, record the daily single 60 Hz timing and verify
   that the M3 path made no writes.

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
