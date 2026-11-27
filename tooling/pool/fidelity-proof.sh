#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host proof of T-20 plan A's copies (ANALYSIS-A.md check C14), both volumes, run as root on the Mac: the archives of the pinned OS
# (141-22233-021.dmg 40d8282e...) and SystemOS (systemos.img a5346509...) windows made by make-volume-archive.sh, then
# tooling/pool/pool-device.sh itself (preflight, create, extract, verify for os and systemos, fstab, undo) against a stand-in for the
# owner's Data container (a fresh APFS container in a sparse image, its own "Data" volume) with both pinned images attached read-only
# as the "old windows" (the window hash checks run for real). Negative checks: a changed byte and a removed extended attribute make
# verify FAIL; a second create and fstab are no-ops; undo removes only the tool's volumes and gives fstab back.
#
#   sudo fidelity-proof.sh [OUT]       (default out/pool/proof)
# Set POOL_OS_IMAGE and POOL_SYSTEMOS_IMAGE to the input images.
set -uo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$D/out/pool/proof}
OS_IMG=${POOL_OS_IMAGE:?set POOL_OS_IMAGE to the read-only OS image}
OS_SHA=${POOL_OS_SHA:-40d8282eb8650da9b4d7437a981764c48d47ee4bdc5b8b362248f25ecbce0e70}
SYS_IMG=${POOL_SYSTEMOS_IMAGE:?set POOL_SYSTEMOS_IMAGE to the read-only SystemOS image}
SYS_SHA=${POOL_SYSTEMOS_SHA:-a5346509f5a6db99a4b317c91a25494f47c36cab06d3a1f8d431bb6804ae68f7}
[ "$(id -u)" = 0 ] || { echo 'run as root' >&2; exit 1; }
# the stand-in windows are attached read-only images (quiesced): plan A's raw window hash applies (T-20 v2: pool-device.sh's default is off)
export POOL_CHECK_WINDOW=1
mkdir -p "$OUT"
LOG=$OUT/proof.log
: > "$LOG"
say() { echo "$*" | tee -a "$LOG"; }
fails=0
check() { if [ "$1" = 0 ]; then say "  ok    $2"; else say "  FAIL  $2"; fails=$((fails + 1)); fi; }
P=$D/tooling/pool/pool-device.sh
WORK=$(mktemp -d /private/tmp/pool-proof.XXXXXX)
run() { bash "$P" "$1" "$WORK" ${2:+"$2"} >> "$LOG" 2>&1; }
must() { check "$@"; [ "$1" = 0 ] || { say "STOP: $2 failed (see $LOG)"; cp "$WORK"/*verify.txt "$WORK"/state.env "$OUT/" 2>/dev/null; run undo; exit 1; }; }
archive() {   # VOL IMG SHA: OUT dir
    local dir=$D/out/pool/$1-${3:0:8}
    if [ ! -f "$dir/archive.env" ] || [ "$(sed -n 's/^source_sha256=//p' "$dir/archive.env")" != "$3" ] || [ ! -f "$dir/volume.aar" ]; then
        bash "$D/tooling/pool/make-volume-archive.sh" "$2" "$dir" "$3" >> "$LOG" 2>&1 || { say "make-volume-archive.sh $1 failed"; exit 1; }
    fi
    mkdir -p "$WORK/$1"
    for f in "$dir"/volume.aar "$dir"/expected.manifest "$dir"/archive.env "$dir"/afsc-*.list; do cp -c "$f" "$WORK/$1/"; done
}
archive os "$OS_IMG" "$OS_SHA"
archive systemos "$SYS_IMG" "$SYS_SHA"
cp -c "$D/out/pool/pool-manifest" "$WORK/"
printf '#!/bin/sh\necho 27.3\n' > "$WORK/fake-sw_vers"; chmod 755 "$WORK/fake-sw_vers"
SP=$WORK/data-standin.sparseimage
hdiutil create -size 40g -type SPARSE -fs APFS -volname Data "$SP" > /dev/null 2>&1
att=$(diskutil image attach --noMount "$SP" 2>&1)
sphys=$(echo "$att" | awk '$1 ~ /^\/dev\/disk[0-9]+$/ {print $1}' | head -1)
cont=$(echo "$att" | awk '/Apple_APFS_Container/ {print $1}' | tail -1 | sed 's|/dev/||')
data=$(echo "$att" | awk '/Apple_APFS_Volume/ {print $1; exit}')
oatt=$(diskutil image attach --readOnly --noMount "$OS_IMG" 2>&1); ophys=$(echo "$oatt" | awk '$1 ~ /^\/dev\/disk[0-9]+$/ {print $1}' | head -1)
patt=$(diskutil image attach --readOnly --noMount "$SYS_IMG" 2>&1); pphys=$(echo "$patt" | awk '$1 ~ /^\/dev\/disk[0-9]+$/ {print $1}' | head -1)
cleanup() { umount "$WORK/mnt" 2>/dev/null; for d in "$sphys" "$ophys" "$pphys"; do hdiutil detach "$d" -force > /dev/null 2>&1; done; rm -rf "$WORK"; }
trap cleanup EXIT
say "stand-in container $cont ($sphys); the pinned OS window at $ophys, SystemOS at $pphys (read-only)"
export POOL_TEST_CONTAINER=$cont POOL_TEST_ROOT_SYSTEMOS=07AD53B3-7565-4004-B574-F45FAC3753FB POOL_SWVERS=$WORK/fake-sw_vers
POOL_DATA_UUID=$(diskutil info -plist "$data" | plutil -extract VolumeUUID raw -o - -) || exit 1
export POOL_DATA_UUID
export POOL_FSTAB=$WORK/fstab-test
printf '# an existing fstab line of the owner\nLABEL=Something none hfs rw,noauto\n' > "$POOL_FSTAB"
FSTAB0=$(shasum -a 256 "$POOL_FSTAB" | awk '{print $1}')
t0=$(date +%s)
for v in os systemos; do run preflight $v; must $? "preflight $v (the window hash = the archive source, free space)"; done
for v in os systemos; do run create $v; must $? "create $v: diskutil apfs addVolume, no role"; done
run create os; check $? 'create os again: a no-op'
[ "$(diskutil list "$cont" | grep -c -E 'PianoOS|PianoSystemOS')" = 2 ]; check $? 'two tool volumes in the container'
for v in os systemos; do
    t1=$(date +%s); run extract $v; must $? "extract $v ($(( $(date +%s) - t1 )) s)"
    t1=$(date +%s); run verify $v; must $? "verify $v: the copy = the archive volume ($(( $(date +%s) - t1 )) s)"
    grep -E '^(EQUAL|DIFFERENT)' "$WORK/$v-verify.txt" | tee -a "$LOG"
done
grep -E '_(inline_uncompressed|restricted_settable|window_read_seconds|extract_seconds|volume_uuid)=' "$WORK/state.env" | sed 's/^/  state: /' | tee -a "$LOG"
for v in os systemos; do
    u=$(sed -n "s/^${v}_volume_uuid=//p" "$WORK/state.env")
    diskutil info "$u" | grep -E 'Volume Name|Volume Used Space|Role' | sed "s/^/  $v copy: /" | tee -a "$LOG"
done
cp "$WORK"/*-verify.txt "$WORK/state.env" "$OUT/" 2>/dev/null
run fstab; check $? 'fstab: the tagged block'
run fstab; check $? 'fstab again: a no-op'
[ "$(grep -c 'none apfs ro,noauto$' "$POOL_FSTAB")" = 4 ] && grep -q '^LABEL=Something' "$POOL_FSTAB"; check $? 'fstab: four noauto lines, the owner line kept'
# negatives: one changed byte (os), one removed extended attribute (systemos): verify must refuse
odisk=$(sed -n 's/^os_volume_disk=//p' "$WORK/state.env"); sdisk=$(sed -n 's/^systemos_volume_disk=//p' "$WORK/state.env")
mkdir -p "$WORK/mnt"; mount -t apfs -o nobrowse "/dev/$odisk" "$WORK/mnt"
# a file SIP lets root change (no SF_RESTRICTED; most of the OS volume has it) and the change itself checked first
f=$(cd "$WORK/mnt" && find usr/share -type f -size +10k ! -flags +compressed ! -flags +restricted 2>/dev/null | head -1)
cp "$WORK/mnt/$f" "$WORK/saved-file"
b=$(dd if="$WORK/mnt/$f" bs=1 skip=100 count=1 2>/dev/null); [ "$b" = X ] && c=Y || c=X
printf '%s' "$c" | dd of="$WORK/mnt/$f" bs=1 seek=100 conv=notrunc 2>/dev/null
cmp -s "$WORK/mnt/$f" "$WORK/saved-file"; changed=$?
umount "$WORK/mnt"
run verify os; [ $changed != 0 ] && [ $? != 0 ] || true
grep -qE 'data [0-9]+' "$WORK/os-verify.txt" && [ $changed != 0 ]; check $? "verify os refuses one changed byte ($f, changed: $([ $changed != 0 ] && echo yes || echo NO))"
mount -t apfs -o nobrowse "/dev/$sdisk" "$WORK/mnt"
x=$(cd "$WORK/mnt" && for c in System/Library/CoreServices/*; do xattr "$c" 2>/dev/null | grep -q com.apple.provenance && { echo "$c"; break; }; done)
xattr -d com.apple.provenance "$WORK/mnt/$x" 2>/dev/null
umount "$WORK/mnt"
run verify systemos; [ $? != 0 ] && grep -q 'xattrs' "$WORK/systemos-verify.txt"; check $? "verify systemos refuses a removed extended attribute ($x)"
run extract os; run verify os; check $? 'os: extract + verify again: repaired, equal'
run extract systemos; run verify systemos; check $? 'systemos: extract + verify again: repaired, equal'
uo=$(sed -n 's/^os_volume_uuid=//p' "$WORK/state.env")
POOL_TEST_ROOT_SYSTEMOS=$(sed -n 's/^systemos_volume_uuid=//p' "$WORK/state.env") bash "$P" undo "$WORK" >> "$LOG" 2>&1; [ $? != 0 ]
check $? 'undo refused while / runs on a copy'
run undo; check $? 'undo'
[ "$(diskutil list "$cont" | grep -c -E 'PianoOS|PianoSystemOS')" = 0 ] && diskutil list "$cont" | grep -q ' Data '; check $? 'undo: the tool volumes gone, the Data volume kept'
[ "$(shasum -a 256 "$POOL_FSTAB" | awk '{print $1}')" = "$FSTAB0" ]; check $? 'undo: fstab byte for byte as before'
say "$([ $fails = 0 ] && echo PASS || echo FAIL): $fails failure(s) in $(( $(date +%s) - t0 )) s"
chown -R "${SUDO_UID:-0}:${SUDO_GID:-0}" "$OUT"
exit $([ $fails = 0 ] && echo 0 || echo 1)
