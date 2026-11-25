# SPDX-License-Identifier: BSD-2-Clause-Patent
from array import array
import importlib.util
import math
from pathlib import Path
import struct
import tempfile
import unittest
import wave

spec = importlib.util.spec_from_file_location('route', Path(__file__).parents[1] / 'piano-mic-route.py')
route = importlib.util.module_from_spec(spec)
spec.loader.exec_module(route)


class RouteFixtureTests(unittest.TestCase):
    def test_acoustic_positive_and_negatives(self):
        with tempfile.TemporaryDirectory() as scratch:
            fixture = Path(scratch) / 'fixture.wav'
            wav = Path(scratch) / 'recorded.wav'
            route.fixture(fixture)
            pcm = array('h')
            with wave.open(str(fixture), 'rb') as f:
                pcm.frombytes(f.readframes(f.getnframes()))
            onset = route.RATE * 2
            noise = [round(5 * math.sin(math.tau * 771 * i / route.RATE)) for i in range(route.RATE * 6)]
            for mode in ('silence', 'attenuated', 'gap'):
                samples = noise[:]
                if mode != 'silence':
                    for i, value in enumerate(pcm):
                        samples[onset+i] += round(value * 0.04)
                if mode == 'gap':
                    gap = route.RATE * 24 // 1000
                    pos = onset + route.RATE // 2
                    samples[pos:pos+gap] = [0] * gap
                route.write_wav(wav, samples)
                result = route.score(wav, 2.07, 997, .20)
                self.assertEqual(result['passed'], mode == 'attenuated', (mode, result))
                if mode == 'gap':
                    self.assertGreaterEqual(result['longest_zero_frames'], gap)

    def test_reader_rejects_stale_wav_epoch_and_size(self):
        with tempfile.TemporaryDirectory() as scratch:
            wav = Path(scratch) / 'capture.wav'
            log = wav.with_suffix('.log')
            wav.with_suffix('.ready').write_text('READY seed=11 frames=96000 root=1 direct-driver=1\n')
            wav.with_suffix('.done').write_text('CAPTURE_DONE frames=288000 seed=11\n')
            log.write_text('CAPTURE PASS seed=11 frames=288000 blocks=600 anchors=100 '
                           'stop_rc=0x0 unmap_ring=0x0 unmap_status=0x0\n'
                           'CONNECTION_CLOSED rc=0x0 result=0\n')
            with self.assertRaisesRegex(ValueError, 'seed'):
                route.reader_frames(wav, log, 12)
            frames = route.reader_frames(wav, log, 11)
            route.write_wav(wav, [0] * (frames - 480))
            with self.assertRaisesRegex(ValueError, 'frame count'):
                route.score(wav, 2.0, 997, .20, frames)

    def test_extract_checks_epoch_and_dec0_and_legacy_mix(self):
        with tempfile.TemporaryDirectory() as scratch:
            window = Path(scratch) / 'raw.pmr'
            prefix = Path(scratch) / 'capture'
            for selection, mono in ((4, (32767, -32768)), (-1, (16384, -16383))):
                period = route.PERIOD.pack(110, 2, 0, 0)
                pcm = struct.pack('<4h', 32767, 1, -32768, 2)
                window.write_bytes(route.HEADER.pack(b'PMR1', 7, 2, selection, 10, 130, 2, 1) + period + pcm)
                with self.assertRaisesRegex(ValueError, 'generation/run'):
                    route.extract(window, prefix, (8, 2, selection))
                with self.assertRaisesRegex(ValueError, 'generation/run'):
                    route.extract(window, prefix, (7, 3, selection))
                result = route.extract(window, prefix, (7, 2, selection))
                self.assertEqual(result['frames'], 2)
                with wave.open(f'{prefix}-selected.wav', 'rb') as f:
                    self.assertEqual(struct.unpack('<2h', f.readframes(2)), mono)
                window.write_bytes(window.read_bytes()[:-2])
                with self.assertRaisesRegex(ValueError, 'size'):
                    route.extract(window, prefix, (7, 2, selection))


if __name__ == '__main__':
    unittest.main()
