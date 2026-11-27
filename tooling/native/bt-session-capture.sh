#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Run on the Mac. BT_SESSION_SSH must implement the harness tsudo --raw 'command' interface.
set -euo pipefail
umask 077
export LC_ALL=C

usage() {
    cat <<'EOF'
usage: bt-session-capture.sh preflight RESULTS_DIR [--wait-seconds N]
       bt-session-capture.sh start RESULTS_PARENT
       bt-session-capture.sh mark RUN LABEL [NOTE]
       bt-session-capture.sh sample RUN LABEL
       bt-session-capture.sh finish RUN
Set BT_SESSION_SSH and REDACT_WIFI to override the host harness paths.
Preflight waits up to 90 seconds by default. Use 0 to take a snapshot without polling.
EOF
}

die() { echo "bt-session-capture: $*" >&2; exit 1; }
[[ ${1:-} != --help ]] || { usage; exit 0; }
case "${1:-}:$#" in
    preflight:2|start:2|mark:3|mark:4|sample:3|finish:2) ;;
    preflight:4)
        [[ $3 == --wait-seconds && $4 =~ ^[0-9]{1,9}$ ]] || { usage >&2; exit 2; }
        ;;
    *) usage >&2; exit 2 ;;
esac

ssh_helper=${BT_SESSION_SSH:-$HOME/piano-harness/bin/tsudo}
redactor=${REDACT_WIFI:-$HOME/piano-harness/bin/redact-wifi.py}
[[ -x $ssh_helper ]] || die "missing executable SSH helper: $ssh_helper"
[[ -r $redactor ]] || die "missing Wi-Fi redactor: $redactor"
python3 "$redactor" </dev/null >/dev/null || die "Wi-Fi redactor failed"

remote() {
    # Bound the whole SSH process tree, including a stalled relay or a child retaining the pipe.
    python3 -c '
import os, signal, subprocess, sys
child = subprocess.Popen(sys.argv[2:], stdin=subprocess.DEVNULL, start_new_session=True)
try:
    sys.exit(child.wait(timeout=int(sys.argv[1])))
except subprocess.TimeoutExpired:
    print("SSH capture timed out", file=sys.stderr)
    os.killpg(child.pid, signal.SIGKILL)
    child.wait()
    sys.exit(124)
except KeyboardInterrupt:
    os.killpg(child.pid, signal.SIGKILL)
    child.wait()
    sys.exit(130)
' "$1" "$ssh_helper" --raw "$2"
}

redact() {
    # IORegistry represents some addresses as OSData; the harness helper handles textual addresses.
    sed -E '/"(local-mac-address|mac-address-bluetooth0|ControllerAddress|DeviceAddress|BD_ADDR)"[[:space:]]*=/s/= .*/= <redacted>/' |
        python3 "$redactor"
}

preflight_reads=$(cat <<'REMOTE'
export LC_ALL=C
set +e
piano_status() {
    /usr/sbin/ioreg -r -c PianoBluetooth -d 1 -l -w0 |
        /usr/bin/grep -E 'piano-bt-(state|host-ready|h4-le-masks|radio-le-rssi)'
}
launchd_status() {
    # launchctl indents the job fields with one tab and nested fields with more.
    /bin/launchctl print "system/$1" |
        /usr/bin/awk '/^system\/[^ ]+ = \{$/ || /^\t(state|pid|program|last exit code) = /'
}
REMOTE
)

preflight_snapshot=$(cat <<'REMOTE'
printf '%s\n' '@@ sw_vers'
/usr/bin/sw_vers
printf '%s\n' '@@ boot'
/usr/sbin/sysctl kern.bootsessionuuid
printf '%s\n' '@@ bootargs'
/usr/sbin/sysctl -n kern.piano_bootargs | /usr/bin/tr ' ' '\n' |
    /usr/bin/grep -E '^piano-(bt(=|-)|usb-net=|wifi(=|-ps=))'
printf '%s\n' '@@ sysctl'
/usr/sbin/sysctl machdep.piano_bt_boot machdep.piano_bt_firmware machdep.piano_bt_lemask machdep.piano_bt_lerdy
printf '%s\n' '@@ dev'
/bin/ls -l /dev/cu.BLTH
printf '%s\n' '@@ pianobt'
piano_status
printf '%s\n' '@@ hci'
/usr/sbin/ioreg -r -c IOBluetoothHCIController -d 1 -w0
printf '%s\n' '@@ service'
/usr/sbin/ioreg -r -n bluetooth -d 1 -l -w0
printf '%s\n' '@@ adt'
/usr/sbin/ioreg -p IODeviceTree -r -n bluetooth -d 1 -l -w0 |
    /usr/bin/grep -E 'transport-(encoding|speed)'
printf '%s\n' '@@ runtime'
(
    cd /private/var/piano/bluetooth-path1 &&
    /usr/bin/shasum -a 256 -c runtime.sha256 &&
    /usr/bin/cmp bin/bluetoothd /usr/sbin/bluetoothd &&
    /usr/bin/cmp bin/BlueTool /usr/sbin/BlueTool &&
    /usr/bin/cmp bin/PianoBluetooth.runtime /usr/sbin/PianoBluetooth.runtime &&
    /usr/bin/cmp lib/QcaBTType0.dylib /usr/lib/QcaBTType0.dylib &&
    printf '%s\n' 'staged and mounted runtime match'
)
printf '%s\n' '@@ helpers'
/bin/ls -l /usr/local/libexec/piano-bluetooth-start /usr/local/libexec/piano-bt-runtime-mount \
    /usr/local/libexec/piano-bt-probe /Library/LaunchDaemons/com.unixnu.piano-bluetooth-path1.plist
printf '%s\n' '@@ mount'
/sbin/mount | /usr/bin/grep -E 'on /usr/(sbin|lib) '
printf '%s\n' '@@ launchd'
for job in com.unixnu.piano-bluetooth-path1 com.apple.BlueTool com.apple.bluetoothd; do
    launchd_status "$job"
done
printf '%s\n' '@@ probe'
/usr/local/libexec/piano-bt-probe --status
printf '%s\n' '@@ profiler'
/usr/sbin/system_profiler -timeout 12 SPBluetoothDataType
printf '%s\n' '@@ lemask-log'
/usr/bin/log show --last boot --style compact --info --no-pager \
    --predicate 'process == "kernel" AND eventMessage CONTAINS "bt-lemask1: LE event mask"'
printf '%s\n' '@@ trace'
/usr/bin/defaults read com.apple.MobileBluetooth.debug
/bin/ls -lt /private/var/root/Library/Logs/Bluetooth/
printf '%s\n' '@@ end' BT_PREFLIGHT_DONE
REMOTE
)

capture_preflight() {
    local results=$1 wait_seconds=$2 remaining deadline sample elapsed stage
    local settle=skipped
    local -a rc=(0 0)
    mkdir -p -- "$results"

    SECONDS=0
    if (( wait_seconds > 0 )); then
        settle=timeout
        while (( (remaining = wait_seconds - SECONDS) > 0 )); do
            deadline=$(( remaining < 5 ? remaining : 5 ))
            if sample=$(remote "$deadline" "$preflight_reads"$'\n''piano_status && launchd_status com.apple.bluetoothd' 2>/dev/null) &&
                grep -Eq '^[[:blank:]]*"piano-bt-host-ready" = Yes$' <<< "$sample" &&
                grep -Eq '^[[:blank:]]*"piano-bt-h4-le-masks-patched" = [0-9]*[1-9][0-9]*$' <<< "$sample" &&
                grep -Eq $'^\tstate = running$' <<< "$sample" &&
                grep -Eq $'^\tpid = [1-9][0-9]*$' <<< "$sample" &&
                (( SECONDS < wait_seconds )); then
                settle=ready
                break
            fi
            (( SECONDS < wait_seconds )) || break
            sleep 1
        done
    fi
    elapsed=$SECONDS
    stage=$(mktemp "$results/.preflight.XXXXXXXX")
    printf 'BT_PREFLIGHT_SETTLE status=%s elapsed_seconds=%s wait_seconds=%s\n' \
        "$settle" "$elapsed" "$wait_seconds" > "$stage"

    remote 75 "$preflight_reads"$'\n'"$preflight_snapshot" 2>&1 | redact >> "$stage" || rc=( "${PIPESTATUS[@]}" )
    if (( rc[1] != 0 )); then
        printf 'BT_PREFLIGHT_CAPTURE_ERROR redactor_exit=%s\n' "${rc[1]}" > "$stage"
    elif (( rc[0] != 0 )); then
        printf '\nBT_PREFLIGHT_CAPTURE_ERROR remote_exit=%s\n' "${rc[0]}" >> "$stage"
    fi
    mv -f -- "$stage" "$results/preflight.txt"
    (( rc[0] == 0 && rc[1] == 0 )) || die "preflight capture failed (SSH=${rc[0]}, redactor=${rc[1]})"
}

record_status() {
    printf '%s\t%s\n' "$1" "$2" >> "$run/status.tsv"
    [[ $2 == ok ]] || printf 'bt-session-capture: %s %s\n' "$1" "$2" >&2
}

save_text() {
    local name=$1 deadline=$2 command=$3 stage
    local -a rc=(0 0)
    stage=$(mktemp "$run/.text.XXXXXXXX")
    remote "$deadline" "$command" 2>&1 | redact > "$stage" || rc=( "${PIPESTATUS[@]}" )
    if (( rc[1] != 0 )); then
        rm -f -- "$stage"
        record_status "$name" "redactor_error=${rc[1]}"
        return 1
    fi
    mv -f -- "$stage" "$run/$name"
    if (( rc[0] != 0 )); then
        record_status "$name" "remote_error=${rc[0]}"
        return 1
    fi
    record_status "$name" ok
}

clock_read='/bin/date -u "+%s %Y-%m-%d %H:%M:%S+0000"'
marker() {
    local label=$1 note=${2:-} host_time stamp epoch time
    host_time=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
    if stamp=$(remote 15 "$clock_read" 2>/dev/null) &&
        [[ $stamp =~ ^([0-9]{10})\ ([0-9]{4}-[0-9]{2}-[0-9]{2}\ [0-9]{2}:[0-9]{2}:[0-9]{2}\+0000)$ ]]; then
        epoch=${BASH_REMATCH[1]}
        time=${BASH_REMATCH[2]}
    else
        epoch=unavailable
        time=unavailable
        record_status "$label.clock" "tablet_time_unavailable"
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$label" "$host_time" "$epoch" "$time" "$note" |
        redact >> "$run/markers.tsv"
    printf '%s %s (tablet %s)\n' "$host_time" "$label" "$time" >&2
    [[ $epoch != unavailable ]]
}

# Explicit reads only. Reading machdep.piano_bt_power_on would ask BlueTool's transport to power on.
counts=$(cat <<'REMOTE'
section() {
    printf '\n== %s ==\n' "$*"
    "$@"
    rc=$?
    if [ "$rc" -ne 0 ]; then printf 'command_exit=%s\n' "$rc"; fi
}
export LC_ALL=C
section /bin/date -u
section /usr/sbin/sysctl kern.bootsessionuuid machdep.piano_bt_firmware machdep.piano_bt_lemask machdep.piano_bt_lerdy
section /usr/sbin/ioreg -r -c PianoBluetooth -d 1 -l -w0
section /usr/bin/pgrep -x bluetoothd
wifi=$(/usr/sbin/networksetup -listallhardwareports | /usr/bin/awk '/^Hardware Port: Wi-Fi$/ { getline; print $2; exit }')
wifi_power=unavailable
if [ -n "$wifi" ]; then
    wifi_power=$(/usr/sbin/networksetup -getairportpower "$wifi" | /usr/bin/awk -F ': ' 'END { print ($NF ~ /^(On|Off)$/ ? $NF : "unavailable") }')
fi
printf 'wifi_power=%s\n' "$wifi_power"
section /usr/sbin/sysctl machdep.piano_wifi_ps
REMOTE
)
snapshot=$(cat <<'REMOTE'
section /usr/bin/sw_vers
args=$(/usr/sbin/sysctl -n kern.piano_bootargs 2>/dev/null || /usr/sbin/sysctl -n kern.bootargs)
printf '\n== Bluetooth and radio boot arguments ==\n'
printf '%s\n' "$args" | /usr/bin/tr ' ' '\n' | /usr/bin/grep -E '^piano-(bt(=|-)|usb-net=|wifi(=|-ps=))'
section /usr/sbin/sysctl machdep.piano_bt_boot
if /usr/sbin/sysctl -N machdep.piano_bt_uptime_ms >/dev/null 2>&1; then
    section /usr/sbin/sysctl machdep.piano_bt_uptime_ms
fi
section /usr/sbin/ioreg -r -c IOBluetoothHCIController -d 1 -l -w0
section /usr/sbin/ioreg -r -n bluetooth -d 1 -l -w0
section /usr/sbin/ioreg -p IODeviceTree -r -n bluetooth -d 1 -l -w0
section /bin/ls -l /dev/cu.BLTH
section /usr/sbin/system_profiler -timeout 12 SPBluetoothDataType
section /usr/bin/hidutil list --matching '{"Transport":"Bluetooth"}'
section /usr/bin/hidutil list --matching '{"Transport":"Bluetooth Low Energy"}'
REMOTE
)

decode_hci() {
    # PacketLogger records: big-endian length (excludes itself), seconds, microseconds, type.
    # Keep only fields needed to diagnose the link. Never emit raw packets or daemon trace text.
    python3 - "$@" 3<&0 <<'PY'
import collections
import datetime
import struct
import sys

start, end = map(int, sys.argv[1:])
counts = collections.Counter()
seen_acl = set()
commands = {
    0x0401: "Inquiry", 0x0402: "Inquiry Cancel", 0x0405: "Classic Create Connection",
    0x0406: "Disconnect", 0x0C03: "Reset", 0x2001: "LE Set Event Mask",
    0x200D: "LE Create Connection", 0x200E: "LE Create Connection Cancel",
    0x2013: "LE Connection Update", 0x2016: "LE Read Remote Features",
    0x2019: "LE Enable Encryption", 0x2022: "LE Set Data Length",
    0x2032: "LE Set PHY", 0x2043: "LE Extended Create Connection",
    0x2085: "LE Extended Create Connection v2",
}


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def handle(data, offset):
    return f"handle=0x{u16(data, offset) & 0xfff:03x}"


def requested(data, offset):
    low, high, latency, timeout = struct.unpack_from("<4H", data, offset)
    return f"interval_min_ms={low * 1.25:g} interval_max_ms={high * 1.25:g} latency={latency} supervision_timeout_ms={timeout * 10}"


def negotiated(data, offset):
    interval, latency, timeout = struct.unpack_from("<3H", data, offset)
    return f"interval_ms={interval * 1.25:g} latency={latency} supervision_timeout_ms={timeout * 10}"


def command(data):
    opcode = u16(data, 0)
    if opcode not in commands:
        return
    prefix = f"CMD 0x{opcode:04x} {commands[opcode]}"
    p = data[3:]
    if opcode == 0x2001 and len(p) == 8:
        yield f"{prefix} host_mask=0x{int.from_bytes(p, 'little'):016x}"
    elif opcode == 0x200D and len(p) >= 25:
        yield f"{prefix} {requested(p, 13)}"
    elif opcode in (0x2043, 0x2085) and len(p) >= (12 if opcode == 0x2085 else 10):
        offset = 12 if opcode == 0x2085 else 10
        phys = p[offset - 1]
        yield f"{prefix} initiating_phys=0x{phys:02x}"
        for bit in range(8):
            if phys & (1 << bit):
                if len(p) >= offset + 16:
                    yield f"  phy_bit={bit} {requested(p, offset + 4)}"
                offset += 16
    elif opcode == 0x2013 and len(p) >= 14:
        yield f"{prefix} {handle(p, 0)} {requested(p, 2)}"
    elif opcode == 0x0406 and len(p) >= 3:
        yield f"{prefix} {handle(p, 0)} reason=0x{p[2]:02x}"
    else:
        yield prefix


def event(code, p):
    if code in (0x0E, 0x0F) and len(p) >= 4:
        opcode = u16(p, 1 if code == 0x0E else 2)
        if opcode in commands:
            kind, status = ("Command Complete", p[3]) if code == 0x0E else ("Command Status", p[0])
            return f"{kind} 0x{opcode:04x} {commands[opcode]} status=0x{status:02x}"
    if code == 0x01 and p:
        return f"Inquiry Complete status=0x{p[0]:02x}"
    if code == 0x03 and len(p) >= 3:
        return f"Classic Connection Complete status=0x{p[0]:02x} {handle(p, 1)}"
    if code == 0x05 and len(p) >= 4:
        return f"Disconnection Complete status=0x{p[0]:02x} {handle(p, 1)} reason=0x{p[3]:02x}"
    if code in (0x06, 0x08, 0x0C, 0x30, 0x36, 0x59) and len(p) >= 3:
        names = {0x06: "Authentication Complete", 0x08: "Encryption Change",
                 0x0C: "Remote Version Complete", 0x30: "Encryption Key Refresh",
                 0x36: "Simple Pairing Complete", 0x59: "Encryption Change v2"}
        # Simple Pairing Complete contains an address at this offset, not a handle.
        link = "" if code == 0x36 else f" {handle(p, 1)}"
        enabled = f" enabled=0x{p[3]:02x}" if code in (0x08, 0x59) and len(p) >= 4 else ""
        return f"{names[code]} status=0x{p[0]:02x}{link}{enabled}"
    if code == 0x10 and p:
        return f"Hardware Error code=0x{p[0]:02x}"
    if code != 0x3E or not p:
        return
    sub = p[0]
    if sub in (0x01, 0x0A, 0x29) and len(p) >= 4:
        prefix = f"LE Connection Complete subevent=0x{sub:02x} status=0x{p[1]:02x}"
        offset = 12 if sub == 0x01 else 24
        if p[1] == 0 and len(p) >= offset + 6:
            return f"{prefix} {handle(p, 2)} role={p[4]} {negotiated(p, offset)}"
        return prefix
    if sub == 0x03 and len(p) >= 10:
        return f"LE Connection Update Complete status=0x{p[1]:02x} {handle(p, 2)} {negotiated(p, 4)}"
    if sub == 0x04 and len(p) >= 12:
        return (f"LE Remote Features Complete status=0x{p[1]:02x} {handle(p, 2)} "
                f"dle={int(bool(p[4] & 0x20))} phy_2m={int(bool(p[5] & 1))}")
    if sub == 0x07 and len(p) >= 11:
        tx_octets, tx_us, rx_octets, rx_us = struct.unpack_from("<4H", p, 3)
        return (f"LE Data Length Change {handle(p, 1)} tx_octets={tx_octets} tx_us={tx_us} "
                f"rx_octets={rx_octets} rx_us={rx_us}")
    if sub == 0x0C and len(p) >= 6:
        return f"LE PHY Update Complete status=0x{p[1]:02x} {handle(p, 2)} tx_phy={p[4]} rx_phy={p[5]}"
    if sub == 0x06 and len(p) >= 11:
        return f"LE Remote Connection Parameter Request {handle(p, 1)} {requested(p, 3)}"


def acl(kind, data):
    # A start fragment has an L2CAP header. Only report the first ATT/SMP packet per
    # direction and handle within this file, even if the payload spans fragments.
    if len(data) < 9 or (u16(data, 0) & 0x3000) == 0x1000:
        return
    proto = {4: "ATT", 6: "LE SMP"}.get(u16(data, 6))
    if proto:
        key = (kind, handle(data, 0), proto)
        if key not in seen_acl:
            seen_acl.add(key)
            return f'first {proto} direction={"host-to-controller" if kind == 2 else "controller-to-host"} {key[1]}'


first = last = None
issue = None
with open(3, "rb") as trace:
    while header := trace.read(13):
        if len(header) != 13:
            issue = "trailing partial PacketLogger header"
            break
        length, seconds, usec, kind = struct.unpack(">IIIB", header)
        if not 9 <= length <= 4 * 1024 * 1024 or usec >= 1000000:
            issue = "invalid PacketLogger record header"
            break
        data = trace.read(length - 9)
        if len(data) != length - 9:
            issue = "trailing partial PacketLogger record"
            break
        if not start <= seconds <= end:
            continue
        first = seconds if first is None else min(first, seconds)
        last = seconds if last is None else max(last, seconds)
        date = datetime.datetime.fromtimestamp(seconds, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
        timestamp = f"{date}.{usec:06d}Z"
        if kind == 0 and len(data) >= 3 and data[2] == len(data) - 3:
            for line in command(data):
                print(timestamp, line)
        elif kind == 1 and len(data) >= 2 and data[1] == len(data) - 2:
            code, p = data[0], data[2:]
            key = f"0x{code:02x}" + (f"/0x{p[0]:02x}" if code == 0x3E and p else "")
            counts[key] += 1
            detail = event(code, p)
            if detail:
                print(timestamp, f"EVT {key}", detail)
            if code == 0x05 and len(p) >= 4:
                seen_acl = {key for key in seen_acl if key[1] != handle(p, 1)}
        elif kind in (2, 3):
            detail = acl(kind, data)
            if detail:
                print(timestamp, detail)

if first is not None:
    print(f"record_window_utc_epoch={first}..{last}")
print(f"hci_event_count={sum(counts.values())}")
for key, count in sorted(counts.items()):
    print(f"hci_event={key} count={count}")
if issue:
    print(f"partial_trace={issue}")
    sys.exit(3)
if not counts:
    print("coverage=no HCI event records in the marked window")
    sys.exit(4)
PY
}

capture_hci() {
    local paths trace index=0 found=0 stage
    local -a rc
    local list_traces
    list_traces=$(cat <<'REMOTE'
        for f in /private/var/root/Library/Logs/Bluetooth/*.pklg; do
            [ -f "$f" ] && [ ! -L "$f" ] || continue
            modified=$(/usr/bin/stat -f %m "$f") || continue
            [ "$modified" -ge "$start" ] && printf "%s\n" "${f##*/}"
        done
        exit 0
REMOTE
)
    paths=$(remote 20 "start=$start_epoch"$'\n'"$list_traces" 2>/dev/null) ||
        { record_status hci "trace_listing_failed"; return 0; }
    : > "$run/hci.txt"
    while IFS= read -r trace; do
        [[ $trace =~ ^[a-zA-Z0-9_.-]+\.pklg$ ]] || continue
        index=$((index + 1))
        stage=$(mktemp "$run/.hci.XXXXXXXX")
        rc=(0 0 0)
        remote 75 "/bin/cat '/private/var/root/Library/Logs/Bluetooth/$trace'" 2>/dev/null |
            decode_hci "$start_epoch" "$end_epoch" | redact > "$stage" || rc=( "${PIPESTATUS[@]}" )
        if (( rc[2] == 0 )); then
            printf '\n== HCI trace %s ==\n' "$index" >> "$run/hci.txt"
            cat "$stage" >> "$run/hci.txt"
        fi
        rm -f -- "$stage"
        if (( rc[0] || rc[2] || (rc[1] != 0 && rc[1] != 4) )); then
            record_status "hci.$index" "remote=${rc[0]} decoder=${rc[1]} redactor=${rc[2]}"
        elif (( rc[1] == 0 )); then
            found=1
            record_status "hci.$index" ok
        fi
    done <<< "$paths"
    if (( ! found )); then
        printf '\ncoverage=no complete HCI trace with events in the marked window\n' >> "$run/hci.txt"
        record_status hci "no_complete_trace_with_events"
    fi
}

case "$1" in
    preflight)
        capture_preflight "$2" "$((10#${4:-90}))"
        ;;
    start)
        mkdir -p -- "$2"
        run=$(mktemp -d "$2/bt-session-$(date -u '+%Y%m%dT%H%M%SZ').XXXXXXXX")
        printf 'label\thost_utc\ttablet_epoch\ttablet_utc\tnote\n' > "$run/markers.tsv"
        printf 'capture\tstatus\n' > "$run/status.tsv"
        echo "$run"
        marker session-start || :
        save_text before.txt 55 "$counts"$'\n'"$snapshot" || :
        ;;
    mark|sample|finish)
        run=$2
        [[ -f $run/markers.tsv && -f $run/status.tsv ]] || die "not a session folder: $run"
        if [[ $1 != finish ]]; then
            [[ $3 =~ ^[a-zA-Z0-9]+(-[a-zA-Z0-9]+)*$ && $3 != session-start && $3 != session-end ]] ||
                die "use a label such as keyboard-on-connect"
            [[ ${4:-} != *[$'\r\n\t']* ]] || die "notes must fit on one line"
            marker "$3" "${4:-}" || :
            if [[ $1 == sample ]]; then
                save_text "$3.counters.txt" 25 "$counts" || :
            fi
            exit 0
        fi
        if ! /usr/bin/awk -F '\t' '$1 == "session-end" && $3 != "unavailable" { found=1 } END { exit !found }' "$run/markers.tsv"; then
            marker session-end || :
        fi
        save_text after.txt 55 "$counts"$'\n'"$snapshot" || :
        start_epoch=$(/usr/bin/awk -F '\t' '$1 != "session-end" && $3 ~ /^[0-9]+$/ { print $3; exit }' "$run/markers.tsv")
        end_epoch=$(/usr/bin/awk -F '\t' '$1 == "session-end" && $3 != "unavailable" { print $3; exit }' "$run/markers.tsv")
        if [[ $start_epoch =~ ^[0-9]{10}$ && $end_epoch =~ ^[0-9]{10}$ && $end_epoch -ge $start_epoch ]]; then
            start_time=$(date -u -r "$start_epoch" '+%Y-%m-%d %H:%M:%S+0000')
            end_time=$(date -u -r "$((end_epoch + 1))" '+%Y-%m-%d %H:%M:%S+0000')
            predicate='process == "bluetoothd" OR process == "BlueTool" OR (process == "kernel" AND eventMessage CONTAINS "PianoBluetooth:")'
            save_text bluetoothd.log 55 "/usr/bin/log show --style compact --info --debug --no-pager --timezone UTC --start '$start_time' --end '$end_time' --predicate '$predicate'" || :
            capture_hci
        else
            record_status bluetoothd.log "tablet_window_unavailable"
            record_status hci "tablet_window_unavailable"
        fi
        ;;
esac
