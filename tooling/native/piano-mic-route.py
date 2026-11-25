#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Generate a coded fixture and check saved mic-route1 PCM without device access."""
import argparse
from array import array
from itertools import groupby
import json
import math
from pathlib import Path
import re
import struct
import sys
import wave

RATE = 48000
CHIP_MS = 100
CHIPS = (1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1)
DURATION = len(CHIPS) * CHIP_MS / 1000
HEADER = struct.Struct('<4sIIiIIII')
PERIOD = struct.Struct('<IIII')


def write_wav(path, samples):
    pcm = array('h', samples)
    if sys.byteorder != 'little':
        pcm.byteswap()
    with wave.open(str(path), 'wb') as out:
        out.setparams((1, 2, RATE, 0, 'NONE', 'not compressed'))
        out.writeframes(pcm.tobytes())


def fixture(path, carrier=997):
    span = RATE * CHIP_MS // 1000
    ramp = RATE // 500
    previous = 0
    samples = []
    for level in CHIPS:
        gain = 1 if level else 0.18
        for i in range(span):
            envelope = previous + (gain - previous) * min(1, i / ramp)
            if len(samples) >= span * len(CHIPS) - ramp:
                envelope *= (span * len(CHIPS) - 1 - len(samples)) / ramp
            samples.append(round(32767 * 0.1 * envelope * math.sin(math.tau * carrier * len(samples) / RATE)))
        previous = gain
    write_wav(path, samples)


def extract(path, prefix, expected):
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError('empty or truncated pre-mix window')
    magic, gen, run, route, start, end, frames, periods = HEADER.unpack_from(data)
    if magic != b'PMR1' or (gen, run, route) != expected:
        raise ValueError('wrong route generation/run or window format')
    offset = HEADER.size + periods * PERIOD.size
    if not 0 < periods <= 800 or not 0 < frames <= 384000 or len(data) != offset + frames * 4:
        raise ValueError('invalid pre-mix window size')
    arrivals = []
    dsp_times = []
    count = 0
    for host_ms, n, dsp_lo, dsp_hi in PERIOD.iter_unpack(data[HEADER.size:offset]):
        stamp = (host_ms - start) & 0xffffffff
        if not 100 <= stamp < 8000 or not 0 < n <= 480 or (arrivals and stamp < arrivals[-1]):
            raise ValueError('invalid pre-mix period timestamps or frame count')
        arrivals.append(stamp)
        dsp_times.append((dsp_hi << 32) | dsp_lo)
        count += n
    elapsed = (end - start) & 0xffffffff
    if count != frames or elapsed < arrivals[-1]:
        raise ValueError('inconsistent pre-mix totals')
    pcm = array('h')
    pcm.frombytes(data[offset:])
    if sys.byteorder != 'little':
        pcm.byteswap()
    dec0, dec1 = pcm[::2], pcm[1::2]
    write_wav(f'{prefix}-DEC0.wav', dec0)
    write_wav(f'{prefix}-DEC1.wav', dec1)
    expected_mono = (int((a + b) / 2) for a, b in zip(dec0, dec1)) if route == -1 else dec0
    write_wav(f'{prefix}-selected.wav', expected_mono)
    return dict(generation=gen, run=run, route=route, start_ms=start, end_ms=end, frames=frames,
                periods=periods, expected_period_frames=periods * 480,
                expected_span_frames=(arrivals[-1] - arrivals[0] + 10) * 48,
                first_arrival_ms=arrivals[0], first_dsp_timestamp=dsp_times[0], last_dsp_timestamp=dsp_times[-1],
                longest_arrival_gap_ms=max((b - a for a, b in zip(arrivals, arrivals[1:])), default=0))


def reader_frames(wav, log, seed):
    ready = wav.with_suffix('.ready').read_text().strip()
    done = wav.with_suffix('.done').read_text().strip()
    report = log.read_text()
    ready_match = re.fullmatch(r'READY seed=(\d+) frames=(\d+) root=1 direct-driver=1', ready)
    done_match = re.fullmatch(r'CAPTURE_DONE frames=(\d+) seed=(\d+)', done)
    result = re.search(r'^CAPTURE PASS seed=(\d+) frames=(\d+) blocks=(\d+) anchors=(\d+) '
                       r'stop_rc=0x0 unmap_ring=0x0 unmap_status=0x0$', report, re.M)
    if not (ready_match and done_match and result and 'CONNECTION_CLOSED rc=0x0 result=0' in report):
        raise ValueError('incomplete safe-reader lifecycle')
    ready_seed, min_frames = map(int, ready_match.groups())
    done_frames, done_seed = map(int, done_match.groups())
    result_seed, frames, blocks, anchors = map(int, result.groups())
    if not (ready_seed == done_seed == result_seed == seed and 0 < seed and
            96000 <= min_frames <= frames == done_frames == blocks * 480 and anchors > 0):
        raise ValueError('wrong safe-reader seed or frame count')
    return frames


def score(path, onset, carrier, search, expected_frames=None):
    with wave.open(str(path), 'rb') as f:
        if (f.getnchannels(), f.getsampwidth(), f.getframerate()) != (1, 2, RATE):
            raise ValueError('expected mono S16 48 kHz WAV')
        pcm = array('h')
        frames = f.getnframes()
        pcm.frombytes(f.readframes(frames))
    if len(pcm) != frames or (expected_frames is not None and frames != expected_frames):
        raise ValueError('wrong WAV frame count')
    if sys.byteorder != 'little':
        pcm.byteswap()
    width = RATE // 100
    cos = [math.cos(math.tau * carrier * k / RATE) for k in range(width)]
    sin = [math.sin(math.tau * carrier * k / RATE) for k in range(width)]
    first, last = round((onset - search) * 100), round((onset + search) * 100)
    chips = CHIP_MS // 10
    span = chips * len(CHIPS)
    if first > last or first < 75 or (last + span + 75) * width > len(pcm):
        raise ValueError('capture needs complete search, source-off, and source-on windows')

    def tone(block):
        x = pcm[block * width:(block + 1) * width]
        re = sum(a * b for a, b in zip(x, cos))
        im = sum(a * b for a, b in zip(x, sin))
        return 2 * math.hypot(re, im) / width

    amplitudes = [tone(b) for b in range(first - 75, last + span + 75)]

    def levels(start):
        return [sum(amplitudes[k + 2:k + chips - 2]) / (chips - 4)
                for k in range(start, start + span, chips)]

    def correlation(values):
        means = [sum(v for v, c in zip(values, CHIPS) if c == k) / CHIPS.count(k) for k in (0, 1)]
        mean = sum(values) / len(values)
        variance = sum((v - mean) ** 2 for v in values)
        # Pearson r for a binary reference, with positive polarity required.
        return ((means[1] - means[0]) * math.sqrt(CHIPS.count(0) * CHIPS.count(1) / len(CHIPS))
                / math.sqrt(variance)) if variance else 0.0

    start = max(range(75, last - first + 76), key=lambda k: correlation(levels(k)))
    r = correlation(levels(start))
    on = (first - 75 + start) * width
    end = on + span * width
    quiet = width * 50
    pad = width * 25
    off_pcm = pcm[on-pad-quiet:on-pad] + pcm[end+pad:end+pad+quiet]
    on_pcm = pcm[on:end]
    off_tones = amplitudes[start-75:start-25] + amplitudes[start+span+25:start+span+75]
    high = [v for v, c in zip(levels(start), CHIPS) if c]
    def dbfs(values):
        return 10 * math.log10(max(sum(x*x for x in values) / len(values), 1e-12) / 32768**2)

    snr = dbfs(high) - dbfs(off_tones)
    zero = max((sum(1 for _ in group) for is_zero, group in groupby(on_pcm, key=lambda x: x == 0) if is_zero), default=0)
    clip = sum(v in (-32768, 32767) for v in on_pcm) / len(on_pcm)
    return dict(passed=snr >= 10 and r >= 0.7 and zero < width * 2 and clip < 0.001, correlation=r,
                tone_over_off_db=snr, on_rms_dbfs=dbfs(on_pcm), off_rms_dbfs=dbfs(off_pcm),
                clipping_fraction=clip, longest_zero_frames=zero, scored_frames=len(on_pcm),
                wav_frames=len(pcm), found_onset_s=on / RATE)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    f = sub.add_parser('fixture')
    f.add_argument('wav', type=Path)
    f.add_argument('--carrier', type=int, default=997)
    e = sub.add_parser('extract')
    e.add_argument('window', type=Path)
    e.add_argument('prefix', type=Path)
    e.add_argument('--gen', type=int, required=True)
    e.add_argument('--run', type=int, required=True)
    e.add_argument('--route', type=int, required=True, choices=(-1, 0, 2, 4, 6))
    s = sub.add_parser('score')
    s.add_argument('wav', type=Path)
    s.add_argument('--onset', type=float, required=True, help='approximate source-on position from phase markers, seconds in WAV')
    s.add_argument('--search', type=float, default=.25)
    s.add_argument('--carrier', type=int, default=997)
    s.add_argument('--probe-log', type=Path)
    s.add_argument('--seed', type=int, help='current input HAL ring seed from the console')
    a = p.parse_args()
    try:
        if a.command == 'fixture':
            fixture(a.wav, a.carrier)
        elif a.command == 'extract':
            print(json.dumps(extract(a.window, a.prefix, (a.gen, a.run, a.route)), sort_keys=True))
        else:
            if (a.probe_log is None) != (a.seed is None):
                p.error('--probe-log and --seed must be used together')
            frames = reader_frames(a.wav, a.probe_log, a.seed) if a.probe_log else None
            result = score(a.wav, a.onset, a.carrier, a.search, frames)
            print(json.dumps(result, sort_keys=True))
            return not result['passed']
    except (ValueError, OSError, EOFError, wave.Error) as error:
        print(f'capture check failed: {error}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
