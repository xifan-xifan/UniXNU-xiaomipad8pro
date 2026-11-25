#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""Audit externally supplied hvc1 files without a VideoToolbox session.

Build hevc_host.cpp and pass --host PATH followed by movies. No external movie
or extracted parameter set is installed in this repository. --expect-golden
additionally verifies the independently recorded target-asset identity/counts.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def audit(host, movie, golden):
    report = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_data',
        '-show_entries', 'stream=codec_name,codec_tag_string,width,height,profile,extradata:packet=pos,size,pts,dts,flags',
        '-of', 'json', str(movie),
    ]))
    stream, = report['streams']
    assert stream['codec_name'] == 'hevc' and stream['codec_tag_string'] == 'hvc1'
    hvcc = bytes.fromhex(''.join(line.split(': ', 1)[1].split('  ', 1)[0].replace(' ', '')
                               for line in stream['extradata'].splitlines() if ': ' in line))
    assert hvcc[0] == 1 and hvcc[21] & 3 == 3
    vcl, temporal, nal = collections.Counter(), collections.Counter(), collections.Counter()
    sizes, negatives = [], 0
    with tempfile.TemporaryDirectory(prefix='hevc-audit-') as folder:
        config, samples = Path(folder) / 'config.bin', Path(folder) / 'samples.bin'
        config.write_bytes(hvcc)
        with movie.open('rb') as source, samples.open('wb') as output:
            for packet in report['packets']:
                size, position = int(packet['size']), int(packet['pos'])
                source.seek(position)
                data = source.read(size)
                assert len(data) == size
                at, picture = 0, []
                while at < size:
                    length, = struct.unpack_from('>I', data, at)
                    at += 4
                    n = data[at:at + length]
                    assert length >= 3 and len(n) == length
                    kind, tid = (n[0] >> 1) & 63, (n[1] & 7) - 1
                    assert not n[0] & 0x81 and not n[1] & 0xf8 and tid >= 0
                    nal[kind] += 1
                    if kind < 32:
                        picture.append(kind)
                        vcl[kind] += 1
                        temporal[tid] += 1
                        assert bool(n[2] & 0x80)  # first_slice_segment_in_pic_flag
                    at += length
                assert at == size and len(picture) == 1
                assert bool('K' in packet['flags']) == bool(16 <= picture[0] <= 21)
                if not sizes:
                    assert picture[0] in (19, 20)
                negatives += int(packet.get('dts', 0)) < 0
                output.write(struct.pack('>I', size))
                output.write(data)
                sizes.append(size)
        result = subprocess.run([str(host), str(config), str(samples)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
    line, = [line for line in result.stdout.splitlines() if line.startswith('HEVC_CORPUS_PASS ')]
    fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', line)}
    assert fields['samples'] == len(sizes) and fields['max_au'] == max(sizes)
    assert fields['width'] == stream['width'] and fields['height'] == stream['height']
    assert fields['temporal_mask'] == sum(1 << i for i in temporal)
    assert fields['nal_mask'] == sum(1 << i for i in vcl)
    if golden:
        assert hashlib.sha256(movie.read_bytes()).hexdigest() == '2d647fcf9ae0421671e4dfc16fd4a5b5475be61a9087451e8a4eea0359ec3844'
        assert len(sizes) == 14400 and max(sizes) == 542920
        assert vcl == {19: 13, 1: 888, 2: 7104, 3: 6216, 7: 179}
        assert set(temporal) == set(range(5)) and nal[40] == 13
        assert fields['depth'] == 10 and fields['profile'] == 2 and fields['level'] == 183 and fields['tier'] == 1
        assert fields['RPS'] == 36 and fields['WPP'] == 1 and fields['dpb'] == 6 and fields['reorder'] == 4
    print(line)
    print('HEVC_FILES_PASS ' + json.dumps({'movie': movie.name, 'samples': len(sizes), 'vcl': vcl,
          'temporal': temporal, 'negative_dts': negatives, 'SEI_retained': nal[39] + nal[40],
          'normalized_with_and_without_parameters': len(sizes), 'no_VT_session': True}, sort_keys=True))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True, type=Path)
    parser.add_argument('--expect-golden', action='store_true')
    parser.add_argument('movies', type=Path, nargs='+')
    args = parser.parse_args()
    assert not args.expect_golden or len(args.movies) == 1
    for path in args.movies:
        audit(args.host.resolve(), path, args.expect_golden)
