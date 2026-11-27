#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify the PianoPower sources mirrored by other native kexts."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MIRRORS = {
    'PianoWifi': ('FatVolume.c', 'FatVolume.h'),
    'PianoAudio': ('FatVolume.c', 'FatVolume.h'),
    'PianoSensors': ('FatVolume.c', 'FatVolume.h'),
    'PianoVideo': ('FatVolume.c', 'FatVolume.h', 'QcomMdt.c', 'QcomMdt.h'),
}


class MirrorTests(unittest.TestCase):
    def test_mirrors_match_piano_power(self):
        for package, names in MIRRORS.items():
            for name in names:
                mirror = Path('package', package, name)
                owner = Path('package/PianoPower', name)
                with self.subTest(mirror=mirror):
                    # The owner has the SPDX line. Each mirror adds two provenance lines.
                    _, sep, source = (ROOT / owner).read_bytes().partition(b'\n')
                    copy = (ROOT / mirror).read_bytes().split(b'\n', 3)
                    self.assertTrue(sep and source, f'{owner}: missing source after SPDX line')
                    self.assertTrue(len(copy) == 4 and copy[-1], f'{mirror}: missing source after three-line header')
                    self.assertTrue(copy[-1] == source, f'{mirror} differs from {owner} after its header')


if __name__ == '__main__':
    unittest.main()
