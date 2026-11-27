# SPDX-License-Identifier: MIT
"""Host regressions for the offline GPU qualification gate."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CHECKER = Path(__file__).resolve().parents[1] / 'check-gpu-qualification.py'
spec = importlib.util.spec_from_file_location('qualification', CHECKER)
qualification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qualification)


def table_capture():
    frequencies = [160000, 222000, 342000, 389000, 443000, 525000, 607000, 660000, 734000, 832000, 900000]
    corners = [0, 0x32, 0x34, 0x38, 0x3c, 0x40, 0x50, 0x80, 0x90, 0xc0, 0xe0, 0x100]
    lines = []
    for name in ('gfx', 'mx', 'cx'):
        lines.append(f'R3a ARC {name} present=1 count={len(corners)}')
        lines.extend(f'R3a ARC {name} index={i} vlvl={corner:x}' for i, corner in enumerate(corners))
    lines.append('R3a GX 0 khz=0 gx=0 dep=ffffffff')
    for i, (khz, corner) in enumerate(zip(frequencies, corners[1:]), 1):
        vote = (corner << 16) | (i << 8) | i
        lines.append(f'R3a GX {i} khz={khz} gx={vote:x} dep={0xffffc0ff | i << 8:x}')
    for i, (khz, level) in enumerate(((0, 0), (500000, 5), (650000, 7))):
        lines.append(f'R3a CX {i} khz={khz} vote={corners[level] << 16 | level << 8 | level:x}')
    lines.extend(f'R3a BCM {i} addr={i + 1:x} unit=1000 width=16 vcd={i}' for i in range(4))
    for i in range(12):
        value = 0x60000000 if i else 0x40000000
        lines.append(f'R3a DDR {i} kbps={i} wait=7 words={value | i:x},{value | i * 4:x},{value | (4 if i >= 7 else 0):x}')
    lines.append('R3a C5-607-v1 full-table verified M1=49 GX12 CX3 M2=160')
    return '\n'.join(lines) + '\n'


class QualificationTests(unittest.TestCase):
    def grade(self, text, *flags):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / 'console.txt'
            capture.write_text(text)
            return subprocess.run([sys.executable, *flags, str(CHECKER), str(capture), '--tables-only'],
                                  capture_output=True, text=True)

    def test_valid_tables_in_normal_and_optimized_python(self):
        for flags in ((), ('-O',)):
            with self.subTest(flags=flags):
                result = self.grade(table_capture(), *flags)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_verification_marker_fails_under_optimization(self):
        result = self.grade(table_capture().replace('full-table verified', 'marker missing'), '-O')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('FAIL C5 evidence:', result.stdout)

    def test_duplicate_table_rows_and_zero_bcm_unit_fail_cleanly(self):
        capture = table_capture()
        for broken, reason in ((capture + 'R3a GX 0 khz=0 gx=0 dep=ffffffff\n', 'GX'),
                               (capture.replace('unit=1000', 'unit=0', 1), 'BCM')):
            with self.subTest(reason=reason):
                result = self.grade(broken)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('FAIL C5 evidence:', result.stdout)
                self.assertIn(reason, result.stdout)
                self.assertEqual(result.stderr, '')

    def test_heartbeat_requires_order_and_coverage_at_both_edges(self):
        end = 600_000_000
        times = [i * 1_000_000 for i in range(600)]
        def check(stamps):
            return qualification.heartbeat([{'at-us': str(t)} for t in stamps], 0, end, 'test')
        self.assertEqual(len(check(times)), 600)
        for stamps in (times[3:], times[:-3], times[:20] + times[19:],
                       times[:20] + [times[21], times[20]] + times[22:], times[:20] + times[23:]):
            with self.subTest(size=len(stamps)), self.assertRaisesRegex(ValueError, 'test heartbeat'):
                check(stamps)


if __name__ == '__main__':
    unittest.main()
