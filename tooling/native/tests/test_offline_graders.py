#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host regressions for malformed evidence and standalone image manifests."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

NATIVE = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, NATIVE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(name, *args):
    return subprocess.run([sys.executable, str(NATIVE / (name + '.py')), *map(str, args)],
                          capture_output=True, text=True)


class Graders(unittest.TestCase):
    def test_v6_missing_hash_and_wrong_no_output_count(self):
        v6 = load('video-v6-verdict')
        digest = 'a' * 64
        phases = {'ibp-async': 60, 'ibp-temporal': 60, 'ip-temporal': 60,
                  'ibp-no-output': 60, 'recreate-at-idr': 30}
        manifest = {'fixtures': {clip: {'output': [
            {'pts': i * 3000, 'sha256': digest} for i in range(60)]} for clip in ('ibp', 'ip')}}
        lines = ['video-v6: begin pid=42', 'video-v6: PASS original-one-golden-callback rc=0',
                 'video-v6: PASS cancel-had-pending rc=0', 'video-v6: PASS no-callback-after-invalidate rc=0']
        for phase, count in phases.items():
            nulls = int(phase == 'ibp-no-output')
            lines.append(f'video-v6: summary phase={phase} submitted={count} callbacks={count} '
                         f'images={count-nulls} nulls={nulls} errors=0 early=1 high-water=2')
            for i in range(count):
                image = int(not nulls or i > 0)
                sha = digest if image else '-'
                lines.append(f'video-v6: frame phase={phase} token={i+1} callback=1 status=0 '
                             f'pts={i}/30 image={image} sha256={sha}')
        lines += ['video-v6: PASS qualification failures=0 bounded-stream-correctness-only=1', 'V6_TOOL_EXIT=0']
        console = [
            'V6 bounded AVC streaming ready', 'V3 first-HFI PASS:',
            'V6 resume session 1 BITSTREAM RAW cause DRAIN',
            'V6: drain barrier session 1 ACK LAST pending0 terminal0',
        ]
        for session in range(1, 7):
            sid = f'{session:08x}'
            count = 60 if session <= 4 else 30
            console.append(f'V6 native close session {sid} result 0 ETB {count} EBD {count} PSC 1 decoded {count} '
                           'drain-ack 1 LAST 1 STOP 1/1 CLOSE 1 teardown 0 dma-held 0 watchdog 0 fault-bits 00000000')
            console.append(f'V6 ownership session {sid} input-reuse 20 raw-reuse 17 high-water 4')
            if count == 60:
                for token in range(1, 61):
                    picture = 1 if token <= 2 else 2
                    console.extend([f'V6: accepted session {session} token {token} flags 0 normalized-bytes 9 SHA256 {digest}',
                                    f'V6 native submit session {sid} token {token} bytes 9 SHA256 {digest}',
                                    f'V6 EBD session {sid} index 0 token {token} count {token}',
                                    f'V6 FBD session {sid} token {token} index 0 picture {picture} count {token}'])
        console.extend(f'V6: worker close session {sid} status 0 pending0 cancelled 0 surface-open 1 '
                       'surface-close 1 maps 0 high-water 4 worker-joined-next 1' for sid in range(7))
        console = '\n'.join(console)
        tool = '\n'.join(lines)
        self.assertEqual(v6.evaluate(console, tool, manifest), [])
        self.assertIn('tool completed all checks', v6.evaluate(console, tool + '\nV6_TOOL_EXIT=1', manifest))
        # An exact PTS outside the reference and a missing digest once compared equal.
        bad_hash = tool.replace(f'pts=0/30 image=1 sha256={digest}', 'pts=60/30 image=1', 1)
        self.assertIn('ibp-async per-PTS software hash', v6.evaluate(console, bad_hash, manifest))
        bad_null = tool.replace(f'pts=1/30 image=1 sha256={digest}\nvideo-v6: frame phase=ibp-no-output',
                                'pts=1/30 image=0 sha256=-\nvideo-v6: frame phase=ibp-no-output')
        self.assertIn('ibp-no-output captured no-output count', v6.evaluate(console, bad_null, manifest))

    def test_v6_cli_reports_malformed_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            console, tool, manifest = (Path(tmp) / name for name in ('console', 'tool', 'manifest'))
            console.write_text('V6 native close session invalid\n')
            tool.write_text('video-v6: summary missing-phase\n')
            manifest.write_text(json.dumps({'fixtures': {clip: {'output': []} for clip in ('ip', 'ibp')}}))
            for paths in ((console, tool, manifest), (console, tool, Path(tmp) / 'missing')):
                result = run('video-v6-verdict', *paths)
                self.assertEqual(result.returncode, 1)
                self.assertIn('V6_EVIDENCE_FAIL checks=', result.stdout)
                self.assertNotIn('Traceback', result.stderr)

    def test_v3_missing_and_truncated_capture_fails_with_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for body, status in ((None, 'INCONCLUSIVE'), ('', 'INCONCLUSIVE'),
                                 ('PianoVideo: V3 evidence contract vid-v3b:\n', 'FAIL / INCOMPLETE')):
                if body is not None:
                    (root / 'console.out').write_text(body)
                result = run('video-v3-verdict', root)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn(status, (root / 'V3-VERDICT.md').read_text())

    def test_v3_uses_current_complete_mailbox_and_requires_both_sids(self):
        lines = [
            'PianoVideo: V3 evidence contract vid-v3b:',
            'PianoVideo: V3 power ready 1 rails 1 bus 1 controller 1 codec 1 AXI 1/1',
            'PianoVideo: V3 core after-power',
            'PianoVideo: write smmu+0010',
            'PianoVideo: smmu: verify SCTLR want 00001027 got 00001027 mask 00000027 mismatch 00000000 PASS',
            'PianoVideo: smmu: verify TCR2 want 00000000 got 00000000 mask 0003801f mismatch 00000000 PASS',
            'PianoVideo: smmu: owners before-routes 0->0 ASID 1 result 1',
            *['PianoVideo: smmu: verify S2CR fields mismatch 00000000 PASS'] * 2,
            *[f'PianoVideo: smmu: verify SMR want {sid} got {sid} mask ffffffff mismatch 00000000 PASS'
              for sid in ('80001940', '80001947')],
            *['PianoVideo: smmu: verify release-SMR fields mismatch 00000000 PASS'] * 2,
            'PianoVideo: smmu: release group 0 result 1',
            'PianoVideo: smmu: release group 1 result 1',
            'PianoVideo: smmu: release bank 0 M0 1 details result 1',
            'PianoVideo: smmu: release complete result 1 routes invalid bank M0 TLB complete',
            'PianoVideo: V3 teardown result 0 PAS 0 power 0 smmu 1 dma-held 0 watchdog 0',
            'PianoVideo: V3 outcome first-HFI PASS first-error 0 stage 0 teardown 0 dma-held 0',
            'PianoVideo: V3 first-HFI PASS: INIT success version received cmd-consumed 113 doorbells 3 fsr 00000000 watchdog 0',
        ]
        lines += [f'PianoVideo: V3 lifecycle {i} {phase} result 0' for i, phase in enumerate((
            'memory-prepared', 'power-on', 'S1-apply', 'routes-effective', 'release-begin',
            'device-quiesced', 'domain-release', 'power-off', 'DMA-freed', 'release-end'))]
        terminal = 'PianoVideo: bring-up: V3 ok: synthetic complete'
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'console.out').write_text(terminal + '\n' + lines[0] + '\n')
            mailbox = root / 'mailbox.txt'
            whole = '\n'.join([*lines, terminal, 'MACH Reboot'])
            mailbox.write_text(whole)
            result = run('video-v3-verdict', root)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn('source=mailbox.txt', result.stdout)
            mailbox.write_text(whole.replace('80001947', '80001940'))
            result = run('video-v3-verdict', root)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn('FAIL / INCOMPLETE', result.stdout)

    def test_gpu_incomplete_tables_cannot_pass_optimized_python(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = Path(tmp) / 'capture'
            lines = [
                *[f'R3a ARC {rail} present=1 count=2\nR3a ARC {rail} index=0 vlvl=0\nR3a ARC {rail} index=1 vlvl=100'
                  for rail in ('gfx', 'mx', 'cx')],
                'R3a GX 0 khz=0 gx=0 dep=ffffffff',
                *[f'R3a GX {i} khz={freq} gx=1000101 dep=ffffc1ff'
                  for i, freq in enumerate((160000, 222000, 342000, 389000, 443000, 525000,
                                            607000, 660000, 734000, 832000, 900000), 1)],
                'R3a CX 0 khz=0 vote=0',
                'R3a CX 1 khz=500000 vote=1000101',
                'R3a CX 2 khz=650000 vote=1000101',
                *[f'R3a BCM {i} addr=0 unit=1 width=1 vcd=0' for i in range(4)],
                *[f'R3a DDR {i} kbps=0 wait=4 words=0,0,{(1 << 30) + (4 if i >= 7 else 0):x}'
                  for i in range(12)],
            ]
            banner = 'C5-607-v1 full-table verified M1=49 GX12 CX3 M2=160'
            cmd = [sys.executable, '-O', str(NATIVE / 'check-gpu-qualification.py'), '--tables-only', str(capture)]
            for data, want in (('', 1), ('\n'.join(lines), 1), ('\n'.join([*lines, banner]), 0)):
                capture.write_text(data)
                result = subprocess.run(cmd, capture_output=True, text=True)
                self.assertEqual(result.returncode, want, result.stdout + result.stderr)
                self.assertIn('FAIL C5 evidence:' if want else 'PASS C5 table encoding', result.stdout)
                self.assertNotIn('Traceback', result.stderr)
            result = run('check-gpu-qualification', capture / 'missing')
            self.assertEqual(result.returncode, 1)
            self.assertNotIn('Traceback', result.stderr)

    def test_gpu_heartbeat_requires_unique_samples_and_covers_boundaries(self):
        gpu = load('check-gpu-qualification')
        records = [{'at-us': i * 1_000_000} for i in range(600)]
        self.assertEqual(len(gpu.heartbeat(records, 0, 600_000_000, 'test')), 600)
        for bad in ([records[0]] * 600, records[3:], records[:-3], records[:100] + records[103:]):
            with self.assertRaises(ValueError):
                gpu.heartbeat(bad, 0, 600_000_000, 'test')

    def test_mic_windows_require_all_phases_and_finite_times(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            measure = root / 'measure.py'
            measure.write_text('def read(pcm): return bytes(40 * 48000), 48000, 1\n'
                               'def metrics(*args): return {"tone_dbfs": -60}\n')
            report = root / 'report'
            report.write_text(json.dumps(dict(failures=[], file_onset=8, phases=[dict(carrier_amplitude=1)])))
            timeline = root / 'timeline'
            plays = [dict(event='play', phase=p, before=100 + i * 8, rc=0)
                     for i, p in enumerate(('unity', 'lower', 'mute', 'recover'))]
            silence = [dict(event='silence-start', at=93), dict(event='silence-end', at=99)]
            for rows, wanted in ((plays, 0), (plays[:1], 2),
                                 ([dict(plays[0], before=float('nan')), *plays[1:]], 2)):
                timeline.write_text('\n'.join(json.dumps(row) for row in silence + rows))
                result = run('check-mic-diag', 'windows', measure, root / 'pcm', timeline, report)
                self.assertEqual(result.returncode, wanted, result.stdout + result.stderr)

    def test_boot_timeline_empty_and_mixed_timestamps(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = Path(tmp) / 'capture'
            for data in (None, '', '  \n\t\n', '100.000\t\n', '9' * 400 + '.000\tbad timestamp'):
                if data is not None:
                    capture.write_text(data)
                result = run('boot-timeline', capture)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn('Traceback', result.stderr)
            capture.write_text('console header\n100.000\tKernel boot args: fixture\n')
            result = run('boot-timeline', capture, '--xnu-start', '00:00:00')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('T from', result.stdout)
            result = run('boot-timeline', capture, '--xnu-start', '99:10:00')
            self.assertEqual(result.returncode, 1)
            self.assertNotIn('Traceback', result.stderr)

    def test_halt_timeout_ignores_platform_action_after_cut(self):
        halt = load('halt-trace-verdict')
        lines = [
            'piano-halt: PEHaltRestart type 1 at t=100 ms',
            "piano-halt: phase 'Notifying HaltRestart action handlers'",
            'piano-halt: IOPlatformHaltRestartAction -> first',
            'piano-halt: timed out after 30 s at t=30100 ms',
            'piano-halt: IOPlatformHaltRestartAction -> later',
        ]
        self.assertIn('action of first ', halt.verdict(lines)[1])
        with tempfile.TemporaryDirectory() as tmp:
            capture = Path(tmp) / 'capture'
            capture.touch()
            self.assertEqual(run('halt-trace-verdict', capture).returncode, 1)
            result = run('halt-trace-verdict', capture.with_name('missing'))
            self.assertEqual(result.returncode, 1)
            self.assertNotIn('Traceback', result.stderr)
        self.assertEqual(run('halt-trace-verdict', '--self-test').returncode, 0)


class RootManifest(unittest.TestCase):
    def test_output_failure_keeps_previous_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            image = root / 'image'
            image.write_bytes(bytes(512))
            out = root / 'out'
            out.mkdir()
            (out / 'manifest.bin').mkdir()
            layout = out / 'layout.json'
            layout.write_text('previous layout')
            result = run('make-root-manifest', out, f'OS={image}')
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertEqual(layout.read_text(), 'previous layout')

    def test_v1_writer_matches_wire_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = root / 'out'
            paths = [root / 'first=fixture.img', root / 'second.img']
            for i, path in enumerate(paths):
                path.write_bytes(bytes([i]) * 512)
            result = run('make-root-manifest', out, f'OS={paths[0]}', f'SystemOS={paths[1]}')
            self.assertEqual(result.returncode, 0, result.stderr)
            header = (out / 'manifest.bin').read_bytes()
            self.assertEqual(len(header), 4096)
            self.assertEqual(struct.unpack_from('<8sIIQ', header), (b'SHNGROOT', 1, 2, 3 << 20))
            self.assertEqual(header[4064:], hashlib.sha256(header[:4064]).digest())
            receipt = json.loads((out / 'layout.json').read_text())
            for i, name in enumerate(('OS', 'SystemOS')):
                offset = (i + 1) << 20
                sha = hashlib.sha256(paths[i].read_bytes()).digest()
                entry = struct.unpack_from('<32sQQII32s40s', header, 24 + 128*i)
                self.assertEqual(entry, (name.encode().ljust(32, b'\0'), offset, 512, 512, 1, sha, bytes(40)))
                self.assertEqual(receipt['windows'][i], dict(name=name, image=str(paths[i]), offset=offset,
                                                             size=512, sha256=sha.hex()))

    def test_rejects_layouts_the_v1_reader_cannot_use(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            good, empty, odd = (root / name for name in ('good', 'empty', 'odd'))
            good.write_bytes(bytes(512))
            empty.touch()
            odd.write_bytes(b'\0')
            for specs in ([], [f'={good}'], [f'OS={empty}'], [f'OS={odd}'], [str(good)], [f'OS={root}'],
                          [f'OS={good}', f'OS={good}'], [f'é={good}'], [f'OS\n={good}'],
                          [f'{n}={good}' for n in 'ABCDE']):
                with self.subTest(specs=specs):
                    out = root / 'out'
                    result = run('make-root-manifest', out, *specs)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertNotIn('Traceback', result.stderr)
                    self.assertFalse((out / 'manifest.bin').exists())


if __name__ == '__main__':
    unittest.main()
