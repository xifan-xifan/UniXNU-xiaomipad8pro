#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Grade one installed-system capture offline."""
import csv
import math
import re
import sys
from pathlib import Path

SECONDS = 600
WINDOW = 10
FPS_MIN = 30.0
P99_MAX_MS = 40.0
RECOVERY_KEYS = ('recoveries', 'failures', 'reports', 'watchdog', 'jobs_failed', 'lost_cause')
BOOT_UUID = re.compile(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', re.I)
FAULT = re.compile(
    r'GPU (?:LOST|RECOVER\b|restarts?\b|restarted\b|resets?\b|resetting\b|hang\b|hung\b|context is lost)'
    r'|DEVICE LOST|hang snapshot|recovery done|recovery: (?:culprit|the driver)'
    r'|MTLCommandBuffer(?:StatusError|ErrorDomain)|R3a UNSAFE|R3b3 exit|panic\(cpu', re.I)
FREQUENCY = re.compile(r'^at_us=(\d+) lvl=(\d+) mhz=(\d+)(?=\s|$)')
CONSOLE_MARKER = re.compile(r'PIANO_METAL_SUSTAIN ([A-Za-z0-9]+) (BEGIN|END)\b')
CONSOLE_EVENT = re.compile(
    r'(?P<touch>PianoTouch: contact [0-9]+ down\b)'
    r'|(?P<browser>PianoA830: (?:Safari|Google Chrome|com\.apple\.WebKit)[^\s]* opens the accelerator\b)'
    r'(?:, type \S+: \S+ \(at (?P<browser_ms>[0-9]+) ms\))?'
    r'|R3b LV at-us=(?P<heartbeat_us>[0-9]+) lvl=\d+ mhz=\d+\b')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def valid_frequency(level, mhz):
    # The fullopp guard reports GMU levels 0 through 14, with 0 representing off.
    return 0 <= level <= 14 and 0 <= mhz <= 1100 and (level == 0) == (mhz == 0)


def snapshot(path):
    lines = path.read_text().splitlines()
    boots = [line.removeprefix('BOOT=') for line in lines if line.startswith('BOOT=')]
    require(len(boots) == 1 and BOOT_UUID.fullmatch(boots[0]), 'missing or invalid boot UUID')
    states = [match for line in lines if (match := FREQUENCY.match(line))]
    require(len(states) == 1, 'missing or ambiguous GPU state')
    at, level, mhz = map(int, states[0].groups())
    require(at > 0 and valid_frequency(level, mhz), 'invalid GPU state frequency')
    rows = [line.split() for line in lines if line.startswith('replay ')]
    require(len(rows) == 1 and len(rows[0]) % 2 == 0, 'incomplete or ambiguous GPU recovery counters')
    keys, values = rows[0][::2], rows[0][1::2]
    require(len(keys) == len(set(keys)) and set(RECOVERY_KEYS).issubset(keys) and
            all(re.fullmatch(r'[0-9]{1,10}', v) and int(v) <= 0xffffffff for v in values),
            'incomplete or invalid GPU recovery counters')
    recovery = {key: int(value) for key, value in zip(keys, values) if key in RECOVERY_KEYS}
    return boots[0].lower(), (at, level, mhz), recovery


def percentile(samples, p):
    return samples[math.ceil(len(samples) * p) - 1]


def number(cell):
    value = float(cell)
    require(math.isfinite(value) and value > 0, 'invalid frame timestamp')
    return value


def console_confounds(console):
    touches = browser_opens = 0
    ranges = []
    previous_heartbeat = -math.inf
    untimed_event = False
    for event in CONSOLE_EVENT.finditer(console):
        if event['heartbeat_us'] is not None:
            heartbeat = int(event['heartbeat_us']) / 1_000_000
            if untimed_event:
                ranges.append((min(previous_heartbeat, heartbeat), max(previous_heartbeat, heartbeat)))
                untimed_event = False
            previous_heartbeat = heartbeat
        elif event['touch'] is not None:
            touches += 1
            untimed_event = True
        else:
            browser_opens += 1
            if event['browser_ms'] is None:
                untimed_event = True
            else:
                stamp = int(event['browser_ms']) / 1000
                ranges.append((stamp, stamp))
    if untimed_event:
        ranges.append((previous_heartbeat, math.inf))
    return touches, browser_opens, ranges


def grade(root):
    console = (root / 'console.txt').read_text(errors='replace')
    markers = list(CONSOLE_MARKER.finditer(console))
    require(len(markers) == 2 and markers[0][1] == markers[1][1] and
            [marker[2] for marker in markers] == ['BEGIN', 'END'], 'incomplete console window')
    bounded_console = console[markers[0].end():markers[1].start()]
    touches, browser_opens, confound_ranges = console_confounds(bounded_console)
    print(f'Console confounds touch_contacts={touches} browser_accelerator_opens={browser_opens}')
    fault = FAULT.search(console)
    require(fault is None, f'GPU hang/reset/error in console window: {fault.group() if fault else ""}')
    require(re.search(r'R3b LV .*\blvl=\d+ .*\bmhz=\d+', bounded_console), 'no in-window GPU console heartbeat')

    before_boot, before_freq, before_recovery = snapshot(root / 'before.txt')
    after_boot, after_freq, after_recovery = snapshot(root / 'after.txt')
    require(before_boot == after_boot, 'boot changed during the test')
    require(before_recovery == after_recovery and before_recovery['lost_cause'] == 0,
            f'GPU recovery/fault/watchdog changed or GPU lost: {before_recovery} -> {after_recovery}')

    log = (root / 'summary.log').read_text().splitlines()
    require(not any(line.startswith(('METAL_SUSTAIN_ERROR ', 'METAL_SUSTAIN_COMMAND_ERROR ')) for line in log),
            'Metal or command-buffer error')
    statuses = [line for line in (root / 'run.txt').read_text().splitlines() if line.startswith('METAL_SUSTAIN_REMOTE_RC=')]
    require(statuses == ['METAL_SUSTAIN_REMOTE_RC=0'],
            'console-user command did not confirm exit 0')
    begins = [line for line in log if line.startswith('METAL_SUSTAIN_BEGIN ')]
    ends = [fields(line) for line in log if line.startswith('METAL_SUSTAIN_END ')]
    require(len(begins) == 1 and 'device="Adreno' in begins[0], 'Adreno hardware start is missing')
    begin = fields(begins[0])
    require(all(begin.get(k) == v for k, v in dict(mode='onscreen', seconds='600', width='1920',
                                                   height='1280', draws='257').items()), 'wrong workload settings')
    require(len(ends) == 1 and all(ends[0].get(k) == v for k, v in
            dict(rc='0', elapsed_s='600.000000', windows='60', unfinished='0').items()), 'incomplete 600-second run')
    summaries = [fields(line) for line in log if line.startswith('WINDOW ')]
    require(len(summaries) == SECONDS // WINDOW, 'missing 10-second windows')

    frequency = [fields(line) for line in log if line.startswith('GPU_STATE ')]
    frequency_at = before_freq[0]
    low_seen = before_freq[1] <= 3
    stepped = False
    readings = [[] for _ in summaries]
    for state in frequency:
        i, at, level, mhz = (int(state[k]) for k in ('window', 'at_us', 'lvl', 'mhz'))
        elapsed = float(state['elapsed_s'])
        require(0 <= i < len(readings) and i * WINDOW <= elapsed <= (i + 1) * WINDOW and
                frequency_at <= at <= after_freq[0] and valid_frequency(level, mhz), 'invalid frequency sample')
        require(not readings[i] or at > readings[i][-1][0], 'repeated or out-of-order frequency sample')
        frequency_at = at
        low_seen |= level <= 3
        stepped |= low_seen and level > 3
        readings[i].append((at, level, mhz))
    require(stepped, 'no observed frequency step from level 3 or below to above level 3')

    with (root / 'frames.csv').open(newline='') as stream:
        rows = csv.DictReader(stream)
        cohorts = [[] for _ in summaries]
        sequence, last_begin = None, 0.0
        start = number(begin['start'])
        for row in rows:
            i, seq = int(row['window']), int(row['frame'])
            cpu, encode, submit, completion = (number(row[k]) for k in
                ('cpu_begin_s', 'cpu_encode_begin_s', 'cpu_submit_s', 'completion_callback_s'))
            require(0 <= i < len(cohorts) and abs(cpu - start - i * WINDOW - WINDOW / 2) <= WINDOW / 2 + 1e-6,
                    'frame outside its window')
            require(row['status'] == 'completed' and last_begin <= cpu <= encode <= submit <= completion and
                    (sequence is None or seq == sequence + 1), 'truncated or invalid frame sequence')
            sequence, last_begin = seq, cpu
            cohorts[i].append(row)

    last_completion = 0.0
    worst_p99, lowest_fps, stalls, total, presented = 0.0, math.inf, 0, 0, 0
    stall_windows, slow_windows, low_fps_windows = [], [], []
    window_metrics = []
    for i, (summary, frames, samples) in enumerate(zip(summaries, cohorts, readings)):
        require(int(summary['index']) == i and int(summary['start_s']) == i * WINDOW and
                float(summary['seconds']) == WINDOW and int(summary['submitted']) == len(frames) and
                int(summary['completed']) == len(frames) and summary['errors'] == summary['pending'] == '0',
                f'window {i}: frame counts do not match')
        require(samples and int(summary['freq_samples']) == len(samples) and
                int(summary['min_level']) == min(s[1] for s in samples) and
                int(summary['max_level']) == max(s[1] for s in samples) and
                int(summary['max_mhz']) == max(s[2] for s in samples), f'window {i}: missing or invalid frequency readings')
        gpu_work = []
        for frame in frames:
            if frame['gpu_start_s'] and frame['gpu_end_s']:
                duration = number(frame['gpu_end_s']) - number(frame['gpu_start_s'])
                if duration > 0:
                    gpu_work.append(duration * 1000)
        require(gpu_work and len(gpu_work) == int(summary['gpu_timed']), f'window {i}: GPU timing missing or invalid')
        completions = sorted(number(f['completion_callback_s']) for f in frames)
        distinct = []
        for stamp in completions:
            if stamp <= last_completion:
                continue
            distinct.append((stamp, last_completion))
            last_completion = stamp
        intervals = sorted((stamp - last) * 1000 for stamp, last in distinct if last)
        fps = len(distinct) / WINDOW
        require(intervals and int(summary['render_frame_n']) == len(intervals) and
                abs(float(summary['render_fps']) - fps) < 1e-5, f'window {i}: incomplete completion cadence')
        for key, value in (('p50', percentile(intervals, .50)), ('p95', percentile(intervals, .95)),
                           ('p99', percentile(intervals, .99)), ('max', intervals[-1])):
            require(abs(float(summary[f'render_frame_{key}_ms']) - value) < .01,
                    f'window {i}: completion {key} does not match CSV')
        p99 = percentile(intervals, .99)
        window_stalls = sum(t > 50 for t in intervals)
        require(abs(int(summary['render_frame_stalls']) - window_stalls) <= sum(abs(t - 50) <= .002 for t in intervals),
                f'window {i}: completion stall count does not match CSV')
        if window_stalls:
            stall_windows.append(i)
        if fps < FPS_MIN:
            low_fps_windows.append(i)
        # The CSV rounds absolute stamps to nanoseconds before subtraction.
        if p99 > P99_MAX_MS + .000002:
            slow_windows.append(i)
        window_metrics.append((fps, p99, max(gpu_work)))
        worst_p99, lowest_fps = max(worst_p99, p99), min(lowest_fps, fps)
        stalls += window_stalls
        total += len(distinct)
        presented += int(summary['presented'])

    # Untimestamped events may fall anywhere between their surrounding console heartbeats.
    confound_windows = [i for i in range(len(summaries)) if any(
        first < start + (i + 1) * WINDOW and last >= start + i * WINDOW for first, last in confound_ranges)]
    for i, (fps, p99, gpu_work_max) in enumerate(window_metrics):
        print(f'WINDOW index={i} fps={fps:.2f} p99_ms={p99:.2f} gpu_work_max_ms={gpu_work_max:.3f}')
    peak_level = max(level for samples in readings for _, level, _ in samples)
    print(f'Adreno 3D completed={total} fps={total / SECONDS:.2f} min_10s_fps={lowest_fps:.2f} '
          f'worst_10s_p99_ms={worst_p99:.2f} stalls_gt_50_ms={stalls} stall_windows={stall_windows} '
          f'peak_level={peak_level} presented={presented} '
          f'undisturbed_windows={len(summaries) - len(confound_windows)}/{len(summaries)} '
          f'confound_windows={confound_windows}')
    if not presented:
        print('Drawable presentation timestamps unavailable. FPS and frame intervals use successful Metal completions.')
    floor_failure = low_fps_windows or slow_windows
    floor_details = f'fps < {FPS_MIN:g} in {low_fps_windows}, p99 > {P99_MAX_MS:g} ms in {slow_windows}'
    if touches or browser_opens:
        if floor_failure:
            print(f'Performance floor missed ({floor_details})')
        print('VOID installed Metal sustain (CONFOUND): external interaction in bounded console capture')
        return 2
    require(not floor_failure, f'performance floor: {floor_details}')
    print('PASS installed Metal sustain')
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: grade.py RESULT_DIRECTORY')
    try:
        sys.exit(grade(Path(sys.argv[1])))
    except (KeyError, OSError, StopIteration, TypeError, ValueError, csv.Error) as error:
        sys.exit(f'FAIL installed Metal sustain: {error or "missing capture record"}')
