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

FIRST = 0x0b
LAST = 0xca

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

def table_at(raw, base):
    """Print every non-empty slot of the table that starts at `base`."""
    if base < 0 or base + (LAST - FIRST + 1) * 32 > len(raw):
        return 0
    return entries(raw[base:], FIRST, LAST)

def best_run(raw):
    """Longest stretch of consecutive 32-byte entries whose four words agree."""
    best = (0, 0, 0)
    off = 0
    while off + 32 <= len(raw):
        w = struct.unpack_from('<4Q', raw, off)
        if w[0] == w[1] == w[2] == w[3] and w[0] != 0:
            start = off
            while off + 32 <= len(raw):
                w = struct.unpack_from('<4Q', raw, off)
                if not (w[0] == w[1] == w[2] == w[3] and w[0] != 0):
                    break
                off += 32
            if (off - start) // 32 > best[0]:
                best = ((off - start) // 32, start, w[0])
            continue
        off += 8
    return best

def scan(raw):
    """Find 32-byte entries whose four words are the same probe code."""
    found = []
    for off in range(0, len(raw) - 31, 8):
        w = struct.unpack_from('<4Q', raw, off)
        if w[0] == w[1] == w[2] == w[3] and 0xa0 <= w[0] <= 0xa6:
            found.append((off, w[0]))
    found.sort()
    if not found:
        print('no probe entry found in the window', file=sys.stderr)
        n, start, val = best_run(raw)
        if n:
            print(f'longest run of agreeing entries: {n} at {start:#08x} value {val:#x}', file=sys.stderr)
            base = start - (0x0b - FIRST) * 32
            if n >= 1:
                print(f'if that is slot 0x0b the table starts at {base:#08x}; other non-empty slots:', file=sys.stderr)
                table_at(raw, base)
        return []
    print('probe entries:')
    bases = {}
    for off, code in found:
        base = off - (code - FIRST) * 32
        bases.setdefault(base, []).append(code)
        print(f'  offset {off:#08x} code {code:#04x} table_base {base:#08x}')
    best = max(bases.items(), key=lambda kv: len(kv[1]))
    print(f'consensus table base {best[0]:#08x} from codes ' + ' '.join(f'{c:#04x}' for c in sorted(best[1])), file=sys.stderr)
    others = table_at(raw, best[0])
    print(f'{others} non-empty slot(s) in that table', file=sys.stderr)
    return found

def main():
    args = [a for a in sys.argv[1:] if a != '--scan']
    if not args:
        sys.exit('usage: decode-slots.py FILE [--scan] [FIRST [LAST]]')
    raw = open(args[0], 'rb').read()
    if '--scan' in sys.argv[1:]:
        hits = scan(raw)
        sys.exit(0 if hits else 1)
    first = int(args[1], 16) if len(args) > 1 else FIRST
    last = int(args[2], 16) if len(args) > 2 else LAST
    n = last - first + 1
    if len(raw) < n * 32:
        sys.exit(f'{len(raw)} bytes is short of {n * 32} for codes {first:#x}..{last:#x}')
    hits = entries(raw, first, last)
    print(f'{hits} non-empty entries', file=sys.stderr)

main()