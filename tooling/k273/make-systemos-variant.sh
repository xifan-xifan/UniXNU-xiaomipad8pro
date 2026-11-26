#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# A modified copy of the 27.3 line's SystemOS window (tooling/k273/make-systemos.sh's output): a Metal plugin bundle added under
# /System/Library/Extensions, the one plugin directory WebKit's GPU process may map executable (GPUProcess.sb; ANALYSIS.md section 10 of
# private/host-checks/20261124-browser-gpu). The copy keeps the pinned window's volume UUID, so the ADT's owner-systemos-uuid and every
# userland reference stay as they are; it may therefore only be published on a boot that does not also publish the pinned window.
#
#   make-systemos-variant.sh PINNED.img PINNED_SHA256 BUNDLE OUT.img [--restricted]
#
#   PINNED.img     the pinned window image (out/inputs/k273/root/systemos.img, a5346509...): only read, its sha256 checked before
#                  and after
#   BUNDLE         the plugin bundle (<name>.bundle/Contents/{Info.plist,MacOS/...}); its Mach-Os must be in the trust cache of the
#                  image that boots the copy (their CDHashes are printed and recorded)
#   OUT.img        the copy (a clone of PINNED where the file system allows it); must not exist, must not be PINNED, must not be
#                  under out/frozen or $PIANO_BACKUP_DIR
#   VARIANT_PLACE=app (env; default sle): put the bundle under the App cryptex,
#                  System/Volumes/Preboot/Cryptexes/App/System/Library/Extensions/<name>, and make
#                  System/Library/Extensions/<name> an absolute symlink to /System/Volumes/Preboot/Cryptexes/App/System/Library/Extensions/<name>.
#                  Chrome's GPU sandbox (common.sb) maps executable code only from the cryptex paths, and a sandbox matches a symlink's
#                  real path (LT-B3, ANALYSIS.md section 11). Metal's trusted bundle search follows the symlink (live3).
#   --restricted   also give the bundle Apple's own plugins' protection: SF_RESTRICTED on every entry and an empty com.apple.rootless
#                  attribute on the bundle directory (AppleParavirtGPUMetalIOGPUFamily.bundle carries exactly these in the 27.3 OS
#                  volume). The tablet runs with SIP on, so such files can never be changed there: only for a window that is replaced
#                  as a whole.
#
# sudo (SUDO_ASKPASS=... sudo -A, set SUDO='sudo -A') is used for: the copy's mount with ownership, ditto, chown/chmod/chflags/xattr
# inside the mounted copy. Nothing outside OUT is written; PINNED is attached by nobody.
# Output: OUT.img and OUT.img.json {base, base_sha256, volume_uuid, size, sha256, bundle, files: [{path, sha256, cdhash}], restricted}.
set -euo pipefail
PINNED=${1:?PINNED.img} PINNED_SHA=${2:?PINNED_SHA256} BUNDLE=${3:?BUNDLE} OUT=${4:?OUT.img} FLAG=${5:-}
SUDO=${SUDO:-sudo}
D=$(cd "$(dirname "$0")/../.." && pwd)
RESTRICTED=0
PLACE=${VARIANT_PLACE:-sle}
case "$PLACE" in sle|app) ;; *) echo "VARIANT_PLACE must be sle or app" >&2; exit 2 ;; esac
APPEXT=System/Volumes/Preboot/Cryptexes/App/System/Library/Extensions
case "$FLAG" in --restricted) RESTRICTED=1 ;; '') ;; *) echo "unknown option $FLAG" >&2; exit 2 ;; esac
real() { python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1"; }
[ -f "$PINNED" ] || { echo "no $PINNED" >&2; exit 1; }
[ -f "$BUNDLE/Contents/Info.plist" ] && [ -d "$BUNDLE/Contents/MacOS" ] || { echo "$BUNDLE: not a plugin bundle" >&2; exit 1; }
NAME=$(basename "$BUNDLE")
case "$NAME" in *.bundle) ;; *) echo "$BUNDLE: the name must end in .bundle" >&2; exit 1 ;; esac
[ -e "$OUT" ] && { echo "$OUT exists" >&2; exit 1; }
OUT_DIR=$(real "$(dirname "$OUT")"); OUT_REAL=$OUT_DIR/$(basename "$OUT")
[ "$OUT_REAL" = "$(real "$PINNED")" ] && { echo "OUT is the pinned image" >&2; exit 1; }
case "$OUT_REAL/" in */out/frozen/*) echo "refusing to write under out/frozen" >&2; exit 1 ;; esac
if [ -n "${PIANO_BACKUP_DIR:-}" ] && [ -d "$PIANO_BACKUP_DIR" ]; then
  case "$OUT_REAL/" in "$(real "$PIANO_BACKUP_DIR")"/*) echo "refusing to write under PIANO_BACKUP_DIR" >&2; exit 1 ;; esac
fi
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
echo "checking the pinned image's sha256" >&2
[ "$(sha "$PINNED")" = "$PINNED_SHA" ] || { echo "$PINNED: sha256 is not $PINNED_SHA" >&2; exit 1; }

WORK=$(mktemp -d /tmp/systemos-variant.XXXXXX)
WHOLE=""
cleanup() {
  if [ -n "$WHOLE" ]; then
    mount | grep -q " on $(cd "$WORK/mnt" 2>/dev/null && pwd -P) " && $SUDO diskutil unmount force "$WORK/mnt" > /dev/null 2>&1 || true
    hdiutil detach -force "$WHOLE" > /dev/null 2>&1 || true
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$WORK/mnt"
cp -c "$PINNED" "$OUT" 2> /dev/null || cp "$PINNED" "$OUT"
chmod u+w "$OUT"
[ "$(sha "$OUT")" = "$PINNED_SHA" ] || { echo "the copy's sha256 differs from the pinned one" >&2; rm -f "$OUT"; exit 1; }

# The copy's disk (checked by its image path), its one APFS volume, mounted read-write with ownership by hdiutil alone.
DISKOF="python3 $D/tooling/k273/disk-of.py"
hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -nobrowse "$OUT" > /dev/null
WHOLE=$($DISKOF whole "$OUT")
[ "$(diskutil info -plist "$WHOLE" | plutil -extract TotalSize raw -o - -)" = "$(stat -f %z "$OUT")" ] \
  && diskutil info "$WHOLE" | grep -q "Disk Image" || { echo "$WHOLE is not $OUT's disk image" >&2; exit 1; }
VOLUME=$($DISKOF volume "$WHOLE")
UUID=$($DISKOF uuid "$VOLUME")
$SUDO mount -t apfs "$VOLUME" "$WORK/mnt"
M=$WORK/mnt
$SUDO diskutil enableOwnership "$M" > /dev/null
[ -d "$M/System/Library/Extensions" ] && [ -d "$M/System/Volumes/Preboot/Cryptexes/App" ] \
  || { echo "$OUT: not a SystemOS window (no System/Library/Extensions or Preboot/Cryptexes/App)" >&2; exit 1; }
LINK=$M/System/Library/Extensions/$NAME
[ -e "$LINK" ] || [ -L "$LINK" ] && { echo "$OUT already has System/Library/Extensions/$NAME" >&2; exit 1; }
if [ "$PLACE" = app ]; then
  REL=$APPEXT/$NAME; T=$M/$REL
  [ -e "$T" ] && { echo "$OUT already has $REL" >&2; exit 1; }
  [ -d "$M/$APPEXT" ] || { $SUDO mkdir "$M/$APPEXT"; $SUDO chown root:wheel "$M/$APPEXT"; $SUDO chmod 755 "$M/$APPEXT"; }
else
  REL=System/Library/Extensions/$NAME; T=$LINK
fi
$SUDO ditto --noextattr --noacl "$BUNDLE" "$T"
$SUDO chown -R root:wheel "$T"
$SUDO find "$T" -type d -exec chmod 755 {} +
$SUDO find "$T" -type f -exec sh -c 'for f; do if [ -x "$f" ]; then chmod 755 "$f"; else chmod 644 "$f"; fi; done' sh {} +
$SUDO xattr -cr "$T" 2> /dev/null || true
if [ "$PLACE" = app ]; then
  $SUDO ln -s "/$REL" "$LINK"
  $SUDO chown -h root:wheel "$LINK"
  [ "$(readlink "$LINK")" = "/$REL" ] || { echo "the symlink is not /$REL" >&2; exit 1; }
fi
if [ "$RESTRICTED" = 1 ]; then
  $SUDO xattr -w com.apple.rootless "" "$T"
  $SUDO chflags -R restricted "$T"
fi
FILES=$(cd "$T" && find . -type f | sort | while read -r f; do
  f=${f#./}
  c=$(python3 - "$T/$f" "$D" <<'PY'
import importlib.util, sys
from pathlib import Path
spec = importlib.util.spec_from_file_location('ssm', Path(sys.argv[2]) / 'tooling/k27/stage-soft-metal.py')
sys.path.insert(0, str(Path(sys.argv[2]) / 'tooling/k27'))
try:
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    h = m.cdhash(Path(sys.argv[1]))
except SystemExit:
    h = None
print(h.hex() if h else '-')
PY
)
  printf '%s %s %s\n' "$(shasum -a 256 "$T/$f" | cut -d' ' -f1)" "$c" "$f"
done)
SRC_FILES=$(cd "$BUNDLE" && find . -type f | sort | while read -r f; do printf '%s %s\n' "$(shasum -a 256 "$f" | cut -d' ' -f1)" "${f#./}"; done)
[ "$(printf '%s\n' "$FILES" | awk '{print $1, $3}')" = "$SRC_FILES" ] || { echo "the copied bundle differs from $BUNDLE" >&2; exit 1; }
echo "$REL in the copy:" >&2
ls -lnaO@R "$T" >&2
[ "$PLACE" = app ] && ls -lnaO@ "$LINK" >&2
sync
$SUDO umount "$M"
hdiutil detach "$WHOLE" > /dev/null
WHOLE=""
SHA=$(sha "$OUT")
[ "$(sha "$PINNED")" = "$PINNED_SHA" ] || { echo "the pinned image changed during the run" >&2; exit 1; }
python3 - "$OUT.json" "$(real "$PINNED")" "$PINNED_SHA" "$UUID" "$(stat -f %z "$OUT")" "$SHA" "$NAME" "$RESTRICTED" "$FILES" "$PLACE" "$REL" <<'PY'
import json, sys
out, base, base_sha, uuid, size, sha, name, restricted, files, place, rel = sys.argv[1:12]
rows = [line.split(' ', 2) for line in files.splitlines() if line]
json.dump({'base': base, 'base_sha256': base_sha, 'volume_uuid': uuid, 'size': int(size), 'sha256': sha,
           'bundle': rel, 'place': place,
           'link': (f'System/Library/Extensions/{name} -> /{rel}' if place == 'app' else None), 'restricted': restricted == '1',
           'files': [{'path': p, 'sha256': s, 'cdhash': None if c == '-' else c} for s, c, p in rows]},
          open(out, 'w'), indent=1)
PY
cat "$OUT.json"
