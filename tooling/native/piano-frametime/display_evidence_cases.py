# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


def snapshot(sequence=1, time=1_000_000, first=100, second=200, enabled='00000003', hz=120):
    return (f'snapshot {sequence} at {time} us: 18 loads, 0 stores\n'
            f'intf1: en 00000001; counters en {enabled} frame {first} line 31\n'
            f'intf2: en 00000001; counters en {enabled} frame {second} line 32\n'
            f'mode: live {hz} Hz (fixture)\n')


class DisplayEvidenceTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.directory = Path(temp.name)

    def report(self, before, after, log=None, *, status=0):
        args = [BINARY]
        for name, body in [('before', before), ('after', after), ('log', log)]:
            if name == 'log' and body is None:
                continue
            path = self.directory / name
            path.unlink(missing_ok=True)
            if isinstance(body, str):
                path.write_text(body, encoding='utf-8')
            elif body is not None:
                path.write_bytes(body)
            args.append(path)
        result = subprocess.run(args, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, status, result.stderr)
        return result

    def test_format_and_positive_windows(self):
        log = (
            'kernel: PianoDpu: cadence 7: 1000 ms: 120 flips (120.00/s), 3 again, 0 gaps; '
            'iv n 119 p50 8300 p95 8400 p99 8600 max 8900 mean 8333 us; bands 119/0/0/0/0/0; thr 2\n'
            'PianoDpu: cadence 8: 1000 ms: 1 flips (1.00/s), 0 again, 0 gaps; '
            'iv n 0 p50 0 p95 0 p99 0 max 0 us\n'
            'PianoDpu: flips: 120 in the last 1 s (1000 in all: latch mean 20 us\n'
            'PianoDpu: flips: 0 in the last 1 s (18446744073709551615 in all:\n'
            'NativeGopDisplay: period 1000 ms: 119 VBL ticks, 2 times the tick stopped while nothing moved\n'
            'NativeGopDisplay-ext: period 1000 ms: 60 VBL ticks, 0 times the tick stopped while nothing moved\n'
        )
        result = self.report(snapshot(first=2**32 - 56), snapshot(2, 2_000_000, 64, 320), log)
        report = json.loads(result.stdout)
        self.assertEqual(result.stdout.count('\n'), 1)
        self.assertTrue(result.stdout.endswith('\n'))
        self.assertEqual(list(report), sorted(report))
        self.assertEqual(report['schema_version'], 1)
        scanout = report['scanout']
        self.assertEqual(scanout['status'], 'ok')
        self.assertEqual(scanout['source'], 'machdep.piano_dpu_snapshot')
        self.assertEqual(scanout['before'], {'serial': 1, 'time_us': 1_000_000, 'classified_mode_hz': 120,
                                             'intf1_frame': 2**32 - 56, 'intf2_frame': 200, 'status': 'ok'})
        self.assertEqual(scanout['span_s'], 1)
        for link in ('intf1', 'intf2'):
            self.assertEqual(scanout[link], {'frame_delta': 120, 'scanout_hz': 120})
        evidence = report['driver_log']
        self.assertEqual(evidence['dpu_positive_cadence_windows'], 1)
        self.assertEqual(len(evidence['dpu_cadence_windows']), 2)
        self.assertEqual(evidence['dpu_cadence_windows'][0], {
            'sequence': 7, 'window_ms': 1000, 'new_surface_flips': 120, 'same_surface': 3, 'gaps_ge_100ms': 0,
            'new_surface_hz': 120, 'intervals': 119, 'p50_us': 8300, 'p95_us': 8400, 'p99_us': 8600, 'max_us': 8900,
        })
        self.assertEqual(evidence['dpu_new_surface_flip_totals'], [1000, 2**64 - 1])
        self.assertEqual(evidence['gop_software_vbl_windows'], [
            {'window_ms': 1000, 'software_ticks': 119, 'idle_stops': 2},
        ])
        self.assertIn('INTF1 120.000 Hz, INTF2 120.000 Hz', result.stderr)
        self.assertIn('1 positive DPU cadence windows, 1 software VBL windows', result.stderr)

    def test_scanout_statuses(self):
        before = snapshot(hz=60)
        cases = [
            (snapshot(2, 2_000_000, 160, 260, hz=60), 'ok'),
            (snapshot(2, 1_000_000), 'stale_or_reversed_snapshot'),
            (snapshot(2, 999_999), 'stale_or_reversed_snapshot'),
            (snapshot(1, 2_000_000), 'stale_or_reversed_snapshot'),
            (snapshot(0, 2_000_000), 'missing_counter_evidence'),
            (snapshot(2, 2_000_000, 160, 260, enabled='00000001'), 'missing_counter_evidence'),
            (snapshot(2, 2_000_000, 100, 260), 'counter_not_advancing'),
            (snapshot(2, 2_000_000, 99, 200), 'counter_reset_or_implausible_delta'),
        ]
        for after, status in cases:
            with self.subTest(status=status, after=after):
                result = self.report(before, after, status=int(status != 'ok'))
                report = json.loads(result.stdout)
                self.assertEqual(report['scanout']['status'], status)
                self.assertIsNone(report['driver_log'])
                if status == 'ok':
                    self.assertEqual(report['scanout']['intf1']['scanout_hz'], 60)

    def test_malformed_snapshots(self):
        valid = snapshot()
        for original, malformed in [
            ('snapshot 1', 'snapshot -1'),
            ('snapshot 1', 'snapshot 18446744073709551616'),
            ('us:', 'us?'),
            ('intf1:', 'intf1junk:'),
            ('00000003 frame', '100000003 frame'),
            ('frame 100', 'frame 4294967296'),
            ('frame 100', 'frame -1'),
            ('frame 100', 'frame 100junk'),
            ('frame 100', 'frame 100\0junk'),
        ]:
            with self.subTest(malformed=malformed):
                result = self.report(valid.replace(original, malformed, 1), snapshot(2, 2_000_000), status=1)
                before = json.loads(result.stdout)['scanout']['before']
                self.assertEqual(before['status'], 'missing_snapshot_or_enabled_counters')
        result = self.report(valid.replace('120 Hz', '120oops'), snapshot(2, 2_000_000, 160, 260))
        self.assertNotIn('classified_mode_hz', json.loads(result.stdout)['scanout']['before'])

    def test_malformed_log_has_no_positive_evidence(self):
        log = (
            'PianoDpu: cadence 1: 0 ms: 1 flips (1.00/s), 0 again, 0 gaps; iv n 1 p50 1 p95 1 p99 1 max 1 us\n'
            'PianoDpu: cadence -1: 100 ms: 1 flips (1.00/s), 0 again, 0 gaps; iv n 1 p50 1 p95 1 p99 1 max 1 us\n'
            'PianoDpu: cadence 2: 100 ms: 1 flips (1.00/s), 0 again, 0 gaps; iv n 1 p50 1 p95 1 p99 1 max 1\n'
            'PianoDpu: cadence 3: 100 ms: 1 flips (1.00/s), 0 again, 0 gaps; iv n 1 p50 1 p95 1 p99 1 max 1junk us\n'
            'PianoDpu: flips: 1 in the last 1 s (18446744073709551616 in all:\n'
            'PianoDpu: flips: 1 in the last 1 s (25 in all?\n'
            'NativeGopDisplay: period 100 ms: 6 VBL ticks, -1 times the tick stopped\n'
            'NativeGopDisplay: period 100 ms: 6 VBL ticks, 1 bogus\n'
            'unrelated text\n'
        )
        result = self.report(snapshot(), snapshot(2, 2_000_000, 160, 260), log, status=1)
        report = json.loads(result.stdout)
        self.assertEqual(report['scanout']['status'], 'ok')
        self.assertEqual(report['driver_log'], {
            'dpu_cadence_windows': [], 'dpu_positive_cadence_windows': 0,
            'dpu_new_surface_flip_totals': [], 'gop_software_vbl_windows': [],
        })

    def test_software_ticks_do_not_make_a_positive_report(self):
        log = 'NativeGopDisplay: period 1000 ms: 60 VBL ticks, 0 times the tick stopped while nothing moved\n'
        result = self.report(snapshot(), snapshot(2, 2_000_000, 160, 260), log, status=1)
        driver_log = json.loads(result.stdout)['driver_log']
        self.assertEqual(driver_log['dpu_positive_cadence_windows'], 0)
        self.assertEqual(len(driver_log['gop_software_vbl_windows']), 1)

    def test_missing_or_invalid_utf8_capture(self):
        for body in (None, b'\xff'):
            with self.subTest(body=body):
                result = self.report(body, snapshot(2, 2_000_000), status=1)
                self.assertEqual(result.stdout, '')
                self.assertIn('cannot read a kernel capture', result.stderr)


if __name__ == '__main__':
    BINARY = Path(sys.argv.pop(1)).resolve()
    unittest.main()
