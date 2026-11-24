#!/usr/bin/env python3
"""Host-only test of decode-slots.py: the probe table is located inside a larger dump by sliding its base,
the four-word vote recovers a slot whose words disagree, a decoy run does not outvote a longer one, and a
window without probe entries reports the longest agreeing run instead of a false hit. No device, no dump."""
import contextlib
import importlib.util
import io
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('decode_slots', HERE / 'decode-slots.py')
decode_slots = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decode_slots)

failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what, flush=True)
    if not ok:
        failures.append(what)


def slot(off, words):
    return off, struct.pack('<4Q', *words)


def synth(size, entries):
    raw = bytearray(size)
    for off, payload in entries:
        raw[off:off + len(payload)] = payload
    return bytes(raw)


def run_scan(raw, lo, hi):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        hits = decode_slots.scan(raw, lo, hi)
    return hits, out.getvalue(), err.getvalue()


def main():
    first = decode_slots.FIRST
    base = 0x1200
    codes = list(range(0xb0, 0xb8))
    entries = [slot(base + (c - first) * 32, [c, c, c, c]) for c in codes]
    # A single decoy entry of another probe code must not win the consensus.
    entries.append(slot(base + (0xa0 - first) * 32, [0xa0, 0xa0, 0xa0, 0xa0]))
    raw = synth(0x4000, entries)
    hits, out, err = run_scan(raw, 0xb0, 0xb7)
    check(len(hits) == len(codes), f'scan finds every 0xb0-0xb7 entry ({len(hits)} of {len(codes)})')
    check(f'consensus table base {base:#08x}' in err,
          f'scan reports the slid table base {base:#08x}: {err.strip().splitlines()[-1] if err.strip() else "no stderr"}')
    check('0xb0' in out and '0xb7' in out, 'scan prints the recovered codes')

    # Default probe window still recognises the 0xa0..0xa6 codes at another base.
    base2 = 0x800
    raw2 = synth(0x2000, [slot(base2 + (c - first) * 32, [c, c, c, c]) for c in range(0xa0, 0xa7)])
    hits2, _, err2 = run_scan(raw2, decode_slots.PROBE_LO, decode_slots.PROBE_HI)
    check(len(hits2) == 7 and f'{base2:#08x}' in err2, 'the default 0xa0-0xa6 window is located at a second base')

    # A window with no probe code falls back to the longest agreeing run and returns no hit.
    base3 = 0x1000
    raw3 = synth(0x1800, [slot(base3 + (0x40 - first) * 32, [0x401, 0x401, 0x401, 0x401])])
    hits3, _, err3 = run_scan(raw3, 0xb0, 0xb7)
    check(hits3 == [], 'a window without probe entries reports no hit')
    check('longest run of agreeing entries' in err3, 'the miss path names the longest agreeing run')

    # vote(): one disagreeing word still recovers the value written by the other three.
    value = decode_slots.vote([0x1111, 0x1111, 0x1111, 0x1110])
    check(value == 0x1111, 'vote recovers a slot whose fourth word lost one bit')

    # entries(): the table starts at code 0x0b and prints each non-empty slot once.
    raw4 = synth((decode_slots.LAST - first + 1) * 32,
                 [slot((0xb1 - first) * 32, [0xb1, 0xb1, 0xb1, 0xb1]),
                  slot((0xb5 - first) * 32, [0x1234, 0x1234, 0x1234, 0x1234])])
    out4, err4 = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out4), contextlib.redirect_stderr(err4):
        n = decode_slots.entries(raw4, first, decode_slots.LAST)
    check(n == 2, f'entries() counts two non-empty slots ({n})')
    check('value=0x0000000000001234' in out4.getvalue(), 'entries() prints the voted value of the second slot')

    print('decode slots: ' + ('all checks passed' if not failures else f'{len(failures)} FAILED'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
