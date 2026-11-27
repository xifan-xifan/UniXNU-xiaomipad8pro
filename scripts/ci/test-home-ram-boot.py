#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check RAM boot helper paths and checksums with a simulated fastboot."""
import hashlib
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

BOOT = Path(__file__).resolve().parents[1] / 'home-ram-boot.sh'
DEFAULT = 'boot-piano-uefi-ram-B-own303.img'


class RamBoot(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.ram = self.root / 'ram images'
        self.ram.mkdir()
        self.put_image(self.ram / DEFAULT, b'default image')
        tools = self.root / 'fake tools'
        tools.mkdir()
        self.fastboot = tools / 'fastboot'
        self.fastboot.write_text(
            '#!/bin/sh\ncase "$1" in\n'
            '  --version) echo fixture-fastboot ;;\n'
            '  devices) echo "fixture fastboot" ;;\n'
            '  *) exit 99 ;;\nesac\n'
        )
        adb = tools / 'adb'
        adb.write_text('#!/bin/sh\necho fixture-adb\n')
        hostname = tools / 'hostname'
        hostname.write_text('#!/bin/sh\necho fixture-host\n')
        for executable in (self.fastboot, adb, hostname):
            executable.chmod(0o755)
        self.env = dict(os.environ, FASTBOOT=str(self.fastboot), PIANO_RAM_DIR=str(self.ram),
                        PIANO_CHECK_ALL='1', PATH=str(tools) + os.pathsep + os.environ['PATH'])

    def put_image(self, path, data, marker=' '):
        path.write_bytes(data)
        (path.parent / 'SHA256SUMS').write_text(f'{hashlib.sha256(data).hexdigest()} {marker}{path.name}\n')

    def boot(self, *args):
        return subprocess.run(['bash', str(BOOT), *args], cwd=self.root, env=self.env,
                              capture_output=True, text=True, timeout=10)

    def test_checks_with_fastboot_path_containing_spaces(self):
        result = self.boot('--check')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('fixture-fastboot', result.stdout)
        self.assertIn('fixture-adb', result.stdout)
        self.assertIn('fixture fastboot\n', result.stdout)

    def test_explicit_image_wins_over_same_name_in_ram_directory(self):
        external = self.root / 'alternate'
        external.mkdir()
        image = external / DEFAULT
        self.put_image(image, b'an alternate image')
        result = self.boot('--list', '--image', str(image))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(str(image), result.stdout)

    def test_image_with_spaces_and_binary_checksum_marker(self):
        image = self.ram / 'alternate image.img'
        self.put_image(image, b'custom image', marker='*')
        result = self.boot('--check', '--image', image.name)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('1 通过, 0 失败, 0 缺失', result.stdout)


if __name__ == '__main__':
    unittest.main()
