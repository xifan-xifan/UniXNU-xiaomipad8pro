#!/bin/bash
# Build package/PianoDpu (the display processor; level 1: a read-only survey of what the boot stages left driving the panel: pipes,
# CTL paths, layer mixers, DSPP colour blocks, DSC, INTF and DSI timing, the DPU's SMMU route, the refresh measured from the INTF frame
# counters; B1b's zero-copy flips in DpuFlip.cpp at piano-dpu=2; and the opt-in writing slices B1c refresh, B1d Night Light, B1e panel
# sleep in DpuSlices.cpp) as a k27 native kext: the host tests first (tests/dpu_host.cpp: the clock gate and what is and is never
# loaded, the decoders, the path and flip verdicts on a modelled boot-loader state, the SMMU scan and the page-table walk;
# tests/dpu_flip_host.cpp: B1b; tests/dpu_refresh_host.cpp, dpu_pcc_host.cpp, dpu_panel_host.cpp: the slices' sequences on a timed
# register model), then the objects in out/native-kexts/PianoDpu for tooling/k27/kext/PianoDpu.json.
# Inert without the boot argument piano-dpu (docs/drivers/dpu.md).
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes), K27_OBJ (default k27-native-obj), CXX (host compiler
#                for the tests, default c++), K27_KERNEL (for the import check; skipped when absent),
#                KC_BASE (the base collection with IOSurface, default $PIANO_K27_TREE/out/kc/kc27.bin)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoDpu; O=$D/out/native-kexts/PianoDpu
mkdir -p "$O/host-tests"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/dpu_host.cpp" -o "$O/host-tests/dpu_host"
"$O/host-tests/dpu_host" || { echo "Error: PianoDpu host tests failed" >&2; exit 1; }
# B1b (zero-copy flips): the SMMU bank in place, the windows, the flip engine and its safety, against DPU/SMMU/RAM models.
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/dpu_flip_host.cpp" -o "$O/host-tests/dpu_flip_host"
"$O/host-tests/dpu_flip_host" || { echo "Error: PianoDpu B1b host tests failed" >&2; exit 1; }
# The writing slices B1c/B1d/B1e (DpuRefresh.h, DpuPcc.h, DpuPanel.h on the timed register model tests/dpu_model.h).
# B1 milestone 1 (store-free): the 144/120/90/60 Hz timing planner (DpuModes.h) against installed y12k's survey words, and the
# run-time snapshot (DpuSnapshot.h, machdep.piano_dpu_snapshot with piano-dpu-snapshot=1) on the register model.
for t in refresh pcc panel modes mode_switch snapshot clean; do
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/dpu_${t}_host.cpp" -o "$O/host-tests/dpu_${t}_host"
  "$O/host-tests/dpu_${t}_host" || { echo "Error: PianoDpu $t host tests failed" >&2; exit 1; }
done
# Bind the optional accessor only when the target IOSurface supplies a unique definition.
IOSURFACE_CACHE_MODE=0
if python3 -B - "$K/scripts" "${KC_BASE:-$K/out/kc/kc27.bin}" <<'PY'
from pathlib import Path
from struct import error as StructError
import sys

sys.path.insert(0, sys.argv[1])
try:
    from fileset_kc import Kernelcache
    kc = Kernelcache(Path(sys.argv[2]).read_bytes())
    symbols = kc.symbols(kc.macho('com.apple.iokit.IOSurface'))
except (ImportError, OSError, ValueError, KeyError, StructError):
    sys.exit(1)
matches = [name for name, kind, _, _, _ in symbols
           if name == '__ZNK9IOSurface12getCacheModeEv' and kind & 0xee == 0x0e]
sys.exit(len(matches) != 1)
PY
then
  IOSURFACE_CACHE_MODE=1
else
  echo "PianoDpu: IOSurface cache-mode accessor unavailable; M2 will require full cleans" >&2
fi
python3 "$D/tooling/k27/build-native-kext.py" PianoDpu "$P/PianoDpu.cpp" "$P/DpuFlip.cpp" "$P/DpuSlices.cpp" "$P/DpuSnapshot.cpp" "$D/tooling/k27/NativeKextGlue.cpp" -I "$P" \
  -D "PIANO_DPU_IOSURFACE_CACHE_MODE=$IOSURFACE_CACHE_MODE" --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# piano-dpu-ahb-vote takes PianoA830's GCC vote lock word (sm8750_gcc_vote_owner): PianoA830 links first, a library of PianoDpu.json.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" --library-prefix _sm8750_gcc_vote_owner \
  --library-prefix __ZNK9IOSurface12getCacheModeEv \
  | cut -c1-160 >&2
