#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of tooling/pool/pool-device.sh against a mock tablet (tooling/pool/mock-tablet.sh: no root, no disk, no tablet), both
# volumes (os, systemos): the real aa and pool-manifest on two small made-up trees. Every refusal of preflight, the idempotent steps,
# a tampered copy refused by verify, the fstab block added once and given back byte for byte, undo refused while / runs on a copy and
# never touching a volume it did not make.
#
#   test-pool-device.sh [--keep]
set -u
D=$(cd "$(dirname "$0")/../.." && pwd)
P=$D/tooling/pool/pool-device.sh
T=$(mktemp -d /tmp/pool-dev-test.XXXXXX)
[ "${1:-}" = --keep ] || trap 'chmod -R u+w "$T" 2>/dev/null; rm -rf "$T"' EXIT
TOOL=$T/pool-manifest
bash "$D/tooling/pool/build-pool-manifest.sh" "$TOOL" > /dev/null || exit 1
fails=0
check() { if [ "$1" = 0 ]; then echo "  ok    $2"; else echo "  FAIL  $2"; fails=$((fails + 1)); fi; }
OS_OLD=84581E99-85BE-4957-AC53-2947E887B3E4
SYS_OLD=07AD53B3-7565-4004-B574-F45FAC3753FB
M=$T/mock
W=$T/work
mkdir -p "$W/os" "$W/systemos"
bash "$D/tooling/pool/mock-tablet.sh" "$M"
cp "$TOOL" "$W/pool-manifest"
# -- two small volumes, their archives and manifests (as make-volume-archive.sh makes them) ---------------------------------------
mkvol() {   # VOL OLD_UUID RAWDISK
    local s=$T/src-$1
    mkdir -p "$s/usr/lib" "$s/System/Library"
    for i in 1 2 3; do head -c $((i * 5000)) /dev/urandom > "$s/usr/lib/lib$i.dylib"; done
    echo strings > "$s/System/Library/Localizable.strings"; echo dict > "$s/System/Library/Localizable.stringsdict"
    ln "$s/usr/lib/lib1.dylib" "$s/usr/lib/lib1-link.dylib"
    ln -s lib2.dylib "$s/usr/lib/libalias.dylib"
    xattr -w com.example.a "$1" "$s/usr/lib/lib3.dylib"
    "$TOOL" "$s" > "$W/$1/expected.manifest"
    aa archive -d "$s" -o "$W/$1/volume.aar" -include-field attr,xat,acl,sh2
    head -c 1048576 /dev/urandom > "$M/raw-$3"
    printf 'archive_sha256=%s\nmanifest_file_sha256=%s\nsource_sha256=%s\nsource_volume_uuid=%s\nsource_personality=APFS\nfamilies=\n' \
        "$(shasum -a 256 "$W/$1/volume.aar" | awk '{print $1}')" "$(shasum -a 256 "$W/$1/expected.manifest" | awk '{print $1}')" \
        "$(shasum -a 256 "$M/raw-$3" | awk '{print $1}')" "$2" > "$W/$1/archive.env"
}
mkvol os "$OS_OLD" disk4
mkvol systemos "$SYS_OLD" disk5
export MOCKD=$M POOL_DISKUTIL=$M/bin/diskutil POOL_MOUNT=$M/bin/mount POOL_UMOUNT=$M/bin/umount POOL_CHFLAGS=$M/bin/chflags
export POOL_ID=$M/bin/id POOL_SWVERS=$M/bin/sw_vers POOL_FSTAB=$T/fstab POOL_RAWDEV_PREFIX=$M/raw- MOCK_ROOT_UUID=$SYS_OLD
export POOL_DATA_UUID=DA7A0000-0000-4000-8000-000000000001
printf 'LABEL=Owner none hfs rw,noauto\n' > "$POOL_FSTAB"
FSTAB0=$(shasum -a 256 "$POOL_FSTAB" | awk '{print $1}')
run() { bash "$P" "$1" "$W" ${2:+"$2"} > "$T/last.log" 2>&1; }
refused() {   # VOL WHY ENV...: preflight of VOL with ENV refuses, saying WHY
    local vol=$1 why=$2; shift 2
    env "$@" bash "$P" preflight "$W" "$vol" > "$T/last.log" 2>&1; local rc=$?
    [ $rc != 0 ] && grep -q "$why" "$T/last.log"; check $? "preflight $vol refuses: $why"
}
echo '== pool-device.sh against a mock tablet (os + systemos)'
bash "$P" preflight "$W" > "$T/last.log" 2>&1; [ $? = 2 ]; check $? 'a volume step without os|systemos: usage'
refused os 'not root' MOCK_UID=501
refused os 'not macOS 27.3' MOCK_VERSION=27.2
refused os 'set POOL_DATA_UUID' POOL_DATA_UUID=
refused os 'not the owner.s Data' MOCK_DATA_UUID=00000000-0000-0000-0000-000000000000
refused os 'bytes free' MOCK_FREE=1000
refused os 'unknown origin' MOCK_FOREIGN=PianoOS
refused systemos 'unknown origin' MOCK_FOREIGN=PianoSystemOS
refused os 'boot the old entry' MOCK_ROOT_UUID=AAAAAAAA-0000-0000-0000-000000000000
cp "$M/raw-disk4" "$M/raw-good"; printf 'x' | dd of="$M/raw-disk4" bs=1 seek=7 conv=notrunc 2>/dev/null
refused os 'make the archive from this window' POOL_CHECK_WINDOW=1
cp "$M/raw-good" "$M/raw-disk4"
cp "$W/systemos/volume.aar" "$T/aar-good"; printf 'x' >> "$W/systemos/volume.aar"
refused systemos 'volume.aar hashes to'
cp "$T/aar-good" "$W/systemos/volume.aar"
cp "$W/os/archive.env" "$T/env-good"; sed -i '' "s/^source_volume_uuid=.*/source_volume_uuid=$SYS_OLD/" "$W/os/archive.env"
refused os 'the os archive is of volume'
cp "$T/env-good" "$W/os/archive.env"
run create os; [ $? != 0 ] && grep -q 'preflight has not passed' "$T/last.log"; check $? 'create refused before preflight'
run preflight os; check $? 'preflight os (the OS window = its archive source)'
run preflight systemos; check $? 'preflight systemos'
grep -q '^os_window_check=live$' "$W/state.env"; check $? 'preflight: the raw window read off (R1a/R1b instead), recorded'
run create os; [ $? != 0 ] && grep -q 'R1b: no accepted live comparison' "$T/last.log"; check $? 'create refused without the R1b verdict'
run live; [ $? != 0 ] && grep -q 'no live-paths.txt' "$T/last.log"; check $? 'live refused without the host list'
awk -F'\t' 'NF == 14 { print "s\t" $1 }' "$W/systemos/expected.manifest" > "$W/live-paths.txt"
"$M/bin/mount" > "$W/live-mounts.txt"
POOL_LIVE_ROOT=$T/src-systemos run live; check $? 'live: the listed paths read through the live root'
grep -q '^# entries .* missing 0 .* errors 0$' "$W/live.txt" && [ "$(sed -n 's/^live_sha256=//p' "$W/state.env")" = "$(shasum -a 256 "$W/live.txt" | awk '{print $1}')" ]
check $? 'live: live.txt complete, its sha256 in the state'
python3 "$D/tooling/pool/pool-live-check.py" compare --systemos "$W/systemos/expected.manifest" --os /dev/null --mounts "$W/live-mounts.txt" --list "$W/live-paths.txt" --live "$W/live.txt" > /dev/null 2>&1
[ $? = 2 ]; check $? 'the comparator refuses an incomplete OS manifest (VOID)'
bash "$P" accept-live "$W" 0000000000000000000000000000000000000000000000000000000000000000 > "$T/last.log" 2>&1; [ $? != 0 ] && grep -q 'the verdict is for' "$T/last.log"
check $? 'accept-live refuses a verdict for another live output'
printf 'devfs on /dev (devfs)\n' > "$M/mounts.txt"; run live; [ $? != 0 ] && grep -q 'not the ones the list was made from' "$T/last.log"
check $? 'live refused when the mounts changed since the list'
rm -f "$M/mounts.txt"; POOL_LIVE_ROOT=$T/src-systemos run live
bash "$P" accept-live "$W" "$(shasum -a 256 "$W/live.txt" | awk '{print $1}')" > "$T/last.log" 2>&1; check $? 'accept-live for exactly that live.txt'
run create os; check $? 'create os'
run create systemos; check $? 'create systemos'
grep -q 'apfs addVolume disk3 APFS PianoOS -nomount' "$M/calls" && grep -q 'apfs addVolume disk3 APFS PianoSystemOS -nomount' "$M/calls"
check $? 'create: addVolume PianoOS and PianoSystemOS on the Data container, no -role'
run create os; [ $? = 0 ] && [ "$(grep -c addVolume "$M/calls")" = 2 ]; check $? 'create again: no third addVolume'
run fstab; [ $? != 0 ] && grep -q 'both copies verified first' "$T/last.log"; check $? 'fstab refused before both are verified'
run extract os; [ $? != 0 ] && grep -q 'SF_RESTRICTED cannot be set' "$T/last.log"; check $? 'extract refuses when SF_RESTRICTED cannot be set'
export POOL_ACCEPT_UNRESTRICTED=1
printf '#!/bin/sh\nexit 1\n' > "$T/umount-fails"
chmod +x "$T/umount-fails"
POOL_UMOUNT=$T/umount-fails run extract os; [ $? != 0 ] && ! grep -qx 'os_extracted=yes' "$W/state.env"
check $? 'extract refuses to certify a volume it could not unmount'
"$M/bin/umount" "$W/mnt" || exit 1
run extract os; check $? 'extract os (POOL_ACCEPT_UNRESTRICTED=1)'
run verify os; check $? 'verify os'
run extract systemos; check $? 'extract systemos'
run verify systemos; check $? 'verify systemos'
grep -q '^os_restricted_settable=no' "$W/state.env" && grep -q '^systemos_verified=yes' "$W/state.env"; check $? 'state: both volumes, the relaxation recorded'
[ -f "$M/vols/disk3s10/System/Library/Localizable.stringsdict" ] && xattr -p com.example.a "$M/vols/disk3s9/usr/lib/lib3.dylib" | grep -qx systemos
check $? 'each copy holds its own volume (os on disk3s10, systemos on disk3s9)'
run fstab; check $? 'fstab'
sed -i '' '/^fstab=yes$/d' "$W/state.env"
run fstab; [ $? = 0 ] && [ "$(grep -c 'apfs ro,noauto$' "$POOL_FSTAB")" = 4 ]; check $? 'fstab again: still four lines'
grep -qx 'fstab=yes' "$W/state.env"; check $? 'fstab retry restores the state marker after an interrupted run'
grep -q '^LABEL=Owner' "$POOL_FSTAB" && grep -q "^UUID=$OS_OLD none apfs ro,noauto$" "$POOL_FSTAB"; check $? 'fstab: the owner line kept, the old windows noauto'
chmod u+w "$M/vols/disk3s10/usr/lib/lib2.dylib" 2>/dev/null; printf 'x' | dd of="$M/vols/disk3s10/usr/lib/lib2.dylib" bs=1 seek=10 conv=notrunc 2>/dev/null
run verify os; [ $? != 0 ] && grep -q 'data 1' "$W/os-verify.txt"; check $? 'verify refuses a changed byte'
! grep -qx 'os_verified=yes' "$W/state.env"; check $? 'a failed recheck invalidates the previous verification'
run extract os && run verify os; check $? 'extract + verify again: equal'
MOCK_ROOT_UUID=9A0B0C0D-1111-2222-3333-444455556666 bash "$P" undo "$W" > "$T/last.log" 2>&1; [ $? != 0 ] && grep -q 'boot macos-27.3 first' "$T/last.log"
check $? 'undo refused while / runs on the copy'
MOCK_ROOT_UUID=9A0B0C0D-1111-2222-3333-444455556666 bash "$P" extract "$W" os > "$T/last.log" 2>&1; [ $? != 0 ] && grep -q 'never written while booted' "$T/last.log"
check $? 'extract refused while / runs on a copy'
MOCK_ROOT_UUID= run extract os; [ $? != 0 ] && grep -q 'cannot identify the root volume' "$T/last.log"
check $? 'extract refuses when the root volume cannot be identified'
MOCK_ROOT_UUID= run undo; [ $? != 0 ] && grep -q 'cannot identify the root volume' "$T/last.log" &&
    [ -f "$M/vol-PianoOS.uuid" ] && [ -f "$M/vol-PianoSystemOS.uuid" ]
check $? 'undo preserves both volumes when the root volume cannot be identified'
MOCK_NEWNAME=Other bash "$P" undo "$W" > "$T/last.log" 2>&1; [ $? != 0 ] && grep -q 'not deleted' "$T/last.log" && [ -f "$M/vol-PianoOS.uuid" ]
check $? 'undo refuses a volume whose name is not its own'
run undo; check $? 'undo'
[ ! -f "$M/vol-PianoOS.uuid" ] && [ ! -f "$M/vol-PianoSystemOS.uuid" ] && grep -q 'apfs deleteVolume disk3s9' "$M/calls" && grep -q 'apfs deleteVolume disk3s10' "$M/calls"
check $? 'undo: deleteVolume of the two tool volumes'
[ "$(shasum -a 256 "$POOL_FSTAB" | awk '{print $1}')" = "$FSTAB0" ]; check $? 'undo: fstab byte for byte as before'
run undo; check $? 'undo again: nothing to do'
! grep -qE 'deleteVolume disk3s1$|deleteVolume disk[45]|addVolume.*-role' "$M/calls"; check $? 'never the Data volume, never an old window, never a role'
echo "$([ $fails = 0 ] && echo PASS || echo FAIL): $fails failure(s)"
exit $([ $fails = 0 ] && echo 0 || echo 1)
