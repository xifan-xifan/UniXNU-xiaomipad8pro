#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""SW-B safety contract over synthetic boot images and GPT copies held in memory.

Only the GPT byte transformations are shared with maintenance tooling. The
transaction and its failures are modeled with byte-array writes. Recovery after
a power cut runs once the simulated disk is available again.
"""
import hashlib
import importlib.util
import struct
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'maint', Path(__file__).resolve().parents[1] / 'persist' / 'maint-session.py')
maint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(maint)

PAGE = 4096
BOOT_ENTRY = 13  # 1-based, as on LUN 4
SLOT_SUCCESS = 1 << 54
ENTRY_SIZE = 128
ENTRY_COUNT = 96
TABLE_SIZE = ENTRY_SIZE * ENTRY_COUNT
GPT_HEADER_SIZE = 92
LAST_LBA = 999
# (first LBA in the fixture, header LBA, entry array LBA). The backup array
# begins two blocks before the last five LBAs of LUN 4.
GPT_LAYOUT = {'gpt-primary': (0, 1, 2), 'gpt-backup': (LAST_LBA - 6, LAST_LBA, LAST_LBA - 6)}
BACKUP_REGIONS = ('boot_a', *GPT_LAYOUT)
EXCLUDED = (
    'userdata', 'super', 'metadata', 'boot_b', 'vendor_boot_b', 'init_boot_b', 'system_b', 'future_b',
    'xbl', 'abl', 'tz', 'hyp', 'vbmeta', 'dtbo', 'persist',
    'xbl_a', 'abl_a', 'tz_a', 'hyp_a', 'vbmeta_a', 'dtbo_a', 'xbl_b', 'abl_b', 'vbmeta_b', 'dtbo_b',
    'vendor_boot_a', 'init_boot_a', 'boot', 'BOOT_A', 'boot_a ',
)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def flip_byte(data, offset):
    return data[:offset] + bytes([data[offset] ^ 1]) + data[offset + 1:]


def boot_image(marker):
    header = bytearray(PAGE)
    struct.pack_into('<8s4I', header, 0, b'ANDROID!', PAGE, PAGE, 0, 1584)
    struct.pack_into('<I', header, 40, 4)
    header[44] = marker
    return bytes(header) + bytes([marker]) * PAGE + bytes([marker + 1]) * PAGE


def gpt_copies():
    entries = bytearray(TABLE_SIZE)
    for number, name in ((12, 'boot_b'), (BOOT_ENTRY, 'boot_a'), (14, 'vendor_boot_a')):
        offset = (number - 1) * ENTRY_SIZE
        struct.pack_into('<16s16sQQQ72s', entries, offset, b'T' * 16, bytes([number]) * 16,
                         3 * number, 3 * number + 2, 0x003F000000000000, name.encode('utf-16le'))

    def header(current, alternate, table):
        block = bytearray(PAGE)
        struct.pack_into('<8sIIIIQQQQ16sQIII', block, 0, b'EFI PART', 0x10000, GPT_HEADER_SIZE, 0, 0,
                         current, alternate, 6, LAST_LBA - 7, b'G' * 16, table, ENTRY_COUNT, ENTRY_SIZE,
                         zlib.crc32(entries))
        struct.pack_into('<I', block, 16, zlib.crc32(block[:GPT_HEADER_SIZE]))
        return bytes(block)

    mbr = bytearray(PAGE)
    struct.pack_into('<B3xII', mbr, 450, 0xEE, 1, LAST_LBA)
    mbr[510:512] = b'\x55\xaa'
    return {'gpt-primary': bytes(mbr) + header(1, LAST_LBA, 2) + entries + bytes(PAGE),
            'gpt-backup': bytes(entries) + bytes(3 * PAGE) + header(LAST_LBA, 1, LAST_LBA - 6)}


def gpt_offsets(region):
    first, header, table = GPT_LAYOUT[region]
    return (header - first) * PAGE, (table - first) * PAGE


def checked_entries(blob, region):
    header, table = gpt_offsets(region)
    header_bytes = blob[header:header + GPT_HEADER_SIZE]
    entries = blob[table:table + TABLE_SIZE]
    _, header_lba, table_lba = GPT_LAYOUT[region]
    signature, _, header_size = struct.unpack_from('<8sII', header_bytes)
    if (signature, header_size) != (b'EFI PART', GPT_HEADER_SIZE) or (
            struct.unpack_from('<Q', header_bytes, 24)[0] != header_lba or
            struct.unpack_from('<QII', header_bytes, 72) != (table_lba, ENTRY_COUNT, ENTRY_SIZE)):
        raise ValueError(f'{region} GPT layout mismatch')
    stored_header_crc, = struct.unpack_from('<I', header_bytes, 16)
    stored_table_crc, = struct.unpack_from('<I', header_bytes, 88)
    if stored_header_crc != zlib.crc32(header_bytes[:16] + bytes(4) + header_bytes[20:]):
        raise ValueError(f'{region} header CRC mismatch')
    if stored_table_crc != zlib.crc32(entries):
        raise ValueError(f'{region} table CRC mismatch')
    return entries


class PowerLoss(Exception):
    pass


class MemoryDisk:
    def __init__(self, regions):
        self.regions = {name: bytearray(data) for name, data in regions.items()}
        self.trace = []

    def read(self, region):
        data = bytes(self.regions[region])
        self.trace.append(('read', region, 0, len(data)))
        return data

    def write(self, region, offset, data):
        self.trace.append(('write', region, offset, len(data)))
        self.regions[region][offset:offset + len(data)] = data

    def writes(self):
        return [event for event in self.trace if event[0] == 'write']

    def snapshot(self):
        return {name: bytes(data) for name, data in self.regions.items()}


def write_boot(disk, image):
    for offset in range(PAGE, len(image), PAGE):
        disk.write('boot_a', offset, image[offset:offset + PAGE])
    disk.write('boot_a', 0, image[:PAGE])


def simulate_switch(disk, target, image, allowlist, backups, manifest, *, mark_slot_success=False):
    if target != 'boot_a':
        return 'refused: target must be boot_a'
    digest = sha256(image)
    if digest not in allowlist:
        return 'refused: candidate SHA-256 is not allowlisted'

    verified = {}
    for region in BACKUP_REGIONS:
        saved = backups.get(region)
        if saved is None or sha256(saved) != manifest.get(region) or disk.read(region) != saved:
            return f'refused: {region} backup or SHA-256 manifest verification failed'
        verified[region] = saved
    old_boot = verified['boot_a']
    if sha256(old_boot) not in allowlist:
        return 'refused: installed boot_a SHA-256 is not allowlisted'
    if len(image) != len(old_boot) or len(image) <= PAGE:
        return 'refused: unexpected boot_a image length'

    primary, backup = (verified[region] for region in GPT_LAYOUT)
    try:
        if checked_entries(primary, 'gpt-primary') != checked_entries(backup, 'gpt-backup'):
            return 'refused: GPT entry arrays disagree'
    except ValueError as exc:
        return f'refused: {exc}'

    try:
        write_boot(disk, image)
    except PowerLoss:
        # Resume in the simulation after power returns.
        failure = 'power loss during body write'
    else:
        failure = None if sha256(disk.read('boot_a')) == digest else 'boot_a read-back SHA-256 mismatch'
    if failure:
        write_boot(disk, old_boot)
        if sha256(disk.read('boot_a')) != manifest['boot_a']:
            return f'aborted: {failure}; rollback verification failed'
        return f'aborted: {failure}; rollback verified'

    if mark_slot_success:
        updated = maint.gpt_set_attr_bit(primary, 0, 1, BOOT_ENTRY, 54)
        entries = updated[2 * PAGE:2 * PAGE + TABLE_SIZE]
        disk.write('gpt-backup', 0, entries)
        disk.write('gpt-backup', len(backup) - PAGE, maint.gpt_backup_header_with(backup[-PAGE:], entries))
        disk.write('gpt-primary', PAGE, updated[PAGE:])
    return 'switch verified'


class SwitchSafety(unittest.TestCase):
    def setUp(self):
        self.old = boot_image(1)
        self.image = boot_image(3)
        self.allowlist = frozenset((sha256(self.old), sha256(self.image)))
        self.initial = dict.fromkeys(EXCLUDED, b'untouched partition')
        self.initial.update(boot_a=self.old, **gpt_copies())
        self.disk = MemoryDisk(self.initial)
        self.backups = {region: self.initial[region] for region in BACKUP_REGIONS}
        self.manifest = {region: sha256(saved) for region, saved in self.backups.items()}
        self.boot_writes = [('write', 'boot_a', offset, PAGE) for offset in (PAGE, 2 * PAGE, 0)]
        self.boot_read = ('read', 'boot_a', 0, len(self.image))

    def switch(self, target='boot_a', image=None, *, mark_slot_success=False):
        return simulate_switch(self.disk, target, self.image if image is None else image, self.allowlist,
                               self.backups, self.manifest, mark_slot_success=mark_slot_success)

    def assert_refused(self, expected, **kwargs):
        before = self.disk.snapshot()
        self.assertEqual(self.switch(**kwargs), 'refused: ' + expected)
        self.assertEqual(self.disk.writes(), [])
        self.assertEqual(self.disk.snapshot(), before)

    def test_exact_boot_a_target_only(self):
        for target in (*EXCLUDED, *GPT_LAYOUT, ''):
            with self.subTest(target=target):
                self.assert_refused('target must be boot_a', target=target, mark_slot_success=True)
        self.assertEqual(self.disk.trace, [])

    def test_candidate_sha256_allowlist_covers_header_and_body(self):
        for image in (boot_image(5), flip_byte(self.image, 44), flip_byte(self.image, len(self.image) - 1)):
            with self.subTest(digest=sha256(image)):
                self.assert_refused('candidate SHA-256 is not allowlisted', image=image)
        self.assertEqual(self.disk.trace, [])

    def test_unknown_installed_image_is_refused_even_if_backed_up(self):
        unknown = boot_image(5)
        self.disk.regions['boot_a'][:] = unknown
        self.backups['boot_a'] = unknown
        self.manifest['boot_a'] = sha256(unknown)
        self.assert_refused('installed boot_a SHA-256 is not allowlisted')

    def test_backups_and_sha256_manifest_are_required_before_any_write(self):
        backups, manifest = self.backups.copy(), self.manifest.copy()
        for region, saved in backups.items():
            digest = manifest[region]
            corrupt = flip_byte(saved, 0)
            for fault, copy, recorded in (
                    ('missing backup', None, digest),
                    ('missing manifest entry', saved, None),
                    ('bad digest', saved, sha256(b'wrong digest')),
                    ('corrupt copy', corrupt, digest),
                    ('stale copy', corrupt, sha256(corrupt))):
                self.backups = backups.copy()
                self.manifest = manifest.copy()
                self.backups[region], self.manifest[region] = copy, recorded
                if copy is None:
                    del self.backups[region]
                if recorded is None:
                    del self.manifest[region]
                for mark_slot_success in (False, True):
                    with self.subTest(region=region, fault=fault, mark_slot_success=mark_slot_success):
                        self.assert_refused(f'{region} backup or SHA-256 manifest verification failed',
                                            mark_slot_success=mark_slot_success)

    def test_verified_switch_writes_all_body_pages_before_header_then_reads_back(self):
        self.assertEqual(self.switch(), 'switch verified')
        self.assertEqual(self.disk.trace[-4:], self.boot_writes + [self.boot_read])
        self.assertEqual(self.disk.writes(), self.boot_writes)
        self.assertEqual(self.disk.snapshot(), dict(self.initial, boot_a=self.image))

    def test_power_loss_mid_body_leaves_old_header_and_rolls_back(self):
        write = self.disk.write
        interrupted = []

        def cut_power(region, offset, data):
            if offset == 2 * PAGE and not interrupted:
                write(region, offset, data[:PAGE // 2])
                interrupted.append(self.disk.snapshot()['boot_a'])
                raise PowerLoss
            write(region, offset, data)

        with patch.object(self.disk, 'write', side_effect=cut_power):
            self.assertEqual(self.switch(mark_slot_success=True), 'aborted: power loss during body write; rollback verified')
        cut = 2 * PAGE + PAGE // 2
        self.assertEqual(interrupted, [self.old[:PAGE] + self.image[PAGE:cut] + self.old[cut:]])
        attempted = [self.boot_writes[0], ('write', 'boot_a', 2 * PAGE, PAGE // 2)]
        self.assertEqual(self.disk.writes(), attempted + self.boot_writes)
        self.assertEqual(self.disk.trace[-4:], self.boot_writes + [self.boot_read])
        self.assertEqual(self.disk.snapshot(), self.initial)

    def test_readback_mismatch_aborts_and_verifies_rollback(self):
        for offset in (44, len(self.image) - 1):
            with self.subTest(offset=offset):
                self.disk = MemoryDisk(self.initial)
                write = self.disk.write

                def corrupt_commit(region, start, data):
                    write(region, start, data)
                    if start == 0 and data == self.image[:PAGE]:
                        self.disk.regions[region][offset] ^= 1

                with patch.object(self.disk, 'write', side_effect=corrupt_commit):
                    self.assertEqual(self.switch(mark_slot_success=True),
                                     'aborted: boot_a read-back SHA-256 mismatch; rollback verified')
                self.assertEqual(self.disk.writes(), self.boot_writes * 2)
                self.assertEqual(self.disk.trace[-8:], (self.boot_writes + [self.boot_read]) * 2)
                self.assertEqual(self.disk.snapshot(), self.initial)

    def test_failed_rollback_verification_is_reported(self):
        write = self.disk.write

        def corrupt_header(region, offset, data):
            write(region, offset, data if offset else flip_byte(data, 44))

        with patch.object(self.disk, 'write', side_effect=corrupt_header):
            self.assertEqual(self.switch(mark_slot_success=True),
                             'aborted: boot_a read-back SHA-256 mismatch; rollback verification failed')
        self.assertEqual(self.disk.writes(), self.boot_writes * 2)
        self.assertEqual(self.disk.snapshot(), dict(self.initial, boot_a=flip_byte(self.old, 44)))

    def test_slot_success_changes_only_boot_a_attribute_and_both_crcs(self):
        backups, manifest = self.backups.copy(), self.manifest.copy()
        self.assertEqual(self.switch(mark_slot_success=True), 'switch verified')
        self.assertEqual(self.disk.trace[-7:], self.boot_writes + [self.boot_read] + [
            ('write', 'gpt-backup', 0, TABLE_SIZE),
            ('write', 'gpt-backup', 6 * PAGE, PAGE),
            ('write', 'gpt-primary', PAGE, 5 * PAGE),
        ])
        after = self.disk.snapshot()
        self.assertEqual(after.pop('boot_a'), self.image)
        tables = []
        for region in GPT_LAYOUT:
            header, table = gpt_offsets(region)
            changed = after.pop(region)
            tables.append(checked_entries(changed, region))
            expected = bytearray(backups[region])
            attrs_offset = table + (BOOT_ENTRY - 1) * ENTRY_SIZE + 48
            before_attrs, = struct.unpack_from('<Q', expected, attrs_offset)
            struct.pack_into('<Q', expected, attrs_offset, before_attrs | SLOT_SUCCESS)
            for offset in (header + 16, header + 88):
                self.assertNotEqual(changed[offset:offset + 4], expected[offset:offset + 4])
                expected[offset:offset + 4] = changed[offset:offset + 4]
            self.assertEqual(changed, expected, region)
        self.assertEqual(tables[0], tables[1])
        self.assertEqual(after, {name: self.initial[name] for name in EXCLUDED})
        self.assertEqual(self.backups, backups)
        self.assertEqual(self.manifest, manifest)

    def test_bad_gpt_crcs_refused_even_with_matching_backup_manifest(self):
        for region in GPT_LAYOUT:
            header, table = gpt_offsets(region)
            for offset, field in ((header + 16, 'header'), (table, 'table')):
                with self.subTest(region=region, field=field):
                    damaged = flip_byte(self.initial[region], offset)
                    self.disk.regions[region][:] = damaged
                    self.backups[region], self.manifest[region] = damaged, sha256(damaged)
                    self.assert_refused(f'{region} {field} CRC mismatch', mark_slot_success=True)
            self.disk.regions[region][:] = self.initial[region]
            self.backups[region] = self.initial[region]
            self.manifest[region] = sha256(self.backups[region])


if __name__ == '__main__':
    unittest.main(verbosity=2)
