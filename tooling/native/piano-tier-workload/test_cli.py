#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build and test piano-tier-workload on the Mac without opening a Metal device."""

import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def snapshot(used=0, allocations=0, mode=1, state='ready', denials=0):
    return (f'mode={mode} state={state} runs=3 checked=3 pass=3 fail=0 pass-mask=0x7 '
            f'free-mib={870 - used} used-mib={used} refusals=0 allocs={allocations} '
            f'legacy-small=1 legacy-large=1 contiguous=0 private-req={allocations} '
            f'host-import=2 scanout-import=1 cpu-maps=3 tier-import-denied={denials} tier-cpu-map-denied=0\n')


class WorkloadCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = tempfile.TemporaryDirectory(prefix='piano-tier-workload-')
        cls.addClassCleanup(scratch.cleanup)
        out = Path(scratch.name)
        subprocess.run(['make', 'SDK=macosx', f'OUT={out}', 'all', 'test'], cwd=HERE, check=True)
        cls.binary = out / 'piano-tier-workload'

    def run_cli(self, args, status):
        result = subprocess.run([self.binary, *args], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        return result

    def report(self, lines, status=0, options=()):
        with tempfile.TemporaryDirectory() as scratch:
            paths = [Path(scratch) / label for label in ('before', 'during', 'after')]
            for path, line in zip(paths, lines):
                if line is not None:
                    path.write_bytes(line.encode() if isinstance(line, str) else line)
            return self.run_cli(['--tier-report', *paths, *options], status)

    def test_help(self):
        for flag in ('-h', '--help'):
            with self.subTest(flag=flag):
                result = self.run_cli([flag], 0)
                self.assertIn('--dry-run', result.stdout)
                self.assertIn('mode=1 state=ready', result.stdout)
                self.assertEqual(result.stderr, '')

    def test_dry_run_defaults(self):
        result = self.run_cli(['--dry-run'], 0)
        self.assertEqual(result.stderr, '')
        self.assertRegex(result.stdout, r'^config paths=r8g8,swap,ubwc sizes-mib=2,4,8,16,32,64 '
                         r'pairs=1 seconds=60 linger=10 expect-private-bos=6 expect-tier-mib=126 pid=\d+\n\Z')

    def test_dry_run_custom_plan(self):
        result = self.run_cli(['--dry-run', '--paths', 'ubwc,swap', '--sizes-mib', '8,3,2,4',
                               '--pairs', '16', '--seconds', '0.25', '--linger', '0'], 0)
        self.assertIn('paths=swap,ubwc sizes-mib=2,3,4,8 pairs=16 seconds=0.25 linger=0 '
                      'expect-private-bos=3 expect-tier-mib=14', result.stdout)

    def test_usage_errors(self):
        cases = (['--unknown'], ['--help', '--unknown'], ['--dry-run', '--require-tier'], ['--tier-report'],
                 ['--dry-run', '--tier-report', 'before', 'during', 'after'], ['--paths'],
                 *(['--paths', s] for s in ('', 'all,swap', ',swap', 'swap,', 'swap,,ubwc', 'bad')),
                 *(['--sizes-mib', s] for s in ('', '1', '257', '2,', ',2', '2,,4', '2MiB', '2, 4', ','.join(['2'] * 33))),
                 *(['--pairs', s] for s in ('0', '17', '1junk', '4294967297', '-1', '18446744073709551616')),
                 *(['--seconds', s] for s in ('-1', 'nan', 'inf', '1e999', '1.5s', ' 1', '0x10')),
                 *(['--linger', s] for s in ('', '0.2oops', '-1', 'nan')))
        for args in cases:
            with self.subTest(args=args):
                result = self.run_cli(args, 2)
                self.assertIn('usage:', result.stderr)
                self.assertEqual(result.stdout, '')

    def test_report_ready_mode_one(self):
        result = self.report([snapshot(), snapshot(126, 6), snapshot(0, 6)])
        self.assertTrue(result.stdout.startswith('tier-report PASS '))
        self.assertEqual(result.stderr, '')

    def test_report_custom_thresholds(self):
        result = self.report([snapshot(), snapshot(2, 1), snapshot(0, 1)],
                             options=['--expect-bos', '1', '--expect-mib', '2', '--slack-mib', '0'])
        self.assertTrue(result.stdout.startswith('tier-report PASS '))

    def test_report_checks_mode_and_state(self):
        for i in range(3):
            for mode, state in ((0, 'off'), (2, 'ready'), (1, 'active')):
                with self.subTest(sample=i, mode=mode, state=state):
                    lines = [snapshot(), snapshot(126, 6), snapshot(0, 6)]
                    lines[i] = lines[i].replace('mode=1 state=ready', f'mode={mode} state={state}')
                    self.assertTrue(self.report(lines, 1).stdout.startswith('tier-report FAIL mode '))

    def test_report_checks_residency_return_and_denial(self):
        result = self.report([snapshot(), snapshot(124, 5, denials=1), snapshot(3, 5)], 1)
        self.assertIn('tier-report FAIL private-req allocs used-mib return denied', result.stdout)

    def test_report_rejects_bad_samples(self):
        for line in (None, '', snapshot() * 2, snapshot().replace('mode=1', 'mode=1 mode=1'),
                     snapshot().replace('allocs=0', 'allocs=-1'), snapshot().encode() + b'\0',
                     snapshot().rstrip('\n').ljust(1024)):
            with self.subTest(line=line):
                result = self.report([snapshot(), snapshot(126, 6), line], 1)
                self.assertTrue(result.stdout.startswith('tier-report FAIL unreadable '))

    def test_report_usage_errors(self):
        for options in (['--expect-bos'], ['--expect-bos', '4294967297'], ['--expect-bos', '0'],
                        ['--expect-mib', '0'], ['--expect-mib', '2MiB'], ['--slack-mib', '-1'], ['--dry-run', 'x']):
            with self.subTest(options=options):
                result = self.report([snapshot()] * 3, 2, options=options)
                self.assertIn('usage:', result.stderr)
                self.assertEqual(result.stdout, '')


if __name__ == '__main__':
    unittest.main()
