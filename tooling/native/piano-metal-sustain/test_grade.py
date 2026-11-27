# SPDX-License-Identifier: MIT
"""Exercise the offline grader with generated, synthetic captures."""

import csv
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


GRADER = Path(__file__).with_name('grade.py')
BEGIN = 'PIANO_METAL_SUSTAIN Fixture BEGIN\n'
END = 'PIANO_METAL_SUSTAIN Fixture END\n'
TOUCH = 'PianoTouch: contact 7 down, peak 100, area 4 nodes\n'
HEARTBEAT = 'PianoA830: R3b LV at-us={} lvl=4 mhz=300\n'
SAFARI = 'PianoA830: Safari opens the accelerator, type 0x4147: 0 (at 132000 ms)\n'


def write_capture(root, *, slow_p99=False, low_fps=False, events='', before_begin='', after_end='',
                  recovery=False, command_error=False):
    boot = 'BOOT=00000000-0000-4000-8000-000000000000\n'
    counters = ('replay 0 recoveries {} failures 0 reports 0 watchdog 0 '
                'jobs_failed 0 lost_cause 0\n')
    (root / 'before.txt').write_text(boot + 'at_us=99000000 lvl=2 mhz=200\n' + counters.format(0))
    (root / 'after.txt').write_text(boot + 'at_us=701000000 lvl=4 mhz=300\n' + counters.format(int(recovery)))
    (root / 'run.txt').write_text(f'METAL_SUSTAIN_REMOTE_RC={int(command_error)}\n')
    (root / 'console.txt').write_text(before_begin + BEGIN + HEARTBEAT.format(100_000_000) + events + END + after_end)

    summary = ['METAL_SUSTAIN_BEGIN mode=onscreen seconds=600 width=1920 height=1280 draws=257 '
               'device="Adreno Synthetic" start=100.000000000']
    previous = None
    sequence = 0
    with (root / 'frames.csv').open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(('frame', 'window', 'cpu_begin_s', 'cpu_encode_begin_s', 'cpu_submit_s',
                         'gpu_start_s', 'gpu_end_s', 'completion_callback_s', 'status'))
        for window in range(60):
            elapsed = window * 10
            frame_count = 299 if low_fps and window == 3 else 300
            intervals = []
            for index in range(frame_count):
                completion = 100_020 + elapsed * 1000 + index * 10_000 // frame_count
                if slow_p99 and window == 3 and index in (1, 3, 5, 7, 9):
                    completion += 20
                timestamps = [f'{(completion - offset) / 1000:.9f}' for offset in (10, 9, 8, 7, 2, 0)]
                writer.writerow((sequence, window, *timestamps, 'completed'))
                sequence += 1
                if previous is not None:
                    intervals.append(completion - previous)
                previous = completion
            intervals.sort()
            stats = ' '.join(f'render_frame_{label}_ms={intervals[math.ceil(len(intervals) * rank) - 1]}'
                             for label, rank in (('p50', .50), ('p95', .95), ('p99', .99), ('max', 1)))
            summary.append(f'GPU_STATE window={window} elapsed_s={elapsed + 1} '
                           f'at_us={(101 + elapsed) * 1_000_000} lvl=4 mhz=300')
            summary.append(f'WINDOW index={window} start_s={elapsed} seconds=10.000 submitted={frame_count} '
                           f'completed={frame_count} errors=0 pending=0 freq_samples=1 min_level=4 max_level=4 '
                           f'max_mhz=300 gpu_timed={frame_count} presented=0 render_fps={frame_count / 10:.6f} '
                           f'render_frame_n={len(intervals)} render_frame_stalls={sum(t > 50 for t in intervals)} '
                           f'{stats} gpu_work_max_ms=5.000000')
    if command_error:
        summary.append('METAL_SUSTAIN_COMMAND_ERROR frame=900')
    summary.append(f'METAL_SUSTAIN_END rc={int(command_error)} elapsed_s=600.000000 windows=60 unfinished=0')
    (root / 'summary.log').write_text('\n'.join(summary) + '\n')


class GradeTests(unittest.TestCase):
    def grade(self, replacements=(), **fixture):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_capture(root, **fixture)
            for name, old, new in replacements:
                path = root / name
                text = path.read_text()
                self.assertIn(old, text)
                path.write_text(text.replace(old, new, 1))
            return subprocess.run([sys.executable, str(GRADER), directory], check=False,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def test_snapshots_require_unambiguous_boot_and_recovery_counters(self):
        for old, new, reason in (
                ('BOOT=00000000-0000-4000-8000-000000000000', 'BOOT=' + '-' * 36, 'boot UUID'),
                ('reports 0', 'reports 1 reports 0', 'recovery counters'),
                ('reports 0', 'reports -1', 'recovery counters'),
                ('reports 0', 'reports 4294967296', 'recovery counters')):
            with self.subTest(new=new):
                result = self.grade(replacements=[(name, old, new) for name in ('before.txt', 'after.txt')])
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn(reason, result.stdout)

    def test_conflicting_remote_exit_status_fails(self):
        result = self.grade(replacements=[('run.txt', 'METAL_SUSTAIN_REMOTE_RC=0\n',
                                          'METAL_SUSTAIN_REMOTE_RC=1\nMETAL_SUSTAIN_REMOTE_RC=0\n')])
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('command did not confirm exit 0', result.stdout)

    def test_out_of_range_frequency_cannot_prove_a_step(self):
        result = self.grade(replacements=[('summary.log', 'lvl=4 mhz=300', 'lvl=255 mhz=300'),
                                          ('summary.log', 'min_level=4 max_level=4', 'min_level=255 max_level=255')])
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('invalid frequency sample', result.stdout)

    def test_undisturbed_pass_ignores_events_outside_markers(self):
        events = ('PianoTouch: native: contact 7 down as touch 1\n'
                  'PianoTouch: contact 7 up after 50 ms\nPianoA830: Safari reads MetalPluginName\n')
        result = self.grade(before_begin=TOUCH, after_end=SAFARI, events=events)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('PASS installed Metal sustain', result.stdout)
        self.assertIn('touch_contacts=0 browser_accelerator_opens=0', result.stdout)
        self.assertIn('undisturbed_windows=60/60', result.stdout)
        self.assertIn('min_10s_fps=30.00', result.stdout)
        self.assertIn('WINDOW index=3 fps=30.00 p99_ms=34.00 gpu_work_max_ms=5.000', result.stdout)
        self.assertEqual(result.stdout.count('gpu_work_max_ms='), 60)

    def test_undisturbed_high_p99_fails(self):
        result = self.grade(slow_p99=True)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('FAIL installed Metal sustain: performance floor', result.stdout)
        self.assertIn('p99 > 40 ms in [3]', result.stdout)
        self.assertNotIn('VOID', result.stdout)

    def test_undisturbed_low_fps_fails(self):
        result = self.grade(low_fps=True)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('fps < 30 in [3]', result.stdout)
        self.assertNotIn('VOID', result.stdout)

    def test_touch_and_safari_void_high_p99(self):
        events = HEARTBEAT.format(129_500_000) + TOUCH + HEARTBEAT.format(130_500_000) + SAFARI * 2
        result = self.grade(slow_p99=True, events=events)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn('VOID installed Metal sustain (CONFOUND)', result.stdout)
        self.assertIn('touch_contacts=1 browser_accelerator_opens=2', result.stdout)
        self.assertIn('undisturbed_windows=58/60 confound_windows=[2, 3]', result.stdout)
        self.assertIn('p99 > 40 ms in [3]', result.stdout)
        self.assertNotIn('PASS installed Metal sustain', result.stdout)
        self.assertNotIn('GPU hang/reset/error', result.stdout)

    def test_browser_opens_void_even_within_performance_floor(self):
        events = ('PianoA830: Google Chrome opens the accelerator, type 0x4147: 0 (at 124000 ms)\n'
                  'PianoA830: com.apple.WebKit.GP opens the accelerator, type 0x4147: 0 (at 135000 ms)\n')
        result = self.grade(events=events)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn('touch_contacts=0 browser_accelerator_opens=2', result.stdout)
        self.assertIn('undisturbed_windows=58/60 confound_windows=[2, 3]', result.stdout)
        self.assertNotIn('Performance floor missed', result.stdout)
        self.assertNotIn('PASS installed Metal sustain', result.stdout)

    def test_touch_without_following_heartbeat_voids_run(self):
        result = self.grade(events=TOUCH)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn('touch_contacts=1 browser_accelerator_opens=0', result.stdout)
        self.assertIn('undisturbed_windows=0/60', result.stdout)
        self.assertNotIn('PASS installed Metal sustain', result.stdout)

    def test_gpu_fault_takes_precedence_over_touch(self):
        cases = [(f'{TOUCH}PianoA830: GPU hang\n', {}, 'GPU hang/reset/error'),
                 (f'{TOUCH}PianoA830: GPU reset\n', {}, 'GPU hang/reset/error'),
                 (TOUCH, {'recovery': True}, 'GPU recovery/fault/watchdog changed'),
                 (TOUCH, {'command_error': True}, 'Metal or command-buffer error')]
        for events, fixture, diagnostic in cases:
            with self.subTest(events=events, fixture=fixture):
                result = self.grade(events=events, slow_p99=True, **fixture)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn('FAIL installed Metal sustain', result.stdout)
                self.assertIn(diagnostic, result.stdout)
                self.assertIn('touch_contacts=1 browser_accelerator_opens=0', result.stdout)
                self.assertNotIn('VOID', result.stdout)


if __name__ == '__main__':
    unittest.main()
