#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""The camera thumbnails piano-camera-grab printed on the tablet's console, as PNG files (no tablet access needed: the console log or
the mailbox copy has them).

    console-thumb.py LOG [--out DIR]      every complete thumbnail in LOG -> DIR/<sensor>-<n>.png (default: next to LOG)
    console-thumb.py --self-test

Lines: `piano-camera-grab: t <sensor> <W>x<H> r<row> p<part> <hex RGB>` (tooling/native/piano-camera-grab: rows of up to 24 pixels
a part). A thumbnail with a missing or cut line keeps that row grey and is reported. The console may replay its retained log, so
the same thumbnail can appear more than once: each run of rows 0..H-1 is one picture.
"""
import argparse
import re
import struct
import sys
import tempfile
import zlib
from pathlib import Path

LINE = re.compile(rb'piano-camera-grab: t (\w+) (\d{1,3})x(\d{1,4}) r(\d{1,4}) p(\d{1,2})(?:[ \t]+(\S+))?(?!\S)')
HEX = re.compile(rb'[0-9a-f]+')
PART_WIDTH = 24
MAX_WIDTH = 256  # piano-camera-grab's --thumb limit
MAX_HEIGHT = 4096  # Bound storage even for malformed console logs.


def png(path, w, h, rgb):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    raw = b''.join(b'\0' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def decode(data):
    """[(sensor, w, h, rgb bytearray, missing rows)] in the order they appear."""
    pictures, current = [], {}
    for m in LINE.finditer(data):
        name, w, h, row, part, hexes = m.group(1).decode(), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), m.group(6)
        x = part * PART_WIDTH
        if not (0 < w <= MAX_WIDTH and 0 < h <= MAX_HEIGHT and row < h and x < w):
            continue
        pic = current.get(name)
        if pic is None or pic[1] != w or pic[2] != h or (row == 0 and part == 0 and pic[4]):
            pic = [name, w, h, bytearray(b'\x80' * (w * h * 3)), set()]
            current[name] = pic
            pictures.append(pic)
        if not hexes or len(hexes) != 6 * min(PART_WIDTH, w - x) or not HEX.fullmatch(hexes):
            continue
        px = bytes.fromhex(hexes.decode())
        at = (row * w + x) * 3
        pic[3][at:at + len(px)] = px
        pic[4].add((row, part))
    out = []
    for name, w, h, rgb, seen in pictures:
        parts = (w + PART_WIDTH - 1) // PART_WIDTH
        missing = sorted({r for r in range(h) for p in range(parts) if (r, p) not in seen})
        out.append((name, w, h, rgb, missing))
    return out


def self_test():
    w, h = 50, 3
    rgb = bytes((x * 5 + y * 40 + c * 90) % 256 for y in range(h) for x in range(w) for c in range(3))
    lines = []
    for y in range(h):
        for part, x0 in enumerate(range(0, w, 24)):
            seg = rgb[(y * w + x0) * 3:(y * w + min(x0 + 24, w)) * 3]
            lines.append(f'PianoX noise\npiano-camera-grab: t front {w}x{h} r{y} p{part} {seg.hex()}\n')
    log = ''.join(lines).encode()
    pics = decode(log + log)   # replayed once: two pictures
    assert len(pics) == 2 and all(p[3] == rgb and not p[4] for p in pics), pics[0][4]
    cut = log.replace(f'r1 p1 {rgb[(w + 24) * 3:(w + 48) * 3].hex()}'.encode(), b'r1 p1 zz')
    pics = decode(cut)
    assert len(pics) == 1 and pics[0][4] == [1], pics[0][4]
    first, rest = log.split(b' r0 p0 ', 1)
    _, rest = rest.split(b'\n', 1)
    for payload in (b'', b' truncated'):
        pics = decode(log + first + b' r0 p0' + payload + b'\n' + rest)
        assert len(pics) == 2 and pics[0][3] == rgb and not pics[0][4]
        assert pics[1][3][:72] == b'\x80' * 72 and pics[1][3][72:] == rgb[72:] and pics[1][4] == [0]
    prefix = b'piano-camera-grab: t front 25x2 '
    assert decode(prefix + b'r0 p2 000000') == []
    assert not decode(b'piano-camera-grab: t front 999x9999 r0 p0 000000')
    assert not decode(b'piano-camera-grab: t front 0x2 r0 p0 000000')
    for bad in (b'000000', b'00' * 72 + b'x'):
        pics = decode(prefix + b'r0 p0 ' + bad + b'\n' + prefix + b'r1 p0 ' + b'00' * 72)
        assert pics[0][3][:75] == b'\x80' * 75 and pics[0][4] == [0, 1]
    pics = decode(prefix + b'r0 p1 000000000000\n' + prefix + b'r1 p0 ' + b'11' * 72)
    assert pics[0][3][72:78] == bytes.fromhex('808080111111') and pics[0][4] == [0, 1]
    with tempfile.TemporaryDirectory() as d:
        png(Path(d) / 't.png', w, h, rgb)
        assert (Path(d) / 't.png').read_bytes()[:8] == b'\x89PNG\r\n\x1a\n'
    print('console-thumb.py: self-test passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', nargs='?', type=Path)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.log:
        ap.error('a log file')
    out = a.out or a.log.parent
    out.mkdir(parents=True, exist_ok=True)
    pics = decode(a.log.read_bytes())
    if not pics:
        sys.exit(f'console-thumb.py: no thumbnail in {a.log}')
    count = {}
    for name, w, h, rgb, missing in pics:
        count[name] = count.get(name, 0) + 1
        path = out / f'{name}-{count[name]}.png'
        png(path, w, h, rgb)
        print(f'{path}: {w}x{h}' + (f', {len(missing)} row(s) incomplete: {missing[:12]}' if missing else ''))


if __name__ == '__main__':
    main()
