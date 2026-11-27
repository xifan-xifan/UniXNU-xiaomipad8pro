#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# One volume's archive for T-20 plan A (tooling/pool/pool-device.sh), made on the Mac from a raw window image: the OS window (the
# IPSW's System volume container 141-22233-021.dmg 40d8282e..., the root image's OS window) or the SystemOS window (the pinned
# out/inputs/k273/root/systemos.img a5346509...), or a window pulled from the tablet when the inventory finds it differs. Run as root
# (mounting and reading every file; nothing outside OUT is written; the image is attached read-only, never mounted read-write).
#
#   sudo make-volume-archive.sh RAW_IMAGE OUT [EXPECT_SHA256]
#
# OUT/: volume.aar (Apple Archive: every entry of the volume but its .fseventsd journal, with attr,xat,acl,sh2), expected.manifest
# (tooling/pool/pool-manifest of the same volume, the same exclusion), afsc-<family>.list (the compressed files by decmpfs family:
# afsc-lists.py), pool-manifest (the binary the tablet runs), archive.env (the facts pool-device.sh checks) and archive.json.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
IMG=${1:?raw SystemOS image}
OUT=${2:?output folder}
EXPECT=${3:-}
[ "$(id -u)" = 0 ] || { echo 'run as root' >&2; exit 1; }
TOOL=$D/out/pool/pool-manifest
[ -x "$TOOL" ] || { echo "no $TOOL (tooling/pool/build-pool-manifest.sh)" >&2; exit 1; }
mkdir -p "$OUT"
SRC_SHA=$(shasum -a 256 "$IMG" | awk '{print $1}')
if [ -n "$EXPECT" ] && [ "$SRC_SHA" != "$EXPECT" ]; then echo "$IMG hashes to $SRC_SHA, not $EXPECT" >&2; exit 1; fi
MNT=$(mktemp -d /private/tmp/pool-src.XXXXXX)
att=$(diskutil image attach --readOnly --noMount "$IMG" 2>&1)
phys=$(echo "$att" | awk '$1 ~ /^\/dev\/disk[0-9]+$/ {print $1}' | head -1)
vol=$(echo "$att" | awk '/Apple_APFS_Volume/ {print $1}' | head -1)
cleanup() { umount "$MNT" 2>/dev/null || true; [ -n "$phys" ] && hdiutil detach "$phys" -force > /dev/null 2>&1 || true; rmdir "$MNT" 2>/dev/null || true; }
trap cleanup EXIT
[ -n "$vol" ] || { echo "no APFS volume in $IMG: $att" >&2; exit 1; }
VOL_UUID=$(diskutil info -plist "$vol" | plutil -extract VolumeUUID raw -o - -)
VOL_NAME=$(diskutil info -plist "$vol" | plutil -extract VolumeName raw -o - -)
PERSONALITY=$(diskutil info -plist "$vol" | plutil -extract FilesystemName raw -o - -)
USED=$(diskutil info -plist "$vol" | plutil -extract CapacityInUse raw -o - -)
mount -t apfs -o rdonly,nobrowse "$vol" "$MNT"
"$TOOL" --exclude .fseventsd "$MNT" > "$OUT/expected.manifest" 2> "$OUT/expected.manifest.err"
rm -f "$OUT/volume.aar"
aa archive -x -d "$MNT" -o "$OUT/volume.aar" -exclude-path .fseventsd -include-field attr,xat,acl,sh2 -a lzfse -b 4m
umount "$MNT"
ENTRIES=$(aa list -i "$OUT/volume.aar" | wc -l | tr -d ' ')
python3 "$D/tooling/pool/afsc-lists.py" "$OUT/expected.manifest" "$OUT" > /dev/null
cp -p "$TOOL" "$OUT/pool-manifest"
AAR_SHA=$(shasum -a 256 "$OUT/volume.aar" | awk '{print $1}')
MF_SHA=$(shasum -a 256 "$OUT/expected.manifest" | awk '{print $1}')
TOOL_SHA=$(shasum -a 256 "$OUT/pool-manifest" | awk '{print $1}')
python3 - "$OUT" "$IMG" "$SRC_SHA" "$VOL_UUID" "$VOL_NAME" "$PERSONALITY" "$USED" "$AAR_SHA" "$MF_SHA" "$TOOL_SHA" "$ENTRIES" <<'PY'
import json, sys
from pathlib import Path
out, img, src, uuid, name, pers, used, aar, mf, tool, entries = sys.argv[1:]
out = Path(out)
lines = [l for l in (out / 'expected.manifest').read_text().splitlines()]
summary = lines[-1]
assert summary.startswith('# entries') and summary.endswith('errors 0'), summary
fams = json.loads((out / 'afsc-lists.json').read_text())['families']
paths = [l.split('\t', 1)[0] for l in lines[:-1]]
listed = set()
for fam in fams:
    listed |= {p for p in (out / f'afsc-{fam}.list').read_text().splitlines()}
# aa's -include-path matches a prefix: a listed path that is a prefix of another archive path would pull that one into its pass too
import re
raw = [re.sub(r'\\x([0-9a-f]{2})', lambda m: chr(int(m.group(1), 16)), p) for p in paths]
srt = sorted(raw)
# (pool-device.sh extracts those through anchored -include-regex; recorded here)
collide = sorted({a for a, b in zip(srt, srt[1:]) if a in listed and b.startswith(a) and b != a})
if any('^' in a or '\n' in a for a in collide):
    raise SystemExit('a prefix path with ^ or a newline: pool-device.sh has no regex for it')
if int(entries) != len(paths):
    raise SystemExit(f'the archive lists {entries} entries, the manifest {len(paths)}')
rec = {'source_image': Path(img).name, 'source_sha256': src, 'source_volume_uuid': uuid, 'source_volume_name': name,
       'source_personality': pers, 'source_used_bytes': int(used), 'archive': 'volume.aar', 'archive_sha256': aar,
       'archive_bytes': (out / 'volume.aar').stat().st_size, 'archive_entries': int(entries),
       'manifest': 'expected.manifest', 'manifest_file_sha256': mf, 'manifest_summary': summary,
       'families': fams, 'excluded': ['.fseventsd'], 'pool_manifest_sha256': tool,
       'aa_fields': 'attr,xat,acl,sh2', 'prefix_paths_listed': len(collide), 'note': 'T-20 plan A: tooling/pool/pool-device.sh extracts this into a new role-less volume of the Data container'}
(out / 'archive.json').write_text(json.dumps(rec, indent=1) + '\n')
env = {'archive_sha256': aar, 'manifest_file_sha256': mf, 'source_sha256': src, 'source_volume_uuid': uuid,
       'source_volume_name': name.replace(' ', '_'), 'source_personality': pers.replace(' ', '_'), 'source_used_bytes': used,
       'families': ' '.join(sorted(fams)), 'archive_entries': entries, 'pool_manifest_sha256': tool}
(out / 'archive.env').write_text(''.join(f'{k}={v}\n' for k, v in env.items()))
print(json.dumps({k: rec[k] for k in ('source_sha256', 'source_volume_uuid', 'archive_sha256', 'archive_bytes', 'archive_entries',
                                         'manifest_summary', 'families')}, indent=1))
PY
chown -R "${SUDO_UID:-0}:${SUDO_GID:-0}" "$OUT"
