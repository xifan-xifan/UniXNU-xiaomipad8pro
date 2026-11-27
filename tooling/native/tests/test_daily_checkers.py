#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""Host regressions for battery units and final microphone lifecycle artifacts."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

NATIVE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('capacity', NATIVE / 'check-battery-capacity.py')
capacity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capacity)


class BatteryTests(unittest.TestCase):
    def test_raw_capacity_before_percentage(self):
        text = '      "AppleRawCurrentCapacity" = 2560\n\t"CurrentCapacity"\t=\t30\n'
        self.assertEqual(capacity.current_capacity(text), 30)

    def test_requires_exact_key_and_one_percentage(self):
        for text in ('"AppleRawCurrentCapacity" = 30\n',
                     'prefix "CurrentCapacity" = 30\n',
                     '"CurrentCapacity" = 101\n',
                     '"CurrentCapacity" = 30\n"CurrentCapacity" = broken\n',
                     '"CurrentCapacity" = 30\n"CurrentCapacity" = 31\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                capacity.current_capacity(text)


class MicTests(unittest.TestCase):
    def grade(self, change=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'mic-console-window.txt').write_text(
                'PianoAudio: HAL ring: input mode 0 ring 7680 seed 1\n'
                'PianoAudio: HAL ring: input mode 0 ring 7680 seed 2\n')
            for name, seed, blocks, anchors in (
                    ('first-probe.txt', 1, 5405, 338),
                    ('restart-probe.txt', 2, 908, 57)):
                frames = blocks * 480
                (root / name).write_text(
                    f'READY seed={seed} frames=96000 root=1 direct-driver=1\n'
                    f'CAPTURE_DONE frames={frames} seed={seed}\n'
                    f'CAPTURE PASS seed={seed} frames={frames} blocks={blocks} '
                    f'anchors={anchors} stop_rc=0x0 unmap_ring=0x0 unmap_status=0x0\n'
                    'CONNECTION_CLOSED rc=0x0 result=0\n')
            # An earlier readiness snapshot must not be graded instead.
            (root / 'ready-1.txt').write_text('READY seed=1 frames=96000\n')
            if change:
                change(root)
            return subprocess.run([sys.executable, str(NATIVE / 'check-mic-diag.py'),
                                   'lifecycle-dir', str(root)], capture_output=True, text=True)

    def test_completed_54_second_first_probe(self):
        result = self.grade()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('mic diagnostic acceptance: PASS', result.stdout)

    def test_incomplete_final_file_refused_despite_readiness(self):
        result = self.grade(lambda root: (root / 'first-probe.txt').write_text('READY\n'))
        self.assertNotEqual(result.returncode, 0)

    def test_wrong_frame_count_refused(self):
        def corrupt(root):
            p = root / 'first-probe.txt'
            p.write_text(p.read_text().replace('blocks=5405', 'blocks=5404'))
        self.assertNotEqual(self.grade(corrupt).returncode, 0)

    def test_missing_restart_close_refused(self):
        def corrupt(root):
            p = root / 'restart-probe.txt'
            p.write_text(p.read_text().replace('CONNECTION_CLOSED rc=0x0 result=0', ''))
        self.assertNotEqual(self.grade(corrupt).returncode, 0)

    def test_failed_or_ambiguous_restart_close_refused(self):
        for suffix in ('01', '1\nCONNECTION_CLOSED rc=0x0 result=0'):
            def corrupt(root):
                p = root / 'restart-probe.txt'
                p.write_text(p.read_text().replace('result=0', 'result=' + suffix))
            with self.subTest(suffix=suffix):
                self.assertNotEqual(self.grade(corrupt).returncode, 0)

    def test_capture_epochs_must_match_current_console(self):
        for seed in (1, 3):
            def corrupt(root):
                p = root / 'restart-probe.txt'
                p.write_text(p.read_text().replace('seed=2', f'seed={seed}'))
            with self.subTest(seed=seed):
                self.assertNotEqual(self.grade(corrupt).returncode, 0)


if __name__ == '__main__':
    unittest.main()
