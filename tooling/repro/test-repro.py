#!/usr/bin/env python3
"""Host tests of tooling/repro (no network, no tablet, no sudo; nothing from Apple needed):

  - pins.json is complete and agrees with the constants the build tools carry (ipsw-members.py's URL and components,
    decompress-kernelcache.py's PIN, make-root-image.sh's OS sha256, maint-session.py's maintenance image, make-systemos.sh's size);
  - the pinned root layout and the reference manifest recompute to the recorded manifest sha256;
  - fetch-ipsw.py: --dry-run opens no socket; --check passes on matching files, fails closed on a changed byte, reports missing files;
    a real run stops before any download when the CDN's size or sha256 differ from the pins;
  - volume-manifest.py: deterministic, content-sensitive, host metadata ignored;
  - verify-root.py on a small synthetic root image: PASS, and FAIL on a changed window byte or a wrong manifest;
  - unit-profile.py on synthetic GPTs: the reference layout is SAME-LAYOUT, a moved partition DIFFERENT;
  - boot-dtb.py's v2 parser; fetch-and-build.sh --dry-run runs nothing.
  - scripts/fetch-pins.py --dry-run plans the locked source checkouts without creating them.

  python3 tooling/repro/test-repro.py
"""
import hashlib
import importlib.util
import io
import json
import os
import re
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zlib
from contextlib import redirect_stdout
from pathlib import Path

D = Path(__file__).resolve().parents[2]
R = D / 'tooling/repro'
PINS = json.loads((R / 'pins.json').read_text())
HEX64 = re.compile(r'[0-9a-f]{64}')


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fetch = load('fetch_ipsw', R / 'fetch-ipsw.py')
vm = load('volume_manifest', R / 'volume-manifest.py')
vr = load('verify_root', R / 'verify-root.py')
up = load('unit_profile', R / 'unit-profile.py')
dtb = load('boot_dtb', R / 'boot-dtb.py')
repo_manifest = load('repo_manifest', D / 'scripts/repo_manifest.py')


class NoNetwork:
    def __enter__(self):
        self.saved = socket.socket, socket.create_connection

        def refuse(*a, **k):
            raise AssertionError('network used')
        socket.socket = refuse
        socket.create_connection = refuse
        return self

    def __exit__(self, *exc):
        socket.socket, socket.create_connection = self.saved


class Pins(unittest.TestCase):
    def test_complete(self):
        ipsw = PINS['ipsw']
        self.assertTrue(ipsw['url'].startswith('https://updates.cdn-apple.com/'))
        for value in (ipsw['server_sha256'], ipsw['build_manifest_sha256'], PINS['kernelcache']['sha256'],
                      PINS['maintenance_image']['sha256'], PINS['boot_dtb']['sha256']):
            self.assertRegex(value, HEX64)
        seen = set()
        for m in PINS['members']:
            self.assertRegex(m['sha256'], HEX64)
            self.assertRegex(m['crc32'], r'[0-9a-f]{8}')
            self.assertGreater(m['size'], 0)
            self.assertEqual(Path(m['path']).name, m['file'])
            self.assertNotIn(m['file'], seen)
            seen.add(m['file'])
            if m['file'].endswith('.aea'):
                self.assertIn('decrypted', m, m['file'])
                self.assertEqual(m['decrypted']['file'], m['file'][:-4])
                self.assertRegex(m['decrypted']['sha256'], HEX64)

    def test_agree_with_the_tools(self):
        members = (D / 'tooling/k273/ipsw-members.py').read_text()
        self.assertIn(PINS['ipsw']['url'].split('/')[-1], members)
        self.assertIn(PINS['ipsw']['url'].split('/')[-2], members)
        default = re.search(r"DEFAULT = \[(.*?)\]", members, re.S).group(1)
        self.assertEqual(sorted(re.findall(r"'([^']+)'", default)), sorted(m['component'] for m in PINS['members']))
        self.assertIn(f"PIN = '{PINS['kernelcache']['sha256']}'", (D / 'tooling/k273/decompress-kernelcache.py').read_text())
        os_sha = PINS['root']['os_window']['sha256']
        self.assertIn(f'[ "$OS_SHA" = {os_sha} ]', (D / 'tooling/k273/make-root-image.sh').read_text())
        self.assertEqual(os_sha, next(m for m in PINS['members'] if m['component'] == 'OS')['decrypted']['sha256'])
        self.assertIn(f"MAINT_IMAGE_SHA256 = '{PINS['maintenance_image']['sha256']}'",
                      (D / 'tooling/persist/maint-session.py').read_text())
        size_mib = int(re.search(r'SIZE_MIB=\$\{5:-(\d+)\}', (D / 'tooling/k273/make-systemos.sh').read_text()).group(1))
        self.assertEqual(size_mib << 20, PINS['root']['systemos']['size'])
        adt = (D / 'configs/k273-adt.toml').read_text()
        self.assertIn(f'"root_match_volume_uuid" = "{PINS["root"]["os_window"]["volume_uuid"]}"', adt)

    def test_root_layout_and_reference_manifest(self):
        r = PINS['root']
        mib = 1 << 20
        self.assertEqual(r['os_window']['offset'], mib)
        self.assertEqual(r['systemos']['offset'], mib + (r['os_window']['size'] + mib - 1) // mib * mib)
        windows = [{'name': 'OS', 'offset': r['os_window']['offset'], 'size': r['os_window']['size'], 'sha256': r['os_window']['sha256']},
                   {'name': 'SystemOS', 'offset': r['systemos']['offset'], 'size': r['systemos']['size'],
                    'sha256': r['systemos']['reference_build']['sha256']}]
        header, total = vr.manifest_bytes(windows)
        self.assertEqual(total, r['image']['size'])
        self.assertEqual(hashlib.sha256(header).hexdigest(), r['image']['reference_build']['manifest_sha256'])
        fm = r['systemos']['file_manifest']
        self.assertRegex(fm['digest'], HEX64)
        self.assertEqual(fm['entries'], sum(fm['counts'].values()))
        self.assertFalse(r['systemos']['reproducible'])

    def test_no_payload_in_the_pins(self):
        # hashes, sizes, names and URLs only: no long base64 or hex blobs beyond sha256 length
        text = (R / 'pins.json').read_text()
        self.assertIsNone(re.search(r'[A-Za-z0-9+/=]{100,}', text))
        self.assertNotIn('base64:', text)


def fake_inputs(tmp):
    """A pins file and member files of a few bytes each (the real pins' layout, fake content)."""
    pins = json.loads(json.dumps(PINS))
    members = tmp / pins['ipsw']['build']
    members.mkdir(parents=True)
    (tmp / 'kc').mkdir()
    for i, m in enumerate(pins['members']):
        data = f'member {i}'.encode()
        if 'decrypted' in m:
            plain = f'plain {i}'.encode()
            (members / m['decrypted']['file']).write_bytes(plain)
            m['decrypted'].update(size=len(plain), sha256=hashlib.sha256(plain).hexdigest())
        else:
            (members / m['file']).write_bytes(data)
        m.update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
    manifest = b'<plist/>'
    (members / 'BuildManifest.plist').write_bytes(manifest)
    pins['ipsw']['build_manifest_sha256'] = hashlib.sha256(manifest).hexdigest()
    kc = b'\xcf\xfa\xed\xfe kc'
    (tmp / pins['kernelcache']['file']).write_bytes(kc)
    pins['kernelcache'].update(size=len(kc), sha256=hashlib.sha256(kc).hexdigest())
    path = tmp / 'pins.json'
    path.write_text(json.dumps(pins))
    return pins, path


class FetchIpsw(unittest.TestCase):
    def test_dry_run_uses_no_network(self):
        with NoNetwork(), tempfile.TemporaryDirectory() as t, redirect_stdout(io.StringIO()) as out:
            self.assertEqual(fetch.main(['--dry-run', '--inputs', str(Path(t) / 'in')]), 0)
        self.assertIn('dry run: nothing was fetched or written', out.getvalue())
        self.assertIn(PINS['ipsw']['url'], out.getvalue())

    def test_check(self):
        with NoNetwork(), tempfile.TemporaryDirectory() as t:
            tmp = Path(t)
            pins, path = fake_inputs(tmp)
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(fetch.main(['--check', '--inputs', str(tmp), '--pins', str(path)]), 0)
            self.assertIn('PASS', out.getvalue())
            victim = tmp / pins['ipsw']['build'] / pins['members'][0]['file']
            victim.write_bytes(victim.read_bytes()[:-1] + b'X')
            with redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as stop:
                fetch.main(['--check', '--inputs', str(tmp), '--pins', str(path)])
            self.assertIn('the pin says', str(stop.exception.code))
            victim.unlink()
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(fetch.main(['--check', '--inputs', str(tmp), '--pins', str(path)]), 1)
            self.assertIn(pins['members'][0]['file'], out.getvalue())

    def test_fetch_stops_on_another_ipsw(self):
        saved_head, saved_tools = fetch.head, fetch.tools
        calls = []
        fetch.head = lambda url: (PINS['ipsw']['size'], '0' * 64)
        fetch.tools = lambda: [('x', '/bin/sh', 'test')]
        saved_run = fetch.run
        fetch.run = lambda cmd: calls.append(cmd)
        try:
            with NoNetwork(), tempfile.TemporaryDirectory() as t, redirect_stdout(io.StringIO()), \
                    self.assertRaises(SystemExit) as stop:
                fetch.main(['--inputs', t])
            self.assertIn('not the pinned IPSW', str(stop.exception.code))
            self.assertEqual(calls, [])
        finally:
            fetch.head, fetch.tools, fetch.run = saved_head, saved_tools, saved_run


class VolumeManifest(unittest.TestCase):
    def tree(self, root):
        (root / 'System/Library').mkdir(parents=True)
        (root / 'System/Library/a.txt').write_text('a')
        (root / 'usr/bin').mkdir(parents=True)
        (root / 'usr/bin/tool').write_text('#!/bin/sh\n')
        os.symlink('private/var', root / 'var')
        (root / '.fseventsd').mkdir()
        (root / '.fseventsd/0001').write_text('host noise')

    def test_deterministic_and_content_sensitive(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.tree(Path(a))
            self.tree(Path(b))
            (Path(b) / '.fseventsd/0002').write_text('more host noise')
            la, ua = vm.walk(a)
            lb, ub = vm.walk(b)
            self.assertEqual(ua, [])
            self.assertEqual(vm.summarize(la)['digest'], vm.summarize(lb)['digest'])
            self.assertFalse(any('.fseventsd' in line for line in la))
            self.assertTrue(any(line.startswith('l ') and 'private/var' in line for line in la))
            (Path(b) / 'System/Library/a.txt').write_text('b')
            lb, _ = vm.walk(b)
            sa, sb = vm.summarize(la), vm.summarize(lb)
            self.assertNotEqual(sa['digest'], sb['digest'])
            self.assertEqual([k for k in sa['groups'] if sa['groups'][k] != sb['groups'].get(k)], ['System/Library/a.txt'])


class VerifyRoot(unittest.TestCase):
    def build(self, tmp):
        os_data, sys_data = os.urandom(8192), os.urandom(4096)
        mib = 1 << 20
        pins = json.loads(json.dumps(PINS))
        r = pins['root']
        r['os_window'].update(offset=mib, size=len(os_data), sha256=hashlib.sha256(os_data).hexdigest())
        r['systemos'].update(offset=2 * mib, size=len(sys_data))
        r['image']['size'] = 3 * mib
        windows = [{'name': 'OS', 'offset': mib, 'size': len(os_data), 'sha256': r['os_window']['sha256']},
                   {'name': 'SystemOS', 'offset': 2 * mib, 'size': len(sys_data), 'sha256': hashlib.sha256(sys_data).hexdigest()}]
        header, total = vr.manifest_bytes(windows)
        image = bytearray(total)
        image[:4096] = header
        image[mib:mib + len(os_data)] = os_data
        image[2 * mib:2 * mib + len(sys_data)] = sys_data
        (tmp / 'manifest').mkdir()
        (tmp / 'manifest/manifest.bin').write_bytes(header)
        (tmp / 'root.img').write_bytes(image)
        sysos_uuid = str(uuid.uuid4()).upper()
        (tmp / 'systemos.img.json').write_text(json.dumps({'volume_uuid': sysos_uuid, 'size': len(sys_data), 'sha256': windows[1]['sha256']}))
        (tmp / 'root.json').write_text(json.dumps({'os_volume_uuid': r['os_window']['volume_uuid'], 'systemos_volume_uuid': sysos_uuid,
                                                   'image_bytes': total, 'image_sha256': hashlib.sha256(image).hexdigest(),
                                                   'manifest_sha256': hashlib.sha256(header).hexdigest(), 'windows': windows}))
        (tmp / 'pins.json').write_text(json.dumps(pins))
        return tmp / 'pins.json'

    def test_pass_and_fail(self):
        with tempfile.TemporaryDirectory() as t:
            tmp = Path(t)
            pins = self.build(tmp)
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(vr.main([str(tmp), '--pins', str(pins)]), 0)
            self.assertIn('PASS', out.getvalue())
            image = bytearray((tmp / 'root.img').read_bytes())
            image[(1 << 20) + 5] ^= 1
            (tmp / 'root.img').write_bytes(image)
            with redirect_stdout(io.StringIO()) as out, self.assertRaises(SystemExit):
                vr.main([str(tmp), '--pins', str(pins)])
            self.assertIn('window OS', out.getvalue())
            image[(1 << 20) + 5] ^= 1
            image[100] ^= 1
            (tmp / 'root.img').write_bytes(image)
            with redirect_stdout(io.StringIO()) as out, self.assertRaises(SystemExit):
                vr.main([str(tmp), '--pins', str(pins), '--quick'])
            self.assertIn('block 0', out.getvalue())

    def test_full_verification_checks_padding_against_recorded_image_hash(self):
        with tempfile.TemporaryDirectory() as t:
            tmp = Path(t)
            pins = self.build(tmp)
            with (tmp / 'root.img').open('r+b') as image:
                image.seek(-1, os.SEEK_END)
                image.write(b'\x01')
            args = [str(tmp), '--pins', str(pins)]
            with redirect_stdout(io.StringIO()):
                self.assertEqual(vr.main([*args, '--quick']), 0)
            with redirect_stdout(io.StringIO()) as out, self.assertRaises(SystemExit):
                vr.main(args)
            self.assertIn('root.json image_sha256', out.getvalue())


def gpt_head(parts, blocks, lba=4096):
    """A protective MBR + GPT header + entries (6 blocks) with PARTS [(number, name, first, last)]."""
    entries = bytearray(128 * 128)
    for number, name, first, last in parts:
        e = bytearray(128)
        e[0:16] = uuid.uuid4().bytes_le
        e[16:32] = uuid.uuid4().bytes_le
        struct.pack_into('<QQ', e, 32, first, last)
        e[56:56 + 2 * len(name)] = name.encode('utf-16-le')
        entries[128 * (number - 1):128 * number] = e
    h = bytearray(92)
    h[0:8] = b'EFI PART'
    struct.pack_into('<IIII', h, 8, 0x10000, 92, 0, 0)
    struct.pack_into('<QQQQ', h, 24, 1, blocks - 1, 6, blocks - 6)
    h[56:72] = uuid.uuid4().bytes_le
    struct.pack_into('<QIII', h, 72, 2, 128, 128, zlib.crc32(entries))
    struct.pack_into('<I', h, 16, zlib.crc32(h))
    data = bytearray(6 * lba)
    data[lba:lba + 92] = h
    data[2 * lba:2 * lba + len(entries)] = entries
    return bytes(data)


class UnitProfile(unittest.TestCase):
    def test_layouts(self):
        c = up.common()
        good = [(c.ROOTFS_INDEX + 1, 'linux_rootfs', c.ROOTFS_FIRST, c.ROOTFS_LAST),
                (c.DATA_INDEX + 1, 'linux_data', c.DATA_FIRST, c.DATA_LAST)]
        moved = [good[0], (c.DATA_INDEX + 1, 'linux_data', c.DATA_FIRST, c.DATA_LAST - 7864320)]   # a 256 GB-like linux_data
        with tempfile.TemporaryDirectory() as t:
            a, b = Path(t) / 'a.bin', Path(t) / 'b.bin'
            a.write_bytes(gpt_head(good, c.LUN0_BLOCKS))
            b.write_bytes(gpt_head(moved, c.LUN0_BLOCKS))
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(up.main(['--lun0', str(a)]), 0)
            self.assertIn('SAME-LAYOUT', out.getvalue())
            self.assertIn('DIFF  LUN 0 primary GPT sha256', out.getvalue())   # another unit's GUIDs: never the reference hash
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(up.main(['--lun0', str(b)]), 2)
            self.assertIn('DIFF  linux_data last LBA', out.getvalue())


class BootDtb(unittest.TestCase):
    def test_parser(self):
        page, blob = 4096, b'\xd0\x0d\xfe\xed' + os.urandom(1000)
        kernel, ramdisk = os.urandom(5000), os.urandom(3000)
        header = bytearray(page)
        fields = [b'ANDROID!', len(kernel), 0, len(ramdisk), 0, 0, 0, 0x100, page, 2, 0, b'', b'', b'', b'', 0, 0, 1660, len(blob), 0]
        dtb.HEADER.pack_into(header, 0, *fields)
        up_ = lambda n: (n + page - 1) // page * page
        image = bytes(header) + kernel.ljust(up_(len(kernel)), b'\0') + ramdisk.ljust(up_(len(ramdisk)), b'\0') + blob
        self.assertEqual(dtb.dtb_of(image), blob)


class Scripts(unittest.TestCase):
    def test_shell_syntax(self):
        for s in ('check-host.sh', 'fetch-and-build.sh'):
            subprocess.run(['bash', '-n', str(R / s)], check=True)

    def test_entry_dry_run_runs_nothing(self):
        with tempfile.TemporaryDirectory() as t:
            inputs = Path(t) / 'in'
            env = dict(os.environ, K273_INPUTS=str(inputs), K273_ROOT_PART='linux_data', PIANO_GPU='2',
                       PIANO_DATA_PERSIST='2', PIANO_RESET_AFTER='900')
            got = subprocess.run(['bash', str(R / 'fetch-and-build.sh'), '--dry-run', 'inputs', 'systemos', 'root', 'kernel', 'fd', 'image'],
                                 capture_output=True, text=True, env=env, timeout=120)
            self.assertFalse(inputs.exists())
        self.assertEqual(got.returncode, 0, got.stderr[-500:])
        self.assertIn('dry run: nothing was fetched, built or written', got.stdout)
        self.assertNotIn('\nrun: ', got.stdout)
        for command in ('make-systemos.sh', 'make-root-image.sh', 'prepare-k27-tree.sh', 'build-fd.sh', 'build-k273-image.sh'):
            self.assertIn(command, got.stdout)
        self.assertIn('K273_ROOT_PART=linux_data PIANO_GPU=2 PIANO_DATA_PERSIST=0 PIANO_RESET_AFTER=900', got.stdout)

    def test_entry_dry_run_keeps_manifest_path_as_one_argument(self):
        with tempfile.TemporaryDirectory() as t:
            inputs = Path(t) / 'input files'
            root = inputs / 'root'
            root.mkdir(parents=True)
            manifest = root / 'systemos.manifest.json'
            manifest.write_text('{}\n')
            env = dict(os.environ, K273_INPUTS=str(inputs), SUDO='sudo -A', REMOVE_AEA='0')
            got = subprocess.run(['bash', str(R / 'fetch-and-build.sh'), '--dry-run', 'inputs', 'systemos', 'root'],
                                 capture_output=True, text=True, env=env, timeout=30, check=True)
            self.assertEqual(list(root.iterdir()), [manifest])
        commands = [shlex.split(line[len('would run: '):]) for line in got.stdout.splitlines()
                    if line.startswith('would run: ')]
        self.assertIn(['python3', '-I', 'tooling/repro/verify-root.py', str(root), '--manifest-json', str(manifest)], commands)
        self.assertTrue(any(cmd[:4] == ['sudo', '-A', 'python3', '-I'] for cmd in commands))
        self.assertNotIn('AEA archives removed after decryption', got.stdout)

    def test_fetch_pins_refuses_conflicting_modes(self):
        for option in ('--verify', '--outdated'):
            got = subprocess.run([sys.executable, str(D / 'scripts/fetch-pins.py'), '--dry-run', option],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(got.returncode, 2, got.stderr)
            self.assertIn('not allowed with argument --dry-run', got.stderr)

    def test_fetch_pins_dry_run_plans_locked_checkouts(self):
        with tempfile.TemporaryDirectory() as t:
            research = Path(t) / 'unixnu-research'
            env = dict(os.environ, UNIXNU_RESEARCH=str(research))
            command = [sys.executable, str(D / 'scripts/fetch-pins.py'), '--dry-run']
            got = subprocess.run(command, cwd=t, env=env, capture_output=True, text=True, timeout=30)
            self.assertFalse(research.exists())
            self.assertEqual(got.returncode, 0, got.stderr)
            unknown = subprocess.run([*command, '--only', 'missing-pin'], cwd=t, env=env,
                                     capture_output=True, text=True, timeout=30)
            self.assertEqual(unknown.returncode, 2, unknown.stderr)
            self.assertIn('unknown pin(s)', unknown.stderr)
        pins = repo_manifest.load_manifest(D / 'unixnu.lock')['pin']
        expected_checkouts = {'UniXNU': 'UniXNU', 'Mu-Silicium': 'Mu-Silicium-piano',
                              'piano-uefi': 'piano-uefi-UniXNU-piano'}
        for name, checkout in expected_checkouts.items():
            self.assertIn(f'would fetch {name} -> {research / checkout} @ {pins[name]["commit"][:10]}', got.stdout)
        self.assertIn(f'would fetch the k27 tree -> {research / "k27-tree"} @ '
                      f'{pins["k27"]["commit"][:10]} + {pins["k27"]["patches"]}', got.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=1)
