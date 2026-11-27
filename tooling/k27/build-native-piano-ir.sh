#!/bin/bash
# Build package/PianoIR (piano's IR transmitter on QUP2 SE7 "SE15": level 1 a read-only survey, level 2 the GENI SE DMA transmitter
# with an emission-free loopback) as a k27 native kext. Run the portable suite and, on macOS, build and test the CLI first.
# Objects go to out/native-kexts/PianoIR for tooling/k27/kext/PianoIR.json.
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX/CC (host
#                compilers for the tests and the tool), K27_KERNEL (for the import check; skipped when absent)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoIR; O=$D/out/native-kexts/PianoIR
INC=(-I "$P" -I "$D/package/PianoTouch" -I "$D/package/PianoDwc3" -I "$D/package/include")
bash "$P/tests/run-ir-host.sh" --out "$O/host-tests" || { echo "Error: PianoIR host tests failed" >&2; exit 1; }
python3 "$D/tooling/k27/build-native-kext.py" PianoIR "$P/PianoIR.cpp" "$D/package/PianoTouch/GeniSpi.c" "$D/tooling/k27/NativeKextGlue.cpp" \
  "${INC[@]}" --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
if [ -f "$KERNEL" ] || [ -n "${K27_KERNEL:-}" ]; then
  python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _sm8750_gcc_vote_owner
else
  echo "PianoIR: kernel Mach-O missing; import check skipped" >&2
fi
