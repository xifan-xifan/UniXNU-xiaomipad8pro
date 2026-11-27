#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the byte-for-byte copy of a host policy header added by a kernel patch."""
import argparse
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', help='header path in the patch')
    parser.add_argument('header', type=Path, help='host test header')
    parser.add_argument('patch', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='piano-policy-') as scratch:
        root = Path(scratch).resolve()
        # Keep Git in this scratch directory even when TMPDIR is inside a checkout.
        result = subprocess.run(['git', f'--git-dir={root / ".git"}', 'apply', '--no-index',
                                 f'--include={args.destination}', '--whitespace=nowarn', str(args.patch.resolve())],
                                cwd=root, capture_output=True, text=True)
        if result.returncode:
            raise SystemExit(f'{args.patch.name}: {result.stderr.strip()}')
        header = root / args.destination
        if not header.is_file() or header.read_bytes() != args.header.read_bytes():
            raise SystemExit(f'{args.patch.name}: does not add {args.header.name} byte for byte')
    print(f'{args.patch.name}: adds {args.header.name} byte for byte')


if __name__ == '__main__':
    main()
