#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of firmware/patches/darwinboot/0002 (RAM16G R1 and R3): DarwinBoot at unixnu.lock's pin + 0001 + 0002 in a temporary copy,
# then its own host tests: Test/window-size.c (0001), Test/extra-ram.c (region1 collection, region0 map checks and encoding),
# Test/test-extra-ram-adt.py (the production AdtNode on /chosen piano-extra-ram), test-kernel-parser.py and test-packed-ramdisk.py
# (unchanged parsers still pass), and an ADT made by tooling/adt/adt_extra_ram.py parsed by that AdtNode.
#   DARWINBOOT=<checkout> tooling/firmware/test-darwinboot-extra-ram.sh   (default $UNIXNU_RESEARCH/DarwinBoot; absent: SKIP)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
SRC=${DARWINBOOT:-$R/DarwinBoot}
PIN=$(python3 -c "import tomllib;print(tomllib.load(open('$D/unixnu.lock','rb'))['pin']['DarwinBoot']['commit'])")
git -C "$SRC" cat-file -e "$PIN^{commit}" 2>/dev/null || { echo "SKIP: no DarwinBoot checkout with $PIN (DARWINBOOT=...)"; exit 0; }
W=$(mktemp -d "${TMPDIR:-/tmp}/darwinboot-r1.XXXXXX"); trap 'rm -rf "$W"' EXIT
git -C "$SRC" archive "$PIN" | tar -x -C "$W"
for p in "$D"/firmware/patches/darwinboot/0*.patch; do (cd "$W" && patch -s -p1 < "$p") || { echo "FAIL: $p does not apply"; exit 1; }; done
cd "$W"
cc -O2 -Wall -Wextra -Werror Test/window-size.c -o window-size && ./window-size && echo "PASS: window-size"
cc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined Test/extra-ram.c -o extra-ram && ./extra-ram
python3 Test/test-extra-ram-adt.py
python3 Test/test-kernel-parser.py
python3 Test/test-packed-ramdisk.py
# the placeholder as tooling/adt/adt_extra_ram.py writes it, read by the production AdtNode
python3 - "$D/tooling/adt/adt_extra_ram.py" <<'PY'
import importlib.util, sys
s = importlib.util.spec_from_file_location('x', sys.argv[1]); x = importlib.util.module_from_spec(s); s.loader.exec_module(x)
src = open('Test/test-extra-ram-adt.py').read()
pre = src[:src.index("with tempfile.TemporaryDirectory() as tmp:")]
ns = {'__file__': 'Test/test-extra-ram-adt.py'}
exec(pre, ns)
import ctypes, subprocess, tempfile
from pathlib import Path
with tempfile.TemporaryDirectory() as tmp:
    w = Path(tmp); (w / 'adt.c').write_text(ns['pre'] + ns['fn'] + ns['stage_fn'] + ns['glue'])
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-shared', '-fPIC', '-I', 'DarwinBootPkg/Driver',
                    str(w / 'adt.c'), '-o', str(w / 'adt.dylib')], check=True)
    lib = ctypes.CDLL(str(w / 'adt.dylib')); lib.check.restype = ctypes.c_longlong
    prop, node = ns['prop'], ns['node']
    tree = node([prop('name', b'device-tree\0')], [node([prop('name', b'chosen\0')]), node([prop('name', b'cpus\0')])])
    out = x.add_extra_ram(tree)
    off = lib.check(ctypes.create_string_buffer(out), len(out))
    assert off > 0 and out[off - 36:off - 36 + 15] == b'piano-extra-ram', off
print('PASS: adt_extra_ram.py placeholder found by the production AdtNode')
PY
echo "RESULT PASS"
