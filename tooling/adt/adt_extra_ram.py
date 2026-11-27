#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Add a zero-filled /chosen/piano-extra-ram placeholder to a flattened ADT.
Patched DarwinBoot fills it with up to 16 little-endian records of 24 bytes
each (u64 base, u64 size, u32 bank, u32 use). An older FD leaves it zeroed.

    adt_extra_ram.py IN OUT      (also make-piano-adt.py --extra-ram)
"""
import struct
import sys
from pathlib import Path

NAME = 'piano-extra-ram'
BYTES = 24 * 16


def _chosen(t):
    chosen = None

    def walk(off, depth):
        nonlocal chosen
        if depth > 64 or len(t) - off < 8:
            raise ValueError('invalid ADT node header or nesting depth')
        nprops, nchildren = struct.unpack_from('<II', t, off)
        o = off + 8
        name = value = None
        for _ in range(nprops):
            if len(t) - o < 36:
                raise ValueError('truncated ADT property header')
            pname, terminator, _ = t[o:o + 32].partition(b'\0')
            if not terminator:
                raise ValueError('unterminated ADT property name')
            word, = struct.unpack_from('<I', t, o + 32)
            length = word & 0x7fffffff
            start = o + 36
            o = start + ((length + 3) & ~3)
            if o > len(t):
                raise ValueError('truncated ADT property value or padding')
            if pname == b'name':
                if name is not None:
                    raise ValueError('duplicate ADT node name property')
                name = t[start:start + length]
            if pname == NAME.encode():
                if value is not None:
                    raise ValueError(f'duplicate {NAME} properties')
                if word & 0x80000000:
                    raise ValueError(f'{NAME} has a flagged length')
                value = bytes(t[start:start + length])
        if depth == 1 and name == b'chosen\0':
            if chosen is not None:
                raise ValueError('duplicate /chosen nodes')
            chosen = off, o, value
        elif value is not None:
            raise ValueError(f'{NAME} outside /chosen')
        for _ in range(nchildren):
            o = walk(o, depth + 1)
        return o

    if walk(0, 0) != len(t):
        raise ValueError('trailing bytes after the ADT root')
    if chosen is None:
        raise ValueError('no /chosen node')
    return chosen


def find_chosen(t):
    """(node offset, props end) of /chosen (a child of the root)."""
    off, props_end, _ = _chosen(t)
    return off, props_end


def extra_ram_property(t):
    """Return the raw /chosen property, or None if it is absent."""
    return _chosen(t)[2]


def has_extra_ram(t):
    return extra_ram_property(t) is not None


def add_extra_ram(tree):
    t = bytearray(tree)
    off, props_end, value = _chosen(t)
    if value is not None:
        raise ValueError(f'/chosen already has {NAME}')
    prop = NAME.encode().ljust(32, b'\0') + struct.pack('<I', BYTES) + bytes(BYTES)
    t[props_end:props_end] = prop
    struct.pack_into('<I', t, off, struct.unpack_from('<I', t, off)[0] + 1)
    return bytes(t)


def ensure_extra_ram_placeholder(tree):
    """For image staging, accept a placeholder that make-piano-adt.py already added."""
    value = extra_ram_property(tree)
    if value is None:
        return add_extra_ram(tree)
    if value != bytes(BYTES):
        raise ValueError(f'/chosen {NAME} must be a {BYTES}-byte zeroed placeholder')
    return tree


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        data = add_extra_ram(Path(sys.argv[1]).read_bytes())
    except (OSError, ValueError) as error:
        sys.exit(str(error))
    Path(sys.argv[2]).write_bytes(data)
    print(f'{sys.argv[2]}: /chosen {NAME} placeholder ({BYTES} bytes) added')
