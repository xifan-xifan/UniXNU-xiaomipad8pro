#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
# Real compressed samples, independent normalization/software pixels and actual
# CoreMedia ImageDescriptions. No decoder session on the tablet or host.
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

folder = Path(__file__).parent / 'fixtures' / 'v8'
manifest = json.loads((folder / 'manifest.json').read_text())
for name, reference in manifest['fixtures'].items():
    movie = folder / (name + '.mp4')
    assert hashlib.sha256(movie.read_bytes()).hexdigest() == reference['movie_sha256']
    result = subprocess.run([sys.argv[1], str(movie.resolve())], capture_output=True, text=True, check=True)
    lines = re.findall(r'V8_PACKET index=(\d+) bytes=(\d+) compressed_sha=([a-f0-9]+) normalized_bytes=(\d+) normalized_sha=([a-f0-9]+) pts=(-?\d+)/90000 dts=(-?\d+)/90000', result.stdout)
    assert len(lines) == 60 and 'V8_AUDIT_PASS samples=60 no_VT_session=1' in result.stdout
    offset = int(lines[0][6]) - reference['packets'][0]['dts']
    for line, packet in zip(lines, reference['packets']):
        i, size, compressed, normalized_size, normalized, pts, dts = line
        assert int(i) == packet['index'] and int(size) == int(packet['size'])
        assert compressed == packet['data_hash'][7:] and normalized == packet['normalized_sha256']
        assert int(normalized_size) == packet['normalized_bytes']
        assert int(pts) == packet['pts'] and int(dts) == packet['dts'] + offset
    s = reference['stream']
    frame = s['width'] * s['height'] * 3 // 2
    pixels = subprocess.check_output(['ffmpeg', '-nostdin', '-v', 'error', '-hwaccel', 'none', '-i', str(movie), '-pix_fmt', 'nv12', '-f', 'rawvideo', '-'])
    assert len(pixels) == 60 * frame
    for i, truth in enumerate(reference['output']):
        assert hashlib.sha256(pixels[i*frame:(i+1)*frame]).hexdigest() == truth['sha256']
    print(result.stdout.splitlines()[0])
    print(f'AVC_FILES_PASS {name} packets=60 normalization=60 software_NV12=60 CoreMedia=60 no_VT_session=1')
