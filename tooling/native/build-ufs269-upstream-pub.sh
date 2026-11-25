#!/bin/bash
# The upstream root-publication driver, linked into a macOS 27.3 kernel collection (2026-11-25).
#
#   tooling/native/build-ufs269-upstream-pub.sh        environment: COLLECTION, K27_BUILDER, BASE, KERNEL, NATIVE_UFS_OBJECTS, OUT
#
# Why this exists: since 3a753d8c the package/NativeUfs driver in this repository is the upstream production driver
# (registerService on the window, then the mediaWithUuidExists poll; no hand-built IOMedia, no IOService::startMatching) and the
# k27 compile recipe is on this host inside WSL UbuntuPiano, so the target state named by
# docs/results-2026-11-25-upstream-publication-diff.md is now built from source instead of patched into a shipped collection's
# zero holes. See docs/results-2026-11-25-k27-tree-in-wsl.md.
#
# The kernel, the Apple 27.3 base and the other 25 driver descriptors are the exact bytes the last source-built collection that
# booted used (out/no-demura-control/blendfix.argv.json, run 2026-11-22), so the driver is the only delta. The 2026-11-20/22
# working set in the WSL tmp is copied under OUT so the link survives a tmp cleanup; nothing here touches the tablet.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
SRC=${COLLECTION:-/tmp/piano-verified-collection.68bZb6}
BUILDER_SRC=${K27_BUILDER:-/tmp/piano-linkinfo-relink.yab5k88c/k27/kc-build273-piano.py}
OUT=${OUT:-$D/out/upstream-pub-20261125}
BASE=${BASE:-$D/out/no-demura-control/apple-normalized.bin}
KERNEL=${KERNEL:-$D/out/no-demura-control/blendfix.macho}
UFS_OBJECTS=${NATIVE_UFS_OBJECTS:-$D/out/native-kexts/NativeUfs-upstream/objects.json}
KC=${KC:-$OUT/piano-ufs269.kc}
[ -d "$SRC" ] || { echo "no $SRC (the 2026-11-20/22 collection working set); see docs/results-2026-11-25-k27-tree-in-wsl.md" >&2; exit 1; }
[ -f "$UFS_OBJECTS" ] || { echo "no $UFS_OBJECTS: tooling/k27/build-native-kext.py with --target and --probe-warnings" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/collection" "$OUT/k27"
# Descriptors name objects beside the JSONs and under archive/<Kext>/, so both come over; the Apple base and old outputs stay out.
cp -f "$SRC"/*.json "$OUT/collection/"
find "$SRC" -maxdepth 1 -name "*.o" -exec cp -f {} "$OUT/collection/" ";"
[ -d "$SRC/archive" ] && cp -a "$SRC/archive" "$OUT/collection/"
# The builder is a wrapper around its sibling scripts/kc-build.py and reads k27/segaddrs.json plus configs/, so its tree comes with it.
BROOT=$(dirname "$(dirname "$BUILDER_SRC")")
cp -f "$BUILDER_SRC" "$OUT/k27/"
[ -d "$BROOT/scripts" ] && cp -a "$BROOT/scripts" "$OUT/"
[ -d "$BROOT/configs" ] && cp -a "$BROOT/configs" "$OUT/"
[ -f "$(dirname "$BUILDER_SRC")/segaddrs.json" ] && cp -f "$(dirname "$BUILDER_SRC")/segaddrs.json" "$OUT/k27/"
find "$OUT/collection" -name "*.json" -print0 | xargs -0 sed -i "s#$SRC#$OUT/collection#g"
# The driver under test: the collection's own bundle, personality and vtable, with the new objects.
python3 - "$OUT" "$UFS_OBJECTS" <<PY
import json, sys
out, objects = sys.argv[1], sys.argv[2]
d = json.load(open(out + "/collection/NativeUfs.json"))
d["objects"] = objects
json.dump(d, open(out + "/NativeUfsPub.json", "w"), separators=(", ", ": "))
PY
# Replay the recorded link command with only the four paths that change (base, kernel, output, driver descriptor).
python3 - "$D" "$OUT" "$BASE" "$KERNEL" "$KC" <<PY | tee "$OUT/link.log" | tail -12
import json, subprocess, sys
root, out, base, kernel, kc = sys.argv[1:6]
a = json.load(open(root + "/out/no-demura-control/blendfix.argv.json"))
a[1] = out + "/k27/kc-build273-piano.py"; a[2] = base; a[3] = kernel; a[4] = kc
copy = out + "/collection/"
a = [copy + x.rsplit("/", 1)[-1] if x.startswith("/tmp/piano-verified-collection") else x for x in a]
a = [(out + "/NativeUfsPub.json" if x.endswith(".json") and "NativeUfs" in x.split("/")[-1] and "objects" not in x else x) for x in a]
r = subprocess.run(a, capture_output=True, text=True)
sys.stdout.write(r.stdout)
sys.stderr.write(r.stderr)
raise SystemExit(r.returncode)
PY
sha256sum "$KC" "$KERNEL" "$BASE" "$UFS_OBJECTS" | tee "$KC.sha256"
