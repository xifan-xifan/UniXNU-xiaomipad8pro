#!/usr/bin/env python3
"""Check the exact captured Android A-slot rollback images offline."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--manifest', type=Path, default=Path(__file__).resolve().parents[2]
                        / 'outputs/manifests/native-macos-27.3.json')
    args = parser.parse_args()
    try:
        pins = json.loads(args.manifest.read_text(encoding='utf-8'))['readonly_device_captures']['partitions']
        for name in ('boot_a', 'init_boot_a', 'vendor_boot_a'):
            pin = pins[name]
            with (args.directory / f'{name}.img').open('rb') as image:
                digest = hashlib.file_digest(image, 'sha256').hexdigest()
                size = image.tell()
            if size != pin['bytes'] or digest != pin['sha256']:
                parser.exit(1, f'{name}: does not match the recorded A-slot capture ({size} bytes, SHA-256 {digest})\n')
            print(f'{name}: verified {size} bytes, SHA-256 {digest}')
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
