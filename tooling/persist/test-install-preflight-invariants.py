#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Offline regressions for install preflight, chunk verification and rollback."""

import base64
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zlib
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

# The standalone commands and CI both keep generated files in temporary directories.
sys.dont_write_bytecode = True
import common


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


maint = load('maint-session')
gpt = load('build-gpt')
chunks = load('fetch-chunks')
B = common.LBA
PROTECTED = ('userdata', 'super', 'metadata', 'boot_b', 'vendor_boot_b', 'init_boot_b', 'xbl', 'abl',
             'tz', 'hyp', 'vbmeta', 'dtbo', 'persist')
GPT_REGIONS = ((0, 'lun0-gpt-primary', 'gpt-head.bin'), (maint.TAIL_LBA, 'lun0-gpt-backup', 'gpt-tail.bin'))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def flip(data):
    return bytes([data[0] ^ 1]) + data[1:]


def original_gpt():
    entries = bytearray(4 * B)
    partitions = [(i, name, 10 + i * 10, 19 + i * 10) for i, name in enumerate(PROTECTED)]
    partitions += [(common.ROOTFS_INDEX, 'linux_rootfs', common.ROOTFS_FIRST, common.ROOTFS_LAST),
                   (common.DATA_INDEX, 'linux_data', common.DATA_FIRST, common.DATA_LAST)]
    for index, name, first, last in partitions:
        offset = index * gpt.ENTRY
        struct.pack_into('<16s16sQQQ', entries, offset, uuid.UUID(int=1).bytes_le, uuid.UUID(int=index + 2).bytes_le,
                         first, last, 0)
        encoded = name.encode('utf-16le')
        entries[offset + 56:offset + 56 + len(encoded)] = encoded
    end = common.LUN0_BLOCKS - 1

    def header(current, alternate, table):
        block = bytearray(B)
        struct.pack_into('<8sIIIIQQQQ16sQIII', block, 0, b'EFI PART', 0x10000, 92, 0, 0, current, alternate,
                         6, common.DATA_LAST, uuid.UUID(int=99).bytes_le, table, 64, gpt.ENTRY,
                         zlib.crc32(entries[:64 * gpt.ENTRY]))
        struct.pack_into('<I', block, 16, gpt.header_crc(block, 92))
        return block

    mbr = bytearray(B)
    mbr[510:512] = b'\x55\xaa'
    return bytes(mbr + header(1, end, 2) + entries), bytes(entries + header(end, 1, end - 4))


class OfflineTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.start(mock.patch.object(socket, 'socket', side_effect=AssertionError('unexpected socket')))
        self.start(mock.patch.object(subprocess, 'Popen', side_effect=AssertionError('unexpected subprocess')))

    def start(self, patcher):
        value = patcher.start()
        self.addCleanup(patcher.stop)
        return value


class InstallPreflight(OfflineTest):
    def setUp(self):
        super().setUp()
        self.out = self.root / 'out'
        self.session = self.root / 'session'
        self.nas = self.root / 'backups' / 'test-set'
        for folder in (self.out, self.session, self.nas):
            folder.mkdir(parents=True)
        self.start(mock.patch.dict(os.environ, {'PATH': os.defpath, 'HOME': str(self.root),
                                                'PIANO_BACKUP_DIR': str(self.nas.parent)}, clear=True))
        self.start(mock.patch.object(maint, 'OUT', self.out))
        self.probe = self.start(mock.patch.object(maint.localnet, 'preflight', return_value=('allowed', 'fixture')))
        self.tablet = self.start(mock.patch.object(maint.Session, 't', side_effect=self.tablet_command))
        self.served_files = {}
        self.serve = self.start(mock.patch.object(maint.Session, 'serve', side_effect=self.served_files.update))
        self.job = self.start(mock.patch.object(maint.Session, 'job', side_effect=self.restore_region))
        self.readback = self.start(mock.patch.object(maint.Session, 'region_sha', side_effect=self.hash_region))
        self.start(mock.patch.object(maint.Session, 'shell', return_value='No problems found'))

        self.primary, self.backup = original_gpt()
        self.start(mock.patch.multiple(common, GPT_PRIMARY_SHA256=sha(self.primary), GPT_BACKUP_SHA256=sha(self.backup)))
        head, tail, _, _, guid, *_ = gpt.build_tables(self.primary, self.backup)
        self.record = {'esp_guid': str(guid)}
        self.state = {'backup_name': self.nas.name, 'gpt_state': 'old', 'backups': {}}
        self.media = {}
        self.bad_write = None
        for (lba, key, name), original, update in zip(GPT_REGIONS, (self.primary, self.backup), (head, tail)):
            path = self.root / f'disk-{lba}'
            path.write_bytes(original)
            self.media[lba] = path
            saved = self.nas / f'{key}.img'
            saved.write_bytes(original)
            self.state['backups'][key] = dict(file=saved.name, sha256=sha(original), bytes=len(original), verified=True,
                                              skip=lba, count=len(original) // B, bs=B)
            (self.out / name).write_bytes(update)
            self.record[name] = {'sha256': sha(update)}
        self.write_records()

    def write_records(self):
        (self.out / 'gpt.json').write_text(json.dumps(self.record))
        (self.session / 'state.json').write_text(json.dumps(self.state))

    def run_session(self, *args):
        self.served_files.clear()
        with mock.patch.object(sys, 'argv', ['maint-session.py', '--session', str(self.session), *args]), \
             contextlib.redirect_stdout(io.StringIO()) as output, contextlib.redirect_stderr(io.StringIO()) as errors:
            try:
                maint.main()
            except SystemExit as exc:
                return exc.code, output.getvalue() + errors.getvalue()
        return 0, output.getvalue() + errors.getvalue()

    def tablet_command(self, command, *args, **kwargs):
        if command == 'map':
            updated = common.sha(self.media[0]) == self.record['gpt-head.bin']['sha256']
            last = common.ROOTFS_NEW_LAST if updated else common.ROOTFS_LAST
            esp = f'sda37 {common.ESP_FIRST * 8} {common.ESP_BLOCKS * 8}' if updated else 'none'
            return dict(lun0='sda', lun0_block_size=str(B), lun0_bytes=str(maint.LUN0_BYTES), mounted='0', esp=esp,
                        linux_rootfs=f'sda35 {common.ROOTFS_FIRST * 8} {(last - common.ROOTFS_FIRST + 1) * 8}',
                        linux_data=f'sda36 {common.DATA_FIRST * 8} {(common.DATA_LAST - common.DATA_FIRST + 1) * 8}')
        if command == 'fetch':
            name, digest, _ = args
            self.assertEqual(common.sha(self.served_files[name]), digest)
        elif command == 'write':
            self.write_region(*args)
        elif command != 'rereadpt':
            self.fail(f'unexpected tablet command {command}')

    def restore_region(self, job, command, name, dev, lba, bs, port, *, timeout):
        self.assertEqual(command, 'restore')
        self.write_region(name, dev, lba, bs)

    def write_region(self, name, dev, lba, bs):
        self.assertEqual((dev, bs), ('sda', B))
        data = self.served_files[name].read_bytes()
        self.media[lba].write_bytes(flip(data) if name == self.bad_write else data)

    def hash_region(self, dev, lba, count, bs=B, name='hash'):
        self.assertEqual((dev, bs), ('sda', B))
        self.assertEqual(self.media[lba].stat().st_size, count * bs)
        return common.sha(self.media[lba])

    def assert_stopped(self, result, message):
        code, log = result
        self.assertEqual(code, 3, log)
        self.assertIn(message, log)
        self.assertNotIn('\ndone', log)
        return log

    def assert_before_write(self, result, message):
        log = self.assert_stopped(result, message)
        self.assertNotIn('  WRITE ', log)
        self.assertTrue(all(call.args == ('map',) for call in self.tablet.call_args_list))
        self.serve.assert_not_called()
        self.job.assert_not_called()
        self.assertEqual(self.media[0].read_bytes(), self.primary)
        self.assertEqual(self.media[maint.TAIL_LBA].read_bytes(), self.backup)

    def test_missing_backup_manifest(self):
        self.assert_before_write(self.run_session('--rootfs-backup-set', self.nas.name, 'rollback', 'rootfs-clear'),
                                 'no manifest.json in that set')

    def test_missing_gpt_backup_receipt(self):
        del self.state['backups']['lun0-gpt-primary']
        self.write_records()
        self.assert_before_write(self.run_session('gpt'), 'no verified NAS backup of lun0-gpt-primary')

    def test_backup_bytes_must_match_the_receipt(self):
        (self.nas / 'lun0-gpt-backup.img').write_bytes(flip(self.backup))
        self.assert_before_write(self.run_session('gpt'), 'lun0-gpt-backup.img still matches the verified size and SHA-256')

    def test_missing_or_corrupt_gpt_bins(self):
        for _, _, name in GPT_REGIONS:
            path = self.out / name
            data = path.read_bytes()
            with self.subTest(name=name, problem='missing'):
                path.unlink()
                self.assert_before_write(self.run_session('gpt'), f'{name} matches the permitted offline GPT update')
            with self.subTest(name=name, problem='same-length byte flip'):
                path.write_bytes(flip(data))
                self.assert_before_write(self.run_session('gpt'), f'{name} matches the permitted offline GPT update')
            path.write_bytes(data)

    def test_truncated_or_corrupt_digests(self):
        for _, key, name in GPT_REGIONS:
            for receipt, message in ((self.state['backups'][key], f'no verified NAS backup of {key}'),
                                     (self.record[name], f'{name} matches the permitted offline GPT update')):
                digest = receipt['sha256']
                for wrong in (digest[:-1], '!' + digest[1:], ('0' if digest[0] != '0' else '1') + digest[1:]):
                    with self.subTest(name=name, message=message, digest=wrong):
                        receipt['sha256'] = wrong
                        self.write_records()
                        self.assert_before_write(self.run_session('gpt'), message)
                receipt['sha256'] = digest

    def test_hold_blocks_mixed_write_session_before_contact(self):
        (self.out / 'HOLD').touch()
        self.assert_before_write(self.run_session('map', 'gpt'), 'HOLD')
        self.probe.assert_not_called()
        self.tablet.assert_not_called()

    def test_gpt_install_succeeds_with_valid_inputs_and_no_hold(self):
        code, log = self.run_session('gpt')
        self.assertEqual(code, 0, log)
        self.assertIn('\ndone', log)
        self.assertEqual(json.loads((self.session / 'state.json').read_text())['gpt_state'], 'new')
        for lba, _, name in GPT_REGIONS:
            self.assertEqual(self.media[lba].read_bytes(), (self.out / name).read_bytes())

    def test_android_partitions_are_excluded_from_selectable_write_targets(self):
        image = self.root / 'root.img'
        image.write_bytes(bytes(B))
        for name in (*PROTECTED, 'any_future_partition_b'):
            with self.subTest(partition=name):
                args = SimpleNamespace(root_part=name, root_image=str(image), root_sha256=common.sha(image))
                session = maint.Session.__new__(maint.Session)
                session.a, session.execute, session.state = args, True, {}
                session.save = mock.Mock()
                with self.assertRaisesRegex(maint.Stop, 'the partitions macOS owns'):
                    maint.linux_data_root(session)
                with self.assertRaisesRegex(maint.Stop, 'not one that tablet.sh map reports'):
                    maint.part_geometry(session, name)
        self.serve.assert_not_called()
        self.job.assert_not_called()

    def test_failed_install_and_failed_restore_readbacks_stop_the_session(self):
        self.bad_write = 'gpt-tail.bin'
        log = self.assert_stopped(self.run_session('gpt', 'check'), 'STOPPED: backup GPT read back')
        self.assertIn('rollback: maint-session.py rollback gpt', log)
        self.assertNotIn('-- step check:', log)
        self.assertEqual(self.media[0].read_bytes(), self.primary)
        self.assertNotEqual(self.media[maint.TAIL_LBA].read_bytes(), self.backup)

        # A held install must still allow the operator to restore the verified backups.
        (self.out / 'HOLD').touch()
        state_path = self.session / 'state.json'
        state = json.loads(state_path.read_text())
        self.assertEqual(state['gpt_state'], 'old')
        state['gpt_state'] = 'new'  # A rollback of an earlier install must not record "old" on failure.
        state_path.write_text(json.dumps(state))
        self.bad_write = 'lun0-gpt-backup.img'
        self.job.reset_mock()
        self.assert_stopped(self.run_session('rollback', 'gpt'), 'STOPPED: lun0-gpt-backup restored: read back')
        self.assertEqual([call.args[0] for call in self.job.call_args_list], ['restore-lun0-gpt-backup'])
        self.assertEqual(json.loads(state_path.read_text())['gpt_state'], 'new')

        self.bad_write = None
        self.readback.reset_mock()
        code, log = self.run_session('rollback', 'gpt')
        self.assertEqual(code, 0, log)
        self.assertIn('\ndone', log)
        self.assertEqual(self.readback.call_args_list, [mock.call('sda', maint.TAIL_LBA, 5, B, name='check-lun0-gpt-backup'),
                                                       mock.call('sda', 0, 6, B, name='check-lun0-gpt-primary')])
        self.assertEqual(self.media[0].read_bytes(), self.primary)
        self.assertEqual(self.media[maint.TAIL_LBA].read_bytes(), self.backup)
        self.assertEqual(json.loads(state_path.read_text())['gpt_state'], 'old')


class ChunkVerification(OfflineTest):
    def setUp(self):
        super().setUp()
        self.first = b'A' * chunks.CHUNK_SIZE
        self.last = b'the final short chunk'
        self.payload = self.first + self.last
        self.digest = sha(self.payload)
        self.destination = self.root / 'received'
        self.part = self.root / 'received.part'
        self.receipt = self.root / 'received.part.resume'
        self.remote = self.start(mock.patch.object(chunks, 'remote'))
        self.start(mock.patch.object(chunks.time, 'sleep'))
        for stream in ('stdout', 'stderr'):
            self.start(mock.patch.object(sys, stream, io.StringIO()))

    @staticmethod
    def frame(index, data, digest=None):
        return f'CHUNK {index} {digest or sha(data)}\n'.encode() + base64.b64encode(data) + f'\nEND {index}\n'.encode()

    def fetch(self, *frames, digest=None):
        self.remote.side_effect = [f'FILE {len(self.payload)} {self.digest}\n'.encode(), *frames]
        chunks.fetch('fixture', 'source', self.destination, self.digest if digest is None else digest, 1)

    def test_truncated_flipped_reordered_and_duplicate_chunks_are_retried(self):
        first = self.frame(0, self.first)
        last = self.frame(1, self.last)
        self.fetch(self.frame(0, self.first[:-1]), self.frame(0, flip(self.first), sha(self.first)), last, first, first, last)
        requests = [call.args[2][-1] for call in self.remote.call_args_list if call.args[1] == chunks.READ_CHUNK]
        self.assertEqual(requests, [0, 0, 0, 0, 1, 1])
        self.assertEqual(self.destination.read_bytes(), self.payload)
        self.assertEqual(common.sha(self.destination), self.digest)
        self.assertFalse(self.part.exists())
        self.assertFalse(self.receipt.exists())

    def test_retry_exhaustion_preserves_destination_and_verified_prefix(self):
        self.destination.write_bytes(b'existing backup')
        first = self.frame(0, self.first)
        bad = self.frame(1, flip(self.last), sha(self.last))
        with self.assertRaisesRegex(chunks.FetchError, 'chunk 1 retry/deadline exhausted'):
            self.fetch(first, *([bad] * chunks.ATTEMPTS))
        self.assertEqual(self.remote.call_count, chunks.ATTEMPTS + 2)
        self.assertEqual(self.destination.read_bytes(), b'existing backup')
        self.assertEqual(self.part.read_bytes(), self.first)
        state = json.loads(self.receipt.read_text())
        self.assertEqual((state['offset'], state['prefix_sha']), (len(self.first), sha(self.first)))
        self.fetch(self.frame(1, self.last))
        self.assertEqual(common.sha(self.destination), self.digest)

    def test_expected_backup_hash_requires_an_exact_match(self):
        for digest in (self.digest[:-1], sha(flip(self.payload))):
            with self.subTest(digest=digest), self.assertRaisesRegex(chunks.FetchError, 'differs from the backup receipt'):
                self.fetch(digest=digest)
            self.assertFalse(self.part.exists())
            self.assertFalse(self.destination.exists())

    def test_matching_chunk_hashes_cannot_replace_the_whole_file_hash(self):
        with self.assertRaisesRegex(chunks.FetchError, 'complete received SHA-256 differs'):
            self.fetch(self.frame(0, self.first), self.frame(1, flip(self.last)))
        self.assertFalse(self.destination.exists())

    def test_cache_free_readback_cannot_publish_corrupt_bytes(self):
        real_fcntl = chunks.fcntl.fcntl

        def corrupt_before_readback(fd, command, value):
            self.assertEqual((command, value), (chunks.F_NOCACHE, 1))
            self.assertEqual(self.part.read_bytes(), self.payload)
            self.part.write_bytes(flip(self.payload))
            return real_fcntl(fd, command, value)

        with mock.patch.object(chunks.fcntl, 'fcntl', side_effect=corrupt_before_readback):
            with self.assertRaisesRegex(chunks.FetchError, 'cache-free SSD SHA-256 differs'):
                self.fetch(self.frame(0, self.first), self.frame(1, self.last))
        self.assertFalse(self.destination.exists())


if __name__ == '__main__':
    unittest.main()
