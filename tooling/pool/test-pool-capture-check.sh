#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of tooling/pool/pool-capture-check.sh (no root, no tablet). By default the disk commands are simulated and the files
# carry fixture NXSB headers. Use --real-apfs to create and attach fresh APFS images with hdiutil/diskutil.
set -u
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d /tmp/pool-capcheck.XXXXXX)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if [ "$1" = 0 ]; then echo "  ok    $2"; else echo "  FAIL  $2"; fails=$((fails + 1)); fi; }
mk() {   # NAME VOLNAME
    hdiutil create -quiet -size 24m -fs APFS -volname "$2" -layout NONE "$T/$1.dmg" 2>/dev/null &&
        hdiutil convert -quiet "$T/$1.dmg" -format UDTO -o "$T/$1" 2>/dev/null && mv "$T/$1.cdr" "$T/w/$1.img" && rm -f "$T/$1.dmg"
}
mkdir -p "$T/w"
uuid() { local a p v; a=$(diskutil image attach --readOnly --noMount "$1" 2>&1); p=$(echo "$a" | grep -oE '^/dev/disk[0-9]+' | head -1)
         v=$(echo "$a" | awk '/Apple_APFS_Volume/ {print $1}' | head -1); diskutil info -plist "$v" | plutil -extract VolumeUUID raw -o - -
         hdiutil detach "$p" > /dev/null 2>&1; }
case ${1:-} in
    --real-apfs)
        mk os 'Macintosh HD' && mk systemos SystemOS || { echo 'FAIL: could not make the test images'; exit 1; }
        POOL_EXPECT_OS_UUID=$(uuid "$T/w/os.img") && POOL_EXPECT_SYSTEMOS_UUID=$(uuid "$T/w/systemos.img") || exit 1
        ;;
    '')
        mkdir "$T/bin"
        cat > "$T/bin/diskutil" <<'SH'
#!/bin/bash
case "$*" in
    'image attach --readOnly --noMount '*'/w/os.img') printf '/dev/disk700\n/dev/disk700s1 Apple_APFS_Volume\n';;
    'image attach --readOnly --noMount '*'/w/systemos.img') printf '/dev/disk701\n/dev/disk701s1 Apple_APFS_Volume\n';;
    'info -plist /dev/disk700s1') printf '{ VolumeUUID = "00000000-0000-4000-8000-000000000001"; }\n';;
    'info -plist /dev/disk701s1') printf '{ VolumeUUID = "00000000-0000-4000-8000-000000000002"; }\n';;
    *) exit 1;;
esac
SH
        cat > "$T/bin/hdiutil" <<'SH'
#!/bin/bash
[ "$*" = 'detach /dev/disk700' ] || [ "$*" = 'detach /dev/disk701' ]
SH
        chmod +x "$T/bin/diskutil" "$T/bin/hdiutil"
        export PATH="$T/bin:$PATH"
        POOL_EXPECT_OS_UUID=00000000-0000-4000-8000-000000000001
        POOL_EXPECT_SYSTEMOS_UUID=00000000-0000-4000-8000-000000000002
        python3 - "$T/w" <<'PY' || exit 1
import sys
from pathlib import Path

for name in ('os', 'systemos'):
    with (Path(sys.argv[1]) / f'{name}.img').open('wb') as image:
        image.truncate(24 << 20)
        image.seek(32)
        image.write(b'NXSB')
PY
        ;;
    *) echo 'usage: test-pool-capture-check.sh [--real-apfs]' >&2; exit 2;;
esac
export POOL_EXPECT_OS_UUID POOL_EXPECT_SYSTEMOS_UUID
record() {
    python3 - "$T/w" <<'PY'
import hashlib, json, sys
from pathlib import Path
w = Path(sys.argv[1]); out = {'windows': {}}
for v in ('os', 'systemos'):
    b = (w / f'{v}.img').read_bytes()
    out['windows'][v] = {'size': len(b), 'sha256': hashlib.sha256(b).hexdigest(), 'nx_uuid': b[72:88].hex()}
(w / 'CAPTURE.json').write_text(json.dumps(out))
PY
}
record
echo "== pool-capture-check.sh (${1:---simulated-disks})"
bash "$D/tooling/pool/pool-capture-check.sh" "$T/w" > "$T/out.txt" 2>&1; check $? "the good capture: $(tail -1 "$T/out.txt")"
[ "$(grep -c '^PASS' "$T/out.txt")" = 10 ]; check $? 'ten gates PASS (five per window)'
cp "$T/w/systemos.img" "$T/keep.img"; printf 'Z' | dd of="$T/w/systemos.img" bs=1 seek=$((12 << 20)) conv=notrunc 2>/dev/null
bash "$D/tooling/pool/pool-capture-check.sh" "$T/w" > "$T/out.txt" 2>&1; [ $? = 1 ] && grep -q 'FAIL systemos: sha256 without' "$T/out.txt"
check $? 'a changed byte: FAIL (sha256)'
cp "$T/keep.img" "$T/w/systemos.img"
POOL_EXPECT_SYSTEMOS_UUID=07AD53B3-7565-4004-B574-F45FAC3753FB bash "$D/tooling/pool/pool-capture-check.sh" "$T/w" > "$T/out.txt" 2>&1
[ $? = 1 ] && grep -q 'FAIL systemos: attached read-only' "$T/out.txt"; check $? 'a volume that is not the expected one: FAIL'
printf 'x' >> "$T/w/os.img"
bash "$D/tooling/pool/pool-capture-check.sh" "$T/w" > "$T/out.txt" 2>&1; [ $? = 1 ] && grep -q 'FAIL os: ' "$T/out.txt"; check $? 'another size: FAIL'
truncate -s 25165824 "$T/w/os.img"; printf 'XXXX' | dd of="$T/w/os.img" bs=1 seek=32 conv=notrunc 2>/dev/null; record
bash "$D/tooling/pool/pool-capture-check.sh" "$T/w" > "$T/out.txt" 2>&1; [ $? = 1 ] && grep -q 'FAIL os: APFS container superblock' "$T/out.txt"
check $? 'no NXSB at the window start: FAIL'
echo "$([ $fails = 0 ] && echo PASS || echo FAIL): $fails failure(s)"
exit $([ $fails = 0 ] && echo 0 || echo 1)
