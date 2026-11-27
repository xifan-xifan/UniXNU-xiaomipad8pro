#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Listen to the tablet through a microphone on the Mac: record, then find test tones (no numpy needed).

  audio-listen.py record --seconds N --out FILE.wav [--device "HD Pro Webcam C920"]   16 kHz mono through ffmpeg (avfoundation)
  audio-listen.py detect FILE.wav [--freq 1000] [--window 0.05] [--min-db 10]         the stretches where FREQ stands out

detect prints one line per stretch where the tone's power (Goertzel at FREQ) is at least --min-db above the mean of two
neighbouring probe frequencies (0.8 and 1.25 x FREQ) and above the recording's quiet floor: start and end (s from the start of
the file), duration, the tone's level (dBFS) and its margin. Then a summary line: `tones: N` and the levels in order, which
docs/drivers/audio.md's tone walk (four slots TL, TR, BL, BR, about 3 s each) maps to the speakers by order and timing.
"""
import argparse, math, subprocess, sys, time

from pcm16_wav import read_pcm16_header, read_samples


def record(args):
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        raise ValueError('expected a positive recording duration')
    loglevel = 'info' if args.progress else 'error'
    cmd = ['ffmpeg', '-hide_banner', '-loglevel', loglevel, '-f', 'avfoundation', '-i', ':' + args.device,
           '-t', str(args.seconds)]
    if args.progress:
        # Keep the input and duration arguments in the C920 shim's order.
        # Its GUI daemon watches ashowinfo for PCM; direct callers see progress.
        cmd += ['-af', 'ashowinfo', '-progress', 'pipe:1', '-stats_period', '0.2']
    cmd += ['-ac', '1', '-ar', '16000', '-y', args.out]
    if not args.progress:
        return subprocess.call(cmd)

    start = time.monotonic()
    status = subprocess.run(cmd, timeout=args.seconds + 15).returncode
    if status == 0 and time.monotonic() - start < args.seconds - 0.25:
        print('audio-listen: capture ended before the requested wall-clock duration', file=sys.stderr)
        return 2
    return status


def goertzel_power(samples, start, n, freq, rate):
    k = 2.0 * math.cos(2.0 * math.pi * freq / rate)
    s1 = s2 = 0.0
    for i in range(start, start + n):
        s0 = samples[i] + k * s1 - s2
        s2, s1 = s1, s0
    p = s1 * s1 + s2 * s2 - k * s1 * s2
    return p / (n * n / 4.0)          # a full-scale sine at freq gives about 1.0


def db(p):
    return 10.0 * math.log10(p) if p > 1e-20 else -200.0


def detect(args):
    if not all(math.isfinite(v) for v in (args.freq, args.window, args.min_db, args.min_len)):
        raise ValueError('expected finite detection parameters')
    with open(args.file, 'rb') as wav:
        rate, channels, frames = read_pcm16_header(wav)
        if not 0 < args.freq < rate / 2 or args.window <= 0 or args.min_len < 0:
            raise ValueError('invalid detection frequency, window or minimum length')
        n = max(64, int(args.window * rate))
        raw = read_samples(wav, frames, channels)
    samples = [v / 32768.0 for v in raw[::channels]]
    window = n / rate
    rows = []
    for start in range(0, len(samples) - n + 1, n):
        tone = goertzel_power(samples, start, n, args.freq, rate)
        probe = (goertzel_power(samples, start, n, args.freq * 0.8, rate) +
                 goertzel_power(samples, start, n, args.freq * 1.25, rate)) / 2.0
        rows.append((start / rate, db(tone), db(tone) - db(probe)))
    if not rows:
        print('tones: 0 (recording too short)')
        return 1
    levels = sorted(r[1] for r in rows)
    floor = levels[len(levels) // 4]          # the quiet quarter: the room and the mic's own noise
    segs, cur = [], None
    for t, lvl, margin in rows:
        on = margin >= args.min_db and lvl >= floor + args.min_db
        if on and cur is None:
            cur = [t, t, [lvl], [margin]]
        elif on:
            cur[1] = t; cur[2].append(lvl); cur[3].append(margin)
        elif cur is not None:
            segs.append(cur); cur = None
    if cur is not None:
        segs.append(cur)
    segs = [s for s in segs if s[1] + window - s[0] >= args.min_len]
    print(f'floor {floor:.1f} dBFS at {args.freq:.0f} Hz ({len(rows)} windows of {window * 1000:.0f} ms)')
    for s in segs:
        lv = sorted(s[2])[len(s[2]) // 2]; mg = sorted(s[3])[len(s[3]) // 2]
        print(f'tone {s[0]:8.2f} s - {s[1] + window:8.2f} s ({s[1] + window - s[0]:.2f} s): {lv:.1f} dBFS, '
              f'{mg:.1f} dB over the probes, {lv - floor:.1f} dB over the floor')
    print(f'tones: {len(segs)}' + (' (levels ' + ', '.join(f'{sorted(s[2])[len(s[2]) // 2]:.1f}' for s in segs) + ' dBFS)'
                                  if segs else ''))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('record'); r.add_argument('--seconds', type=float, required=True); r.add_argument('--out', required=True)
    r.add_argument('--device', default='HD Pro Webcam C920')
    r.add_argument('--progress', action='store_true', help='emit ffmpeg progress and bound the capture time')
    d = sub.add_parser('detect'); d.add_argument('file'); d.add_argument('--freq', type=float, default=1000.0)
    d.add_argument('--window', type=float, default=0.05); d.add_argument('--min-db', type=float, default=10.0)
    d.add_argument('--min-len', type=float, default=0.3)
    args = ap.parse_args()
    try:
        return record(args) if args.cmd == 'record' else detect(args)
    except (OSError, OverflowError, ValueError, subprocess.TimeoutExpired) as error:
        print(f'audio-listen: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
