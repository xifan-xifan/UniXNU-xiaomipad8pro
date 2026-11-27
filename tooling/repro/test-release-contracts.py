#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Offline release, firmware pin, and userland contracts on Python 3.9 and newer."""

import glob
import gzip
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
USERLAND = ROOT / 'configs/userland.toml'
LOCK = ROOT / 'unixnu.lock'
BOOTARGS_HEADER = 'package/include/PianoBootArgs.h'
LIBEXEC = '/usr/local/libexec/'
USB_READER = LIBEXEC + 'piano-usbnet-address'
USB_HELPER = LIBEXEC + 'piano-bootargs.sh'

sys.dont_write_bytecode = True


def load_script(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


repo_checks = load_script('repo_checks', 'scripts/ci/test-repo-manifests.py')
pin_reader = load_script('fetch_pins', 'scripts/fetch-pins.py')
stage = load_script('userland_stage', 'tooling/userland/stage.py')
boot_args = sys.modules['boot_args']
volume = load_script('volume_manifest', 'tooling/repro/volume-manifest.py')
release = load_script('release_assets_tests', 'tooling/ci/test-release-assets.py')
fetch_ipsw = load_script('fetch_ipsw', 'tooling/repro/fetch-ipsw.py')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def child_env(bin_dir):
    bin_dir.mkdir()
    (bin_dir / 'python3').symlink_to(sys.executable)
    return {
        'PATH': os.pathsep.join((str(bin_dir), os.defpath)),
        'LC_ALL': 'C',
        'GIT_CONFIG_GLOBAL': os.devnull,
        'GIT_CONFIG_NOSYSTEM': '1',
        'GIT_ATTR_NOSYSTEM': '1',
        'GIT_ALLOW_PROTOCOL': '',
    }


def canonical_lock(lock):
    """Render the lock's string fields and inline submodule tables in sorted TOML."""
    lines = []
    for section, entries in sorted(lock.items()):
        for name, fields in sorted(entries.items()):
            lines.append(f'[{section}.{json.dumps(name)}]')
            for field, value in sorted(fields.items()):
                if field == 'submodules':
                    members = ', '.join(f'{json.dumps(path)} = {json.dumps(sha)}' for path, sha in sorted(value.items()))
                    encoded = '{ ' + members + ' }'
                else:
                    encoded = json.dumps(value, ensure_ascii=False)
                lines.append(f'{field} = {encoded}')
            lines.append('')
    return '\n'.join(lines)


class ManifestContracts(unittest.TestCase):
    def relative_path(self, name):
        self.assertTrue(pin_reader.valid_path(name), name)
        self.assertNotRegex(name, r'^(?:~|[A-Za-z]:)')
        self.assertNotIn('private', name.casefold().split('/'), name)
        return Path(name)

    def public_path(self, name):
        path = ROOT / self.relative_path(name)
        resolved = path.resolve()
        self.assertTrue(resolved.is_relative_to(ROOT), name)
        self.assertNotIn('private', resolved.relative_to(ROOT).as_posix().casefold().split('/'), name)
        return path

    def test_userland_sources_destinations_and_metadata(self):
        # Check names before the stager opens any source path.
        contents = USERLAND.read_text(encoding='utf-8')
        # /private/var is the tablet's Data namespace.
        self.assertNotRegex(contents, r'(?i)(?<![\w/.-])(?:\./)?private/')
        doc = stage.tomllib.loads(contents)
        for unit in doc['unit']:
            for entry in unit.get('file', []):
                with self.subTest(unit=unit['name'], dest=entry['dest']):
                    self.assertFalse('source' in entry and 'sources' in entry)
                    for name in [entry['source']] if 'source' in entry else entry.get('sources', []):
                        self.assertTrue(self.public_path(name).is_file(), name)
                    for pattern in entry.get('inputs', []):
                        self.public_path(pattern)
                        matches = [self.public_path(p.relative_to(ROOT).as_posix())
                                   for p in map(Path, glob.glob(str(ROOT / pattern)))]
                        self.assertTrue(any(p.is_file() for p in matches), f'no file matches {pattern}')
                    for arg in entry.get('command', []):
                        if '/' in arg:
                            path = self.public_path(arg)
                            if path.relative_to(ROOT).parts[0] != 'out':
                                self.assertTrue(path.is_file(), arg)
                    if 'output' in entry:
                        self.assertEqual(self.public_path(entry['output']).relative_to(ROOT).parts[0], 'out')
                    if 'staged' in entry:
                        self.public_path(entry['staged'])
                    flags = entry.get('cflags', [])
                    for index, flag in enumerate(flags):
                        if flag.startswith('-I'):
                            include = flags[index + 1] if flag == '-I' else flag[2:]
                            self.assertTrue(self.public_path(include).is_dir(), include)
                    self.assertNotIn('private', entry['dest'].casefold().split('/'))

        plan = stage.load_manifest(USERLAND)
        destinations = set()
        jobs = []
        for unit in plan['units']:
            for entry in unit['files']:
                dest = entry['dest']
                with self.subTest(dest=dest):
                    self.assertNotIn(dest.casefold(), destinations)
                    destinations.add(dest.casefold())
                    self.assertEqual(entry['owner'], 'root:wheel')
                    data_file = dest == USB_HELPER or dest.startswith('/usr/local/share/piano/wallpaper/')
                    job = entry['kind'] in ('agent', 'daemon')
                    self.assertEqual(entry['mode'], '0644' if job or data_file else '0755')
                    if job and not unit['planned']:
                        self.assertEqual(entry['build'], 'copy')
                        jobs.append((entry, self.public_path(entry['sources'][0]).read_bytes()))
        stage.check_jobs(plan, jobs)

    def test_native_kext_inventory(self):
        names = repo_checks.kext_names(ROOT)
        self.assertEqual(len(names), len({name.casefold() for name in names}))
        repo_checks.validate_descriptors(ROOT, names)
        repo_checks.validate_kext_build(ROOT, names)

    def test_merged_bootargs_reader_receipt(self):
        plan = stage.load_manifest(USERLAND)
        entries = {entry['dest']: entry for unit in plan['units'] if not unit['planned'] for entry in unit['files']}
        header = (ROOT / BOOTARGS_HEADER).read_bytes()
        oid = re.search(rb'pianoBootArgsOID\[\][^\n]*= "([^"]+)";', header)
        self.assertIsNotNone(oid, BOOTARGS_HEADER)
        marker = oid[1].replace(rb'\0', b'\0') + b'\0'
        self.assertEqual(marker, boot_args.READER_ABI)

        expected = {LIBEXEC + name for name in boot_args.MERGED_READERS}
        self.assertIn(USB_READER, expected)
        built = []
        for dest in sorted(expected | {USB_HELPER, '/usr/local/bin/piano-camera-grab'}):
            with self.subTest(dest=dest):
                entry = entries[dest]
                if entry['build'] == 'copy':
                    path = self.public_path(entry['sources'][0])
                    data = path.read_bytes()
                elif dest in expected:
                    if entry['build'] == 'cc':
                        source = next(p for p in entry['sources'] if Path(p).stem == entry['name'])
                    else:
                        self.assertEqual(dest, LIBEXEC + 'piano-rotated')
                        source = 'package/PianoAuto/daemon/piano-rotated.m'
                        wrapper = self.public_path(entry['command'][0])
                        self.assertIn(b'/daemon/piano-rotated.m', wrapper.read_bytes())
                    path = self.public_path(source)
                    text = path.read_bytes()
                    self.assertRegex(text, rb'#include "[^"]*PianoBootArgs\.h"')
                    self.assertIn(b'pianoBootArgs(', text)
                    stamp_inputs = {p for pattern in entry['inputs'] for p in glob.glob(str(ROOT / pattern))}
                    self.assertIn(str(ROOT / BOOTARGS_HEADER), stamp_inputs)
                    if entry['build'] == 'script':
                        self.assertIn(str(path), stamp_inputs)
                    data = b'synthetic compiled reader\0' + marker
                else:
                    path = self.public_path(entry['sources'][0])
                    data = b'synthetic legacy reader'
                built.append((dict(entry, path=path, on=True, sha256=sha256(data), cdhash=None), data))
        files = stage.files_record(built)
        self.assertEqual({row['dest'] for row in files if row.get('merged_bootargs') is True}, expected)
        self.assertTrue(all('merged_bootargs' not in row for row in files if row['dest'] not in expected))
        legacy = [(entry, data.replace(marker, b'')) for entry, data in built if entry['dest'] != USB_HELPER]
        self.assertTrue(all('merged_bootargs' not in row for row in stage.files_record(legacy)))

        with tempfile.TemporaryDirectory() as tmp:
            layer = Path(tmp)
            archive = write(layer / 'userland-update.tar', b'synthetic userland archive')
            record = {'archive_sha256': sha256(archive.read_bytes()), 'files': files}
            write(layer / 'live.json', json.dumps(record).encode())
            self.assertEqual(boot_args.staged_readers(None, True, layer), set(boot_args.MERGED_READERS))
            archive.write_bytes(b'changed synthetic archive')
            with self.assertRaisesRegex(SystemExit, 'userland-update.tar differs from live.json'):
                boot_args.staged_readers(None, True, layer)

    def test_source_and_firmware_pins_round_trip(self):
        repo_checks.validate_manifests(ROOT)
        lock = repo_checks.validate_lock(LOCK.read_text(encoding='utf-8'))
        self.assertEqual(pin_reader.load_manifest(LOCK), lock)
        pins = pin_reader.read_pins(LOCK)
        self.assertEqual(pins, lock['pin'])
        for name in ('UniXNU', 'Mu-Silicium', 'Device-Binaries', 'piano-uefi', 'uefi-sheng', 'DarwinBoot'):
            self.assertIn(name, pins)
        self.assertEqual(lock['source']['edk2']['commit'], pins['Mu-Silicium']['submodules']['Mu_Basecore'])
        self.assertEqual(lock['source']['sm8750']['commit'], pins['UniXNU-sm8750-common']['commit'])
        self.assertTrue(self.public_path(pins['k27']['patches']).is_dir())
        for pin in pins.values():
            for name in pin.get('submodules', {}):
                # Submodule paths are relative to the upstream repository.
                self.relative_path(name)

        normalized = canonical_lock(lock)
        reparsed = repo_checks.validate_lock(normalized)
        self.assertEqual(reparsed, lock)
        self.assertEqual(canonical_lock(reparsed), normalized)
        with tempfile.TemporaryDirectory() as tmp:
            path = write(Path(tmp) / LOCK.name, normalized.encode('utf-8'))
            self.assertEqual(pin_reader.load_manifest(path), lock)

    def test_repro_input_pin_paths_and_digests(self):
        path = ROOT / 'tooling/repro/pins.json'
        pins = json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=repo_checks.json_pairs,
                          parse_constant=repo_checks.invalid_json_constant)
        self.assertEqual(fetch_ipsw.load_pins(path), pins)
        ipsw = pins['ipsw']
        self.assertRegex(ipsw['url'], r'\Ahttps://updates\.cdn-apple\.com/[\w./-]+\.ipsw\Z')
        self.assertRegex(pins['maintenance_image']['release'],
                         r'\Ahttps://github\.com/[\w.-]+/[\w.-]+/releases/tag/[\w.-]+\Z')
        for node in (ipsw, pins['kernelcache'], pins['maintenance_image'], *pins['members']):
            for key in ('build', 'file', 'path', 'asset', 'member'):
                if key in node:
                    self.relative_path(node[key])
            if 'decrypted' in node:
                self.relative_path(node['decrypted']['file'])
        digests = (ipsw['server_sha256'], ipsw['build_manifest_sha256'], pins['kernelcache']['sha256'],
                   pins['maintenance_image']['sha256'], *(member['sha256'] for member in pins['members']),
                   *(member['decrypted']['sha256'] for member in pins['members'] if 'decrypted' in member))
        for digest in digests:
            self.assertRegex(digest, r'\A[0-9a-f]{64}\Z')


class FirmwareWrapper(unittest.TestCase):
    def test_reads_darwinboot_pin_without_a_real_checkout(self):
        commit = pin_reader.read_pins(LOCK)['DarwinBoot']['commit']
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            device = root / "device's recipe"
            for name in ('tooling/unixnu.sh', 'scripts/repo_manifest.py', LOCK.name):
                write(device / name, (ROOT / name).read_bytes())
            research = root / 'sources'
            write(research / 'DarwinBoot/.git/HEAD', (commit + '\n').encode())
            write(research / 'DarwinBoot/example.txt', b'synthetic source')
            cli = write(research / 'UniXNU/unixnu', b'#!/bin/sh\nprintf "%s\\n" "$@"\n')
            cli.chmod(0o755)
            env = dict(child_env(root / 'bin'), UNIXNU_RESEARCH=str(research))
            result = subprocess.run(['/bin/bash', str(device / 'tooling/unixnu.sh'), 'build', 'firmware'],
                                    env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, '')
            staged = device / 'out/deps/DarwinBoot'
            self.assertIn(f'DarwinBoot={staged}', result.stdout.splitlines())
            self.assertEqual((staged / 'example.txt').read_bytes(), b'synthetic source')
            self.assertFalse((staged / '.git').exists())


class ChecksumContracts(unittest.TestCase):
    def test_volume_manifest_detects_changed_and_missing_entries(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'tree'
            first = write(root / 'System/file one', b'abcd')
            write(root / 'System/file two', b'efgh')
            pin = root.with_name('pins.json')
            output = root.with_name('walk.json')

            def walk(*options):
                args = [str(root), '--json', str(output), *options]
                with patch.object(sys, 'argv', ['volume-manifest.py', *args]), redirect_stdout(io.StringIO()), \
                        patch.object(volume, 'attach', side_effect=AssertionError('unexpected volume attachment')):
                    volume.main()
                return output.read_bytes()

            before = walk()
            manifest = json.loads(before)
            lines, unreadable = volume.walk(root)
            self.assertEqual(unreadable, [])
            self.assertEqual(manifest['digest'], sha256('\n'.join(lines).encode()))
            self.assertEqual(manifest['file_bytes'], 8)
            write(pin, json.dumps({'systemos': {'file_manifest': manifest}}).encode())
            compare = ('--compare', str(pin))
            os.utime(first, (1, 1))
            self.assertEqual(walk(*compare), before)
            first.write_bytes(b'Abcd')
            with self.assertRaisesRegex(SystemExit, r'^1$'):
                walk(*compare)
            first.unlink()
            with self.assertRaisesRegex(SystemExit, r'^1$'):
                walk(*compare)

    def test_release_assets_are_repeatable_and_refuse_dirty_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            env = dict(child_env(root / 'bin'), TAG='v-fixture', ALLOW_NO_SKILL='0',
                       GIT_AUTHOR_DATE='2000-01-01T00:00:00Z', GIT_COMMITTER_DATE='2000-01-01T00:00:00Z')

            def run(*args, check=True):
                return subprocess.run(args, cwd=root, env=env, capture_output=True, text=True, timeout=15, check=check)

            for name in (*release.RECIPES, release.SKILL):
                write(root / name, b'synthetic recipe\n')
            run('git', 'init', '-q', '--template=')
            run('git', 'add', '--', *release.RECIPES, release.SKILL)
            run('git', '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.test', 'commit', '-qm', 'Recipes')
            assets = root / 'release assets'
            for name in release.BINARIES[:2]:
                write(assets / name, release.macho(2) + b'fixture')
            archive = stage.deterministic_tar({'Example/example.o': (b'synthetic object', 0o644)}, 'native-kexts')
            write(assets / release.BINARIES[2], gzip.compress(archive, mtime=0))
            build_info = write(assets / 'BUILD-INFO.txt', b'synthetic build\n')
            sums = write(assets / 'SHA256SUMS', release.checksums(assets, release.BINARIES).encode())

            def package():
                return run('/bin/bash', str(release.SCRIPT), assets.name, 'no FD', check=False)

            def snapshot():
                return {path.name: path.read_bytes() for path in assets.iterdir()}

            result = package()
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            before = snapshot()
            self.assertEqual(set(before), {
                *release.BINARIES, 'BUILD-INFO.txt', 'BUILD-SHA256SUMS', 'piano-recipes.tar.gz',
                'install-piano-macos-skill.tar.gz', 'RELEASE-NOTES.md', 'SHA256SUMS',
            })
            self.assertCountEqual(sums.read_text().splitlines(),
                                  release.checksums(assets, set(before) - {'SHA256SUMS'}).splitlines())
            for path in assets.iterdir():
                os.utime(path, (1, 1))
            result = package()
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(snapshot(), before)

            def refused(message):
                dirty = snapshot()
                result = package()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stdout + result.stderr)
                self.assertEqual(snapshot(), dirty)

            kernel = assets / release.KERNEL
            kernel.write_bytes(release.macho(2) + b'flxture')
            refused(f'{release.KERNEL}: FAILED')
            kernel.write_bytes(before[release.KERNEL])
            build_info.unlink()
            refused('BUILD-INFO.txt (build.yml')
            build_info.write_bytes(before[build_info.name])
            for name in ('extra.cc', '.gitattributes', 'scratch.tmp', '.release-assets.tmp'):
                with self.subTest(name=name):
                    extra = write(assets / name, b'fixture')
                    refused(f'unexpected release asset: {name}')
                    extra.unlink()


if __name__ == '__main__':
    unittest.main()
