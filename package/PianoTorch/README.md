# PianoTorch

Piano's rear torch for native macOS 27: the PM8550 flash LED module (`led-controller@ee00`, SID 1, PPID 0x1ee) with its two
factory-wired channels driven together, as the Ubuntu port does. Backlog B12. Its v1.4.0 release notes also describe a timed
FLASH pulse for photos and a shared green privacy LED; those are provenance facts for the optional T4 pulse, not copied code.

- `piano-torch=1`: a read-only survey over the SPMI observer channel: the flash APID and its owner EE, the module's identity,
  enables, per-channel current/strobe/timer, clamp, thermal thresholds, status bytes and raw bytes. The only store is the
  observer's read command.
- `piano-torch=2`: the survey, then torch control. Writes go only to that peripheral, only to the registers of
  `TorchCore.h`'s allowlist, each read-modify-write under its mask and read back. Off after `piano-torch-timeout` (300 s),
  on the module's own fault/over-temperature bits, at SoC >= `piano-torch-soc-max` (85 C, PianoThermal) or battery >=
  `piano-torch-batt-max` (45 C, PianoPower), when the enables read off, on system sleep and on shutdown. Configured torch-on
  current (`piano-torch-ma`) and default cap (`piano-torch-max-ma`) are both **400 mA total (2 x 200 mA, ITARGET and TORCH_CLAMP 39)**
  per owner decisions `OWNER-0717` and `OWNER-0801` (2026-11-27); lower values remain selectable down to 10 mA and higher values
  clamp to 400 mA. The driver stays default-off (`piano-torch=0`, opt-in only) with all temperature guards and read-backs enabled.
  `piano-torch-test=S` lights it once at boot for S seconds. `piano-torch-ladder=1` runs the 100/200/300/400 mA steps within
  the configured cap, with 10 s lit and 10 s dark per step. It samples PMIC status and flash, SoC and battery temperatures
  every 500 ms and stops with explicit OFF on a fault, an unreadable status sample or a SoC/battery thermal trip.
  `piano-torch-probe=1` with `piano-torch-test=S` selects T4, which runs a guarded S-second factory-pair window with
  STATUS1/2/3 and INT_RT_STS polls every 500 ms before, during and after, plus full-page dumps.
  `piano-torch-probe-spare=1` adds the spare-pair mapping window. `piano-torch-flash=1` adds one timed 50 mA / 80 ms FLASH pulse
  if the requested torch windows complete cleanly and the configured cap permits it. See the
  [attended RAM qualification checklist](../../docs/drivers/torch.md#attended-t4-ram-qualification-checklist).

Control from macOS: `sudo piano-torch on|off|toggle|status` (`tooling/native/piano-torch`, an IORegistry property the kext
accepts from an administrator only); from other drivers: `PianoTorchSet` / `PianoTorchToggle` / `PianoTorchStatus`
(`PianoTorchClient.h`). Design, facts, the console hook PianoDwc3 could add, the test plan: `docs/drivers/torch.md`.

| File | What |
|---|---|
| `PianoTorch.cpp` | the IOService on IOPlatformExpertDevice: the thread, the SPMI windows, the report, the control loop, the properties, setProperties, callPlatformFunction, sleep and shutdown |
| `TorchCore.h` | portable: the survey, the write allowlist, the masked writes with read-back, the on/off sequences, the guard, the state |
| `TorchSpmi.h` | portable: the flash APID's port (observer reads, owner-channel writes) as TorchCore's bus |
| `FlashHw.h` | the facts, each with its source; the current/ITARGET/clamp arithmetic |
| `PianoTorchClient.h` | the names other code uses (properties, platform functions) |
| `tests/torch_host.cpp` | host tests against an SPMI arbiter v7 model |
| `tests/torch_safety_bounds_host.cpp` | ASan/UBSan checks for the total current ceiling, deadlines, fault cutoffs and teardown readback |

Run both host suites with `python3 scripts/ci/self-tests.py --only package/PianoTorch/tests/`. The B12 suite exercises the
portable controller and guard. Its PMIC model simulates the diagnostic FLASH hardware timer. Ordinary torch mode uses the
software deadline. Display-only suspend and client disconnect are host-model events. The T4 probe and the guarded ladder are
host-verified. The lit RAM test remains on **HOLD** until the owner is beside the tablet to watch the LED.

Build: `tooling/k27/build-native-piano-torch.sh` (host tests, a store check of the portable headers, the user-space tool on a
Mac, the objects, the import check). PianoTorch is in `configs/k27-native-kexts.txt` and defaults to off (`piano-torch=0`). It
includes `package/PianoPower/SpmiObserver.h` (not copied). Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
