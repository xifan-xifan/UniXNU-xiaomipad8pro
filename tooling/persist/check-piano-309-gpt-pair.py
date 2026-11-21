#!/usr/bin/env python3
"""Identify the piano 3.0.309 GPT state from saved 4 KiB boundary fragments."""
import argparse
import hashlib
import re
from pathlib import Path


PRIMARY = {
    '9c096761ee7b9c12c1a5c3ef3f100bc27abccbbafb798a141aa4320eb81af25d': 'stock',
    '12710135635beee2a5de1dd910924a3d2d26e09a6f368c191b879fd6b41a499b': 'candidate',
}
BACKUP = {
    'f8b53b4538913ccf2ac385d8bd87eb8ad689aa3f439222480a95d05c625ca4fe': 'stock',
    '9acf222558a6766c4f1fcd09509a41cfd5be10d53f34a2a0f84a8cfe0a18c2f5': 'candidate',
}


def identify(path, length, known):
    with path.open('rb') as fragment:
        data = fragment.read(length + 1)
    if len(data) != length:
        return 'invalid-size'
    return known.get(hashlib.sha256(data).hexdigest(), 'unknown')


def identify_readback(text):
    """Validate the complete output of the maintenance GPT readback command."""
    values = {}
    for line in text.splitlines():
        key, sep, value = line.partition('=')
        if not sep:
            continue
        if key == 'error':
            raise ValueError(f'maintenance readback failed: {value}')
        if key in ('primary_sha256', 'backup_sha256', 'lun0', 'gpt_preflight'):
            if key in values:
                raise ValueError(f'duplicate maintenance readback field: {key}')
            values[key] = value
    if values.get('gpt_preflight') != 'ready' or not re.fullmatch(r'sd[a-z]', values.get('lun0', '')):
        raise ValueError('incomplete maintenance preflight')
    return (PRIMARY.get(values.get('primary_sha256'), 'unknown'),
            BACKUP.get(values.get('backup_sha256'), 'unknown'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('primary', nargs='?', type=Path)
    parser.add_argument('backup', nargs='?', type=Path)
    parser.add_argument('--readback-log', type=Path, help='output from a successful maintenance readback')
    args = parser.parse_args()
    if args.readback_log:
        if args.primary or args.backup:
            parser.error('--readback-log cannot be combined with fragment paths')
    elif not (args.primary and args.backup):
        parser.error('provide two fragment paths or --readback-log')
    try:
        if args.readback_log:
            primary, backup = identify_readback(args.readback_log.read_text(encoding='utf-8'))
        else:
            primary = identify(args.primary, 6 * 4096, PRIMARY)
            backup = identify(args.backup, 5 * 4096, BACKUP)
    except (OSError, ValueError) as exc:
        parser.exit(2, f'{exc}\n')
    print(f'primary={primary} backup={backup}')
    if primary == backup and primary in ('stock', 'candidate'):
        return 0
    print('The GPT copies do not form a verified pair.')
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
