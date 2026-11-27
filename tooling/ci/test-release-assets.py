#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host tests for release packaging using synthetic binaries and recipes."""

import hashlib
import io
import os
import struct
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().with_name('release-assets.sh')
KERNEL = 'kernel-k27-native.macho'
BINARIES = (KERNEL, 'kernel-k273-native.macho', 'piano-kexts.tar.gz')
SKILL = 'skills/install-piano-macos/SKILL.md'
RECIPES = (
    'unixnu.lock', 'feeds.toml', 'board.toml', 'patches/example.patch', 'configs/example.toml',
    'tooling/ci/example.sh', 'tooling/k27/fetch-k27-tree.sh', 'tooling/k27/build-native-image.sh',
    'tooling/k27/kext/example.json', 'tooling/k273/example.sh', 'tooling/kc/example.py',
    'scripts/fetch-pins.py', 'docs/ci.md',
)


def macho(filetype):
    return struct.pack('<IiiIIIII', 0xfeedfacf, 0x0100000c, 0, filetype, 0, 0, 0, 0)


def checksums(directory, names):
    return ''.join(f'{hashlib.sha256((directory / name).read_bytes()).hexdigest()}  {name}\n' for name in sorted(names))


class ReleaseAssets(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='release-test-')
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.assets = self.root / 'release assets'
        self.fd = self.root / 'fd hash'
        self.assets.mkdir()
        self.fd.mkdir()
        for name in (*RECIPES, SKILL):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture\n')
        self.git('init', '-q')
        self.git('add', '--', *RECIPES, SKILL)
        self.commit_recipes()
        for name in BINARIES[:2]:
            (self.assets / name).write_bytes(macho(2))   # MH_EXECUTE
        with tarfile.open(self.assets / BINARIES[2], 'w:gz') as archive:
            data = b'synthetic kext object'
            entry = tarfile.TarInfo('native-kexts/Example/example.o')
            entry.size = len(data)
            archive.addfile(entry, io.BytesIO(data))
        (self.assets / 'BUILD-INFO.txt').write_text('fixture build\n')
        (self.fd / 'fd.sha256').write_text(f'{hashlib.sha256(b"fixture FD").hexdigest()}  piano.fd\n')
        self.write_build_sums()

    def write_build_sums(self, names=BINARIES):
        self.build_sums = checksums(self.assets, names)
        (self.assets / 'SHA256SUMS').write_text(self.build_sums)

    def git(self, *args):
        subprocess.run(['git', *args], cwd=self.root, check=True, capture_output=True)

    def commit_recipes(self):
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.test',
                 'commit', '-qm', 'Recipe fixture')

    def remove_skill(self):
        self.git('rm', '-q', '--', SKILL)
        self.commit_recipes()

    def release(self, allow_no_skill=False):
        env = dict(os.environ, TAG='v-test', ALLOW_NO_SKILL='1' if allow_no_skill else '0')
        return subprocess.run(['bash', str(SCRIPT), self.assets.name, self.fd.name], cwd=self.root, env=env,
                              capture_output=True, text=True, timeout=15)

    def assert_refused(self, message):
        result = self.release()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(message, result.stdout + result.stderr)
        self.assertEqual((self.assets / 'SHA256SUMS').read_text(), self.build_sums)
        self.assertFalse((self.assets / 'BUILD-SHA256SUMS').exists())
        self.assertFalse((self.assets / 'piano-recipes.tar.gz').exists())

    def assert_packaged(self, optional):
        names = {*BINARIES, 'BUILD-INFO.txt', 'BUILD-SHA256SUMS', 'piano-recipes.tar.gz', 'RELEASE-NOTES.md', *optional}
        self.assertEqual({p.name for p in self.assets.iterdir()}, names | {'SHA256SUMS'})
        self.assertEqual((self.assets / 'BUILD-SHA256SUMS').read_text(), self.build_sums)
        manifest = (self.assets / 'SHA256SUMS').read_text()
        self.assertEqual(sorted(manifest.splitlines()), sorted(checksums(self.assets, names).splitlines()))
        with tarfile.open(self.assets / 'piano-recipes.tar.gz') as archive:
            self.assertTrue(set(RECIPES) <= set(archive.getnames()))

    def test_creates_release_assets(self):
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_packaged({'install-piano-macos-skill.tar.gz', 'FD-SHA256.txt'})
        self.assertEqual((self.assets / 'FD-SHA256.txt').read_bytes(), (self.fd / 'fd.sha256').read_bytes())
        with tarfile.open(self.assets / 'install-piano-macos-skill.tar.gz') as archive:
            self.assertIn(SKILL, archive.getnames())

    def test_archives_only_committed_recipe_and_skill_content(self):
        untracked = 'tooling/k273/local-build.img'
        recipe = 'tooling/ci/example.sh'
        (self.root / untracked).write_bytes(b'local build fixture')
        (self.root / recipe).write_bytes(b'working copy edit')
        (self.root / SKILL).with_name('local-notes.txt').write_text('local skill fixture')
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        with tarfile.open(self.assets / 'piano-recipes.tar.gz') as archive:
            self.assertNotIn(untracked, archive.getnames())
            self.assertEqual(archive.extractfile(recipe).read(), b'fixture\n')
        with tarfile.open(self.assets / 'install-piano-macos-skill.tar.gz') as archive:
            self.assertNotIn('skills/install-piano-macos/local-notes.txt', archive.getnames())

    def test_rejects_fileset_kernel_collection(self):
        (self.assets / KERNEL).write_bytes(macho(12))   # MH_FILESET
        self.write_build_sums()
        self.assert_refused('not a kernel Mach-O (filetype 12)')

    def test_rejects_truncated_macho(self):
        (self.assets / KERNEL).write_bytes(macho(2)[:16])
        self.write_build_sums()
        self.assert_refused('truncated Mach-O header')

    def test_rejects_incomplete_build_checksums(self):
        self.write_build_sums(BINARIES[:2])
        self.assert_refused('expected one checksum for each kernel and the kext archive')

    def test_rejects_duplicate_and_outside_checksum_paths(self):
        for names in ((*BINARIES, KERNEL), (*BINARIES[:2], f'../{self.assets.name}/{BINARIES[2]}')):
            with self.subTest(names=names):
                self.write_build_sums(names)
                self.assert_refused('expected one checksum for each kernel and the kext archive')

    def test_repeat_release_is_byte_identical(self):
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        before = {p.name: p.read_bytes() for p in self.assets.iterdir()}
        for path in self.assets.iterdir():
            os.utime(path, (1, 1))
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual({p.name: p.read_bytes() for p in self.assets.iterdir()}, before)

    def test_rejects_changed_binary(self):
        (self.assets / KERNEL).write_bytes(macho(2) + b'changed')
        self.assert_refused(f'{KERNEL}: FAILED')

    def test_rejects_unexpected_asset(self):
        (self.assets / 'extra.ipsw').write_bytes(b'fixture')
        self.assert_refused('unexpected release asset: extra.ipsw')

    def test_requires_skill_unless_overridden(self):
        self.remove_skill()
        self.assert_refused('release without the install skill is refused')
        result = self.release(allow_no_skill=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_packaged({'FD-SHA256.txt'})

    def test_retry_removes_stale_optional_assets(self):
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.remove_skill()
        (self.fd / 'fd.sha256').unlink()
        result = self.release(allow_no_skill=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_packaged(set())
        notes = (self.assets / 'RELEASE-NOTES.md').read_text()
        self.assertNotIn('install-piano-macos-skill.tar.gz', notes)
        self.assertNotIn('FD-SHA256.txt', notes)


if __name__ == '__main__':
    unittest.main()
