#!/usr/bin/env python3
"""Offline regression tests for the GPT, verified backups and live boot identity."""
import hashlib
import importlib.util
import plistlib
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zlib
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, call, patch

import common
import installed_boot_identity as identity


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gpt = load('build-gpt')
maint = load('maint-session')
B = common.LBA


class BootMenuCompatibility(unittest.TestCase):
    def test_direct_esp_builder_starts(self):
        builder = Path(__file__).with_name('build-esp.py')
        result = subprocess.run([sys.executable, str(builder), '--help'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_maintenance_parses_the_standalone_boot_menu(self):
        record = {'image': 'unused-boot-menu-fixture.img',
                  'unixnu_toml': '[[entry]]\nid = "test"\ntype = "action"\naction = "reset"\n'}
        with tempfile.TemporaryDirectory() as tmp, patch.object(maint, 'OUT', Path(tmp)):
            self.assertEqual(maint.entries_using_rootfs(SimpleNamespace(execute=False), record), {})

    def test_maintenance_uses_adt_from_default_xnu_entry_in_fat_image(self):
        session = maint.Session.__new__(maint.Session)
        session.windows = {}
        files = {'EFI/UniXNU/unixnu.toml': b'[boot]\ndefault = "daily"\n[[entry]]\nid = "daily"\n'
                 b'type = "xnu"\npath = "Payloads/k273/kernel.macho"\n',
                 'EFI/UniXNU/Payloads/k273/kernel.macho.adt': b'daily ADT', 'EFI/UniXNU/Payloads/kernel.macho.adt': b'legacy ADT'}
        with tempfile.TemporaryDirectory() as tmp, patch.object(maint, 'OUT', Path(tmp)):
            (Path(tmp) / 'fixture.img').touch()
            with (patch.object(common, 'fat32_read', return_value={'files': files}),
                  patch.object(common, 'adt_ram_window', return_value=13956546560) as window):
                self.assertEqual(session.esp_window({'image': 'fixture.img', 'sha256': 'fixture'}), 13956546560)
                window.assert_called_once_with(b'daily ADT')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def original_gpt(backup_last=common.DATA_LAST):
    entries = bytearray(64 * gpt.ENTRY)
    protected = ('userdata', 'super', 'metadata')
    parts = [(i, name, 10 + i * 100, 99 + i * 100) for i, name in enumerate(protected)]
    parts += [(common.ROOTFS_INDEX, 'linux_rootfs', common.ROOTFS_FIRST, common.ROOTFS_LAST),
              (common.DATA_INDEX, 'linux_data', common.DATA_FIRST, common.DATA_LAST)]
    for index, name, first, last in parts:
        offset = index * gpt.ENTRY
        struct.pack_into('<16s16sQQQ', entries, offset, uuid.UUID(int=1).bytes_le, uuid.UUID(int=index + 2).bytes_le,
                         first, last, 0)
        encoded = name.encode('utf-16le')
        entries[offset + 56:offset + 56 + len(encoded)] = encoded

    def header(current, alternate, table, last):
        blob = bytearray(B)
        struct.pack_into('<8sIIIIQQQQ16sQIII', blob, 0, b'EFI PART', 0x10000, 92, 0, 0, current, alternate,
                         6, last, uuid.UUID(int=99).bytes_le, table, 64, gpt.ENTRY, zlib.crc32(entries))
        struct.pack_into('<I', blob, 16, gpt.header_crc(blob, 92))
        return bytes(blob)

    end = common.LUN0_BLOCKS - 1
    mbr = bytearray(B)
    mbr[510:512] = b'\x55\xaa'
    pad = bytes(4 * B - len(entries))
    return (bytes(mbr) + header(1, end, 2, common.DATA_LAST) + entries + pad,
            bytes(entries) + pad + header(end, 1, end - 4, backup_last))


class PartitionSafety(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.nas = self.root / 'backup' / 'set'
        self.nas.mkdir(parents=True)
        self.s = maint.Session.__new__(maint.Session)
        self.s.execute = True
        self.s.a = SimpleNamespace(http_port=1)
        self.s.dir = self.root
        self.s.env = {'PIANO_BACKUP_DIR': str(self.nas.parent)}
        self.s.state = {'backup_name': self.nas.name, 'backups': {}}
        self.s.nodes = {'lun0': 'fixture-disk'}
        self.s.log = Mock()
        self.s.serve = Mock()
        self.s.fetch = Mock()
        self.s.t = Mock()
        self.s.job = Mock()
        self.s.region_sha = Mock()
        self.s.save = Mock()
        self.primary, self.backup = original_gpt()
        pinned = patch.multiple(common, GPT_PRIMARY_SHA256=sha(self.primary), GPT_BACKUP_SHA256=sha(self.backup))
        pinned.start()
        self.addCleanup(pinned.stop)
        out = patch.object(maint, 'OUT', self.root)
        out.start()
        self.addCleanup(out.stop)
        self.record_backup('lun0-gpt-primary', self.primary, 0)
        self.record_backup('lun0-gpt-backup', self.backup, maint.TAIL_LBA)
        self.head, self.tail, _, _, guid, *_ = gpt.build_tables(self.primary, self.backup)
        for name, data in (('gpt-head.bin', self.head), ('gpt-tail.bin', self.tail)):
            (self.root / name).write_bytes(data)
        self.s.gpt = {'esp_guid': str(guid), 'gpt-head.bin': {'sha256': sha(self.head)},
                      'gpt-tail.bin': {'sha256': sha(self.tail)}}

    def record_backup(self, key, data, seek):
        file = key + '.img'
        (self.nas / file).write_bytes(data)
        self.s.state['backups'][key] = {'file': file, 'sha256': sha(data), 'bytes': len(data),
                                       'skip': seek, 'count': len(data) // B, 'bs': B, 'verified': True}

    def test_android_entries_and_mbr_stay_identical(self):
        self.assertEqual(self.head[:B], self.primary[:B])
        end = 3 * gpt.ENTRY
        self.assertEqual(self.head[2 * B:2 * B + end], self.primary[2 * B:2 * B + end])
        self.assertEqual(self.tail[:end], self.backup[:end])
        maint.validate_gpt_update(self.s, sha(self.head), sha(self.tail))

    def test_disagreeing_original_headers_are_refused(self):
        _, wrong = original_gpt(common.DATA_LAST - 1)
        with patch.object(common, 'GPT_BACKUP_SHA256', sha(wrong)):
            with self.assertRaisesRegex(SystemExit, 'copies disagree'):
                gpt.build_tables(self.primary, wrong)

    def test_gpt_record_cannot_authorize_other_partition_changes(self):
        wrong = bytearray(self.head)
        wrong[2 * B + 32] ^= 1  # first LBA of userdata
        (self.root / 'gpt-head.bin').write_bytes(wrong)
        self.s.gpt['gpt-head.bin']['sha256'] = sha(wrong)
        self.s.region_sha.side_effect = [sha(self.primary), sha(self.backup)]
        mapped = {'lun0_block_size': str(B), 'lun0_bytes': str(maint.LUN0_BYTES), 'mounted': '0'}
        with patch.object(maint, 'map_nodes', return_value=mapped):
            with self.assertRaisesRegex(maint.Stop, 'permitted offline GPT update'):
                maint.gpt_step(self.s)
        self.s.fetch.assert_not_called()
        self.s.t.assert_not_called()

    def test_gpt_reads_live_state_and_aborts_on_tail_readback_mismatch(self):
        mapped = {'lun0_block_size': str(B), 'lun0_bytes': str(maint.LUN0_BYTES), 'mounted': '0'}
        self.s.state['gpt_state'] = 'new'  # stale from a previous operation
        self.s.region_sha.side_effect = [sha(self.primary), sha(self.backup), sha(b'bad read-back')]
        with patch.object(maint, 'map_nodes', return_value=mapped):
            with self.assertRaisesRegex(maint.Stop, 'backup GPT read back'):
                maint.gpt_step(self.s)
        self.s.t.assert_called_once_with('write', 'gpt-tail.bin', 'fixture-disk', maint.TAIL_LBA, B)
        self.s.save.assert_not_called()

    def test_gpt_resumes_checks_after_both_copies_were_written(self):
        self.s.state['gpt_state'] = 'new'  # A separate map can already have recorded both on-disk hashes.
        self.s.region_sha.side_effect = [sha(self.head), sha(self.tail)]
        self.s.shell = Mock(return_value='No problems found')
        mapped = {'lun0_block_size': str(B), 'lun0_bytes': str(maint.LUN0_BYTES), 'mounted': '0',
                  'linux_rootfs': f'fixture-root {common.ROOTFS_FIRST * 8} {(common.ROOTFS_NEW_LAST - common.ROOTFS_FIRST + 1) * 8}',
                  'esp': f'fixture-esp {common.ESP_FIRST * 8} {common.ESP_BLOCKS * 8}'}
        stale_map = dict(mapped, esp='none')
        with patch.object(maint, 'map_nodes', side_effect=[stale_map, mapped]):
            maint.gpt_step(self.s)
        self.s.fetch.assert_not_called()
        self.s.t.assert_called_once_with('rereadpt', 'fixture-disk')
        self.s.save.assert_called_once_with(gpt_state='new', gpt_head=sha(self.head), gpt_tail=sha(self.tail))

    def test_backup_contents_and_restore_geometry_required_before_write(self):
        key = 'lun0-gpt-primary'
        with self.assertRaisesRegex(maint.Stop, 'restore region matches'):
            self.s.restore(key, 'fixture-disk', 1, B)
        self.s.serve.assert_not_called()
        self.s.job.assert_not_called()
        path = self.nas / (key + '.img')
        path.write_bytes(bytes(len(self.primary)))
        with self.assertRaisesRegex(maint.Stop, 'still matches'):
            self.s.require_backup(key)
        path.unlink()
        with self.assertRaisesRegex(maint.Stop, 'still matches'):
            self.s.require_backup(key)


class SelectorWriteSafety(unittest.TestCase):
    record_backup = PartitionSafety.record_backup

    def setUp(self):
        temp = self.enterContext(tempfile.TemporaryDirectory())
        self.root = Path(temp)
        self.nas = self.root / 'backups' / 'set'
        self.nas.mkdir(parents=True)
        self.s = maint.Session.__new__(maint.Session)
        self.s.execute = True
        self.s.a = SimpleNamespace()
        self.s.dir = self.root
        self.s.env = {'PIANO_BACKUP_DIR': str(self.nas.parent)}
        self.s.state = {'backup_name': self.nas.name, 'backups': {}}
        self.s.log = Mock()
        self.s.serve = Mock()
        self.s.fetch = Mock()
        self.s.t = Mock()
        self.s.region_sha = Mock()
        self.s.save = Mock()
        self.enterContext(patch.object(maint, 'OUT', self.root))
        self.enterContext(patch.object(common, 'BOOT_PARTITION_BYTES', maint.MiB))
        self.s.a.boot_image = 'boot-selector-signed.img'
        self.s.state['abl'] = dict(maint.ABL_EXPECT)
        self.s.nodes = dict(boot_a='sde13', vendor_boot_a='sde23', init_boot_a='sde28')
        self.s.sizes = dict.fromkeys(('boot_a', 'vendor_boot_a', 'init_boot_a'), maint.MiB)
        self.s.sim = {}
        self.image = (b'ANDROID!' + maint.SELECTOR_TAG).ljust(maint.MiB, b'\0')
        self.digest = sha(self.image)
        self.esp = {'image': 'esp-test.img', 'sha256': sha(b'esp'), 'ram_window': common.DARWIN_WINDOW_MAX - 2 * maint.MiB}
        self.s.a.selector_r1 = [self.digest, self.esp['sha256']]
        self.s.tablet_esp = Mock(return_value=self.esp['sha256'])
        self.s.esp_by_sha = Mock(return_value=self.esp)
        self.old = b'ANDROID!'.ljust(maint.MiB, b'x')
        self.s.boot = {'signed': {self.s.a.boot_image: {'sha256': self.digest, 'fd_bound': common.DARWIN_WINDOW_MAX},
                                  'old-signed.img': {'sha256': sha(self.old), 'fd_bound': common.DARWIN_WINDOW_MAX}}}
        (self.root / self.s.a.boot_image).write_bytes(self.image)
        mapped = patch.object(maint, 'map_nodes', return_value={'boot_a': 'sde13 618800 196608', 'mounted': '0'})
        self.map = mapped.start()
        self.addCleanup(mapped.stop)
        self.record_backup('boot_a', self.old, 0)
        self.record_backup('boot_b', bytes(4096), 0)
        for key, digest in (('vendor_boot_a', common.ANDROID_VENDOR_BOOT_SHA256),
                            ('init_boot_a', common.ANDROID_INIT_BOOT_MAGISK_SHA256)):
            self.s.state['backups'][key] = {'file': key + '.img', 'sha256': digest, 'verified': True}
        self.s.write_manifest(self.nas, self.s.state['backups'])
        self.s.region_sha.side_effect = [sha(self.old), common.ANDROID_VENDOR_BOOT_SHA256, common.ANDROID_INIT_BOOT_MAGISK_SHA256,
                                         self.digest]
        self.s.t.return_value = {'hex': b'ANDROID!'.hex()}

    def assert_refused_before_write(self, message):
        with self.assertRaisesRegex(maint.Stop, message):
            maint.boot_a_step(self.s)
        self.s.fetch.assert_not_called()
        self.s.t.assert_not_called()

    def test_selector_requires_exact_r1_and_known_live_esp(self):
        for receipt in (None, [self.digest, sha(b'equal-sized different esp')], [sha(b'different boot'), self.esp['sha256']]):
            self.s.a.selector_r1 = receipt
            self.assert_refused_before_write('operator R1 pass')
        self.s.esp_by_sha.return_value = None
        self.assert_refused_before_write('live ESP has no matching build record')

    def test_selector_requires_unlocked_abl_and_compatible_rollback(self):
        self.s.state['abl']['unlocked'] = 'no'
        self.assert_refused_before_write('verify unlocked ABL')
        self.s.state['abl']['unlocked'] = 'yes'
        self.s.boot['signed']['old-signed.img']['fd_bound'] = common.OLD_DARWINBOOT_WINDOW_MAX
        self.assert_refused_before_write('remains compatible for rollback')

    def test_selector_requires_untampered_backup_manifest(self):
        (self.nas / 'SHA256SUMS').unlink()
        self.assert_refused_before_write('boot_a backup manifest.json and SHA256SUMS')

    def test_exact_partition_is_the_only_write_and_readback_must_match(self):
        maint.boot_a_step(self.s)
        self.s.t.assert_any_call('write', 'boot.img', 'sde13', 0, maint.MiB, timeout=600)
        self.assertEqual([c.args[0] for c in self.s.t.call_args_list], ['write', 'peek'])
        self.assertEqual(self.s.save.call_args_list, [call(selector_write_sha256=self.digest),
                                                     call(boot_a_written=self.digest)])
        self.s.save.reset_mock()
        self.s.region_sha.side_effect = [sha(self.old), common.ANDROID_VENDOR_BOOT_SHA256,
                                         common.ANDROID_INIT_BOOT_MAGISK_SHA256, sha(b'bad')]
        with self.assertRaisesRegex(maint.Stop, 'boot_a read back'):
            maint.boot_a_step(self.s)
        self.s.save.assert_called_once_with(selector_write_sha256=self.digest)

    def test_rollback_refuses_wrong_or_mounted_partition(self):
        for mapped in ({'boot_a': 'sde13 618800 196607', 'mounted': '0'}, {'boot_a': 'sde13 618800 196608', 'mounted': '1'}):
            self.map.return_value = mapped
            with self.assertRaisesRegex(maint.Stop, 'expected partition geometry'):
                maint.boot_a_back(self.s)
        self.s.serve.assert_not_called()

    def test_selector_ram_boot_only_checks_abl_and_boots_exact_file(self):
        self.s.esp_state = Mock(return_value=(self.esp['image'], self.esp['sha256']))
        self.s.gpt = {'esp_guid': 'fixture'}
        self.s.serial = 'fixture'
        self.s.wait = Mock(return_value=True)
        values = dict(maint.ABL_EXPECT)
        self.s.fastboot = Mock(side_effect=lambda cmd, *args, **kwargs: f'{args[0]}: {values.get(args[0])}'
                               if cmd == 'getvar' else None)
        maint.ram_boot_a(self.s)
        self.assertEqual(self.s.fastboot.call_args.args, ('boot', str(self.root / self.s.a.boot_image)))
        self.assertTrue(all(c.args[0] == 'getvar' for c in self.s.fastboot.call_args_list[:-1]))
        self.s.fastboot.reset_mock()
        values['unlocked'] = 'no'
        with self.assertRaisesRegex(maint.Stop, 'unlocked = yes'):
            maint.ram_boot_a(self.s)
        self.assertTrue(all(c.args[0] == 'getvar' for c in self.s.fastboot.call_args_list))


class InstalledIdentity(unittest.TestCase):
    def test_conflicting_arguments_across_adt_tail_are_refused(self):
        profile = identity.argument_profile('data-persist=0 piano-boot-args=ram',
                                            {'property': 'piano-boot-args-ram', 'tail': 'data-persist=2 extra=1'})
        identity.need_args(profile['args'], 'extra=1')
        for mode in ('data-persist=0', 'data-persist=2'):
            with self.subTest(mode=mode), self.assertRaisesRegex(ValueError, 'conflicting'):
                identity.need_args(profile['args'], mode)

    def test_chosen_requires_one_boot_and_clean_nul_terminated_text(self):
        boot_id = str(uuid.UUID(int=1))
        prop = 'piano-boot-args-ram'

        def observed(raw, end=boot_id):
            plist = plistlib.dumps([{'IORegistryEntryName': 'chosen', prop: raw}]).decode()
            return identity.chosen_observation(f'{boot_id}\ndata-persist=0\n{plist}{end}\n', prop)

        self.assertEqual(observed(b'extra=1\0\0'), ('data-persist=0', 'extra=1'))
        for raw in (b'extra=1', b'extra=1\0junk\0', b'extra=\xff\0', 'extra=1'):
            with self.subTest(raw=raw), self.assertRaises(identity.MissingBootEvidence):
                observed(raw)
        with self.assertRaises(identity.MissingBootEvidence):
            observed(b'extra=1\0', str(uuid.UUID(int=2)))


if __name__ == '__main__':
    unittest.main()
