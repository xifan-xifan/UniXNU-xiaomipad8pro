#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Our native kexts for the 27.3 line on a machine that has run tooling/ci/k27-kernel.sh (the k27 tree with its native kernel and the
# kernel's object recipes, which every kext compiles with): inside tooling/ci/with-kernelcache.sh (the kernelcache member only, deleted
# afterwards) the 27.3 clone of the tree is prepared and its kernel relinked at the collection's addresses
# (tooling/k273/prepare-k27-tree.sh), then tooling/k27/build-native-image.sh builds the kexts of configs/k27-native-kexts.txt and
# stops there (PIANO_STOP_AFTER=kexts): no kernel collection, ADT or image is made (those are built on the user's side).
#
#   tooling/ci/build-kexts.sh      -> $OUT/piano-kexts.tar.gz (out/native-kexts: our objects only), $OUT/kernel-k273-native.macho, *.sha256
#   Environment: WORK, OUT, K27_DIR, K27_TREE_TOP as tooling/ci/k27-kernel.sh.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
WORK=${WORK:-$PWD/out/ci}; OUT=${OUT:-$WORK/out}; M=${K27_DIR:-$WORK/k27-tree}; TOP=${K27_TREE_TOP:-0066}
[ -f "$M/out/k27/kernel-k27-$TOP-native.macho" ] || { echo "no $M/out/k27/kernel-k27-$TOP-native.macho: tooling/ci/k27-kernel.sh first" >&2; exit 1; }
mkdir -p "$OUT" out/k27 out/k273
export WORK OUT M TOP
bash tooling/ci/with-kernelcache.sh bash -c '
  set -euo pipefail
  T=$WORK/k273-tree
  K=$(K273_TREE=$T PIANO_K27_TREE=$M bash tooling/k273/prepare-k27-tree.sh "$TOP" | tail -1)
  [ -f "$K" ] || { echo "prepare-k27-tree.sh gave no kernel" >&2; exit 1; }
  cp "$K" "$OUT/kernel-k273-native.macho"
  PIANO_K27_TREE=$T K27_KERNEL=$K KC_BASE=$K273_KC KC_BUILDER=k27/kc-build273-piano.py PIANO_ADT_IDENTITY=configs/k273-adt.toml \
    PIANO_STOP_AFTER=kexts bash tooling/k27/build-native-image.sh
'
# our objects only (out/native-kexts/NAME/*.o, no generated headers or host-test outputs) and the descriptors; nothing of the
# kernelcache is in there
( cd out && find native-kexts -type f -name '*.o' | sort > "$WORK/kext-objects.txt" )
[ -s "$WORK/kext-objects.txt" ] || { echo "no kext objects under out/native-kexts" >&2; exit 1; }
tar -C out -czf "$OUT/piano-kexts.tar.gz" -T "$WORK/kext-objects.txt" -C "$D" tooling/k27/kext
( cd "$OUT" && shasum -a 256 piano-kexts.tar.gz kernel-k273-native.macho > kexts.sha256 && cat kexts.sha256 )
