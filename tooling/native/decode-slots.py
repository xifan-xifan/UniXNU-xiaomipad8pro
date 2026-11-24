#!/usr/bin/env python3
"""Decode the in-image piano_stage_slots table (piano_dbg_kv / piano_dbg_slot).

Read it back with:  fastboot -s piano-uefi oem dump 8c5941080 2000 && get_staged slots.bin
The table has one 32-byte entry per code 0x0b..0xca; piano_dbg_kv() stores the same
64-bit value in all four words of the entry, so a bitwise majority over the four is
the value and the distance between copies is the damage seen across the reset.
"""
import struct, sys

raw = open(sys.argv[1], 'rb').read()
first = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x0b
last = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xca
n = (last - first + 1)
if len(raw) < n * 32:
    sys.exit(f'{len(raw)} bytes is short of {n * 32} for codes {first:#x}..{last:#x}')

def vote(words):
    v = 0
    for b in range(64):
        ones = sum((w >> b) & 1 for w in words)
        if ones >= 2:
            v |= 1 << b
    return v

hits = 0
for code in range(first, last + 1):
    off = (code - first) * 32
    w = struct.unpack_from('<4Q', raw, off)
    if not any(w):
        continue
    v = vote(w)
    spread = max(bin(x ^ v).count('1') for x in w)
    hits += 1
    mart = b''.join(struct.pack('<Q', x) for x in w)
    txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in mart)
    print(f'{code:#04x} value={v:#018x} spread={spread:2d} text={txt}')
print(f'{hits} non-empty entries', file=sys.stderr)
