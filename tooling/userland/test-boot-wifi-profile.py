#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Seed-only private fixture safety with no image mount or device."""
import importlib.util
import os
from pathlib import Path
import plistlib
import shlex
import stat
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('stage', Path(__file__).with_name('stage.py'))
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)

STAGING = '/private/var/piano/userland'
BOOT = f'sh {STAGING}/install.sh --boot'
PROFILE = b"ssid=unit ' \" \\ $HOME `true` $(true)\nsecurity=open\n"


class BootWifiProfileTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.path = self.root / 'private-profile'
        self.path.write_bytes(PROFILE)
        self.path.chmod(0o600)

    def boot_profile(self, tree=None, path=None, boot=BOOT):
        with patch.dict(os.environ, PIANO_BOOT_WIFI_PROFILE=str(path or self.path)):
            return stage.boot_wifi_profile({} if tree is None else tree, boot)

    def test_absent_is_noop(self):
        tree = stage.staging_tree({}, [])
        previous = tree.copy()
        with patch.dict(os.environ, PIANO_BOOT_WIFI_PROFILE=''):
            self.assertEqual(stage.boot_wifi_profile(tree, BOOT), BOOT)
        self.assertEqual(tree, previous)

    def test_profile_stays_in_file_and_out_of_manifest(self):
        tree = stage.staging_tree({}, [])
        command = self.boot_profile(tree)
        self.assertEqual(tree['boot-wifi-profile'], (PROFILE, 0o600))
        self.assertNotIn('boot-wifi-profile', tree['manifest.tsv'][0].decode())
        self.assertNotIn('boot-wifi-profile', tree['SHA256SUMS'][0].decode())
        self.assertNotIn(str(self.path), command)
        self.assertNotIn('ssid=', command)
        self.assertNotIn('$(true)', command)
        self.assertIn('-o root -g wheel -m 600', command)
        self.assertLess(command.index('-m 600'), command.index(BOOT))
        staging = '/private/var/install.sh-fixture'
        command = self.boot_profile(boot=f'sh {staging}/install.sh --boot')
        self.assertIn(f'{staging}/boot-wifi-profile', command)

    def test_rejects_public_symlink_and_nonregular_sources_without_paths(self):
        link = self.root / 'link'
        link.symlink_to(self.path)
        fifo = self.root / 'pipe'
        os.mkfifo(fifo)
        self.path.chmod(0o644)
        for path in (self.path, link, self.root, self.root / 'missing', fifo):
            with self.subTest(path=path.name):
                with self.assertRaises(SystemExit) as exc:
                    self.boot_profile(path=path)
                self.assertNotIn(str(path), str(exc.exception))

    def test_size_and_line_boundaries(self):
        accepted = b'x' * 4095 + b'\n'
        self.path.write_bytes(accepted)
        tree = {}
        self.boot_profile(tree)
        self.assertEqual(tree['boot-wifi-profile'], (accepted, 0o600))
        for data in (b'', b'x' * 4096 + b'\n', b'no newline'):
            self.path.write_bytes(data)
            with self.subTest(size=len(data)), self.assertRaises(SystemExit):
                self.boot_profile({})

    def test_staging_replaces_symlink_without_following_it(self):
        staging = self.root / 'stage'
        staging.mkdir()
        outside = self.root / 'outside'
        outside.write_bytes(b'untouched')
        outside.chmod(0o644)
        staged = staging / 'boot-wifi-profile'
        staged.symlink_to(outside)
        tree = {}
        self.boot_profile(tree)
        stage.sync_tree(staging, tree)
        self.assertFalse(staged.is_symlink())
        self.assertEqual(staged.read_bytes(), PROFILE)
        self.assertEqual(stat.S_IMODE(staged.stat().st_mode), 0o600)
        self.assertEqual(outside.read_bytes(), b'untouched')
        self.assertEqual(stat.S_IMODE(outside.stat().st_mode), 0o644)

    def test_boot_copies_literal_bytes_and_keeps_staged_file_on_failure(self):
        tree = {}
        staged = self.root / 'stage area'
        target = self.root / 'wifi db'
        command = self.boot_profile(tree).replace(STAGING, shlex.quote(str(staged))).replace(
            '/private/var/db/piano-wifi', shlex.quote(str(target)))
        stage.sync_tree(staged, tree)
        (staged / 'install.sh').write_text("printf '%s\\n' 'installer ran'\n")
        # Keep the real macOS copy/mode handling and stub the privileged ownership.
        install = self.root / 'install'
        install.write_text("""#!/bin/sh
operation=$1
[ "$operation" != -S ] || [ "$FAIL_INSTALL" != 1 ] || exit 1
[ "$2 $3 $4 $5" = '-o root -g wheel' ] || exit 2
shift 5
exec /usr/bin/install "$operation" "$@"
""")
        install.chmod(0o755)
        for fail in ('1', '0'):
            with self.subTest(fail=fail):
                result = subprocess.run(['/bin/sh', '-c', command], timeout=5, check=True, capture_output=True,
                                        env={**os.environ, 'PATH': f'{self.root}:{os.environ["PATH"]}', 'FAIL_INSTALL': fail})
                self.assertEqual(result.stdout, b'installer ran\n')
                self.assertEqual(result.stderr, b'')
                self.assertEqual((staged / 'boot-wifi-profile').exists(), fail == '1')
                self.assertEqual((target / 'profile').exists(), fail == '0')
        self.assertEqual((target / 'profile').read_bytes(), PROFILE)
        self.assertEqual(stat.S_IMODE((target / 'profile').stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o700)

    def test_boot_job_requires_marker_and_adds_command_once(self):
        job = self.root / 'boot.plist'
        marker = 'sh /private/etc/settime.sh'
        command = self.boot_profile()
        original = plistlib.dumps({'ProgramArguments': ['/bin/sh', '-c', 'echo ready']})
        job.write_bytes(original)
        with self.assertRaisesRegex(SystemExit, 'not the expected boot job'):
            stage.add_boot_command(job, command, marker)
        self.assertEqual(job.read_bytes(), original)
        job.write_bytes(plistlib.dumps({'ProgramArguments': ['/bin/sh', '-c', f'{marker}; echo ready']}))
        script, changed = stage.add_boot_command(job, command, marker)
        self.assertTrue(changed)
        self.assertEqual(script, f'{marker}; {command}; echo ready')
        self.assertEqual(stage.add_boot_command(job, command, marker), (script, False))


if __name__ == '__main__':
    unittest.main()
