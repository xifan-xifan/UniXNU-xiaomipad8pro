#!/usr/bin/env python3
"""Build the SHNGROOT root manifest (UniXNU userdata.py layout, read by NativeUfs findRootLayout) for raw disk images that
will be written into a partition at 1 MiB-aligned windows. Runs anywhere; hashing 31 GB takes a few minutes.
   make-root-manifest.py OUT_DIR NAME=IMAGE [NAME=IMAGE ...]
Writes OUT_DIR/manifest.bin (4096 bytes) and OUT_DIR/layout.json {name, offset, size, sha256}; the writer then puts the
manifest at byte 0 of the partition and each image at its offset (dd ... seek=offset/1MiB bs=1M conv=notrunc)."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import tempfile

MIB = 1 << 20
MAX_WINDOWS = 4  # NativeUfs version 1 reader


def image_digest(path):
    # Inspect the opened file and avoid blocking on non-regular inputs.
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_size <= 0 or before.st_size % 512:
            raise ValueError(f'{path}: expected a nonempty regular image in whole 512-byte sectors')
        digest = hashlib.sha256()
        remaining = before.st_size
        while remaining:
            chunk = stream.read(min(remaining, 8 * MIB))
            if not chunk:
                raise ValueError(f'{path}: image shortened during hashing')
            digest.update(chunk)
            remaining -= len(chunk)
        after = os.stat(path)
        fingerprint = lambda st: (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns)
        if fingerprint(before) != fingerprint(after):
            raise ValueError(f'{path}: image changed during hashing')
    return before.st_size, digest.hexdigest()


def build(specs):
    if not 1 <= len(specs) <= MAX_WINDOWS:
        raise ValueError(f'expected 1 to {MAX_WINDOWS} named images')
    entries, names, cursor = [], set(), MIB
    for spec in specs:
        name, separator, path = spec.partition('=')
        if not separator or not path or not re.fullmatch(r'[ -~]{1,31}', name):
            raise ValueError(f'{spec}: expected NAME=IMAGE with a 1–31 byte printable ASCII name')
        if name in names:
            raise ValueError(f'duplicate window name: {name}')
        names.add(name)
        size, digest = image_digest(path)
        entries.append(dict(name=name, image=path, offset=cursor, size=size, sha256=digest))
        print(f'{name}: {size} bytes at {cursor:#x} sha256 {digest}', flush=True)
        cursor += (size + MIB - 1) // MIB * MIB
        if cursor >= 1 << 64:
            raise ValueError('image layout exceeds the v1 size field')
    header = bytearray(4096)
    struct.pack_into('<8sIIQ', header, 0, b'SHNGROOT', 1, len(entries), cursor)
    for i, entry in enumerate(entries):
        struct.pack_into('<32sQQII32s40s', header, 24 + 128 * i, entry['name'].encode('ascii'),
                         entry['offset'], entry['size'], 512, 1, bytes.fromhex(entry['sha256']), bytes(40))
    header[4064:] = hashlib.sha256(header[:4064]).digest()
    return header, dict(image_bytes=cursor, windows=entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('out', type=Path)
    parser.add_argument('images', nargs='+', metavar='NAME=IMAGE')
    args = parser.parse_args()
    try:
        header, layout = build(args.images)
        args.out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.manifest-', dir=args.out) as tmp:
            staging = Path(tmp)
            (staging / 'layout.json').write_text(json.dumps(layout, indent=2))
            (staging / 'manifest.bin').write_bytes(header)
            publish(staging, args.out)
    except (OSError, ValueError) as exc:
        parser.exit(1, f'make-root-manifest: {exc}\n')
    print('image_bytes', layout['image_bytes'], 'manifest', hashlib.sha256(header).hexdigest())


def publish(staging, out):
    # Serialize writers and keep the previous receipt if publishing the manifest fails.
    fd = os.open(out, os.O_RDONLY)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        layout = out / 'layout.json'
        backup = staging / 'previous-layout.json'
        try:
            os.link(layout, backup, follow_symlinks=False)
        except FileNotFoundError:
            backup = None
        (staging / 'layout.json').replace(layout)
        try:
            (staging / 'manifest.bin').replace(out / 'manifest.bin')
        except OSError:
            if backup is None:
                layout.unlink()
            else:
                backup.replace(layout)
            raise
    finally:
        os.close(fd)


if __name__ == '__main__':
    main()
