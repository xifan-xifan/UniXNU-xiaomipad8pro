# Rear torch: PianoTorch (B12)

## Status (2026-11-27)

- **Owner decisions `OWNER-0717` & `OWNER-0801`.** The configured torch-on current and default cap are **400 mA total**
  (2 x 200 mA per channel, ITARGET 39, TORCH_CLAMP 39) — both `piano-torch-ma` and `piano-torch-max-ma` default to 400 mA
  total (with CLI numeric requests clamped at 400 mA), not only the top step of the ladder. PianoTorch still defaults to off
  (`piano-torch=0`, opt-in boot arg only). The T4 fault/status probe and opt-in `piano-torch-ladder=1` are host-verified.
  The ladder runs 100/200/300/400 mA total within the configured cap, with 10 s lit and 10 s dark per step. Ordinary torch-on
  and both diagnostics retain all temperature and PMIC guards, the write allowlist, activation and explicit OFF read-backs,
  and synchronous OFF on sleep or shutdown. T4's optional FLASH pulse stays at 50 mA total for 80 ms.
  [Driver](../../package/PianoTorch/PianoTorch.cpp)
- **Attended lit RAM boot pending.** Both November 15 T2 RAM runs read back all 12 register writes and the rear LED stayed dark.
  The lit test waits until the owner is beside the tablet to watch the LED and match visible light to the register evidence.
  [Hardware backlog, B12](../BACKLOG-hardware.md#2-backlog-b1b15)

## Attended T4 RAM qualification checklist

1. Build the RAM A bundled image with volatile Data, a 30 s observer setup delay and a 300 s return timer.
   Keep PianoThermal and PianoPower enabled. The T4 fault/status command below uses 15 s per torch pair and a 100 mA total
   cap for the initial mapping check.

   ```sh
   PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_DATA_PERSIST=0 PIANO_RESET_AFTER=300 \
     PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-thermal=1 piano-power=1 piano-power-ps=1 \
     piano-torch=2 piano-torch-ma=100 piano-torch-max-ma=100 piano-torch-delay=30 piano-torch-test=15 \
     piano-torch-probe=1 piano-torch-probe-spare=1 piano-torch-flash=1" \
     tooling/k27/build-native-image.sh
   ```

   For the owner-approved attended ladder, build with all four current steps enabled.

   ```sh
   PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_DATA_PERSIST=0 PIANO_RESET_AFTER=300 \
     PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-thermal=1 piano-power=1 piano-power-ps=1 \
     piano-torch=2 piano-torch-max-ma=400 piano-torch-ladder=1 piano-torch-delay=30" \
     tooling/k27/build-native-image.sh
   ```

2. Put the owner where they can see the rear LED and its reflection on a matte surface in a dim room. Start port 0 capture in
   one terminal and boot once from ABL in a second terminal. Use the locally configured tablet selector. Keep the capture
   and its timestamps through the diagnostic and the scheduled return to ABL.

   ```sh
   python3 tooling/native/usb-console.py --capture 600
   ```

   ```sh
   fastboot -s "$PIANO_SERIAL" boot out/ram/boot-piano-macos-ram-A-bundled.img
   ```

3. For T4, use the `probe … next window` lines as the cue to watch. For the ladder, use the `ladder step` lines and record
   the LED's visible state for each 10 s lit and 10 s dark window. Record the owner's observations with approximate timestamps.

   | Window | Ask the owner |
   |---|---|
   | A, factory channels 1+2 | “Was the rear LED dark before A? Did it turn on at all? Was it faint, steady, flickering, or only a brief blink? Did it go dark again?” |
   | B, spare channels 3+4 | “Did B light the same rear LED, a different emitter, or neither? Was it fainter or brighter than A? Was it dark again at the end?” |
   | FLASH, factory channels 1+2 | “Did you catch one brief blink or a reflection? Was the LED dark immediately afterward?” Record a skipped pulse separately. |
   | End of session | “Is the rear LED still dark?” If any emitter stays lit after its window, end the session with an orderly shutdown and record the OFF failure. |

   After the scheduled return, confirm ABL answers with `fastboot -s "$PIANO_SERIAL" getvar product`.

4. Save the `PianoTorch:` lines with the owner's observations. For the ladder, verify `max=400` and `ladder=1`, TORCH_CLAMP 39,
   ITARGET 9/19/29/39, 12/12 activation read-backs and 4/4 explicit OFF read-backs for each completed step. Check the 500 ms
   PMIC status and flash/SoC/battery temperature samples in both phases. A fault, unreadable status, SoC at or above 85 °C,
   battery at or above 45 °C, sleep or shutdown ends the ladder with explicit OFF.

   For the T4 mapping run, verify the start line has `max=100`, `probe=1 spare=1 flash=1`
   and `test=15 s`, and the survey shows PPID `0x1ee`, owner EE 0, TYPE `0x18`, and SUBTYPE `0x07`. Keep both 256-byte
   dumps (`probe dump before all windows`, `probe dump after all windows`, each 16 rows of 16 bytes). Keep the active and
   post-window register diffs, read-back counts, and any `--` unreadable fields, skipped windows, trips or OFF failures.

   - Torch windows log the immediate activation sample and approximately 500 ms polls before, during and after the window.
     FLASH records activation and samples on a 20 ms cadence, including a timer-boundary sample if it reaches its deadline.
     FLASH samples are written to the console after explicit OFF.
   - Each sample labels the pair and reports raw `STATUS1` (`0x06`, per-channel short/open), `STATUS2` (`0x07`, VPH low,
     BCL, BOB current limits, OTST and BOB thermal overload), `STATUS3` (`0x09`, per-channel timer expiry),
     and `INT_RT_STS` (`0x10`, fault and ramp-up/down done). It also reports `MODULE_EN`, `CHAN_EN` and available temperatures.
     T4 labels channels `ch1` through `ch4`. The original survey uses zero-based indices.
   - For a clean 100 mA torch window, look for 12 of 12 activation writes read back, `MODULE_EN` bit 7 set and
     `CHAN_EN` low nibble `03` for A or `0c` for B. Each selected pair has `ITARGET=09`, its `IRESOLUTION` bits set,
     `TORCH_CLAMP=09` (low seven bits), software-level active-high `STROBE=01` (low seven bits) and `TIMER=00`.
     FLASH requests 50 mA total for 80 ms on A, with `ITARGET=01`, A's resolution bits clear and `TIMER=88`.
     Check each executed window's explicit OFF read-back, and verify bit 7 of `MODULE_EN` and all four `CHAN_EN` bits clear.
   - A confirmed OFF after an A-only open permits the alternate B check. A short, module-wide or temperature trip,
     failed activation or unconfirmed OFF stops further activation. FLASH runs only after the requested torch windows
     complete cleanly. An orderly sleep or shutdown aborts the remaining windows. Wake leaves the diagnostic ended.

5. Match the visible result to the register evidence. Light only on B warrants a wiring check. A channel open or short, VPH
   or BOB fault, or a real-time fault without a channel fault identifies where to investigate. The real-time fault bit alone
   cannot distinguish an open, short or regulator-not-ready condition. If both pairs stay dark with clean activation and OFF
   read-back, the run establishes the result at the qualified current and ambient light. The owner-approved hard cap is
   400 mA total for both T4 torch windows and the ladder.

The Linux flash driver and piano device tree agree on PM8550 channels 1+2, software strobe for torch, the four-channel
register layout and a 100 mA total torch ceiling. The rear camera references the same LED. The LED node defines no separate
BOB/VPH supply control or external strobe GPIO. The raw page also covers mitigation, OTST thresholds and unnamed registers
inside PPID `0x1ee`. The probe reads supply-related fault flags within that peripheral.

## Historical design and bring-up notes

The design, boot arguments, test plans and measurements below describe the builds under test at the time.

Status, 2026-11-15: **runs on the tablet** (T1, T2 twice). T2's writes all read back (Linux's torch state, letter-perfect) and the
rear LED stayed dark both times — the T2 outcome and what it leaves open are in section 8; `piano-torch-probe=1` is the next boot's
instrument. Level 1 is a read-only survey, level 2 is torch control. Backlog item B12 (`../BACKLOG-hardware.md`). Template: `README.md`.

### 1. Purpose and gate

A flashlight, as the Ubuntu port has ("rear torch control, with a check tool that verifies readback and restores the previous
setting"). Its v1.4.0 release notes also describe a timed FLASH pulse for photos and a shared green privacy LED. The port documents
that a live V4L2 flash owner can leave torch mode on after close, so every diagnostic path has an explicit off. No login milestone depends on it; it is a
small daily-use item and the first native driver that writes a PMIC register on purpose, so it also proves the owner-channel write
path the later PMIC work (B4 charging policies, B6 thermistors, B7's repeater) needs.

### 2. Hardware facts

| Item | Value | Source | State |
|---|---|---|---|
| Block | PM8550 (SPMI SID 1) `led-controller@ee00`, `qcom,pm8550-flash-led`: PPID 0x1ee | boot DT `pmic@1` | verified |
| In the boot DT | **`status = "disabled"`** (as mainline's `pm8550.dtsi`): the boot DT leaves the block to the OS | boot DT | verified |
| The factory's own wiring | the vendor tree's `qcom,flash_led@ee00` carries `torch_0`/`torch_1` (qcom,id 0/1), `flash_0`/`flash_1` (id 0/1) and `switch_2` (`qcom,led-mask 0x0f`, symmetry; switch_0 mask 09 and switch_1 mask 06 disabled); the rear camera node `qcom,camera-flash0` wires torch-source = torch_0+torch_1, flash-source = flash_0+flash_1, switch-source = switch_2 → **the rear LED is channels 1+2, the factory's own choice**; each torch child allows `qcom,max-current-ma` 400 a channel (a lamp built for hundreds of mA a channel; Ubuntu's 50 mA a channel is its port's own cap, not the hardware's). The vendor driver's "switch" is a grouping over the same CHAN_EN/MODULE_EN bits — no register exists that our on sequence misses | vendor board.dtb + its live copy in piano-live.dtb; Xiaomi's vendor driver `leds-qti-flash.c` (piano-w-oss, specification only) | verified (source) |
| What Ubuntu used | **the same block**: v1.1.1 (K:82a40d864b46) enables `&pm8550_flash` with one LED, `led-sources = <1 2>` ("the factory uses them as one lamp: the flash and the torch both drive the two channels together"; factory qcom,id 0/1 = channels 1/2), `led-max-microamp = 100000` (torch, the lamp), `flash-max-microamp = 400000`, `flash-max-timeout-us = 300000`; v1.2.0 (K:946e2d7c87c2) links it to the rear sensor; **whether Ubuntu's torch ever visibly lit this unit's LED was never recorded** | Ubuntu dts | verified (source) |
| Identity | TYPE +0x04 = 0x18; SUBTYPE +0x05: 0x07 four channels (this layout), 0x04/0x03 three; REVISION +0x00 = 0x01 on this unit | leds-qcom-flash.c | verified (source + tablet T1) |
| Four-channel registers | STATUS1 +0x06 (2 bits a channel: **first short, second open** — the factory's driver reads STATUS1 as channel open/short), STATUS2 +0x07 (b0 VPH low, b1 BCL, b2/b3 BOB current limit, b4 OTST2, b5 OTST1, b6 BOB thermal overload), STATUS3 +0x09 (timer expired, 2 bits a channel); **INT_RT_STS +0x10 (b0 fault = open/short/vreg-not-ready, b3 all ramp-down done, b4 all ramp-up done)**; TIMER +0x3e+ch (b7 on, 10 ms steps); ITARGET +0x42+ch (b6:0); MODULE_EN +0x46 b7; IRESOLUTION +0x49 (bit ch: 5 mA steps, else 12.5 mA); STROBE +0x4a+ch (b2 hardware, b1 edge, b0 active high); CHAN_EN +0x4e (bit ch); MITIGATION +0x65 (b0 LMH; the factory sets it only for a flash ≥ 1 A); OTST thresholds +0x7a/+0x78 (b2:0); TORCH_CLAMP +0xed (b6:0) | leds-qcom-flash.c; STATUS1's second bit, INT_RT_STS, MITIGATION: the vendor driver | verified (source) |
| Torch current | per channel (ITARGET + 1) · 5 mA; Linux: ITARGET = (lamp µA / channels + 1) / 5000 − 1, clamp = (limit / channels) / 5000 − 1 = **9** for piano; safety timer off in torch mode | leds-qcom-flash.c | verified (source) |
| Write path | SPMI arbiter v7, EE 0 (`qcom,ee = 0`): owner channel at chnls `0xc500000` + 0x1000 · APID (command +0x00, status +0x08, data +0x10, opcode 0 = extended write); the arbiter accepts a write only from the APID's owner (cnfg `0xc42d000` + 4 · APID, b2:0) | boot DT; spmi-pmic-arb.c; PianoDwc3's SDAM write | verified (source; the SDAM write runs natively) |
| Owner of the flash APID | **EE 0**: T1 read APID 617 owner EE 0, and T2's writes went through and read back over the observer channel (a different MMIO window), so the PMIC's register file really changed | tablet T1/T2 | **confirmed on the tablet** |
| Thermistor | `pm8550_flash_therm` = PMK8550 ADC5-gen3 channel 0x145 (a flash-thermal zone without trips) | Ubuntu dts | verified (source); reading it is a conversion request = a PMIC write: B6, not here |
| Reset values on this unit | timers 0x93 ×4 (safety timer on, 190 ms), strobes 01 ×4, thresholds 1/1, clamp 39 (200 mA a channel), 0x4f = 02, 0xe8-0xeb = 80 c0 20 32 (unidentified), the rest read 00; whether STATUS1's open bit is live or latched and when it appears; what 0x20-0x37, 0x50-0x77, 0x80-0xe7 hold (the survey now reads them; no source names them) | tablet T1 | read once; behaviour needs-boot |

### 3. Ubuntu as the specification

`leds-qcom-flash.c` (`qcom_flash_led_brightness_set`, `set_flash_current`, `set_flash_timeout`, `set_flash_strobe`,
`set_flash_module_en`, `qcom_flash_fault_get`, probe's clamp): a torch on is strobe off (CHAN_EN clear) → module off →
current (ITARGET per channel, IRESOLUTION 5 mA bits) → timer 0 → module on → software strobe word 0x01 per channel and CHAN_EN set;
off is the same with 0 mA. Every write is a regmap field update (read-modify-write of that field only). Correction of 2026-11-15:
Linux also lowers the OTST thresholds temporarily, reads STATUS2 to calculate a current budget and restores the thresholds.
For this board's single LED, both four-channel derating ceilings exceed the 100 mA torch limit. Xiaomi's vendor driver
(`leds-qti-flash.c`, the driver the factory
tree binds) reaches the same end state by another route: its probe clears every channel's safety-timer enable bit and leaves the
strobe source bits at software/level; its torch brightness path writes IRESOLUTION + ITARGET then MODULE_EN + CHAN_EN through its
"switch" grouping; it never writes TORCH_CLAMP (the reset 200 mA a channel stands) and touches MITIGATION only at flash ≥ 1 A. No
register exists that both drivers write and PianoTorch does not. Ubuntu's check tool (`tools/check-piano-flash.py`, MIT) writes the
LED class brightness, reads it back, restores it; `piano-camera-light-off` exists because closing V4L2 does not turn the torch off:
an explicit off path matters.

### 4. Design

**Ownership.** PianoTorch is the only writer of PPID 0x1ee; nothing else in the tree touches it. The write channel mapped is the
flash APID's one page; the arbiter itself refuses any other APID's write from that page.

**Level 1** (`piano-torch=1`): the arbiter (v7 check), the APID and owner, then **all 256 bytes of the peripheral** in 8-byte
observer reads (the first two boots read only the five ranges the sources name; after T2's no-light the survey covers everything);
only the identity read when TYPE is not 0x18. Decoded and logged raw (16 lines).

**Level 2** (`piano-torch=2`) starts only when level 1 says: in the map, owner EE 0, TYPE 0x18, SUBTYPE 0x07, the survey complete,
no channel outside the driver's set on (else "someone else drives this module"). Found lit → turned off first. Then:
- **Allowlist** (`TorchCore.h kWritable`, 16 registers): TIMER 0x3e-0x41, ITARGET 0x42-0x45 (b6:0), MODULE_EN 0x46 (b7),
  IRESOLUTION 0x49 (b3:0), STROBE 0x4a-0x4d (b6:0), CHAN_EN 0x4e (b3:0), TORCH_CLAMP 0xed (b6:0). Channels 3/4's registers are
  allowlisted **for the probe only** (`piano-torch-probe`, the one-boot test of which pair the LED really sits on); a normal boot
  never writes them. `Controller::put` refuses anything else. Masked writes read first, merge under the mask, write, and compare read-back. Activation stops on a read or write failure.
  OFF tries every step and can use the last known value during an observer failure.
- **Current** (`piano-torch-ma`): defaults to **400 mA total (2 × 200 mA, ITARGET 39)** per `OWNER-0801` unless a boot arg or
  request names a lower current. `piano-torch-max-ma` defaults to **400 mA total (2 × 200 mA, TORCH_CLAMP 39)** and accepts
  10..400 mA total (hard-capped at 400 mA). Requests respect the configured clamp and round down to 5 mA steps per channel.
- **Guard** (polled every second while lit, `TorchCore.h judge`): the request's time (default `piano-torch-timeout` 300 s, at most
  1800 s); STATUS2 over-temperature/thermal overload, current limit, low supply, INT_RT_STS fault, STATUS1 short **or open** (the open bit is the
  module's own word for a lit register state with no LED current path — since 2026-11-15 it is a fault reason, not an unknown) →
  off and 60 s cool-down; the
  enables no longer on → off; the hottest TSENS controller (PianoThermal's `TemperaturesDeciC`, refreshed at its report interval)
  ≥ `piano-torch-soc-max` (85 °C, its "warm") or the battery (AppleSmartBattery `Temperature`, PianoPower) ≥ `piano-torch-batt-max`
  (45 °C) → off / refused; two unreadable polls in a row → off. Missing drivers only remove their input; the time limit and the
  module's bits always apply. The time limit is software: the module's own safety timer is off for a torch (its range ends at
  1.27 s, a flash's).
- **Power**: joins the power tree (`PianoPowerContract.h`): `setPowerState(kOff)` (system sleep) and `systemWillShutdown` turn it
  off synchronously (four SPMI writes). A panic cannot: see §10.

### 5. IOKit family / ABI: the control path

`PianoTorch : IOService` on IOPlatformExpertDevice, registered (found by class). Properties: `piano-torch`, `piano-torch-survey`,
`piano-torch-state` (e.g. `on 50 mA (2 x 25 mA, itarget 4), off in 287 s`, `off (the time limit)`, `unavailable: ...`),
`piano-torch-lit`, `piano-torch-ma`.

**From macOS (chosen: the least invasive, nothing else changes).** `setProperties` accepts `piano-torch-set` (mA, 0 = off, or a
boolean) and `piano-torch-seconds` from an administrator only (`IOUserClient::clientHasPrivilege`, as PianoBacklight). The tool
`tooling/native/piano-torch/piano-torch.c` (IOKit.framework, built on the Mac like `pianousb-shell`; installed as
`/usr/local/bin/piano-torch` by `configs/userland.toml`, `docs/userland.md`): `sudo piano-torch on [MA [S]]`,
`off`, `toggle`, `status`; exit 0 when the state after the request is the one asked for. Usable from the root shell over USB too.
The CLI clamps its numeric `MA` argument to 400 mA (`OWNER-0801`). With no numeric argument, `on` uses the boot's configured `piano-torch-ma` (default 400 mA total, 2 × 200 mA).

**From other drivers**: `callPlatformFunction` `PianoTorchSet` (mA, seconds, caller), `PianoTorchToggle` (caller),
`PianoTorchStatus` (buffer, size); queued, never blocking on the PMIC (`PianoTorchClient.h`).

**The USB console hook (specified, not made: PianoDwc3 is not changed).** In `PianoDwc3.cpp execute()`, before the "unknown
command" branch, about 15 lines: `torch [on [MA [S]] | off | status]` → `IOService::serviceMatching("PianoTorch")` +
`copyMatchingService`; `on`/`off` → `callPlatformFunction("PianoTorchSet", false, (void *)(uintptr_t)ma, (void *)(uintptr_t)s,
(void *)"the USB console", nullptr)`; then `IOSleep(50)` and `callPlatformFunction("PianoTorchStatus", false, buf, (void *)sizeof buf,
...)` printed with `cprintf`; a help line. No link dependency (PianoBacklight's pattern). A Shortcut / menu-bar item on top of the
tool is user-space work for later.

### 6. Boot args

| Argument | Default | Meaning |
|---|---|---|
| `piano-torch` | 0 | 0 off, 1 survey (read only), 2 survey and control |
| `piano-torch-ma` | 400 | configured lit current of an on without its own (2 × 200 mA per `OWNER-0801`; 10..max, 5 mA a channel steps) |
| `piano-torch-max-ma` | 400 | 10..400 mA total (default 400 mA = 2 × 200 mA per `OWNER-0801`; larger values clamped to 400 mA) |
| `piano-torch-timeout` | 300 | auto-off of a request without its own time (10..1800 s) |
| `piano-torch-test` | 0 | level 2: light once at boot for S s (1..60), then off |
| `piano-torch-soc-max`, `piano-torch-batt-max` | 85, 45 | the guard's temperatures (°C) |
| `piano-torch-delay` | 0 | wait first (s) |
| `piano-torch-probe` | 0 | `1` with level 2 and a nonzero `piano-torch-test` selects T4. The factory-pair window polls all three STATUS bytes, INT_RT_STS and the enables every 500 ms before, during and after the test, with full dumps and diffs. |
| `piano-torch-probe-spare` | 0 | `1` adds the spare-pair mapping window when T4 runs |
| `piano-torch-flash` | 0 | `1` adds an 80 ms, 50 mA FLASH pulse when T4 runs and the cap permits it, followed by explicit off |
| `piano-torch-ladder` | 0 | `1` selects the guarded level 2 ladder of 100/200/300/400 mA total within the configured cap. Each step runs for 10 s lit and 10 s dark with 500 ms status and temperature polls. |

### 7. Kernel collection

`tooling/k27/build-native-piano-torch.sh`: host tests (`tests/torch_host.cpp`: an arbiter v7 model with the flash module and two other
peripherals; level 1 never writes and reads only PPID 0x1ee; the refusals; the on sequence's registers and values, kept bits, the
cap; a read-back mismatch, a denied write, a fault after on; every guard reason), a store check of the portable headers, the tool on a
Mac, the objects, the import check. `PianoTorch` is in `configs/k27-native-kexts.txt`; `build-native-image.sh` links it.

### 8. Hardware test plan (RAM boots, owner present for T2/T3/T4)

```sh
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-torch=1" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-torch1 && cp -R out/ram out/ram-torch1
PIANO_GPU=2 PIANO_VMM_PRESENT=0 PIANO_RESET_AFTER=300 PIANO_EXTRA_ARGS="panic_on_crash_disabled=1 piano-usb=2 piano-thermal=1 piano-torch=2 piano-torch-ma=20 piano-torch-test=5 piano-torch-delay=30" \
  tooling/k27/build-native-image.sh && rm -rf out/ram-torch2 && cp -R out/ram out/ram-torch2
python3 tooling/native/usb-console.py --capture 1000 &   # then: tooling/native/native-loop.sh out/ram-torch1/boot-piano-macos-ram-A-bundled.img
grep -a "PianoTorch" logs/usb-console/<newest>-port0.txt
```

**T1 expected** (`piano-torch=1`, nothing changes; `..` = values to record; **ran 2026-11-14, see below**):
```
PianoTorch: start: piano-torch=1 ma=50 max=100 timeout=300 s test=0 s soc-max=85 C batt-max=45 C delay=0 probe=0 spare=0 flash=0 ladder=0 (PM8550 SID 1 flash 0xee00, PPID 0x1ee)
PianoTorch: level 1: loads only, nothing written (the SPMI observer's read command is the only store)
PianoTorch: stage 1: SPMI: the arbiter and the flash module's APID (read only)
PianoTorch: spmi: arbiter 0x70020000, 896 APIDs; flash PPID 0x1ee: APID .., owner EE 0 (this processor: writes possible)
PianoTorch: stage 2: the flash module's registers over the observer channel (read only)
PianoTorch: flash: revision 0x.. type 0x18 subtype 0x07: four-channel
PianoTorch: raw 00: ..      raw 10: ..      ... (16 lines of 16 bytes: the peripheral's whole 256 bytes)
PianoTorch: flash: module off (00), channels on 00, 5 mA steps 0., torch clamp .. (.. mA a channel), otst thresholds ./.
PianoTorch: flash ch0 (piano's lamp): itarget .. (.. mA in .. steps) strobe .. (...) timer .. (safety timer off|on)
PianoTorch: flash ch1 (piano's lamp): ...          ch2, ch3 (not wired on piano): ...
PianoTorch: flash status1 00 status2 00 status3 00 rt 00: no fault on piano's channels
PianoTorch: summary: 4ch rev .. off; APID .. owner EE 0; ch0/ch1 itarget ../.. ires .. clamp ..; status 00 00 00; writable
PianoTorch: stage 3: done
PianoTorch: done: 33 observer reads (0 failed), 0 PMIC writes
```
**T2 expected** (`piano-torch=2 piano-torch-ma=20 piano-torch-test=5`, 30 s after the kext starts; the owner watches the rear LED: a
dim light for 5 s, then dark): the T1 lines up to `summary:` (without the `level 1` line), then
```
PianoTorch: stage 3: level 2: torch control (writes to the flash module only, each read back)
PianoTorch: level 2: torch control on channels 1+2 at 20 mA (at most 100), auto-off after 300 s, off at SoC >= 85 C or battery >= 45 C; writes only to PPID 0x1ee, 16 allowlisted registers
PianoTorch: test: on at 20 mA for 5 s (piano-torch-test)
PianoTorch: found PianoThermal (its temperature is part of the guard)
PianoTorch: on: 20 mA (2 x 10 mA, itarget 1, clamp 9) for the boot test, 20 mA 5 s; 12 writes, 12 read back; status 00 00 00 rt 00, enables 80 03
PianoTorch: temperatures: SoC ..C, battery 0.00 C (none)
PianoTorch: off (guard): the boot test is over; 4 writes, 4 read back
```
**T2 result, 2026-11-15 (two boots)**: flip4 (20 mA, 5 s, 01:44) and hands1 (50 mA, 10 s, 14:03:57) both ran the sequence
letter-perfect — `12 writes, 12 read back`, `enables 80 03`, guard open, `off (guard)` on time — and **the rear LED stayed dark
both times while the owner watched** (hands1). So: the SPMI owner-write path works, the module accepts Linux's exact torch state,
and something between those registers and photons does not. What those boots never sampled: STATUS1's open bits (the guard then
ignored bit 2n+1), INT_RT_STS 0x10 (the fault bit whose factory meaning is open/short/**vreg-not-ready**), any register outside the
five surveyed ranges, and any window on channels 3/4. Also open: the current itself — the factory drives this lamp at up to
400 mA **a channel**; 10-25 mA a channel behind a diffuser may simply be invisible in a lit room.

**T4** uses the [attended RAM qualification checklist](#attended-t4-ram-qualification-checklist) above.

**T3** (attended, runtime): `piano-torch=2 piano-usb=2 piano-usb-shell=1` (+ `piano-power=1` for the battery input); the tool on the
tablet (built on the Mac by the build script: `out/native-kexts/PianoTorch/host-tests/piano-torch`, ad-hoc signed; pasted over the
root shell with `base64 -D`, or staged with `stage-soft-metal.py --file <tool>:usr/local/bin/piano-torch --boot-run "/bin/chmod 755
/usr/local/bin/piano-torch"`, as staged files are mode 644). In the root shell: `piano-torch on` → `level 2: on 50 mA (2 x 25 mA, itarget 4), off in 300 s`;
`piano-torch off` → `off (asked)`; `piano-torch on 100 10` and wait → `off (the time limit)` after 10 s; `piano-torch on 500` → 100 mA
(the CLI caps the argument before sending it); as a plain user → `not an administrator: use sudo`; lit through 5 minutes once (default timeout)
with a finger on the lens area: warm, not hot; `lit:` lines every 30 s show status 00.

Failure lines: `owner EE n (another EE...)` / `level 2 refused: ...` (nothing written: report the survey); `off (...): a write or its
read-back failed; ... A STEP FAILED` + `failed step: register 0x.. wanted .. read ..` (a field does not hold what Linux writes:
report; the off ran); `off (guard): the module's over-temperature bits` (STATUS2 at a 20 mA torch: stop, report the raw bytes);
`off (a channel's open-circuit bit ...)` (the module reports the channel open — LED, supply or wiring absent: the no-light
signature, report the probe lines); `lit` but no light (run T4, the probe, before concluding anything).

### 9. Fallback

Without `piano-torch` nothing is mapped. Level 2 refusing leaves level 1's survey. Any failed on runs the full off.

### 10. Open questions and risks

1. ~~SUBTYPE/REVISION, the owner EE~~ (T1: 0x07, rev 0x01, EE 0) — answered 2026-11-14; the reset values are recorded in section 2.
2. Are STATUS1/2/3 live or latched, and when does the open bit appear (with the enable only, or after a ramp attempt)? → T4's
   500 ms polls.
3. **What T2's no-light is** (ranked, 2026-11-15): the current far below the factory's (invisible), an open channel path (STATUS1
   bit 2n+1 / INT_RT_STS fault), the module's supply not present ("vreg not ready" — the supply may live in registers no source
   names: 0x4f read 02, 0xe8-0xeb read 80 c0 20 32), or the LED on the spare channels after all → T4 settles the first and the
   last in one boot.
4. **A panic or watchdog reset while lit**: the kext cannot run its off. A PS_HOLD warm reset resets the PMIC's peripherals
   (*inferred*, not verified for this block); ABL/Ubuntu boot next. Check once in T3 (`reset` from the console while lit).
5. The flash thermistor (ADC 0x145) as a direct guard → with B6's ADC conversions.
6. Camera flash (400 mA, 300 ms strobe) is B11's, not this driver's; it would need the derating sequence and a hardware strobe.
7. **Decided 2026-11-27 (`OWNER-0717`, updated `OWNER-0801`).** The owner approved the 400 mA total (2 × 200 mA) factory-safe
   range, set **200 mA per channel (400 mA total, ITARGET 39, TORCH_CLAMP 39)** as the configured torch-on setting (`piano-torch-ma`
   and `piano-torch-max-ma` defaults, CLI clamp 400 mA, not only the ladder top step), and enabled the guarded 100/200/300/400 mA
   ladder for an attended RAM boot. The driver stays default-off (`piano-torch=0`) with all temperature guards and read-back checks on.
   The lit test waits until the owner is beside the tablet watching the LED.

### 11. Licence note

Linux's `leds-qcom-flash.c`, `spmi-pmic-arb.c` and the piano dts (GPL-2.0) are read as the specification only (offsets, fields,
sequences, the current arithmetic); nothing is copied. Xiaomi's vendor `leds-qti-flash.c` (MiCode piano-w-oss, GPL-2.0) and the
factory tree's nodes were read the same way on 2026-11-15 (the channel wiring, STATUS1's open bit, INT_RT_STS, MITIGATION).
Ubuntu's MIT tools are read for behaviour. PianoTorch is BSD-2-Clause-Patent;
provenance in `package/PianoTorch/UPSTREAM`.
