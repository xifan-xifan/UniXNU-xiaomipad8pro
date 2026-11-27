#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host rehearsal of PLAN-v2.md's sudo steps S1 + S2 (T-20 v2) on a stand-in capture, as root on the Mac mini (no tablet):
#   sudo -A bash tooling/pool/rehearse-capture-archive.sh [OUT]          (default out/pool/rehearse-capture)
# A small APFS container image (one volume "SystemOS" with files, a hard link, a symlink, xattrs, a compressed file, a restricted flag
# where SIP lets root set it) is made and converted to a raw image: the "capture". Then exactly what S1 and S2 run: fsck_apfs -n on it
# attached read-only, make-volume-archive.sh CAPTURE OUT SHA. Checks: fsck clean; archive.env source_sha256 = the capture's sha256;
# the manifest ends "errors 0" and its entry count = the archive's; the capture's sha256 unchanged by S1 and S2 (read-only attaches).
set -uo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$D/out/pool/rehearse-capture}
[ "$(id -u)" = 0 ] || { echo 'run as root (sudo -A)' >&2; exit 1; }
[ -x "$D/out/pool/pool-manifest" ] || bash "$D/tooling/pool/build-pool-manifest.sh" > /dev/null
rm -rf "$OUT"; mkdir -p "$OUT"
fails=0
check() { if [ "$1" = 0 ]; then echo "  ok    $2"; else echo "  FAIL  $2"; fails=$((fails + 1)); fi; }
T=$(mktemp -d /private/tmp/pool-rcap.XXXXXX)
cleanup() { [ -n "${DEV:-}" ] && hdiutil detach -force "$DEV" > /dev/null 2>&1; rm -rf "$T"; }
trap cleanup EXIT
hdiutil create -quiet -size 64m -fs APFS -volname SystemOS -layout NONE "$T/src.dmg" 2>/dev/null || exit 1   # a bare container, as a window
A=$(hdiutil attach -nobrowse -mountpoint "$T/m" "$T/src.dmg") || exit 1
DEV=$(echo "$A" | awk 'NR==1{print $1}')
mkdir -p "$T/m/System/Library/CoreServices" "$T/m/usr/lib"
echo plist > "$T/m/System/Library/CoreServices/SystemVersion.plist"
head -c 200000 /dev/urandom > "$T/m/usr/lib/libx.dylib"; ln "$T/m/usr/lib/libx.dylib" "$T/m/usr/lib/libx-link.dylib"
ln -s libx.dylib "$T/m/usr/lib/liby.dylib"; xattr -w com.apple.provenance x "$T/m/usr/lib/libx.dylib"
yes piano | head -c 300000 > "$T/m/usr/lib/big.txt"; ditto --hfsCompression "$T/m/usr/lib/big.txt" "$T/m/usr/lib/big-c.txt"
chflags restricted "$T/m/System/Library/CoreServices/SystemVersion.plist" 2>/dev/null || echo '  info  SF_RESTRICTED not settable here (SIP): left out'
hdiutil detach "$DEV" > /dev/null; DEV=''
hdiutil convert -quiet "$T/src.dmg" -format UDTO -o "$T/cap" && mv "$T/cap.cdr" "$OUT/systemos.img"
SHA=$(shasum -a 256 "$OUT/systemos.img" | awk '{print $1}')
echo "== S1 + S2 on a stand-in capture ($SHA)"
# S1, as PLAN-v2.md runs it
A=$(diskutil image attach --readOnly --noMount "$OUT/systemos.img" 2>&1); DEV=$(echo "$A" | grep -oE '^/dev/disk[0-9]+' | head -1)
C=$(echo "$A" | awk '/Apple_APFS_Container/ {print $1}' | head -1)
fsck_apfs -n "/dev/r${C#/dev/}" > "$OUT/fsck.txt" 2>&1; rc=$?
hdiutil detach "$DEV" > /dev/null 2>&1; DEV=''
[ "$rc" = 0 ] && grep -q 'appears to be OK' "$OUT/fsck.txt"; check $? "S1 fsck_apfs -n of the capture: $(tail -1 "$OUT/fsck.txt")"
# S2
bash "$D/tooling/pool/make-volume-archive.sh" "$OUT/systemos.img" "$OUT/systemos-${SHA:0:8}" "$SHA" > "$OUT/archive.log" 2>&1
check $? 'S2 make-volume-archive.sh CAPTURE OUT SHA'
[ "$(sed -n 's/^source_sha256=//p' "$OUT/systemos-${SHA:0:8}/archive.env")" = "$SHA" ]; check $? 'R1a: archive.env source_sha256 = the capture'
tail -1 "$OUT/systemos-${SHA:0:8}/expected.manifest" | grep -q 'errors 0$'; check $? "the manifest: $(tail -1 "$OUT/systemos-${SHA:0:8}/expected.manifest" | cut -c1-60)"
[ "$(shasum -a 256 "$OUT/systemos.img" | awk '{print $1}')" = "$SHA" ]; check $? 'the capture unchanged by S1 and S2'
chown -R "${SUDO_UID:-0}:${SUDO_GID:-0}" "$OUT"
echo "$([ $fails = 0 ] && echo PASS || echo FAIL): $fails failure(s)"
exit $([ $fails = 0 ] && echo 0 || echo 1)
