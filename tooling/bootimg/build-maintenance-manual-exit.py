#!/usr/bin/env python3
"""Rebuild the pinned Ubuntu piano readback image for manual exit, offline."""
import argparse
import gzip
import hashlib
from pathlib import Path
import runpy
import struct

boot = runpy.run_path(str(Path(__file__).with_name('verify-ram-images.py')))
SOURCE_SHA256 = '64164d4c221386c64b2e92c542efb8474b4f397ca997a2db7f2a9ffe4ed79e3a'


def edit_init(data):
    start = data.index(b'# An initramfs without the maintenance payload')
    end = data.index(b'\necho "<6>PIANO_RAM_INIT', start)
    return data[:start] + b'''# Dedicated readback image. Keep the maintenance shell available after link loss.
mode=maintenance
timeout_seconds=disabled
watchdog_pid=none
''' + data[end:]


def edit_maintenance(data):
    return (data.replace(b'\n\tmap\n', b'\n')
            .replace(b'\n\treboot -f\n', b'\n\treturn 1\n')
            .replace(b'Idle timer: Fastboot after $(limit) s with no open session and no keepalive.',
                     b'Automatic restart disabled in this readback image.'))


def cpio_executable(name, data):
    name += b'\0'
    fields = (0, 0o100755, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0)
    header = b'070701' + ''.join(f'{field:08x}' for field in fields).encode()
    return header + name + bytes(-(len(header) + len(name)) % 4) + data + bytes(-len(data) % 4)


def edit_archive(archive, tablet=None):
    edits = {b'init': edit_init, b'bin/piano-maint': edit_maintenance}
    result = bytearray()
    position = 0
    while archive[position:position + 6] == b'070701':
        header = archive[position:position + 110]
        size = int(header[54:62], 16)
        name_size = int(header[94:102], 16)
        name_end = position + 110 + name_size
        name = archive[position + 110:name_end - 1]
        start = (name_end + 3) & ~3
        end = (start + size + 3) & ~3
        if tablet is not None and name == b'bin/piano-tablet':
            raise ValueError('piano-tablet already exists in the source archive')
        if tablet is not None and name == b'TRAILER!!!':
            result += cpio_executable(b'bin/piano-tablet', tablet)
        if name in edits:
            data = edits.pop(name)(archive[start:start + size])
            result += header[:54] + f'{len(data):08x}'.encode() + header[62:]
            result += archive[position + 110:start] + data + bytes(-len(data) % 4)
        else:
            result += archive[position:end]
        position = end
        if name == b'TRAILER!!!' and not edits and not any(archive[position:]):
            return result + archive[position:]
    raise ValueError('unexpected initramfs layout')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='pinned EFI-pstore-disabled image')
    parser.add_argument('output', type=Path, help='new image file')
    parser.add_argument('--include-tablet', action='store_true', help='embed tablet.sh for explicit invocation')
    args = parser.parse_args()
    image = args.source.read_bytes()
    if hashlib.sha256(image).hexdigest() != SOURCE_SHA256:
        parser.error('source SHA-256 differs from the pinned image')
    header = boot['header'](image)
    kernel, ramdisk, dtb, end = boot['sections'](image, header)
    page = header['page_size']
    if end != len(image) or header['second_size'] or header['recovery_dtbo_size']:
        parser.error('unexpected boot image layout')
    tablet = None
    if args.include_tablet:
        tablet = (Path(__file__).resolve().parents[1] / 'persist/tablet.sh').read_bytes().replace(b'\r\n', b'\n')
    packed = gzip.compress(edit_archive(gzip.decompress(ramdisk), tablet), compresslevel=9, mtime=0)
    prefix_end = page + len(kernel) + (-len(kernel) % page)
    dtb_start = end - len(dtb) - (-len(dtb) % page)
    prefix = bytearray(image[:prefix_end])
    struct.pack_into('<I', prefix, 16, len(packed))
    cmdline = header['cmdline'].rstrip(b'\0').replace(b'panic=15', b'panic=0')
    prefix[64:576] = cmdline.ljust(512, b'\0')
    prefix[576:608] = boot['mkbootimg_id'](kernel, packed, dtb).ljust(32, b'\0')
    output = prefix + packed + bytes(-len(packed) % page) + image[dtb_start:]
    with args.output.open('xb') as stream:
        stream.write(output)
    print(hashlib.sha256(output).hexdigest())


if __name__ == '__main__':
    main()
