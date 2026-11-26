#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host tests for the self-test runner's duration summary."""
import importlib.util
import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('self_tests', Path(__file__).with_name('self-tests.py'))
self_tests = importlib.util.module_from_spec(spec)
spec.loader.exec_module(self_tests)


class SlowestTests(unittest.TestCase):
    def test_reports_ten_slowest_at_end_even_on_failure(self):
        paths = [f'scripts/ci/test-{i:02d}.py' for i in range(12)]
        tests = [('py', path, None) for path in reversed(paths)]
        durations = dict(zip(paths, [2.5, 4.5, 4.5, 6.5, 8.5, 10.5, 12.5, 14.5, 16.5, 18.5, 20.5, 22.5]))

        def run_one(_kind, path, _line):
            ok = path != paths[-1]
            return path, ok, durations[path], '' if ok else 'test failed'

        output = io.StringIO()
        with (
            patch.object(sys, 'argv', ['self-tests.py', '--jobs', '2']),
            patch.object(self_tests, 'collect', return_value=(tests, [])),
            patch.object(self_tests, 'load_skips', return_value={}),
            patch.object(self_tests, 'run_one', side_effect=run_one),
            redirect_stdout(output),
        ):
            self.assertEqual(self_tests.main(), 1)

        summary = output.getvalue().splitlines()[-11:]
        self.assertEqual(summary[0], 'Slowest 10 tests:')
        # The two tied tests compete for the tenth spot, so the earlier path wins.
        expected_paths = [*reversed(paths[3:]), paths[1]]
        self.assertEqual([line.split() for line in summary[1:]], [
            [f'{durations[path]:.1f}s', path] for path in expected_paths
        ])


if __name__ == '__main__':
    unittest.main()
