#!/usr/bin/env python3
"""Host test of the no-cache file reads in tooling/persist/maint-session.py: read_nocache / sha_nocache.

These digest the things a write step trusts: a backup as stored, the image about to be streamed, the held copy that has to equal
the medium. On Windows os.open without O_BINARY hands back a TEXT handle, and the CRT translation behind os.read() turns an image
into other bytes, so a check that should pass fails (or, with the bytes merely rearranged, could pass when it should not). Run:
    python3 tooling/native/test-nocache-read.py
"""
import hashlib
import importlib.util
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
D = HERE.parents[1]
sys.path.insert(0, str(D / 'tooling' / 'persist'))
spec = importlib.util.spec_from_file_location('maint_session', D / 'tooling/persist/maint-session.py')
ms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ms)

FAILS = []


def check(what, ok, note=''):
    print(f'{"PASS" if ok else "FAIL":4}  {what}{": " + note if note else ""}')
    if not ok:
        FAILS.append(what)


def fixture(seed):
    """Bytes that cross the 4 MiB chunk boundary and are full of 0d 0a pairs, the sequence a text-mode handle eats."""
    rng = __import__('random').Random(seed)
    blob = bytearray()
    while len(blob) < 4 * 1024 * 1024 + 65536:
        piece = bytes(rng.getrandbits(8) for _ in range(1024))
        blob += piece.replace(b'\x00', b'\r\n')          # guarantee CRLF pairs without losing the byte variety
    return bytes(blob)


with tempfile.TemporaryDirectory() as td:
    for seed in (1, 2):
        raw = fixture(seed)
        path = Path(td) / f'fixture-{seed}.bin'
        path.write_bytes(raw)
        want = hashlib.sha256(raw).hexdigest()
        check(f'os.O_BINARY is in the flags a no-cache read opens with (seed {seed})',
              ms.O_RDONLY_RAW & (getattr(os, 'O_BINARY', 0) or ms.O_RDONLY_RAW) == getattr(os, 'O_BINARY', 0)
              if hasattr(os, 'O_BINARY') else True, f'O_RDONLY_RAW={ms.O_RDONLY_RAW}')
        chunks = list(ms.read_nocache(path))
        check(f'read_nocache yields the file byte for byte (seed {seed})', b''.join(chunks) == raw,
              f'{sum(len(c) for c in chunks)} of {len(raw)} bytes, {len(raw) - sum(len(c) for c in chunks)} missing')
        check(f'sha_nocache digests the file, not a translation of it (seed {seed})',
              ms.Session.sha_nocache(str(path)) == want, f'{want[:16]} expected')
    check('a CRLF-only file still round-trips', (lambda p: (p.write_bytes(b'\r\n' * 5000),
                                                           ms.Session.sha_nocache(str(p)) == hashlib.sha256(b'\r\n' * 5000).hexdigest())[1])(Path(td) / 'crlf.bin'))

print(f'\n{"FAILURES: " + ", ".join(FAILS) if FAILS else "all checks passed"} ({0 if not FAILS else len(FAILS)})')
sys.exit(1 if FAILS else 0)
