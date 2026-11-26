#!/usr/bin/env python3
"""Read LUN 0's last five blocks (the backup GPT) off the tablet into a host file, for tooling/persist/unit_profile.py.

  fetch-lun0-tail.py OUT [--lba N] [--blocks N] [--start N]

The maintenance image's `tablet.sh peek` reads one block and dumps a byte range of it as hex, so five calls make the five
blocks. This is a read of the medium (its caches dropped by tablet.sh) and writes nothing on the tablet. The tail LBA comes
from the unit profile when --start is not given, which is the same rule maint-session.py uses (common.LUN0_BLOCKS - 5).
"""
import os, re, subprocess, sys
from pathlib import Path

D = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(D / 'tooling/persist'))
import common  # noqa: E402

argv = sys.argv[1:]
if not argv:
    sys.exit('usage: fetch-lun0-tail.py OUT [--start N]')
out = Path(argv[0])
start, blocks, lba = None, 5, 4096
i = 1
while i < len(argv):
    if argv[i] == '--start':
        start = int(argv[i + 1]); i += 2
    elif argv[i] == '--blocks':
        blocks = int(argv[i + 1]); i += 2
    else:
        sys.exit(f'unknown option {argv[i]}')
if start is None:
    start = common.LUN0_BLOCKS - blocks
shell = D / 'tooling/native/maint-shell.py'
blob = b''
for k in range(blocks):
    l = start + k
    command = ('sh /run/pp/tablet.sh rereadpt sda; ' if k == 0 else '') + f'sh /run/pp/tablet.sh peek sda {l} 0 {lba}'
    r = subprocess.run([sys.executable, str(shell), command, '90'], capture_output=True, text=True, timeout=150)
    if r.returncode:
        sys.exit(f'tablet read failed at block {l}: {((r.stdout or "") + (r.stderr or ""))[:300]}')
    text = (r.stdout or '').replace('\r', '').replace('\n', '')
    m = re.search(r'hex=([0-9a-f]+)', text)
    if not m:
        sys.exit(f'no hex in the reply: {text[:200]}')
    piece = bytes.fromhex(m.group(1))
    if len(piece) != lba:
        sys.exit(f'block {l}: {len(piece)} bytes, expected {lba}')
    blob += piece
    print(f'block {l}: {piece[:8].hex()} ...', file=sys.stderr)
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(blob)
import hashlib
print(f'{out} {len(blob)} bytes sha256 {hashlib.sha256(blob).hexdigest()}')