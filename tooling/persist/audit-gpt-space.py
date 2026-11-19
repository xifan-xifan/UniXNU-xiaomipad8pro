#!/usr/bin/env python3
"""Audit unallocated GPT extents from verified first/last 1 MiB captures; no device or disk writes."""
import argparse
import hashlib
import importlib.util
import json
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location('build_gpt', HERE / 'build-gpt.py')
gpt = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = gpt
spec.loader.exec_module(gpt)


def checked_header(block, label):
    header = gpt.header(block)
    size = header['header_size']
    if header['signature'] != b'EFI PART' or not 92 <= size <= len(block):
        raise ValueError(f'{label}: invalid GPT header signature/size')
    if gpt.header_crc(block, size) != header['header_crc32']:
        raise ValueError(f'{label}: header CRC mismatch')
    return header


def audit_lun(record, captures, block_bytes):
    device, blocks = record['device'], record['blocks']
    head = (captures / f'{device}-head.bin').read_bytes()
    tail = (captures / f'{device}-tail.bin').read_bytes()
    expected_bytes = 256 * block_bytes
    if len(head) != expected_bytes or len(tail) != expected_bytes:
        raise ValueError(f'{device}: capture length mismatch')
    if hashlib.sha256(head).hexdigest() != record['head_sha256']:
        raise ValueError(f'{device}: head SHA-256 mismatch')
    if hashlib.sha256(tail).hexdigest() != record['tail_sha256']:
        raise ValueError(f'{device}: tail SHA-256 mismatch')

    primary = checked_header(head[block_bytes:2 * block_bytes], f'{device} primary')
    if primary['my_lba'] != 1 or primary['alternate_lba'] != blocks - 1:
        raise ValueError(f'{device}: primary header LBA mismatch')
    count, entry_size = primary['entry_count'], primary['entry_size']
    if count <= 0 or entry_size != 128:
        raise ValueError(f'{device}: unsupported GPT entry geometry')
    array_size = count * entry_size
    primary_offset = primary['entries_lba'] * block_bytes
    primary_array = head[primary_offset:primary_offset + array_size]
    if len(primary_array) != array_size or zlib.crc32(primary_array) & 0xffffffff != primary['entries_crc32']:
        raise ValueError(f'{device}: primary entry-array CRC/range mismatch')

    backup_block = tail[-block_bytes:]
    backup = checked_header(backup_block, f'{device} backup')
    if backup['my_lba'] != blocks - 1 or backup['alternate_lba'] != 1:
        raise ValueError(f'{device}: backup header LBA mismatch')
    backup_offset = (backup['entries_lba'] - (blocks - 256)) * block_bytes
    backup_array = tail[backup_offset:backup_offset + array_size]
    if backup_offset < 0 or len(backup_array) != array_size:
        raise ValueError(f'{device}: backup entry-array range mismatch')
    if zlib.crc32(backup_array) & 0xffffffff != backup['entries_crc32']:
        raise ValueError(f'{device}: backup entry-array CRC mismatch')
    if primary_array != backup_array:
        raise ValueError(f'{device}: primary/backup entry arrays differ')
    for key in ('first_usable_lba', 'last_usable_lba', 'disk_guid', 'entry_count', 'entry_size', 'entries_crc32'):
        if primary[key] != backup[key]:
            raise ValueError(f'{device}: primary/backup {key} differs')

    entries = sorted(gpt.entries_of(primary_array, count), key=lambda entry: entry['first_lba'])
    cursor, gaps = primary['first_usable_lba'], []
    for entry in entries:
        first, last = entry['first_lba'], entry['last_lba']
        if first < cursor or first > last or last > primary['last_usable_lba']:
            raise ValueError(f'{device}: invalid/overlapping partition {entry["number"]} {entry["name"]}')
        if first > cursor:
            gaps.append((cursor, first - 1))
        cursor = last + 1
    if cursor <= primary['last_usable_lba']:
        gaps.append((cursor, primary['last_usable_lba']))
    return len(entries), gaps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--captures', required=True, type=Path, help='directory containing <device>-head.bin and -tail.bin')
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))['readonly_device_captures']['gpt_boundaries']
    block_bytes = manifest['logical_block_bytes']
    overall_max = (0, None, None, None)
    for record in manifest['luns']:
        count, gaps = audit_lun(record, args.captures, block_bytes)
        largest = max(((last - first + 1, first, last) for first, last in gaps), default=(0, None, None))
        print(f'{record["device"]}: PASS; partitions={count}; free_extents={len(gaps)}; '
              f'largest={largest[0]} blocks ({largest[0] * block_bytes} bytes) at {largest[1]}-{largest[2]}')
        for first, last in gaps:
            print(f'  LBA {first}-{last}: {(last - first + 1) * block_bytes} bytes')
        if largest[0] > overall_max[0]:
            overall_max = (largest[0], record['device'], largest[1], largest[2])
    print(f'PASS: all {len(manifest["luns"])} LUNs verified; largest GPT-free extent is on '
          f'{overall_max[1]} ({overall_max[0] * block_bytes} bytes).')


if __name__ == '__main__':
    main()
