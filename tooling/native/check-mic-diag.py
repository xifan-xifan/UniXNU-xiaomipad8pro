#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""Extra acceptance for a direct-driver recording and the supplied coded fixture.

The original measure.py supplies carrier/envelope/gain/noise measurements.
This checker adds explicit five-second PCM windows, a separate silent-source
window, exact software mute, transport health and diagnostic lifecycle checks.
No device access; arguments are saved artifacts only.
"""
import argparse
import importlib.util
import json
import math
import re
from pathlib import Path


def load_measure(path):
    spec = importlib.util.spec_from_file_location('fixture_measure', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check(ok, label):
    print(('PASS ' if ok else 'FAIL ') + label)
    return not ok


def windows(measure, pcm, timeline, report):
    m = load_measure(measure)
    text = Path(report).read_text()
    info, _ = json.JSONDecoder().raw_decode(text)
    x, rate, channels = m.read(pcm)
    rows = [json.loads(s) for s in Path(timeline).read_text().splitlines() if s]
    plays = [r for r in rows if r['event'] == 'play']
    bad = check(not info['failures'] and rate == 48000 and channels == 1,
                'coded tone/gain/mute/recovery report and mono S16 48k PCM')
    # Use the alignment selected by TWO independent unity chip envelopes.
    origin = plays[0]['before'] - info['file_onset']
    for p in plays:
        start = p['before'] - origin - .75
        end = start + 5
        bad += check(start >= 0 and end <= len(x) / rate,
                     p['phase'] + ' has a complete contiguous 5-second PCM window')
        if start < 0 or end > len(x) / rate:
            continue
        item = m.metrics(x, rate, start, end)
        print(json.dumps(dict(phase=p['phase'], window_seconds=5, **item), sort_keys=True))
        if p['phase'] == 'mute':
            # Controls were applied >=2 seconds before playback and retained
            # >=3 seconds afterwards. This window includes no transitions.
            values = x[round(start * rate):round(end * rate)]
            bad += check(len(values) == 5 * rate and not any(values),
                         'input mute gives exactly zero PCM throughout five seconds')
    a = next(r['at'] for r in rows if r['event'] == 'silence-start')
    b = next(r['at'] for r in rows if r['event'] == 'silence-end')
    start = a - origin + .5
    bad += check(b - a >= 6 and start >= 0 and start + 5 <= len(x) / rate,
                 'separate source-off silence window has five seconds')
    if start >= 0 and start + 5 <= len(x) / rate:
        quiet = m.metrics(x, rate, start, start + 5)
        # An unmodulated room line cannot pass the separate chip-envelope gate.
        tone = info['phases'][0]['carrier_amplitude']
        suppression = 20 * math.log10(max(tone, 1e-12)) - quiet['tone_dbfs']
        print(json.dumps(dict(source_off=quiet, tone_over_source_off_db=suppression), sort_keys=True))
        bad += check(suppression >= 15, 'no 997Hz tone with source off (>=15dB suppression)')
    return bad


def lifecycle(console, logs):
    text = Path(console).read_text(errors='replace')
    bad = check('PianoAudio:' in text, 'current-window console audio known-positive')
    for log in logs:
        content = Path(log).read_text()
        record = re.search(r'^CAPTURE PASS seed=(\d+) frames=(\d+) blocks=(\d+) anchors=(\d+) '
                           r'stop_rc=0x0 unmap_ring=0x0 unmap_status=0x0$', content, re.M)
        bad += check(bool(record) and 'CONNECTION_CLOSED rc=0x0 result=0' in content,
                     Path(log).name + ' explicit stop/unmap/connection close')
        if record:
            seed, frames, blocks, anchors = map(int, record.groups())
            bad += check(seed > 0 and frames >= 2 * 48000 and blocks * 480 == frames and anchors > 0,
                         Path(log).name + ' real clock progress and exact copied frame count')
    # Reuse the supplied existing driver layer checker as a separate gate.
    # Here require two distinct engine epochs, not merely two copied log lines.
    seeds = set(re.findall(r'HAL ring: input mode 0 ring 7680.* seed (\d+)', text))
    bad += check(len(seeds) >= 2, 'two distinct input engine seeds after full release')
    return bad


def main():
    p = argparse.ArgumentParser(description=__doc__)
    s = p.add_subparsers(dest='cmd', required=True)
    w = s.add_parser('windows')
    for name in ('measure', 'pcm', 'timeline', 'report'):
        w.add_argument(name)
    l = s.add_parser('lifecycle')
    l.add_argument('console')
    l.add_argument('logs', nargs=2)
    d = s.add_parser('lifecycle-dir', help='grade the final saved capture artifacts')
    d.add_argument('directory', type=Path)
    args = p.parse_args()
    try:
        if args.cmd == 'lifecycle-dir':
            # READY polling files and an older staged checker are not the final
            # capture. Use this committed checker and the completed probe logs.
            args.console = args.directory / 'mic-console-window.txt'
            args.logs = [args.directory / name for name in
                         ('first-probe.txt', 'restart-probe.txt')]
        bad = (windows(args.measure, args.pcm, args.timeline, args.report)
               if args.cmd == 'windows' else lifecycle(args.console, args.logs))
        print('mic diagnostic acceptance: ' + ('FAIL' if bad else 'PASS'))
        return bool(bad)
    except Exception as exc:
        print('FAIL HARNESS ' + str(exc))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
