#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# pool-install.sh: the T-20 install session moves the System (OS) and SystemOS volumes into the Data container. Run
# from the Mac mini, attached (Local Network privacy), with the tablet on the installed v13.1y10 at the desktop or the login window.
# Wired to v13.1y10's own functions through the pool package (tooling/pool/mk-pool-package.sh: out/pool/install holds copies of the
# v13.1y10 package's install-functions.sh, install-gate.sh, installed-check.py (the pool record kind accepted) and maintenance/, and
# tooling/pool/pool-host.sh = install-v131y10.sh's host functions): rssh/rsudo, put, settle, desk_login,
# go_abl (macOS uptime >= 200 s, one resend), battery_preflight, disk_boot, live_boot, maint-session (backup, three sha256 checks,
# read-back), the silent gates. PIANO_INSTALL_SILENT=1 is the default (owner 12:45: no speaker tone, no microphone take).
#
#   pool-install.sh ROUND --inventory DIR --capture CAPTURE_DIR [--preflight-only]
#
# T-20 v2: the copy source is a quiesced capture of both windows
# (maint-session.py window-capture in the maintenance boot, tooling/pool/pool-capture.sh; CAPTURE_DIR holds CAPTURE.json). R1a (pre):
# each archive's source_sha256 = the capture's sha256 of that window. R1b (device, before any volume is created): the live old
# volumes read through `/` (pool-device.sh live: pool-manifest --paths) and compared on this host with the archives' manifests
# (pool-live-check.py: exact but for the tolerances T1-T7); pool-device.sh accept-live records the EQUAL verdict, create refuses
# without it. The base is main's ledger (any k273-only ESP with the macos-27.3 default: v13.1y12c today); its G2 profile from
# POOL_BASE_GATE_SPEC and POOL_PROFILE_ARGS from the matching base install. POOL_DATA_UUID names the expected Data volume.
#
#   pre     the tablet lock; the inventory gates (pool-inventory-check.sh over tonight's INVENTORY-READS.sh folder: R1 window hashes =
#           the archive sources, R2 SIP, R3 addVolume usable, R4 Data free >= 30 GB; RECORDED the rest); the archives and the package
#           files against their records; the installed ESP = OUT/installed.json's = main's ledger (the rollback target); the battery
#   device  the archives (os 11.8 GB, systemos 3.1 GB), manifests, lists, pool-manifest and pool-device.sh into /private/var/tmp/ROUND
#           (create_remote; each sha256 checked after the copy); pool-device.sh as root, each step detached with its own status file:
#           preflight os/systemos (the old windows read raw again), create, extract, verify of each, fstab; the state fetched. Nothing is
#           written to the ESP unless both copies verified. A refusal here undoes the copies. An unconfirmed step holds for recovery.
#   build   tooling/k273/build-pool-esp.py --state ROUND/state.env: the pool ESP and its record in OUT
#   esp     the battery again; go_abl; the ABL battery voltage (>= 3500 mV); maint-session maint-boot map esp-delta check maint-exit
#           --esp POOL --new-backup (as v13.1y10); a stopped session is a recovery hold (exit 3)
#   gates   G1 disk boot + USB SSH, G2 the pool ESP's exact profile and the resumed owner Data (live_boot), P1 / reports the SystemOS
#           copy, P2 the root mount is the System copy, P3 df / = the Data container, P4 the old windows not mounted, P5 ten firmlinks,
#           P6 Data store state 2, G4 the silent sound gate (its desk login proves the GUI session), G5 Wi-Fi, G3 the boot guard healthy,
#           G10 no panic, L1 two apps launch (the copied dyld cache in use); every one of them rolls back on FAIL
#   rollback ABL; maint-session maint-boot map, rollback esp, map check maint-exit with the base (the y10 entry the default again);
#           the base read back (live_boot), / on the old windows; then the two copies deleted by UUID (pool-device.sh undo); exit 2
# Exit 0 installed and every gate PASS; 1 refused before any ESP write (nothing left behind); 2 rolled back (the base ESP verified back);
# 3 recovery hold (the lock kept: read the live state first). --preflight-only: the host-side pre gates alone (no device action).
set -uo pipefail
umask 077
SELF=$(cd "$(dirname "$0")" && pwd -P)
WORK=$(cd "$SELF/../.." && pwd -P)
MAIN=$(git -C "$WORK" rev-parse --path-format=absolute --git-common-dir); MAIN=${MAIN%/.git}
PACKAGE=${POOL_PACKAGE:-$WORK/out/pool/install}
ARCH_os=${POOL_ARCHIVE_OS:-}
ARCH_systemos=${POOL_ARCHIVE_SYSTEMOS:-}
CAPTURE=${POOL_CAPTURE:-}
POOL_ENTRY=macos-27.3-pool
OS_OLD=84581E99-85BE-4957-AC53-2947E887B3E4
OLD_UUID=07AD53B3-7565-4004-B574-F45FAC3753FB
FILES='volume.aar expected.manifest archive.env'
export PIANO_INSTALL_SILENT=${PIANO_INSTALL_SILENT:-1}
NAME=${1:?round name}
shift
INVENTORY=${POOL_INVENTORY:-}
PREFLIGHT_ONLY=0
while [ $# -gt 0 ]; do
    case $1 in
        --inventory) INVENTORY=${2:?--inventory DIR}; shift 2;;
        --capture) CAPTURE=${2:?--capture DIR}; shift 2;;
        --preflight-only) PREFLIGHT_ONLY=1; shift;;
        *) echo "usage: pool-install.sh ROUND --inventory DIR --capture DIR [--preflight-only]" >&2; exit 1;;
    esac
done
[[ "$NAME" =~ ^[a-zA-Z0-9][a-zA-Z0-9-]{7,63}$ ]] || { echo "bad round name $NAME" >&2; exit 1; }
export PIANO_PERSIST_OUT=${PIANO_PERSIST_OUT:-$WORK/out/persistent}
export PIANO_V110=$PACKAGE/maintenance
export PIANO_MAINT_SHELL=${PIANO_MAINT_SHELL:-$WORK/tooling/native/maint-shell.py}
if [ -z "${PIANO_BACKUP_DIR:-}" ]; then
    PIANO_BACKUP_DIR=$(awk -F= '$1=="PIANO_BACKUP_DIR" || $1=="export PIANO_BACKUP_DIR" {sub(/^[^=]*=/,"");gsub(/"/,"");print;exit}' "$HOME/.config/piano/env")
fi
export PIANO_BACKUP_DIR
LOCK=${POOL_LOCK:-/tmp/piano-tablet.lock}
MAINT=$WORK/tooling/persist/maint-session.py
ROUND=${POOL_RUNS:-$PACKAGE/rounds}/$NAME
REMOTE=${POOL_REMOTE:-/private/var/tmp/$NAME}
PR=$REMOTE/pool
MSESSION=$ROUND/maintenance
CONSOLE=$ROUND/first-disk.console
CAP=''
PHASE=pre
LOCKED=0
FINISHED=0
COPIES_MAY_EXIST=0
[ -d "$PACKAGE" ] && [ -f "$PACKAGE/PACKAGE.tsv" ] || { echo "no pool package $PACKAGE (tooling/pool/mk-pool-package.sh)" >&2; exit 1; }
# Check the required inputs as well as every listed checksum before sourcing package code.
(
    cd "$PACKAGE" &&
    awk '
        BEGIN { split("install-functions.sh install-gate.sh pool-host.sh pool-device.sh pool-manifest", files) }
        { path = $2; sub(/^\*/, "", path); sub(/^\.\//, "", path); seen[path]++ }
        END { for (i in files) if (seen[files[i]] != 1) exit 1 }
    ' PACKAGE.sha256 &&
    shasum -a 256 --strict -c -s PACKAGE.sha256
) || { echo 'refused: the pool package does not match a complete PACKAGE.sha256' >&2; exit 1; }
# the package copies of v13.1y10's functions, then the pool's host functions
source "$PACKAGE/install-functions.sh"
source "$PACKAGE/install-gate.sh"
source "$PACKAGE/pool-host.sh"
declare -F dev_step > /dev/null || { echo 'rebuild the pool package with tooling/pool/mk-pool-package.sh' >&2; exit 1; }
# the fake tablet (tooling/persist/test-pool-session.py): every device seam replaced by an executable; nothing else changes
if [ "${POOL_FAKE:-0}" = 1 ]; then
    for seam in RSSH RSUDO GO_ABL MS DISK_BOOT GUI_GATES BATTERY LIVE_BOOT; do
        v=POOL_$seam; [ -n "${!v:-}" ] || { echo "POOL_FAKE=1 needs $v" >&2; exit 1; }
    done
    rssh() { "$POOL_RSSH" "$1"; }
    rsudo() { "$POOL_RSUDO" "$1"; }
    go_abl() { "$POOL_GO_ABL"; }
    ms() { local log=$1; shift; "$POOL_MS" "$log" "$@"; }
    disk_boot() { "$POOL_DISK_BOOT" "$CONSOLE"; }
    live_boot() { "$POOL_LIVE_BOOT" "$@"; }
    battery_preflight() { "$POOL_BATTERY" > "$ROUND/battery.txt"; }
    abl() { return 1; }
    fb() { echo 'battery-voltage: 4100'; }
    pool_gui_gates() { "$POOL_GUI_GATES" "$ROUND"; }
    MBOOT='' MEXIT=''
    # never main's ledger, never the real tablet lock
    [ -n "${POOL_MAIN_LEDGER:-}" ] && [ -n "${POOL_LOCK:-}" ] && [ "$POOL_LOCK" != /tmp/piano-tablet.lock ] &&
        [ "$POOL_MAIN_LEDGER" != "$MAIN/out/persistent" ] || { echo 'POOL_FAKE=1 needs POOL_MAIN_LEDGER and POOL_LOCK of its own' >&2; exit 1; }
else
    MBOOT=maint-boot MEXIT=maint-exit
fi
serial=$(awk -F= '$1=="PIANO_SERIAL"{sub(/^[^=]*=/,"");gsub(/"/,"");print;exit}' "$HOME/.config/piano/env")
export PIANO_SERIAL=${PIANO_SERIAL:-$serial}
mkdir -p "$(dirname "$ROUND")" || exit 1
mkdir "$ROUND" || { echo "$ROUND exists: a round folder is never reused" >&2; exit 1; }
LOG=$ROUND/pool-install.log
say() { echo "$*" | tee -a "$LOG"; }
cleanup() {
    local rc=$?
    stop_capture
    if [ "$LOCKED" = 1 ]; then
        case "$PHASE" in done|rolled-back|pre|refused) rm -f "$LOCK/owner"; rmdir "$LOCK";;
            *) say "Recovery hold in $ROUND (phase $PHASE). The tablet lock stays.";;
        esac
    fi
    if [ "$rc" = 0 ] && [ "$FINISHED" != 1 ] && [ "$PREFLIGHT_ONLY" != 1 ]; then say "stopped without an exit code in phase $PHASE"; exit 3; fi
}
trap cleanup EXIT
pgate() {   # STATE LABEL: the pool's gate lines, in v13.1y10's verdict file
    gate_line "$1 $2"; say "  $1  $2"
}
must_gate() {   # LABEL FUNCTION ARGS...: 0 PASS, 1 FAIL -> rollback, 2 VOID -> recovery hold
    local label=$1 rc=0; shift
    "$@" || rc=$?
    case "$rc" in 0) pgate PASS "$label";; 1) pgate FAIL "$label"; rollback "$label";; *) pgate VOID "$label"; phase recovery; exit 3;; esac
}
json() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2], {"d": d}))' "$@"; }

pool_put() {   # LOCAL REMOTE: put (v13.1y10's: five tries, sha256 read back) with an ssh time limit for the file's size
    local size
    size=$(stat -f %z "$1")
    SSH_T_PUT=$(( size / 15000000 + 180 )) put_sized "$1" "$2"
}
put_sized() {
    local source=$1 dest=$2 wanted attempt got
    wanted=$(sha "$source") || return 1
    [[ "$wanted" =~ ^[0-9a-f]{64}$ ]] || return 1
    for attempt in 1 2 3 4 5; do
        got=''
        if SSH_T=$SSH_T_PUT rssh "umask 077; cat > '$dest.part'" < "$source"; then
            if got=$(SSH_T=$(( SSH_T_PUT / 4 + 60 )) rssh "set -o pipefail; /usr/bin/shasum -a 256 '$dest.part' | /usr/bin/awk '{print \$1}'") &&
                [ "$got" = "$wanted" ] && rssh "/bin/mv -f '${dest:?}.part' '${dest:?}'"; then return 0; fi
        fi
        say "  copy of $(basename "$source") try $attempt: ${got:-no hash}, want $wanted"
        nap 3
    done
    return 1
}
root_os() {   # the volume UUID of the root mount itself (the first mount on /: the System volume, under the SystemOS union)
    local dev
    dev=$(rssh "mount | awk '\$3 == \"/\" { print \$1; exit }'")
    [ -n "$dev" ] && rssh "diskutil info -plist $dev | plutil -extract VolumeUUID raw -o - -"
}
refuse() {   # WHY: before any ESP write; the copies made so far undone (pool-device.sh undo)
    say "refused: $1"
    if [ "$PHASE" = device ] && [ "$COPIES_MAY_EXIST" = 1 ]; then
        if ! dev_step undo '' 600; then
            say 'recovery hold: undo of the copies failed (device-undo.log)'
            phase recovery
            exit 3
        fi
        say '  the copies made so far undone'
    fi
    phase refused
    exit 1
}

require_clear_hold() {
    local hold
    for hold in "$PIANO_PERSIST_OUT/HOLD" "${POOL_MAIN_LEDGER:-$MAIN/out/persistent}/HOLD"; do
        [ ! -e "$hold" ] && [ ! -L "$hold" ] || refuse "install writes are on HOLD ($hold)"
    done
}

rollback() {   # WHY
    local why=$1 previous=$PHASE r=$ROUND/rollback-$(date +%Y%m%d-%H%M%S) now
    mkdir "$r" || exit 3
    printf '%s\n' "$why" >> "$ROUND/rollback-reason"
    phase recovery
    say "rollback: $why"
    [ -s "$CONSOLE" ] && cp -p "$CONSOLE" "$r/pool-before-rollback.console"
    if [ "$previous" = gates ]; then silent_restore 'before the rollback' > /dev/null 2>&1 || true; fi
    go_abl || { say 'recovery hold: no ABL'; exit 3; }
    ms "$r/map.log" --esp "$BASE_IMG" $MBOOT map &&
        ms "$r/esp.log" --esp "$BASE_IMG" rollback esp &&
        ms "$r/check.log" --esp "$BASE_IMG" map check $MEXIT || { say 'recovery hold: the rollback session failed'; exit 3; }
    read_ledger "$BASE_IMG" "$BASE_SHA" && publish_ledger "$BASE_IMG" "$BASE_SHA" ||
        { say "recovery hold: the ledger does not name $BASE_IMG after the rollback"; exit 3; }
    CONSOLE=$r/base-disk.console
    disk_boot && live_boot "$BASE_SHA" "$NEED" || { say 'recovery hold: the base entry does not boot or is not its profile'; exit 3; }
    now=$(rssh "diskutil info -plist / | plutil -extract VolumeUUID raw -o - -")
    [ "$now" = "$OLD_UUID" ] || { say "recovery hold: / reports $now after the rollback, not $OLD_UUID"; exit 3; }
    now=$(root_os)
    [ "$now" = "$OS_OLD" ] || { say "recovery hold: the root mount is $now after the rollback, not $OS_OLD"; exit 3; }
    rsudo "/bin/rm -f '$PR/os/volume.aar' '$PR/systemos/volume.aar'" > /dev/null 2>&1 || true
    if [ "${POOL_KEEP_VOLUMES:-0}" != 1 ]; then
        dev_step undo '' 600 && say '  the two copies deleted by UUID (pool-device.sh undo)' ||
            say "  the copies' undo did not finish (device-undo.log): they stay inert (noauto); delete them later by UUID"
    fi
    phase rolled-back
    say "rolled back: $BASE_IMG (the y10 entry the default again), the root on the old System and SystemOS windows. Evidence: $ROUND"
    exit 2
}

# == pre ===========================================================================================================================
phase pre
mkdir "$LOCK" 2>/dev/null || { say "the tablet lock $LOCK is held ($(cat "$LOCK/owner" 2>/dev/null)): refused"; exit 1; }
LOCKED=1
printf '%s\n' "$ROUND" > "$LOCK/owner"
say "pool install $NAME (silent=$PIANO_INSTALL_SILENT) from $WORK"
require_clear_hold
[ -n "$INVENTORY" ] && [ -d "$INVENTORY" ] || refuse 'no inventory folder (--inventory: tonight'"'"'s INVENTORY-READS.sh output)'
[ -n "$CAPTURE" ] && [ -f "$CAPTURE/CAPTURE.json" ] || refuse 'no capture (--capture: the windows/ folder of pool-capture.sh with CAPTURE.json)'
[[ "${POOL_DATA_UUID:-}" =~ ^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$ ]] || refuse 'set POOL_DATA_UUID to the expected Data volume UUID'
for v in os systemos; do   # the archives: given, else out/pool/<vol>-<first 8 of the capture sha256> (make-volume-archive.sh's folder)
    a=ARCH_$v
    csha=$(json "$CAPTURE/CAPTURE.json" "d['windows']['$v']['sha256']") || refuse "CAPTURE.json has no $v window"
    [[ "$csha" =~ ^[0-9a-f]{64}$ ]] || refuse "CAPTURE.json has no valid SHA-256 for the $v window"
    [ -n "${!a}" ] || printf -v "$a" '%s' "$WORK/out/pool/$v-${csha:0:8}"
    [ "$(sed -n 's/^source_sha256=//p' "${!a}/archive.env" 2>/dev/null)" = "$csha" ] ||
        refuse "R1a: the $v archive ${!a} is not made from the capture (source $(sed -n 's/^source_sha256=//p' "${!a}/archive.env" 2>/dev/null | cut -c1-16), capture ${csha:0:16})"
    say "  R1a $v: the archive's source = the capture's ${csha:0:16} (${!a})"
done
POOL_DATA_UUID=$POOL_DATA_UUID POOL_R1_CAPTURE=1 bash "$SELF/pool-inventory-check.sh" "$INVENTORY" "$ARCH_os" "$ARCH_systemos" > "$ROUND/inventory-gates.txt" 2>&1
irc=$?
cat "$ROUND/inventory-gates.txt" >> "$LOG"
[ "$irc" = 0 ] || refuse "the inventory gates: $(tail -1 "$ROUND/inventory-gates.txt")"
say "  inventory gates: PASS ($INVENTORY)"
for v in os systemos; do
    a=ARCH_$v; a=${!a}
    [ -f "$a/archive.env" ] || refuse "no $a/archive.env"
    for f in $FILES; do [ -f "$a/$f" ] && [ -s "$a/$f" ] || refuse "missing or empty $a/$f"; done
    for receipt in archive_sha256:volume.aar manifest_file_sha256:expected.manifest; do
        f=${receipt#*:}
        want=$(sed -n "s/^${receipt%:*}=//p" "$a/archive.env")
        [[ "$want" =~ ^[0-9a-f]{64}$ ]] &&
            got=$(shasum -a 256 "$a/$f" | awk '{print $1}') && [ "$got" = "$want" ] ||
            refuse "$a/$f is not its archive.env one"
    done
done
python3 "$SELF/pool-live-check.py" check-manifests --os "$ARCH_os/expected.manifest" --systemos "$ARCH_systemos/expected.manifest" > "$ROUND/manifest-check.txt" 2>&1 ||
    refuse "incomplete or invalid source manifest: $(head -1 "$ROUND/manifest-check.txt")"
[ -n "$PIANO_BACKUP_DIR" ] && [ -d "$PIANO_BACKUP_DIR" ] && [ -w "$PIANO_BACKUP_DIR" ] ||
    refuse 'PIANO_BACKUP_DIR is not a writable directory for the ESP rollback backup'
BASE_IMG=$(json "$PIANO_PERSIST_OUT/installed.json" 'd["image"]') || refuse 'no OUT/installed.json'
[[ "$BASE_IMG" =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]*\.img$ ]] || refuse 'invalid rollback image in OUT/installed.json'
BASE_SHA=$(json "$PIANO_PERSIST_OUT/installed.json" 'd["sha256"]') || refuse 'no rollback SHA-256 in OUT/installed.json'
[[ "$BASE_SHA" =~ ^[0-9a-f]{64}$ ]] || refuse 'invalid rollback SHA-256 in OUT/installed.json'
BASE_JSON=$PIANO_PERSIST_OUT/${BASE_IMG%.img}.json
[ "$(cache_sha "$PIANO_PERSIST_OUT/$BASE_IMG")" = "$BASE_SHA" ] && [ "$(json "$BASE_JSON" 'd["sha256"]')" = "$BASE_SHA" ] ||
    refuse "the installed ESP $BASE_IMG: its image is not here or not its record's"
[ "$(json "$BASE_JSON" 'd["kind"]')" = k273-only ] && [ "$(json "$BASE_JSON" 'd["default"]')" = macos-27.3 ] ||
    refuse "the installed ESP $BASE_IMG is not a k273-only ESP with the macos-27.3 default (the rollback entry)"
cmp -s "$PIANO_PERSIST_OUT/installed.json" "${POOL_MAIN_LEDGER:-$MAIN/out/persistent}/installed.json" ||
    refuse "OUT/installed.json is not main's ledger (after the v13.1y10 install: refresh with mk-pool-package.sh)"
NEED=$(pool_gate_args) || refuse 'set POOL_BASE_GATE_SPEC and POOL_PROFILE_ARGS for the verified base'
say "  G2 profile: $POOL_PROFILE_ARGS"
say "  base (installed, the rollback): $BASE_IMG ${BASE_SHA:0:16}"
if [ "$PREFLIGHT_ONLY" = 1 ]; then say 'PREFLIGHT ONLY: every host-side pre gate passed (the battery and every device step not run)'; FINISHED=1; exit 0; fi
abl && refuse 'the tablet is in ABL fastboot: the copies need the installed system running (boot it first)'
battery_preflight || refuse "battery: $(tr '\n' ' ' < "$ROUND/battery.txt")"
[ "${BATT_ABL_CHECK:-0}" = 0 ] || refuse "battery: pmset unread on macOS ($(tr '\n' ' ' < "$ROUND/battery.txt")); the copies run on macOS for up to an hour"
say "  battery: $(head -1 "$ROUND/battery.txt")"

# == device ========================================================================================================================
require_clear_hold
phase device
create_remote || refuse "the round's tablet folder $REMOTE"
rssh "mkdir -p '$PR/os' '$PR/systemos'" || refuse 'no ssh'
for v in os systemos; do
    a=ARCH_$v; a=${!a}
    for g in $FILES $(cd "$a" && ls afsc-*.list 2>/dev/null); do pool_put "$a/$g" "$PR/$v/$g" || refuse "the copy of $v/$g"; done
done
pool_put "$PACKAGE/pool-manifest" "$PR/pool-manifest" || refuse 'the copy of pool-manifest'
pool_put "$PACKAGE/pool-device.sh" "$PR/pool-device.sh" || refuse 'the copy of pool-device.sh'
rssh "chmod 755 '$PR/pool-manifest'" || refuse 'chmod pool-manifest'
t0=$(date +%s)
r1b() {   # T-20 v2 R1b: the live view of the old volumes on this boot = the capture (pool-live-check.py), then accept-live
    local got want t1
    t1=$(date +%s)
    rssh 'mount' > "$ROUND/live-mounts.txt" && [ -s "$ROUND/live-mounts.txt" ] || refuse 'R1b: no mount table from the tablet'
    python3 "$SELF/pool-live-check.py" list --systemos "$ARCH_systemos/expected.manifest" --os "$ARCH_os/expected.manifest" \
        --mounts "$ROUND/live-mounts.txt" --out "$ROUND/live-paths.txt" >> "$LOG" 2>&1 || refuse 'R1b: the live list'
    pool_put "$ROUND/live-paths.txt" "$PR/live-paths.txt" && pool_put "$ROUND/live-mounts.txt" "$PR/live-mounts.txt" || refuse 'R1b: the list copy'
    dev_step live '' 3600 || refuse "R1b: device step live: $(tail -1 "$ROUND/device-live.log")"
    want=$(rsudo "cat '$PR/state.env'" | sed -n 's/^live_sha256=//p' | tail -1)
    SSH_T=600 rssh "cat '$PR/live.txt'" > "$ROUND/live.txt" || refuse 'R1b: the live output fetch (user ssh)'
    got=$(shasum -a 256 "$ROUND/live.txt" | awk '{print $1}')
    [ -n "$want" ] && [ "$got" = "$want" ] || refuse "R1b: the fetched live output hashes to $got, the tablet's is ${want:-unread}"
    python3 "$SELF/pool-live-check.py" compare --systemos "$ARCH_systemos/expected.manifest" --os "$ARCH_os/expected.manifest" \
        --mounts "$ROUND/live-mounts.txt" --list "$ROUND/live-paths.txt" --live "$ROUND/live.txt" --json "$ROUND/r1b.json" > "$ROUND/r1b.txt" 2>&1
    local rc=$?
    cat "$ROUND/r1b.txt" >> "$LOG"
    [ "$rc" = 0 ] || refuse "R1b: the live old volumes are not the capture: $(head -1 "$ROUND/r1b.txt")"
    dev_step accept-live "$got" 120 || refuse 'R1b: accept-live'
    say "  R1b ($(( $(date +%s) - t1 )) s): $(head -1 "$ROUND/r1b.txt")"
}
for s in 'preflight os 600' 'preflight systemos 600' 'r1b - -' 'create os 300' 'create systemos 300' 'extract os 5400' 'verify os 1800' \
         'extract systemos 2400' 'verify systemos 900' 'fstab - 120'; do
    set -- $s
    if [ "$1" = r1b ]; then r1b; continue; fi
    if [ "$1" = create ]; then COPIES_MAY_EXIST=1; fi
    vol=$2; [ "$vol" = - ] && vol=''
    t1=$(date +%s)
    dev_step "$1" "$vol" "$3" || refuse "device step $1 $vol: $(tail -1 "$ROUND/device-$1${vol:+-$vol}.log")"
    say "  device $1 $vol ($(( $(date +%s) - t1 )) s): $(tail -1 "$ROUND/device-$1${vol:+-$vol}.log")"
done
say "  device steps: $(( $(date +%s) - t0 )) s"
rsudo "cat '$PR/state.env'" > "$ROUND/state.env" || refuse 'the state fetch'
grep -q '^os_verified=yes$' "$ROUND/state.env" && grep -q '^systemos_verified=yes$' "$ROUND/state.env" && grep -q '^fstab=yes$' "$ROUND/state.env" ||
    refuse 'the fetched state is not verified for both copies'
NEW_OS=$(sed -n 's/^os_volume_uuid=//p' "$ROUND/state.env")
NEW_UUID=$(sed -n 's/^systemos_volume_uuid=//p' "$ROUND/state.env")
say "  the copies: System $NEW_OS, SystemOS $NEW_UUID (extract os $(sed -n 's/^os_extract_seconds=//p' "$ROUND/state.env") s, systemos $(sed -n 's/^systemos_extract_seconds=//p' "$ROUND/state.env") s)"

# == build =========================================================================================================================
phase build
POOL_NAME=${BASE_IMG%.img}-pool
POOL_IMG=$POOL_NAME.img
python3 "$WORK/tooling/k273/build-pool-esp.py" --base "$BASE_JSON" --state "$ROUND/state.env" --name "$POOL_NAME" --out "$PIANO_PERSIST_OUT" >> "$LOG" 2>&1 ||
    { phase device; refuse 'build-pool-esp.py refused'; }
POOL_SHA=$(json "$PIANO_PERSIST_OUT/$POOL_NAME.json" 'd["sha256"]')

# == esp ===========================================================================================================================
phase esp-pre
battery_preflight || { phase device; refuse "battery before the ESP write: $(tr '\n' ' ' < "$ROUND/battery.txt")"; }
go_abl || { phase device; refuse 'not in ABL fastboot after the console reset (go-abl.txt)'; }
battery=$(fb getvar battery-voltage | sed -nE 's/^(\(bootloader\) )?battery-voltage: ([0-9]{4,5})[[:space:]]*$/\2/p')
if ! [[ "$battery" =~ ^[0-9]{4,5}$ ]] || [ "$battery" -lt 3500 ]; then
    say "ABL battery-voltage ${battery:-unread} mV (minimum 3500): back to the installed system"
    fb reboot > /dev/null || true
    phase device; refuse 'ABL battery'
fi
phase session
if ! ms "$ROUND/maint-install.log" --esp "$POOL_IMG" --new-backup $MBOOT map esp-delta check $MEXIT; then
    say 'recovery hold: the maintenance session stopped (maint-install.log): read the live ESP, Data and session state under the retained lock'
    exit 3
fi
read_ledger "$POOL_IMG" "$POOL_SHA" || rollback 'installed.json does not name the pool ESP after the session'

# == gates =========================================================================================================================
phase esp
must_gate 'G1 disk boot and USB SSH' disk_boot
must_gate 'G2 the pool ESP profile and the resumed owner Data' live_boot "$POOL_SHA" "$NEED"
phase gates
REMOTE=$REMOTE-gates   # the gates' own folder on this boot (the device folder $PR stays for undo)
create_remote || { phase recovery; say 'recovery hold: the gates folder'; exit 3; }
p1() { [ "$(rssh "diskutil info -plist / | plutil -extract VolumeUUID raw -o - -")" = "$NEW_UUID" ]; }
p2() { [ "$(root_os)" = "$NEW_OS" ]; }
p3() {
    local r d
    r=$(rssh "df -k / | awk 'NR==2 {print \$4}'"); d=$(rssh "df -k /System/Volumes/Data | awk 'NR==2 {print \$4}'")
    printf 'df / %s KiB, /System/Volumes/Data %s KiB\n' "$r" "$d" >> "$ROUND/pool-df.txt"
    [[ "$r" =~ ^[0-9]+$ ]] && [[ "$d" =~ ^[0-9]+$ ]] && [ "$r" -ge $(( d * 99 / 100 )) ] && [ "$r" -gt 26214400 ]
}
p4() { rssh "diskutil info $OLD_UUID | grep -q 'Mounted: *No' && diskutil info $OS_OLD | grep -q 'Mounted: *No'"; }
p5() { [ "$(grep -ac 'k27: firmlink .*: 0$' "$CONSOLE")" -ge 10 ]; }
p6() { grep -aq 'Data store state 2' "$CONSOLE"; }
must_gate 'P1 / reports the SystemOS copy' p1
must_gate 'P2 the root mount is the System copy' p2
must_gate 'P3 df / = the Data container (one pool, > 25 GiB)' p3
must_gate 'P4 the old OS and SystemOS windows not mounted' p4
must_gate 'P5 ten k27 firmlinks (console)' p5
must_gate 'P6 Data store state 2 (console)' p6
if [ "${POOL_FAKE:-0}" = 1 ]; then
    must_gate 'G4 G5 G3 G10 L1 (the fake tablet)' pool_gui_gates
else
    must_gate 'G4 sound output device (silent: no tone), desk login and Finder' speaker_gate
    must_gate 'G5 usable Wi-Fi' wifi_gate
    must_gate 'G3 boot guard healthy' boot_guard_gate
    must_gate 'G10 known console, same disk boot, no panic' panic_free
    must_gate 'L1 two apps launch below 2.5 s (the copied dyld cache in use)' launch_gate
fi
gate_line 'RECORDED the boot-manifest hash readers (C12): unknown; the inventory read and this boot'"'"'s console kept'
gate_line 'OPEN the owner installs the Command Line Tools on the one pool (the morning refusal, gone if / reports the Data container)'
if silent_mode; then silent_restore 'after the last gate' > /dev/null 2>&1 || gate_line 'FAIL the owner sound settings put back (recorded only)'; fi
publish_ledger "$POOL_IMG" "$POOL_SHA" || { phase recovery; say 'recovery hold: the ledger publication'; exit 3; }
rsudo "/bin/rm -f '$PR/os/volume.aar' '$PR/systemos/volume.aar'" > /dev/null 2>&1 || true   # 15 GB back to the pool (manifests and state kept)
phase done
FINISHED=1
say "installed: $POOL_IMG, the root on System $NEW_OS and SystemOS $NEW_UUID in the Data container; $BASE_IMG's macos-27.3 entry is the rollback. Evidence: $ROUND"
exit 0
