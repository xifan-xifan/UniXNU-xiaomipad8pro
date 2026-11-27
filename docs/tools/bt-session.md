# Bluetooth keyboard and mouse owner session

One session on the installed daily image, with about 20 minutes of owner time. Bring the same known LE keyboard that failed
if available, the owner's Bluetooth mouse, fresh batteries or charge, and the tablet's touch or keyboard cover for control.
Use each device's Bluetooth mode and a spare pairing channel if it has one. Disconnect or disable Bluetooth on other hosts
that might reclaim the devices. Keep each peripheral 20–30 cm from the tablet in a fixed position for its pairing attempt
and any retry. Use SSH over the debug USB cable so the capture survives a Wi-Fi toggle.

## Before the owner sits down

Use the installed v13.1y12q daily image or a later installed daily with the same Bluetooth components verified below. The
image needs the PianoBluetooth level-5 kernel path, the LE event-mask patch and the matching signed 26C5035r Path A runtime.
The image recipe supplies the runtime and its trust-cache entries through `PAD_USERLAND=1` and `PIANO_BT_RUNTIME`. Verify on the
running tablet from the Mac over the existing root SSH harness. Capture files belong under the main checkout's ignored
`private/` directory. [Daily status](../STATUS.md) · [Path A runtime](../../tooling/native/piano-bluetooth/README.md)

```bash
export BT_SESSION_SSH="$HOME/piano-harness/bin/tsudo"
export REDACT_WIFI="$HOME/piano-harness/bin/redact-wifi.py"
CAP=tooling/native/bt-session-capture.sh
GRADE=tooling/native/bt-preflight-grade.py

btroot() (set -o pipefail; "$BT_SESSION_SSH" --raw "$1" 2>&1 | python3 "$REDACT_WIFI")
```

With `piano-bt-delay=60`, PianoBluetooth starts bringup after a 60-second delay. The Path A startup job then replaces
the stock `bluetoothd` with the patched daemon. During that restart, launchd can briefly report `spawn scheduled`
and the LE mask counter can still be zero.

The read-only preflight polls for host readiness, a running `bluetoothd` with a PID, and a positive LE mask patch counter.
It waits up to 90 seconds by default and records the elapsed settle time and status in the redacted `preflight.txt`
before saving the full snapshot. Pass `--wait-seconds N` to change the poll budget, or `--wait-seconds 0` to take
an immediate snapshot. The full snapshot has its own SSH deadline.

```bash
BT_RESULTS="$PWD/private/bt-results"
mkdir -p "$BT_RESULTS"
PREFLIGHT=$(mktemp -d "$BT_RESULTS/preflight.XXXXXXXX")
"$CAP" preflight "$PREFLIGHT"
python3 "$GRADE" "$PREFLIGHT/preflight.txt"
```

If the LE mask line is missing from the unified log, pass the saved console log from the same boot as the grader's second
argument. For example, use `python3 "$GRADE" "$PREFLIGHT/preflight.txt" "$BOOT_CONSOLE"` with `BOOT_CONSOLE`
set to that log. The grader exits 0 for `READY`, 1 for `NOT-READY` (a failed check or settle timeout) and 2 for
`VOID` (incomplete capture or unreadable input). Confirm the boot UUID identifies the intended installed boot and
perform the UI check below.

| Check | Required result |
|---|---|
| Boot and effective options | macOS build `26C5035r`, the expected installed boot, `piano-bt=5` and USB debug networking. The full `kern.piano_bootargs` view includes the ADT tail. On the daily profile, `piano-bt-stop` is absent or zero, `machdep.piano_bt_lemask=1` and `machdep.piano_bt_lerdy=0`. The default transport is 3.2 Mbit/s with IBS disabled. |
| Kernel and firmware | `machdep.piano_bt_boot: 0`, a nonempty firmware build label, `"piano-bt-host-ready" = Yes`, a character device at `/dev/cu.BLTH`, a published `bluetooth` service and `IOBluetoothHCIController`. The ADT has transport encoding 1 and speed 3200000 (displayed as little-endian OSData). |
| LE mask applied | `piano-bt-h4-le-masks-patched` is positive. Look for `bt-lemask1: LE event mask 000000000000101f -> 000000000000185f` (or the same bits set on a different input mask). Use the saved boot console for this boot if the line has aged out of the unified log. |
| Userland active | All four runtime hashes pass, staged and mounted copies compare equal, both bindfs views are read-only, all helper paths exist, and launchd has the three jobs. `bluetoothd` is running with a PID. The startup job and BlueTool can exit after completing startup. |
| Controller on | The status line contains `default_object=1 power=1` and System Profiler reports Bluetooth on. Confirm the owner can open the Bluetooth pane and a blank TextEdit document using touch or the cover. |

The preflight `@@ trace` section records the HCI trace preference and file listing. Confirm a fresh or growing file while
the Bluetooth pane is open before relying on it for packet parameters. If tracing is absent, arm the same harness configuration
before the owner arrives. Save the prior preference domain for restoration. This optional preparation restarts bluetoothd once.

```bash
trace_backup=$("$BT_SESSION_SSH" --raw "$(cat <<'TABLET'
set -e
domain=com.apple.MobileBluetooth.debug
backup=$(/usr/bin/mktemp -d /private/var/tmp/bt-session-trace.XXXXXXXX)
printf '%s\n' "$backup"
if /usr/bin/defaults read "$domain" >/dev/null 2>&1; then
    /usr/bin/defaults export "$domain" "$backup/original.plist"
    /usr/bin/touch "$backup/had-domain"
fi
/usr/bin/defaults write "$domain" HCITraces -dict HCIFileTraces -bool true StackDebugEnabled -bool true MaxFileSize -int 50 MaxTraces -int 4
/usr/bin/defaults write "$domain" HCIFileTraces -bool true
/bin/launchctl kickstart -k system/com.apple.bluetoothd
TABLET
)")
```

If you enabled tracing, rerun preflight and the grader, then check for a growing trace. Preserve `preflight.txt` and
defer the owner session if a readiness gate fails. If only the trace is missing, proceed with the UI and counter observations.
The log and registry can still locate several failure boundaries.

```bash
RUN=$("$CAP" start "$BT_RESULTS")
cp "$PREFLIGHT/preflight.txt" "$RUN/preflight.txt"
printf 'Bluetooth results: %s\n' "$RUN"
m() { "$CAP" mark "$RUN" "$@"; }
s() { "$CAP" sample "$RUN" "$1"; }
```

## The 20-minute owner script

The main session enters the capture commands on the Mac. For each attempt, sample before the owner enters pairing mode.
Mark `*-pairing` as the pairing LED starts and `*-connect` just before the owner clicks Connect. Use `*-outcome` with the
visible result, then allow six seconds for the driver's five-second counter refresh before sampling after. In the note, record
the model or device role, LED behavior, UI state and input outcome. Keep network names, peer addresses and passkeys out of notes.
Record the initial Wi-Fi setting. If it was off, turn it on for the baseline and mark `wifi-on 'initially off'`.

| Time | Owner actions and what success looks like | Main session capture |
|---|---|---|
| **0–5 min · LE keyboard, Wi-Fi on** | Start with the known LE keyboard and Bluetooth Settings open. Select the intended channel and put it in pairing mode. Allow up to 60 seconds for its row to appear. Click **Connect** once and allow up to 60 seconds for the result. If macOS displays a pairing code, type it on the Bluetooth keyboard and press Return. Success is **Connected** plus `bt keyboard ok` appearing in TextEdit, typed on that keyboard. | `s keyboard-on-before`, `m keyboard-on-pairing`, `m keyboard-on-connect` if the row appeared, `m keyboard-on-outcome 'connected and typed'` or the actual timeout, missing row, prompt or error, then `s keyboard-on-after`. This is the primary test of the corrected LE mask. |
| **5–9 min · Mouse, Wi-Fi on** | Put the mouse into Bluetooth pairing mode and select its Bluetooth channel. Allow up to 60 seconds to appear and 60 seconds after a single Connect click. Success is **Connected**, pointer movement from the mouse, a click that moves the TextEdit caret, and visible scrolling in Settings or a document long enough to scroll. | `s mouse-on-before`, `m mouse-on-pairing`, `m mouse-on-connect` if visible, `m mouse-on-outcome 'connected; move click scroll passed'` or the actual result, then `s mouse-on-after`. Record whether its transport is known LE or Classic. |
| **9–15 min · Matched retry if either device failed** | In Control Center turn **Wi-Fi Off**, keeping **Bluetooth On**. Wait 15 seconds and confirm Wi-Fi shows Off. Retry the failed LE keyboard first if it failed, otherwise the failed mouse. Keep the same device, distance and channel. Re-enter pairing mode and repeat the same timing and UI checks once. If the earlier attempt left a paired entry, use it for the reconnect and record that this is a reconnect comparison. If both devices worked on Wi-Fi, leave Wi-Fi on and use this time for the next step. | `m wifi-off` after checking the control, then for a keyboard `s keyboard-off-before`, `m keyboard-off-pairing`, `m keyboard-off-connect` when applicable, `m keyboard-off-outcome '…'`, `s keyboard-off-after`. For a mouse use the same labels starting with `mouse-off-`. Compare the before/after counter increments with those from its Wi-Fi-on attempt. |
| **15–18 min · Reconnect and confirm input** | Restore Wi-Fi to its initial state. For each device that paired, turn the peripheral off for 10 seconds and back on, one at a time. Allow up to 30 seconds for reconnect. Verify **Connected** again and repeat its input check. Devices that never paired stay a recorded failure. | `m wifi-restored` if changed, `s reconnect-before`, `m keyboard-reconnect '…'` and/or `m mouse-reconnect '…'`, then `s reconnect-after`. Record a pass for each device only when its input works. Ordinary peripheral reconnect does not test keys across a reboot. |
| **18–20 min · Close** | Read back which devices appeared, connected, accepted input and reconnected. Leave a successful pair in place for the owner. | `m owner-end 'keyboard …, mouse …'` then `"$CAP" finish "$RUN"`. On loss of UI or SSH, save the last visible outcome and run finish as soon as the link returns. The owner can leave while the host pulls logs. |

If the owner has no known LE keyboard, run the same first step with the available keyboard and record its transport as unknown
until the trace establishes it. Report the two devices separately. A Bluetooth row or an HCI link alone establishes only partial progress.

## Read the capture

- `markers.tsv` has host UTC, tablet UTC and tablet epoch per action. `before.txt` and `after.txt` include
  `SPBluetoothDataType`, HCI controller and transport registry entries, PianoBt state and relevant sysctls.
  Each `*.counters.txt` holds the per-attempt registry counters, Wi-Fi power and daemon PID. Compare only samples from the
  same boot, and use the counter differences over the matched windows.
- `bluetoothd.log` is `log show --start` from the tablet's session-start event through session-end. If the start timestamp
  is unavailable, the first confirmed tablet marker after it is used. Find the pairing interval
  using `*-pairing`, `*-connect` and `*-outcome`. Find the corresponding handle and its events in `hci.txt`. That file records
  PacketLogger event counts by code and LE subevent per file, selected command and event timestamps, requested and negotiated
  interval/latency/supervision timeout, length, PHY, and initial ATT/SMP activity. It contains field summaries without packet
  bytes. [Bluetooth Core Specification, HCI command and event parameters](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-60/out/en/host-controller-interface/host-controller-interface-functional-specification.html)
- Read `status.tsv` and any `command_exit=` or `partial_trace=` lines. Empty or incomplete trace coverage means unknown HCI
  evidence. Recorded HCI zero counts and registry deltas from an uninterrupted driver are distinct measurements. If the
  lock or USB connection interrupts finish, run finish again against the same folder to retry the final reads.
- The host pipes text through the existing `redact-wifi.py` stdin/stdout helper before saving it. Keep the folder under
  `private/` because Bluetooth device names and daemon details may remain. Report the device roles and outcomes, clock
  or capture gaps, and the relevant counter changes in any shared summary.

If the preparation enabled tracing, restore its previous state **after** finish. Leave existing tracing alone when it was
already active. Keep the backup path from the preparation in the same Mac shell. The restored preference applies on the
next normal daemon start. Keep the current daemon running to preserve any session-local pairing state.

```bash
if [[ ${trace_backup:-} =~ ^/private/var/tmp/bt-session-trace\.[A-Za-z0-9]+$ ]]; then
    btroot "backup='$trace_backup'
domain=com.apple.MobileBluetooth.debug
if [ -f \"\$backup/had-domain\" ]; then
    /usr/bin/defaults import \"\$domain\" \"\$backup/original.plist\" || exit 1
else
    /usr/bin/defaults delete \"\$domain\" || exit 1
fi
/bin/rm -f \"\$backup/original.plist\" \"\$backup/had-domain\"
/bin/rmdir \"\$backup\"" > "$RUN/trace-restore.txt"
fi
```

## Read-only analysis before the session

On November 20, LE Connection Complete succeeded and bluetoothd read the peer version and features. It then waited in
`LE_READY_WAIT_FOR_LENGTH` until a supervision timeout (HCI reason `0x08`) or a local cancel. GATT, SMP and HID had yet to start.
The November 26 installed results confirmed controller startup and the LE event-mask change. The change makes Data Length Change
and PHY Update Complete visible to bluetoothd when the controller emits them. The first keyboard attempt should test that change
on the daily configuration. [Driver notes §8.5](../drivers/bluetooth.md#85-le-link-ready-2026-11-21-host-tested-tablet-result-pending)
· [November 26 results](../results-2026-11-26.md#also-on-the-daily-line)

The November 22 trace gives a second lead. About 90% of LE reports were at or below −101 dBm, stronger reports clustered near Wi-Fi
scans, and 121 Classic inquiry completions yielded zero results. The shared 2.4 GHz front end is a hypothesis. A matched retry
with the same peripheral, pairing channel and position is the closest comparison available in this session.
[Driver notes §8.6](../drivers/bluetooth.md#86-radio-health-bt-radio1-2026-11-26-host-tested-tablet-result-pending)

| Hypothesis | Evidence to look for in the marked attempt |
|---|---|
| The keyboard stopped advertising, was on the wrong channel, or reconnected elsewhere | Owner confirms the pairing LED and isolates other hosts. A missing row puts the failure in discovery. A visible row and a marked click with no LE Create Connection point to UI or daemon initiation. Aggregate RSSI alone cannot identify the peer. |
| Wi-Fi activity is starving Bluetooth, or the Bluetooth RF path is weak | Compare `piano-bt-radio-le-rssi`, `piano-bt-radio-le-reports`, `piano-bt-radio-classic-results` and `piano-bt-radio-inquiries` deltas with Wi-Fi on and off at the same distance. A stronger histogram plus a successful link off Wi-Fi supports coexistence. The same weak pattern in both windows leaves antenna, controller RF and peer behavior open. |
| Controller initiation or first connection events fail | Find LE Create Connection or Extended Create Connection, its Command Status, then LE Connection Complete. A rejected command, no completion, a nonzero completion such as `0x3e`, and a successful completion followed by Disconnection Complete reason `0x08` are different boundaries. Check requested and negotiated interval, latency and supervision timeout before attributing `0x08` to the link-ready wait. |
| The mask change is missing after daemon startup or reset | Look for a positive `piano-bt-h4-le-masks-patched` counter and the boot console's `LE event mask ...101f -> ...185f` (or another mask with bits 6 and 11 set). When the trace includes startup or reset, check HCI `0x2001` Command Complete status `0x00`. A host-side trace records the command before PianoBt rewrites it. |
| Length or PHY procedure is absent, or bluetoothd stays in link-ready despite events | For the successful connection handle, compare LE Remote Features Complete, Data Length Change (`0x07`), PHY Update Complete (`0x0c`), daemon `LE_READY_WAIT_FOR_LENGTH`/ready messages, and the first ATT/SMP traffic. The optional link-ready assist has separate `piano-bt-h4-lerdy-injected-*` counters. A missing controller event with the mask set can also mean the peer never negotiated a change. |
| Transport corruption or a controller/daemon reset interrupted the attempt | Inspect `piano-bt-host-ready`, H4 backpressure/resync/oversized counters, console `TX-timeouts` or `H4 TX failed`, and the daemon PID or HCI Reset. A counter drop is a discontinuity. |
| The link is ready but security or HID fails | First ATT/SMP traffic moves the diagnosis past the earlier stall. Check Encryption Change, daemon pairing/security/key-store errors, then HID publication and the actual typing, clicking and scrolling checks. A mouse or keyboard using Classic has its own inquiry, Connection Complete and HID path. |
