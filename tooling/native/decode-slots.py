#!/usr/bin/env python3
"""Decode the in-image piano_stage_slots table (piano_dbg_kv / piano_dbg_slot).

Read it back with:  fastboot -s piano-uefi oem dump 8c5000000 1000000 && get_staged region.bin
then run with --scan to find the table inside a larger window: the probe writes the
bare code (0xa0..0xa6) into all four words of its entry, so a run of four equal
words in that range identifies the table even when the slide moves it.

Without --scan the file is treated as the table itself, starting at code
0x0b.
"""
import struct, sys

def vote(words):
    v = 0
    for b in range(64):
        if sum((w >> b) & 1 for w in words) >= 2:
            v |= 1 << b
    return v

def entries(raw, first, last):
    hits = 0
    for code in range(first, last + 1):
        off = (code - first) * 32
        if off + 32 > len(raw):
            break
        w = struct.unpack_from('<4Q', raw, off)
        if not any(w):
            continue
        v = vote(w)
        spread = max(bin(x ^ v).count('1') for x in w)
        hits += 1
        txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in b''.join(struct.pack('<Q', x) for x in w))
        print(f'{code:#04x} value={v:#018x} spread={spread:2d} text={txt}')
    return hits

def scan(raw):
    """Find 32-byte entries whose four words are the same probe code."""
    found = []
    for off in range(0, len(raw) - 31, 8):
        w = struct.unpack_from('<4Q', raw, off)
        if w[0] == w[1] == w[2] == w[3] and 0xa0 <= w[0] <= 0xa6:
            found.append((off, w[0]))
    found.sort()
    if not found:
        return []
    print('probe entries:')
    bases = {}
    for off, code in found:
        base = off - (code - 0x0b) * 32
        bases.setdefault(base, []).append(code)
        print(f'  offset {off:#08x} code {code:#04x} table_base {base:#08x}')
    best = max(bases.items(), key=lambda kv: len(kv[1]))
    print(f'consensus table base {best[0]:#08x} from codes ' + ' '.join(f'{c:#04x}' for c in sorted(best[1])), file=sys.stderr)
    return found

def main():
    args = [a for a in sys.argv[1:] if a != '--scan']
    raw = open(args[0], 'rb').read()
    if '--scan' in sys.argv[1:]:
        hits = scan(raw)
        if not hits:
            print('no probe entry found in the window', file=sys.stderr)
            sys.exit(1)
        return
    first = int(args[1], 16) if len(args) > 1 else 0x0b
    last = int(args[2], 16) if len(args) > 2 else 0xca
    n = last - first + 1
    if len(raw) < n * 32:
        sys.exit(f'{len(raw)} bytes is short of {n * 32} for codes {first:#x}..{last:#x}')
    hits = entries(raw, first, last)
    print(f'{hits} non-empty entries', file=sys.stderr)

main()
