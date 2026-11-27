#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# pool-capture.sh: T-20 v2's tablet session T1: the root image's OS and SystemOS
# windows captured while nothing mounts them, the source of the System and SystemOS copies. Run from the Mac the tablet's USB is on,
# attached (Local Network privacy), with the installed system running (or the tablet in ABL fastboot). Writes NOTHING on the tablet.
#
#   pool-capture.sh ROUND [--preflight-only]
#
#   pre      the tablet lock; the base = OUT/installed.json = main's ledger (a k273-only ESP with the macos-27.3 default); root.json
#            (MAIN/out/inputs/k273/root/root.json, POOL_ROOT_JSON) and its manifest; the battery (macOS, >= 30 %)
#   abl      `sync` as root on the running system (its caches to the medium before the reset), go_abl, the ABL battery >= 3500 mV
#   capture  maint-session.py --esp BASE --root-json ROOT_JSON --new-backup maint-boot map window-capture maint-exit (the Ubuntu
#            maintenance RAM image: map requires nothing mounted; window-capture: three sha256 checks per 4 GiB piece, the captures in
#            PIANO_BACKUP_DIR/<set>/windows/ with CAPTURE.json)
#   back     fastboot reboot into the installed default; the base's profile read back (live_boot); the lock released
#   check    tooling/pool/pool-capture-check.sh on the capture (host, no root)
# Exit 0 captured, checked and the base running again; 1 refused or failed before the maintenance boot (nothing changed); 2 the capture
# or its check failed and the base runs again; 3 recovery hold (the base did not come back: the lock kept, nothing was written).
# The pool package (tooling/pool/mk-pool-package.sh) supplies the base line's functions, as for pool-install.sh.
# Set POOL_BASE_GATE_SPEC and POOL_PROFILE_ARGS for the base line's live boot check.
set -uo pipefail
umask 077
SELF=$(cd "$(dirname "$0")" && pwd -P)
WORK=$(cd "$SELF/../.." && pwd -P)
MAIN=$(git -C "$WORK" rev-parse --path-format=absolute --git-common-dir); MAIN=${MAIN%/.git}
PACKAGE=${POOL_PACKAGE:-$WORK/out/pool/install}
ROOT_JSON=${POOL_ROOT_JSON:-$MAIN/out/inputs/k273/root/root.json}
NAME=${1:?round name}
shift
PREFLIGHT_ONLY=0
[ "${1:-}" = --preflight-only ] && PREFLIGHT_ONLY=1
[[ "$NAME" =~ ^[a-zA-Z0-9][a-zA-Z0-9-]{7,63}$ ]] || { echo "bad round name $NAME" >&2; exit 1; }
export PIANO_PERSIST_OUT=${PIANO_PERSIST_OUT:-$WORK/out/persistent}
export PIANO_V110=$PACKAGE/maintenance
export PIANO_MAINT_SHELL=${PIANO_MAINT_SHELL:-$WORK/tooling/native/maint-shell.py}
export PIANO_INSTALL_SILENT=1
if [ -z "${PIANO_BACKUP_DIR:-}" ]; then
    PIANO_BACKUP_DIR=$(awk -F= '$1=="PIANO_BACKUP_DIR" || $1=="export PIANO_BACKUP_DIR" {sub(/^[^=]*=/,"");gsub(/"/,"");print;exit}' "$HOME/.config/piano/env")
fi
export PIANO_BACKUP_DIR
LOCK=${POOL_LOCK:-/tmp/piano-tablet.lock}
MAINT=$WORK/tooling/persist/maint-session.py
ROUND=${POOL_RUNS:-$PACKAGE/captures}/$NAME
MSESSION=$ROUND/maintenance
CONSOLE=$ROUND/base-disk.console
CAP='' PHASE=pre LOCKED=0 FINISHED=0
[ -f "$PACKAGE/PACKAGE.tsv" ] || { echo "no pool package $PACKAGE (tooling/pool/mk-pool-package.sh)" >&2; exit 1; }
source "$PACKAGE/install-functions.sh"
source "$PACKAGE/install-gate.sh"
source "$PACKAGE/pool-host.sh"
if [ "${POOL_FAKE:-0}" = 1 ]; then   # host rehearsal: the device seams replaced (tooling/persist/test-window-capture.py --flow)
    for seam in RSUDO GO_ABL MS DISK_BOOT BATTERY LIVE_BOOT; do v=POOL_$seam; [ -n "${!v:-}" ] || { echo "POOL_FAKE=1 needs $v" >&2; exit 1; }; done
    rsudo() { "$POOL_RSUDO" "$1"; }
    go_abl() { "$POOL_GO_ABL"; }
    ms() { local log=$1; shift; "$POOL_MS" "$log" "$@"; }
    disk_boot() { "$POOL_DISK_BOOT" "$CONSOLE"; }
    live_boot() { "$POOL_LIVE_BOOT" "$@"; }
    battery_preflight() { "$POOL_BATTERY" > "$ROUND/battery.txt"; }
    abl() { return 1; }
    fb() { echo 'battery-voltage: 4100'; }
    MBOOT='' MEXIT=''
    [ -n "${POOL_MAIN_LEDGER:-}" ] && [ -n "${POOL_LOCK:-}" ] && [ "$POOL_LOCK" != /tmp/piano-tablet.lock ] ||
        { echo 'POOL_FAKE=1 needs POOL_MAIN_LEDGER and POOL_LOCK of its own' >&2; exit 1; }
else
    MBOOT=maint-boot MEXIT=maint-exit
fi
serial=$(awk -F= '$1=="PIANO_SERIAL"{sub(/^[^=]*=/,"");gsub(/"/,"");print;exit}' "$HOME/.config/piano/env" 2>/dev/null)
export PIANO_SERIAL=${PIANO_SERIAL:-$serial}
mkdir -p "$(dirname "$ROUND")" || exit 1
mkdir "$ROUND" || { echo "$ROUND exists: a round folder is never reused" >&2; exit 1; }
LOG=$ROUND/pool-capture.log
say() { echo "$*" | tee -a "$LOG"; }
json() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2], {"d": d}))' "$@"; }
cleanup() {
    local rc=$?
    stop_capture
    if [ "$LOCKED" = 1 ]; then
        case "$PHASE" in done|pre|refused|failed) rm -f "$LOCK/owner"; rmdir "$LOCK";;
            *) say "Recovery hold in $ROUND (phase $PHASE): the tablet lock stays. Nothing was written to the tablet.";;
        esac
    fi
    if [ "$rc" = 0 ] && [ "$FINISHED" != 1 ] && [ "$PREFLIGHT_ONLY" != 1 ]; then say "stopped without an exit code in phase $PHASE"; exit 3; fi
}
trap cleanup EXIT
refuse() { say "refused: $1"; phase refused; exit 1; }
back() {   # the installed default again (nothing was written: the same ESP), its profile read back
    phase back
    disk_boot && live_boot "$BASE_SHA" "$NEED" || { say 'recovery hold: the installed system did not come back with its profile'; exit 3; }
    say "  back on the installed $BASE_IMG"
}

# == pre ==
phase pre
mkdir "$LOCK" 2>/dev/null || { say "the tablet lock $LOCK is held ($(cat "$LOCK/owner" 2>/dev/null)): refused"; exit 1; }
LOCKED=1
printf '%s\n' "$ROUND" > "$LOCK/owner"
say "pool capture $NAME from $WORK"
( cd "$PACKAGE" && shasum -a 256 -c -s PACKAGE.sha256 ) || refuse 'the pool package does not match PACKAGE.sha256'
BASE_IMG=$(json "$PIANO_PERSIST_OUT/installed.json" 'd["image"]') || refuse 'no OUT/installed.json'
BASE_SHA=$(json "$PIANO_PERSIST_OUT/installed.json" 'd["sha256"]')
BASE_JSON=$PIANO_PERSIST_OUT/${BASE_IMG%.img}.json
[ "$(cache_sha "$PIANO_PERSIST_OUT/$BASE_IMG")" = "$BASE_SHA" ] &&
    [ "$(json "$BASE_JSON" 'd["sha256"]')" = "$BASE_SHA" ] && [ "$(json "$BASE_JSON" 'd["kind"]')" = k273-only ] &&
    [ "$(json "$BASE_JSON" 'd["default"]')" = macos-27.3 ] || refuse "the installed ESP $BASE_IMG: not a k273-only ESP record with the macos-27.3 default"
cmp -s "$PIANO_PERSIST_OUT/installed.json" "${POOL_MAIN_LEDGER:-$MAIN/out/persistent}/installed.json" ||
    refuse "OUT/installed.json is not main's ledger (refresh with mk-pool-package.sh)"
[ -f "$ROOT_JSON" ] && json "$ROOT_JSON" '[w["name"] for w in d["windows"]] == ["OS", "SystemOS"] and len(d["manifest_sha256"]) == 64' | grep -qx True ||
    refuse "$ROOT_JSON is not the root image record (windows OS, SystemOS, manifest_sha256)"
[ -n "$PIANO_BACKUP_DIR" ] && [ -d "$PIANO_BACKUP_DIR" ] && [ -w "$PIANO_BACKUP_DIR" ] || refuse 'PIANO_BACKUP_DIR is not a writable directory'
free=$(df -k "$PIANO_BACKUP_DIR" | awk 'NR==2 {print $4}')
need=$(json "$ROOT_JSON" 'sum(w["size"] for w in d["windows"]) // 1024 + (1 << 20)')   # KiB: both windows + 1 GiB
[ "${free:-0}" -ge "$need" ] || refuse "PIANO_BACKUP_DIR has $(( ${free:-0} / 1024 / 1024 )) GiB free, the capture needs $(( need / 1024 / 1024 )) GiB"
NEED=$(pool_gate_args) || refuse 'set POOL_BASE_GATE_SPEC and POOL_PROFILE_ARGS for the verified base'
say "  base: $BASE_IMG ${BASE_SHA:0:16}; root image record $ROOT_JSON; profile $POOL_PROFILE_ARGS"
python3 "$MAINT" --session "$MSESSION.dry" --esp "$BASE_IMG" --root-json "$ROOT_JSON" --dry-run maint-boot map window-capture maint-exit \
    > "$ROUND/dry-run.log" 2>&1 || refuse "the session's dry run (dry-run.log): $(tail -2 "$ROUND/dry-run.log" | tr '\n' ' ')"
if [ "$PREFLIGHT_ONLY" = 1 ]; then say 'PREFLIGHT ONLY: every host-side gate passed (nothing sent to the tablet)'; phase done; FINISHED=1; exit 0; fi
if ! abl; then
    battery_preflight || refuse "battery: $(tr '\n' ' ' < "$ROUND/battery.txt")"
    rsudo '/bin/sync; /bin/sync' > /dev/null 2>&1 || say '  sync on the running system did not answer (the reset follows anyway)'
fi

# == abl ==
phase abl
go_abl || { phase failed; say 'not in ABL fastboot after the console reset (go-abl.txt): nothing done'; exit 1; }
battery=$(fb getvar battery-voltage | sed -nE 's/^(\(bootloader\) )?battery-voltage: ([0-9]{4,5})[[:space:]]*$/\2/p')
if ! [[ "$battery" =~ ^[0-9]{4,5}$ ]] || [ "$battery" -lt 3500 ]; then
    say "ABL battery-voltage ${battery:-unread} mV (minimum 3500)"; back; phase failed; exit 1
fi

# == capture ==
phase capture
t0=$(date +%s)
if ms "$ROUND/maint-capture.log" --esp "$BASE_IMG" --root-json "$ROOT_JSON" --new-backup $MBOOT map window-capture $MEXIT; then
    crc=0
else
    crc=1
    say "the maintenance session stopped (maint-capture.log): $(grep -a 'STOPPED' "$ROUND/maint-capture.log" | tail -1)"
    [ -z "$MEXIT" ] || ms "$ROUND/maint-exit.log" --esp "$BASE_IMG" $MEXIT || say '  maint-exit did not answer (the reset into ABL is tried by the disk boot)'
fi
say "  capture session: $(( $(date +%s) - t0 )) s"
back
WIN=''
if [ "$crc" = 0 ]; then
    set_name=$(json "$MSESSION/state.json" 'd["window_capture"]["set"]' 2>/dev/null)
    WIN=$PIANO_BACKUP_DIR/$set_name/windows
    [ -n "$set_name" ] && [ -f "$WIN/CAPTURE.json" ] || { crc=1; say 'no CAPTURE.json after the session'; }
fi
if [ "$crc" = 0 ]; then
    cp -p "$WIN/CAPTURE.json" "$ROUND/CAPTURE.json"
    printf '%s\n' "$WIN" > "$ROUND/windows.path"
    phase check
    bash "$SELF/pool-capture-check.sh" "$WIN" > "$ROUND/capture-check.txt" 2>&1 || crc=1
    cat "$ROUND/capture-check.txt" >> "$LOG"
fi
phase done
FINISHED=1
if [ "$crc" != 0 ]; then say "capture FAILED (the tablet runs $BASE_IMG again; nothing was written). Evidence: $ROUND"; exit 2; fi
for v in os systemos; do
    say "  $v: $(json "$ROUND/CAPTURE.json" "d['windows']['$v']['sha256']")$([ "$v" = os ] && [ "$(json "$ROUND/CAPTURE.json" "d['windows']['os']['sha256']")" = 40d8282eb8650da9b4d7437a981764c48d47ee4bdc5b8b362248f25ecbce0e70 ] && echo ' (= the pinned 40d8282e: the existing OS archive applies)')"
done
say "captured: $WIN (CAPTURE CHECK PASS); next: the archives (PLAN-v2.md S1-S3), then pool-install.sh --capture $WIN"
exit 0
