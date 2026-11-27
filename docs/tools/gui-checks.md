# Installed GUI checks

Build the arm64 probe on the Mac that owns the tablet's USB and C920 connections.

```sh
mkdir -p out/piano-gui-checks
bash tooling/native/piano-gui-checks/build.sh out/piano-gui-checks/gui
```

Source [the spec fragment](../../tooling/native/piano-gui-checks/gui-checks.spec) from the harness-approved actions file for a `tabletrun.sh` spec with `BOOT=installed`, `KIND=observe` and `REC_C920=0`. The harness owns the installed boot, lock, desk login and teardown. Its compiler binds `verify` steps to registered actions and fixed step lists. Register the GUI action and its step with the harness before submitting a spec. Use `desklogin` and a `shot desktop-reference` before the action. If the plan needs an optical gate, grade that still in the actions file. The harness's `lit` step requires continuous recording.

An actions file can wire `verify gui-checks` as follows. Set `GUI_SSID_FILE` and `GUI_REDACT` in the host actions environment. `GUI_REDACT` points to the harness's `redact-wifi.py`. Set `GUI_BIN` if the build lives outside `out/piano-gui-checks/gui`.

```bash
source tooling/native/piano-gui-checks/gui-checks.spec

verdict() { printf '%s\n' "$1" >> "$R/verdict.txt"; log "verdict: $1"; }
fexpect() { grep -Eq -- "$2" "$R/$1"; }

verify_action() {
    case "$1" in
        gui-checks) gui_checks_installed ;;
        *) return 2 ;;
    esac
}
```

Use a visible, previously saved Wi-Fi test network with DHCP and auto-join enabled. Keep the local UTF-8 fixture file outside the repository. It accepts one SSID line or a single `ssid=` entry. The Wi-Fi phase sends only the SSID on stdin to a mode-0600 file in the tablet's staging directory. It saves scalar receipts through the redactor. The fragment uses C920 stills only during brightness checks because Wi-Fi images could show network names. The desktop account must match the harness USB SSH account, and the tablet must use installed Data (`data-persist=2`).

## Battery Energy Mode picker

For a dedicated installed observation on v13.1y12q, register `verify gui-energy-picker` through the same harness action and desktop gates. Add this case to `verify_action`.

```bash
gui-energy-picker) gui_checks_installed energy-picker ;;
```

Use an unlocked English-language desktop and keep the power source steady. The opt-in action uses the same native build, staging checks and Accessibility probe. It needs neither Wi-Fi fixtures nor camera evidence.

The check reads the active `pmset -g` mode, both preferences from `pmset -g custom` and the original powerd and driver state. It opens System Settings directly to Battery. It finds the pop-up for the active power source and uses AX presses on the named menu items. The cycle visits Low Power, Automatic and High Power, starting after the initial mode so each selection proves an actual change.

Each selection must read back in AX and propagate within 15 seconds to `pmset`, both `notifyutil` states, both `machdep.piano_policy_*` flags, the PianoThermal profile in `machdep.piano_cpufreq_state` and the PianoA830 ceiling in `machdep.piano_gpu_state`. PianoPolicy's written ceiling and fresh GPU state confirm the 342 MHz Low Power limit and its release. The inactive power source's preference must stay at its original value. For Automatic, PianoThermal must report profile 0 for the default policy rule or profile 2 for `piano-policy-profile=2`.

The session journals both original preferences before opening Settings. On success, error or INT/TERM/HUP it quits Settings normally, restores either changed preference with source-specific `pmset` writes and verifies both preferences. The runner then reads back the full chain with a fresh driver sample. The harness exit handler retries cleanup under the same lock as the AX operations and retains the recovery journal if it cannot confirm cleanup.

The private capture is `gui-checks/energy-picker.json` under the harness results directory. When the picker or menu is missing or ambiguous, the receipt includes a bounded AX hierarchy with roles, labels and identifiers. Those labels can contain desktop account details. The pure-Python grader can replay the capture offline.

```sh
python3 tooling/native/piano-gui-checks/grade.py energy-picker "$R/gui-checks/energy-picker.json"
```

**PASS** (0) requires three genuine selections and verified cleanup. **FAIL** (1) means usable evidence shows a wrong selection, propagation mismatch or cleanup failure. **VOID** (2) means the action or its evidence could not establish a result, such as a missing picker, stale sample or empty capture. The host tests build the native helper and exercise the harness with mocked transport and AX receipts. A desktop run is still needed to validate the live Battery AX hierarchy and the installed picker.

## Results

In the harness results directory, read `verdict.txt` for the checks, `gui-checks/` for scalar evidence and `shot-brightness-*.jpg` for the C920 stills. The fragment returns 0 when every check passes, 1 for a graded failure, or 2 for a setup or cleanup failure. In harness dry mode it records an INFO verdict and returns 0 without contacting the tablet or requiring a build or secrets.

- **Wi-Fi row click.** The helper locates the menu-bar Wi-Fi extra owned by Control Center, opens the popover, inventories pressable rows and finds an exact SSID match. It expands Other Networks when necessary. The saved receipts confirm that the menu opened and the target row was found. If the boot already joined the target, the runner disassociates once to create a baseline. The helper checks again immediately before pressing the row. Within 30 seconds of the press, the driver must report the exact SSID on `en0` with active link and a DHCP ACK and lease matching the configured IPv4 address.
- **Wi-Fi toggle.** The same popover switch must cause GET19 to report off and the link to drop within 15 seconds. The runner holds off for at least five seconds, then presses the switch back on. Power, target association and a matching DHCP lease must return within 30 seconds. Both windows use tablet monotonic timestamps. The runner attempts to restore power through the menu if the cycle is interrupted.
- **Brightness via AX value.** The helper opens Control Center Display, reads the original slider level, sets `AXValue` to 30%, then 70%, then the original level. Every target must settle within ten seconds and still pass after its photo.
- **Brightness via simulated drag.** The runner reads the slider's AX bounds and thumb value and uses acknowledged pointer reports from `piano-input` to hold, move and release the left button. It uses Quartz cursor readings to correct for relative pointer acceleration. This path repeats the same targets and readbacks.

Both brightness grades require the AX slider and CoreBrightness's normalized `DisplayServicesGetBrightness` level to land within five percentage points. The `PianoBacklight` registry provides `BrightnessMilliNits` and the code and lit flags for both ICs. The chips must agree and be lit, and applied millinits must be within 5% of CoreBrightness's `DisplayNitsKey`. The hardware code is a physical control value, so the comparison uses nits. The C920 shot of the same center crop must rise by at least 3 YAVG units from 30% to 70% and change in the expected direction on return. If the original level lies within five points of 70%, the return shot is compared with the original shot.

## Accessibility

The fragment verifies the SHA-256 of both staged files and their root-owned installed copies. The remote session checks the boot UUID, console UID, Finder, GUI bootstrap and unlocked state before probing `AXIsProcessTrusted` and running GUI actions via `launchctl asuser` and `sudo -u`. If the probe needs a grant, the session inserts the SSH wrapper's Accessibility row only when the policy is absent, restarts the console user's `tccd`, and probes again. Cleanup matches the inserted row's ID, timestamp, requirement and grant attributes before deleting it and restarting `tccd`. The exit handler attempts both remote and staging cleanup and checks each receipt, including after setup or phase failures.

## Offline verification

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tooling/native/piano-gui-checks/tests -v
shellcheck -s bash tooling/native/piano-gui-checks/{build.sh,session.sh,gui-checks.spec}
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tooling/native/tests -v
PYTHONDONTWRITEBYTECODE=1 python3 tooling/native/test_piano_input.py -v
```

The panel and luminance fixtures contain scalar output from earlier brightness checks. The DHCP packet and GUI receipts use offline fixture data. The native DHCP parser test compiles the probe on arm64 macOS and runs against a file. The AX selectors, installed TCC attribution and simulated drag still require a run against the installed tablet.
