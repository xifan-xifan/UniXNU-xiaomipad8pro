#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of firmware/patches/darwinboot/0002 (RAM16G R1 and R3): DarwinBoot at unixnu.lock's pin + 0001 + 0002 in a temporary copy,
# then its own host tests: Test/window-size.c (0001), Test/extra-ram.c (region1 collection, region0 map checks and encoding),
# Test/test-extra-ram-adt.py (the production AdtNode on /chosen piano-extra-ram), test-kernel-parser.py and test-packed-ramdisk.py
# (unchanged parsers still pass), and an ADT made by tooling/adt/adt_extra_ram.py parsed by that AdtNode.
# Without a checkout, compile the extra-RAM header and standalone tests directly from 0002.
#   DARWINBOOT=<checkout> tooling/firmware/test-darwinboot-extra-ram.sh   (optional default checkout; explicit checkout must contain the pin)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
SRC=${DARWINBOOT:-$R/DarwinBoot}
PIN=$(python3 - "$D/unixnu.lock" <<'PY'
import sys
try:
    import tomllib
except ModuleNotFoundError:
    try:
        import tomli as tomllib
    except ModuleNotFoundError:
        from pip._vendor import tomli as tomllib
with open(sys.argv[1], encoding='utf-8') as lock:
    print(tomllib.loads(lock.read())['pin']['DarwinBoot']['commit'])
PY
)
have_checkout=0
if git -C "$SRC" cat-file -e "$PIN^{commit}" 2>/dev/null; then
  have_checkout=1
elif [ -n "${DARWINBOOT:-}" ]; then
  echo "FAIL: DARWINBOOT checkout does not contain $PIN" >&2
  exit 1
fi
W=$(mktemp -d "${TMPDIR:-/tmp}/darwinboot-r1.XXXXXX")
trap 'rm -rf -- "$W"' EXIT
if [ "$have_checkout" = 1 ]; then
  git -C "$SRC" archive "$PIN" | tar -x -C "$W"
  cd "$W"
  for p in "$D"/firmware/patches/darwinboot/0*.patch; do
    patch -f -s -p1 < "$p" || { echo "FAIL: $p does not apply"; exit 1; }
  done
  cc -O2 -Wall -Wextra -Werror Test/window-size.c -o window-size
  ./window-size
  echo "PASS: window-size"
else
  cd "$W"
  git apply --include=DarwinBootPkg/Driver/ExtraRam.h --include=Test/extra-ram.c "$D"/firmware/patches/darwinboot/0002-*.patch
fi
cc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined Test/extra-ram.c -o extra-ram
./extra-ram
cc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -I DarwinBootPkg/Driver \
  "$D/tooling/firmware/extra-ram-host.c" -o extra-ram-host
./extra-ram-host
if [ "$have_checkout" = 0 ]; then
  echo "RESULT PASS (checkout-independent extra-RAM tests)"
  exit 0
fi
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
    lib = ctypes.CDLL(str(w / 'adt.dylib'))
    lib.check.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
    lib.check.restype = ctypes.c_longlong
    def check(blob):
        return lib.check(ctypes.create_string_buffer(blob), len(blob))
    prop, node = ns['prop'], ns['node']
    tree = node([prop('name', b'device-tree\0')], [node([prop('name', b'chosen\0')]), node([prop('name', b'cpus\0')])])
    out = x.add_extra_ram(tree)
    off = check(out)
    assert off > 0 and out[off - 36:off - 36 + 15] == b'piano-extra-ram', off
    assert off % 4 == 0
    for cut in (0, 7, off - 1, off, off + x.BYTES - 1, len(out) - 1):
        assert check(out[:cut]) == -2, cut
    assert check(out + b'\0') == -2
    wrong_size = node([], [node([prop('name', b'chosen\0'), prop(x.NAME, bytes(x.BYTES - 1))])])
    assert check(wrong_size) == -2
print('PASS: production AdtNode finds the generated placeholder and rejects truncated or mis-sized ADTs')
PY
echo "RESULT PASS"
