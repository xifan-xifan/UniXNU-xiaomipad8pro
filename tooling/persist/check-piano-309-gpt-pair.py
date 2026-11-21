#!/usr/bin/env python3
"""Identify the piano 3.0.309 GPT state from saved 4 KiB boundary fragments."""
import argparse
import hashlib
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('primary', type=Path)
    parser.add_argument('backup', type=Path)
    args = parser.parse_args()
    try:
        primary = identify(args.primary, 6 * 4096, PRIMARY)
        backup = identify(args.backup, 5 * 4096, BACKUP)
    except OSError as exc:
        parser.exit(2, f'{exc}\n')
    print(f'primary={primary} backup={backup}')
    if primary == backup and primary in ('stock', 'candidate'):
        return 0
    print('The GPT copies do not form a verified pair.')
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
