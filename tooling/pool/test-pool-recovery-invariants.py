#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the pool preflight, copy, and ESP recovery code against temporary files."""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
PROTECTED = ('userdata', 'super', 'metadata', 'boot_b', 'future_b', 'xbl', 'abl', 'tz', 'hyp',
             'vbmeta', 'dtbo', 'persist')
DATA = 'DA7A0000-0000-4000-8000-000000000001'
SYSTEMOS = '07AD53B3-7565-4004-B574-F45FAC3753FB'
OS = '84581E99-85BE-4957-AC53-2947E887B3E4'
BACKUP = 'linux_rootfs-tail-esp'

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('pool_maintenance', HERE.parent / 'persist/maint-session.py')
maint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(maint)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data) + '\n')


def snapshot(folder):
    return {path.relative_to(folder): path.read_bytes() for path in folder.rglob('*') if path.is_file()}


def esp_artifacts(folder):
    return {path.name: path.read_bytes() for path in folder.glob('esp-*') if path.is_file()}


class Fixture(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='pool-recovery-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()

    def patch(self, patcher):
        value = patcher.start()
        self.addCleanup(patcher.stop)
        return value

    def directory(self, name):
        path = self.root / name
        path.mkdir(parents=True)
        return path

    def executable(self, path, script):
        path.write_text('#!/bin/bash\nset -u\n' + script)
        path.chmod(0o755)
        return str(path)


class InstallPreflight(Fixture):
    def setUp(self):
        super().setUp()
        self.home = self.directory('home/.config/piano').parents[1]
        (self.home / '.config/piano/env').touch()
        self.bin = self.directory('bin')
        (self.bin / 'python3').symlink_to(sys.executable)
        self.pkg = self.directory('package')
        self.out = self.directory('out')
        self.main = self.directory('main')
        self.backup = self.directory('backup')
        (self.backup / 'prior-esp.img').write_bytes(b'older rollback set')
        self.inventory = self.directory('inventory')
        self.capture = self.directory('capture') / 'CAPTURE.json'
        self.lock = self.root / 'lock'
        self.calls = self.root / 'calls'
        self.remote = self.root / 'remote'
        self.sequence = 0

        for folder in (self.out, self.main):
            for generation in ('y10', 'y12q'):
                image = f'esp-{generation}.img'
                data = f'ESP with {generation} rollback entry'.encode()
                (folder / image).write_bytes(data)
                receipt = dict(image=image, sha256=sha(data), kind='k273-only', default='macos-27.3')
                write_json(folder / f'esp-{generation}.json', receipt)
            write_json(folder / 'installed.json', receipt)

        (self.inventory / 'RESULT.txt').write_text('INVENTORY DONE 2000-01-01 00:00:00: fixture\n')
        (self.inventory / 'csr.txt').write_text('System Integrity Protection status: disabled.\n')
        (self.inventory / 'info-data.txt').write_text('Container Free Space: 90 GB (90000000000 Bytes)\n')
        (self.inventory / 'apfs-list.txt').write_text(
            f'APFS Container Reference: disk3\nVolume UUID: {DATA}\n')
        for name in ('adt-chosen.txt', 'preboot.txt'):
            (self.inventory / name).touch()

        self.archives = {}
        windows = {}
        for vol, uuid in (('os', OS), ('systemos', SYSTEMOS)):
            archive = self.directory(vol)
            self.archives[vol] = archive
            manifest_body = b'.\td\t755\t0\t0\t0\t-\t-\t-\t-\t-\t-\t-\t-\n'
            manifest = manifest_body + f'# entries 1 files 0 bytes 0 sha256 {sha(manifest_body)} errors 0\n'.encode()
            payload = f'{vol} payload fixture'.encode()
            (archive / 'volume.aar').write_bytes(payload)
            (archive / 'expected.manifest').write_bytes(manifest)
            windows[vol] = {'sha256': sha(vol.encode())}
            (archive / 'archive.env').write_text(
                f"source_sha256={windows[vol]['sha256']}\nsource_volume_uuid={uuid}\nsource_personality=APFS\n"
                f'archive_sha256={sha(payload)}\nmanifest_file_sha256={sha(manifest)}\nfamilies=\n')
        write_json(self.capture, {'windows': windows})

        (self.pkg / 'install-functions.sh').write_text(
            'sha() { shasum -a 256 "$1" | awk \'{print $1}\'; }\n'
            'cache_sha() { sha "$1"; }\nnap() { :; }\ncreate_remote() { mkdir -p "$POOL_REMOTE"; }\n')
        for name in ('pool-host.sh', 'pool-device.sh'):
            shutil.copyfile(HERE / name, self.pkg / name)
        (self.pkg / 'install-gate.sh').write_text('gate_line() { :; }\n')
        (self.pkg / 'pool-manifest').write_text('fixture tool\n')
        files = sorted(self.pkg.iterdir())
        self.sums = self.pkg / 'PACKAGE.sha256'
        self.sums.write_text(''.join(f'{sha(path.read_bytes())}  ./{path.name}\n' for path in files))
        (self.pkg / 'PACKAGE.tsv').touch()
        gate_spec = self.root / 'gate-spec'
        gate_spec.write_text('NEED_ARGS="data-persist=2"\n')
        profile = self.root / 'profile'
        profile.write_text('piano-touch=2\n')

        deny = self.executable(self.bin / 'deny', 'printf "unexpected device call\\n" >> "$CALLS"\nexit 90\n')
        self.env = dict(PATH=f'{self.bin}:/usr/bin:/bin', HOME=str(self.home), LC_ALL='C',
                        PYTHONDONTWRITEBYTECODE='1', CALLS=str(self.calls), POOL_FAKE='1',
                        POOL_PACKAGE=str(self.pkg), POOL_MAIN_LEDGER=str(self.main), POOL_LOCK=str(self.lock),
                        POOL_ARCHIVE_OS=str(self.archives['os']), POOL_ARCHIVE_SYSTEMOS=str(self.archives['systemos']),
                        POOL_REMOTE=str(self.remote), POOL_DATA_UUID=DATA, POOL_BASE_GATE_SPEC=str(gate_spec),
                        POOL_PROFILE_ARGS=str(profile), PIANO_BACKUP_DIR=str(self.backup), PIANO_PERSIST_OUT=str(self.out),
                        POOL_RUNS=str(self.root / 'rounds'))
        for seam in ('RSSH', 'RSUDO', 'GO_ABL', 'MS', 'DISK_BOOT', 'GUI_GATES', 'BATTERY', 'LIVE_BOOT'):
            self.env[f'POOL_{seam}'] = deny

    def install(self, code, diagnostic, *flags, name=None, **env):
        if name is None:
            self.sequence += 1
            name = f'fixture-{self.sequence:02}'
        result = subprocess.run(
            ['/bin/bash', str(HERE / 'pool-install.sh'), name, '--inventory', str(self.inventory),
             '--capture', str(self.capture.parent), *flags],
            cwd=self.root, env=dict(self.env, **env), capture_output=True, text=True, timeout=15)
        log = result.stdout + result.stderr
        self.assertEqual(result.returncode, code, log)
        self.assertIn(diagnostic, log)
        return self.root / 'rounds' / name

    def assert_refused(self, diagnostic):
        before = [snapshot(folder) for folder in (self.out, self.main, self.backup)]
        self.install(1, diagnostic)
        self.assertFalse(self.calls.exists())
        self.assertFalse(self.remote.exists())
        self.assertFalse(self.lock.exists())
        self.assertEqual([snapshot(folder) for folder in (self.out, self.main, self.backup)], before)

    def test_complete_preflight(self):
        self.install(0, 'PREFLIGHT ONLY', '--preflight-only')
        self.assertFalse(self.calls.exists())
        self.assertFalse(self.lock.exists())

    def test_missing_and_truncated_inputs(self):
        archive = self.archives['os']
        for path, diagnostic in (
            (self.inventory / 'RESULT.txt', 'inventory gates: INVENTORY CHECK VOID'),
            (self.capture, 'no capture'),
            (archive / 'volume.aar', 'volume.aar'),
            (archive / 'expected.manifest', 'expected.manifest'),
            (self.sums, 'complete PACKAGE.sha256'),
            (self.out / 'esp-y12q.img', 'its image is not here or not its record'),
        ):
            original = path.read_bytes()
            for fault in ('absent', 'truncated'):
                with self.subTest(file=path.name, fault=fault):
                    if fault == 'absent':
                        path.unlink()
                    else:
                        path.write_bytes(original.splitlines(keepends=True)[0] if path == self.sums else original[:4])
                    self.assert_refused('CAPTURE.json has no os window'
                                        if path == self.capture and fault == 'truncated' else diagnostic)
                    path.write_bytes(original)

    def test_manifest_with_a_recomputed_outer_receipt_is_still_checked(self):
        manifest = self.archives['os'] / 'expected.manifest'
        receipt = manifest.with_name('archive.env')
        data = manifest.read_bytes()
        old = sha(data)
        # A same-length manifest mutation keeps the record count, but invalidates the footer hash.
        modified = data.replace(b'\t755\t', b'\t700\t')
        manifest.write_bytes(modified)
        receipt.write_text(receipt.read_text().replace(old, sha(modified)))
        self.assert_refused('manifest SHA-256 does not match its final summary')

    def test_missing_or_short_sha_receipts(self):
        receipt = self.archives['systemos'] / 'archive.env'
        original = receipt.read_text()
        for key in ('archive_sha256', 'manifest_file_sha256', 'source_sha256'):
            for fault in ('missing', 'short'):
                with self.subTest(key=key, fault=fault):
                    lines = original.splitlines()
                    lines = [line if not line.startswith(key + '=') else
                             (line[:-1] if fault == 'short' else '') for line in lines]
                    receipt.write_text('\n'.join(lines) + '\n')
                    if key == 'source_sha256' and fault == 'short':
                        capture = json.loads(self.capture.read_text())
                        capture['windows']['systemos']['sha256'] = capture['windows']['systemos']['sha256'][:-1]
                        write_json(self.capture, capture)
                        self.assert_refused('no valid SHA-256 for the systemos window')
                    else:
                        self.assert_refused("archive.env one" if key != 'source_sha256' else 'R1a')
                    receipt.write_text(original)

    def test_missing_backup_location_and_hold(self):
        self.backup.rename(self.root / 'disconnected-backups')
        self.assert_refused('writable directory for the ESP rollback backup')
        (self.root / 'disconnected-backups').rename(self.backup)
        for folder in (self.out, self.main):
            with self.subTest(folder=folder.name):
                hold = folder / 'HOLD'
                hold.symlink_to('absent')
                self.install(1, 'install writes are on HOLD')
                self.assertFalse(self.calls.exists())
                self.assertFalse(self.lock.exists())
                hold.unlink()

    def test_inventory_rejects_a_partition_in_place_of_the_data_container(self):
        (self.inventory / 'apfs-list.txt').write_text(
            f'APFS Container Reference: userdata\nVolume UUID: {DATA}\n')
        self.assert_refused('inventory gates: INVENTORY CHECK FAIL')

    def test_hold_set_during_preflight_prevents_staging(self):
        battery = self.executable(self.bin / 'battery-hold', 'touch "$POOL_MAIN_LEDGER/HOLD"\necho 80%\n')
        self.install(1, 'install writes are on HOLD', POOL_BATTERY=battery)
        self.assertFalse(self.calls.exists())
        self.assertFalse(self.remote.exists())
        self.assertFalse(self.lock.exists())

    def test_interrupted_and_bad_readback_copies_preserve_verified_destination(self):
        stage = self.remote / 'pool/os/volume.aar'
        stage.parent.mkdir(parents=True)
        stage.write_bytes(b'last verified staging file')
        before = [snapshot(folder) for folder in (self.out, self.main, self.backup)]
        battery = self.executable(self.bin / 'battery', 'echo 80%\n')
        transport = self.executable(self.bin / 'transport', """
target=$POOL_REMOTE/pool/os/volume.aar
case "$1" in
    "mkdir -p "*) mkdir -p "$POOL_REMOTE/pool/os" "$POOL_REMOTE/pool/systemos";;
    "umask 077; cat > '$target.part'")
        echo upload >> "$CALLS"
        case "$FAULT" in
            cut) printf partial > "$target.part"; cat >/dev/null; exit 1;;
            mismatch) printf corrupt > "$target.part"; cat >/dev/null;;
            *) cat > "$target.part";;
        esac;;
    "set -o pipefail; /usr/bin/shasum -a 256 '$target.part'"*)
        /usr/bin/shasum -a 256 "$target.part" | /usr/bin/awk '{print $1}'
        [ "$FAULT" != status ];;
    "/bin/mv -f '$target.part' '$target'")
        echo promote >> "$CALLS"
        mv "$target.part" "$target";;
    "umask 077; cat > '$POOL_REMOTE/pool/os/expected.manifest.part'")
        cat >/dev/null
        exit 1;;
    *) echo "unexpected transport command" >> "$CALLS"; exit 90;;
esac
""")
        env = dict(POOL_BATTERY=battery, POOL_RSSH=transport)
        for fault, partial in (('cut', b'partial'), ('mismatch', b'corrupt'),
                               ('status', (self.archives['os'] / 'volume.aar').read_bytes())):
            with self.subTest(fault=fault):
                self.calls.write_text('')
                self.install(1, 'refused: the copy of os/volume.aar', FAULT=fault, **env)
                self.assertEqual(self.calls.read_text().splitlines(), ['upload'] * 5)
                self.assertEqual(stage.read_bytes(), b'last verified staging file')
                self.assertEqual(stage.with_suffix('.aar.part').read_bytes(), partial)
                self.assertFalse(self.lock.exists())
        self.calls.write_text('')
        self.install(1, 'refused: the copy of os/expected.manifest', name='fixture-retry', FAULT='none', **env)
        self.assertEqual(self.calls.read_text().splitlines(), ['upload', 'promote'])
        self.assertEqual(stage.read_bytes(), (self.archives['os'] / 'volume.aar').read_bytes())
        self.assertFalse(stage.with_suffix('.aar.part').exists())
        self.install(1, 'round folder is never reused', name='fixture-retry', FAULT='none', **env)
        self.assertEqual([snapshot(folder) for folder in (self.out, self.main, self.backup)], before)


class DeviceRecovery(Fixture):
    def setUp(self):
        super().setUp()
        self.work = self.directory('device')
        self.mock = self.root / 'mock'
        self.fstab = self.root / 'fstab'
        self.fstab.write_text('LABEL=Owner none hfs rw,noauto\n')
        self.calls = self.mock / 'calls'
        self.state = self.work / 'state.env'
        subprocess.run(['/bin/bash', str(HERE / 'mock-tablet.sh'), str(self.mock)], check=True, timeout=10)
        volume = self.work / 'os'
        volume.mkdir()
        (volume / 'volume.aar').write_bytes(b'payload')
        (volume / 'expected.manifest').write_bytes(b'manifest')
        (volume / 'archive.env').write_text(
            f'source_volume_uuid={OS}\nsource_personality=APFS\n'
            f'archive_sha256={sha(b"payload")}\nmanifest_file_sha256={sha(b"manifest")}\n')
        self.executable(self.work / 'pool-manifest', 'echo "usage: pool-manifest fixture"\nexit 2\n')
        fail = self.executable(self.root / 'deny', 'echo unexpected-device-command >> "$MOCKD/calls"\nexit 90\n')
        self.env = dict(PATH='/usr/bin:/bin', HOME=str(self.root), LC_ALL='C', MOCKD=str(self.mock),
                        MOCK_ROOT_UUID=SYSTEMOS, POOL_DATA_UUID=DATA, POOL_FSTAB=str(self.fstab),
                        POOL_RAWDEV_PREFIX=str(self.mock / 'raw-'), POOL_AA=fail)
        for command in ('diskutil', 'mount', 'umount', 'id', 'chflags'):
            self.env['POOL_' + command.upper()] = str(self.mock / 'bin' / command)
        self.env['POOL_SWVERS'] = str(self.mock / 'bin/sw_vers')

    def device(self, code, message, step, *args, **env):
        result = subprocess.run(['/bin/bash', str(HERE / 'pool-device.sh'), step, str(self.work), *args],
                                env=dict(self.env, **env), capture_output=True, text=True, timeout=10)
        log = result.stdout + result.stderr
        self.assertEqual(result.returncode, code, log)
        self.assertIn(message, log)

    def accept_live(self):
        live = b'accepted live data fixture'
        (self.work / 'live.txt').write_bytes(live)
        with self.state.open('a') as state:
            state.write(f'live_sha256={sha(live)}\n')
        self.device(0, 'R1b EQUAL accepted', 'accept-live', sha(live))

    def test_create_rejects_protected_targets_even_with_stale_preflight(self):
        self.device(0, 'PASS', 'preflight', 'os')
        self.accept_live()
        for target in PROTECTED:
            with self.subTest(target=target):
                self.device(1, 'invalid APFS container target', 'create', 'os', POOL_TEST_CONTAINER=target)
        self.assertFalse(self.calls.exists())

    def test_failed_preflight_invalidates_previous_pass(self):
        self.device(0, 'PASS', 'preflight', 'os')
        self.accept_live()
        self.executable(self.work / 'pool-manifest', 'exit 2\n')
        self.device(1, 'pool-manifest does not run here', 'preflight', 'os')
        self.device(1, 'preflight', 'create', 'os')
        self.assertIn('os_preflight=no\n', self.state.read_text())
        self.assertFalse(self.calls.exists())

    def test_interrupted_create_refuses_orphan_and_recovers_with_original_receipt(self):
        old_fstab = self.fstab.read_bytes()
        self.device(0, 'PASS', 'preflight', 'os')
        self.accept_live()
        pre_create = self.state.read_bytes()
        self.device(0, 'created disk', 'create', 'os')
        completed = self.state.read_bytes()
        (self.work / 'state.env.new').write_bytes(completed[:-7])
        self.state.write_bytes(pre_create)
        calls = self.calls.read_bytes()
        for _ in range(2):
            self.device(1, 'volume of unknown origin', 'create', 'os')
            self.assertEqual(self.calls.read_bytes(), calls)
            self.assertEqual(self.fstab.read_bytes(), old_fstab)
        self.device(1, 'both copies verified first', 'fstab')
        self.state.write_bytes(completed)
        self.device(0, 'already created', 'create', 'os')
        self.assertEqual(self.calls.read_bytes(), calls)
        # A bad archive after preflight must fail before even the modeled mount.
        (self.work / 'os/volume.aar').write_bytes(b'PAYLOAD')
        self.device(1, 'volume.aar hashes to', 'extract', 'os')
        self.assertEqual(self.calls.read_bytes(), calls)
        self.device(0, 'deleted the os copy', 'undo')
        calls = self.calls.read_bytes()
        self.device(0, 'no os volume of this tool', 'undo')
        self.assertEqual(self.calls.read_bytes(), calls)
        self.assertEqual(self.fstab.read_bytes(), old_fstab)


class EspRecovery(Fixture):
    def setUp(self):
        super().setUp()
        self.out = self.directory('out')
        self.session = self.directory('session')
        self.backups = self.directory('backups')
        self.previous = self.directory('backups/persistent-boot-20000101-000000')
        (self.previous / f'{BACKUP}.img').write_bytes(b'older y10 backup')
        self.media = self.root / 'esp-region'
        self.b = maint.B
        self.base = b'A' * (3 * self.b)
        self.candidate = b'B' * self.b + b'A' * self.b + b'C' * self.b
        self.records = {}
        for name, data in (('y10', b'old menu'), ('y12q', self.base), ('y12q-pool', self.candidate)):
            image = f'esp-{name}.img'
            record = dict(image=image, sha256=sha(data), ram_window=1, seed={},
                          default='macos-27.3' if name != 'y12q-pool' else 'macos-27.3-pool')
            (self.out / image).write_bytes(data)
            write_json(self.out / f'esp-{name}.json', record)
            self.records[name] = record
        self.ledger = self.out / 'installed.json'
        write_json(self.ledger, self.records['y12q'])
        self.media.write_bytes(self.base)
        self.images = esp_artifacts(self.out)
        self.earlier_backup = snapshot(self.previous)

        self.patch(mock.patch.dict(os.environ, {'PATH': '/usr/bin:/bin', 'HOME': str(self.root),
                                               'PIANO_BACKUP_DIR': str(self.backups)}, clear=True))
        self.patch(mock.patch.object(maint, 'OUT', self.out))
        self.patch(mock.patch.object(maint, 'MiB', self.b))
        self.patch(mock.patch.object(maint.common, 'ESP_BLOCKS', 3))
        self.patch(mock.patch.object(socket, 'socket', side_effect=AssertionError('unexpected socket')))
        self.patch(mock.patch.object(subprocess, 'Popen', side_effect=AssertionError('unexpected subprocess')))
        self.probe = self.patch(mock.patch.object(maint, 'localnet_preflight'))
        self.patch(mock.patch.object(maint.Session, 'sha_nocache', side_effect=self.hash_backup_file))
        self.patch(mock.patch.object(maint.Session, 't', side_effect=self.tablet))
        self.patch(mock.patch.object(maint.Session, 'serve', side_effect=self.serve))
        self.patch(mock.patch.object(maint.Session, 'job', side_effect=self.job))
        self.geometry = f'{maint.common.ESP_FIRST * (self.b // 512)} {len(self.base) // 512}'
        self.mapping = dict(lun0='sda', esp=f'sda37 {self.geometry}', mounted='0',
                            lun0_block_size=str(self.b), lun0_bytes=str(maint.LUN0_BYTES))
        self.transfers = {}
        self.writes = []
        self.readback = None
        self.bad_backup_readback = False
        self.cut_after_first_write = False

    def hash_backup_file(self, path):
        if self.bad_backup_readback and path.name == f'{BACKUP}.img':
            return '0' * 64
        return sha(path.read_bytes())

    def serve(self, files):
        self.transfers.update(files)

    def tablet(self, command, *args, **kwargs):
        if command == 'map':
            return self.mapping
        if command == 'hash':
            dev, offset, count, block = args
            self.assertEqual((offset, block, count), (maint.common.ESP_FIRST if dev == 'sda' else 0, self.b, 3))
            self.assertIn(dev, ('sda37', 'sda'))
            digest = sha(self.media.read_bytes())
            return {'sha256': '0' * 64 if self.writes and dev == self.readback else digest}
        if command == 'fetch':
            name, digest, _ = args
            self.assertEqual(sha(self.transfers[name].read_bytes()), digest)
            return {}
        if command == 'write':
            name, dev, offset, block = args
            self.assertEqual((dev, block), ('sda37', self.b))
            self.writes.append(command)
            with self.media.open('r+b') as media:
                media.seek(offset * block)
                media.write(self.transfers[name].read_bytes())
            if self.cut_after_first_write:
                raise maint.Stop('simulated interruption after the first write')
            return {}
        self.fail(f'unexpected tablet command {command}')

    def job(self, tag, command, name, dev, offset, block, port=None, **kwargs):
        if command == 'write':
            self.assertEqual((tag, dev, offset, block), ('esp-write', 'sda37', 0, self.b))
        else:
            self.assertEqual((tag, command, dev, offset, block),
                             (f'restore-{BACKUP}', 'restore', 'sda', maint.common.ESP_FIRST, self.b))
        self.writes.append(command)
        self.media.write_bytes(self.transfers[name].read_bytes())

    def run_session(self, *args, code=0, diagnostic='done'):
        self.writes.clear()
        self.transfers.clear()
        argv = ['maint-session.py', '--session', str(self.session), '--esp', 'esp-y12q-pool.img', *args]
        with mock.patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()) as output:
            try:
                maint.main()
            except SystemExit as error:
                self.assertEqual(error.code, code, output.getvalue())
            else:
                self.assertEqual(code, 0, output.getvalue())
        self.assertIn(diagnostic, output.getvalue())
        self.assertEqual(esp_artifacts(self.out), self.images)
        self.assertEqual(snapshot(self.previous), self.earlier_backup)

    def backup_set(self):
        state = json.loads((self.session / 'state.json').read_text())
        return self.backups / state['backup_name']

    def assert_before_write(self, *args, diagnostic):
        media, ledger, backups = self.media.read_bytes(), self.ledger.read_bytes(), snapshot(self.backups)
        self.run_session(*args, code=3, diagnostic=diagnostic)
        self.assertEqual(self.writes, [])
        self.assertEqual(self.transfers, {})
        self.assertEqual(self.media.read_bytes(), media)
        self.assertEqual(self.ledger.read_bytes(), ledger)
        self.assertEqual(snapshot(self.backups), backups)

    def test_android_partition_names_cannot_be_used_as_an_esp_or_lun(self):
        for target in (*PROTECTED, 'sdb37', 'sda38'):
            for field, value in (('esp', f'{target} {self.geometry}'), ('lun0', target)):
                with self.subTest(field=field, target=target):
                    previous = self.mapping[field]
                    self.mapping[field] = value
                    for steps in (('esp-delta',), ('esp',), ('rollback', 'esp')):
                        self.assert_before_write(*steps, diagnostic='ESP write target')
                    self.mapping[field] = previous

    def test_esp_hold_blocks_apply_but_allows_verified_rollback(self):
        self.run_session('--new-backup', 'esp-delta')
        (self.out / 'HOLD').touch()
        self.probe.reset_mock()
        self.assert_before_write('esp-delta', diagnostic='install writes are on HOLD')
        self.probe.assert_not_called()
        self.run_session('rollback', 'esp')
        self.assertEqual(self.media.read_bytes(), self.base)
        self.assertEqual(json.loads(self.ledger.read_text())['image'], 'esp-y12q.img')

    def test_corrupt_candidate_is_refused_before_backup_or_write(self):
        image = self.out / 'esp-y12q-pool.img'
        image.write_bytes(b'X' + self.candidate[1:])
        self.images[image.name] = image.read_bytes()
        self.assert_before_write('esp-delta', diagnostic='esp-y12q-pool.img hashes to its record')

    def test_receiptless_partial_backup_is_never_overwritten(self):
        partial = self.previous / f'{BACKUP}.img'
        write_json(self.session / 'state.json', dict(backup_name=self.previous.name))
        for _ in range(2):
            self.assert_before_write('esp-delta', diagnostic='already exists without a verified receipt')
        with mock.patch.object(maint.time, 'strftime', return_value='20000101-000000'):
            self.run_session('--new-backup', 'esp-delta')
        self.assertNotEqual(self.backup_set(), self.previous)
        self.assertEqual((self.backup_set() / partial.name).read_bytes(), self.base)
        self.assertEqual(self.media.read_bytes(), self.candidate)

    def test_bad_initial_backup_readback_leaves_evidence_without_authorizing_write(self):
        ledger = self.ledger.read_bytes()
        self.bad_backup_readback = True
        self.run_session('--new-backup', 'esp-delta', code=3, diagnostic='re-read without cache')
        self.assertEqual(self.writes, [])
        self.assertEqual(self.media.read_bytes(), self.base)
        self.assertEqual(self.ledger.read_bytes(), ledger)
        self.assertFalse((self.backup_set() / 'manifest.json').exists())
        self.bad_backup_readback = False
        self.assert_before_write('esp-delta', diagnostic='already exists without a verified receipt')

    def test_missing_truncated_and_corrupt_backup_inputs_block_rollback_and_reapply(self):
        self.run_session('--new-backup', 'esp-delta')
        folder = self.backup_set()
        self.run_session('rollback', 'esp')
        for name, diagnostic in ((f'{BACKUP}.img', 'still matches the verified size and SHA-256'),
                                 ('manifest.json', 'match the verified receipt'),
                                 ('SHA256SUMS', 'match the verified receipt')):
            path = folder / name
            original = path.read_bytes()
            for fault in ('missing', 'short', 'changed'):
                with self.subTest(file=name, fault=fault):
                    if fault == 'missing':
                        path.unlink()
                    else:
                        path.write_bytes(original[:8] if fault == 'short' else b'!' + original[1:])
                    self.assert_before_write('esp-delta', diagnostic=diagnostic)
                    self.assert_before_write('rollback', 'esp', diagnostic=diagnostic)
                    path.write_bytes(original)
        self.run_session('esp-delta')
        self.assertEqual(self.media.read_bytes(), self.candidate)

    def test_missing_rollback_build_record_cannot_replace_the_installed_pointer(self):
        self.run_session('--new-backup', 'esp-delta')
        record = self.out / 'esp-y12q.json'
        del self.images[record.name]
        record.unlink()
        self.assert_before_write('rollback', 'esp', diagnostic='saved rollback ESP build record is present')

    def test_interrupted_rollback_ledger_publication_can_be_retried(self):
        self.run_session('--new-backup', 'esp-delta')
        ledger = self.ledger.read_bytes()
        before = snapshot(self.backups)
        replace = Path.replace

        def interrupted_replace(path, target):
            if target == self.ledger:
                raise OSError('simulated interruption before ledger rename')
            return replace(path, target)

        with mock.patch.object(Path, 'replace', interrupted_replace):
            self.run_session('rollback', 'esp', code=3, diagnostic='could not publish the verified ESP ledger')
        self.assertEqual(self.ledger.read_bytes(), ledger)
        self.assertEqual(self.media.read_bytes(), self.base)
        self.assertEqual(snapshot(self.backups), before)
        self.assertEqual(list(self.out.glob('.installed.json.*.new')), [])
        self.run_session('rollback', 'esp')
        self.assertEqual(json.loads(self.ledger.read_text())['image'], 'esp-y12q.img')
        self.assertEqual(snapshot(self.backups), before)

    def test_full_esp_write_uses_atomic_ledger_after_readback(self):
        self.run_session('--new-backup', 'esp-delta')
        self.run_session('rollback', 'esp')
        ledger = self.ledger.read_bytes()
        stale = self.ledger.with_suffix('.json.new')
        stale.write_bytes(b'interrupted older publisher')
        self.readback = 'sda'
        self.run_session('esp', code=3, diagnostic='ESP read back')
        self.assertEqual(self.ledger.read_bytes(), ledger)
        self.readback = None
        self.run_session('rollback', 'esp')
        self.run_session('esp')
        self.assertEqual(self.writes, ['write'])
        self.assertEqual(json.loads(self.ledger.read_text())['image'], 'esp-y12q-pool.img')
        self.assertEqual(stale.read_bytes(), b'interrupted older publisher')

    def test_apply_readback_failure_preserves_pointer_and_verified_backups(self):
        for name in ('sda37', 'sda'):
            with self.subTest(device=name):
                ledger = self.ledger.read_bytes()
                self.readback = name
                self.run_session('--new-backup', 'esp-delta', code=3, diagnostic='ESP read back')
                self.assertEqual(self.writes, ['write', 'write'])
                self.assertEqual(self.ledger.read_bytes(), ledger)
                folder = self.backup_set()
                before = snapshot(folder)
                self.assertEqual((folder / f'{BACKUP}.img').read_bytes(), self.base)
                self.readback = None
                self.run_session('rollback', 'esp')
                self.assertEqual(snapshot(folder), before)
                self.assertEqual(self.media.read_bytes(), self.base)

    def test_interrupted_apply_then_failed_rollback_readback_preserve_recovery(self):
        ledger = self.ledger.read_bytes()
        self.cut_after_first_write = True
        self.run_session('--new-backup', 'esp-delta', code=3, diagnostic='simulated interruption')
        self.assertEqual(self.writes, ['write'])
        self.assertEqual(self.media.read_bytes(), self.candidate[:self.b] + self.base[self.b:])
        self.assertEqual(self.ledger.read_bytes(), ledger)
        for _ in range(2):
            self.assert_before_write('esp-delta', diagnostic='the tablet\'s ESP = the installed')
        self.cut_after_first_write = False
        self.run_session('rollback', 'esp')
        self.run_session('esp-delta')
        ledger = self.ledger.read_bytes()
        before = snapshot(self.backups)
        self.readback = 'sda'
        self.run_session('rollback', 'esp', code=3, diagnostic='restored: read back')
        self.assertEqual(self.writes, ['restore'])
        self.assertEqual(self.ledger.read_bytes(), ledger)
        self.assertEqual(snapshot(self.backups), before)
        self.readback = None
        for _ in range(2):
            self.run_session('rollback', 'esp')
            self.assertEqual(self.media.read_bytes(), self.base)
            self.assertEqual(json.loads(self.ledger.read_text())['image'], 'esp-y12q.img')
            self.assertEqual(snapshot(self.backups), before)


if __name__ == '__main__':
    unittest.main()
