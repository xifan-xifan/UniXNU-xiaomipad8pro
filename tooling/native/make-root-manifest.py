#!/usr/bin/env python3
"""Build the SHNGROOT root manifest (UniXNU userdata.py layout, read by NativeUfs findRootLayout) for raw disk images that
will be written into a partition at 1 MiB-aligned windows. Runs anywhere; hashing 31 GB takes a few minutes.
   make-root-manifest.py OUT_DIR NAME=IMAGE [NAME=IMAGE ...] [--object NAME=IMAGE ...]
Positional NAME=IMAGE entries are the windows the kernel publishes as block devices (512-byte blocks, flags 1, version 1).
--object NAME=IMAGE adds data the kernel reads itself, a VerifiedRoot hash table named WINDOW.vroot or firmware: 4096-byte
blocks and flags 3, which makes the manifest version 2. findRootLayout indexes the objects after the windows, so the
windows always go first whatever order they were named in, and at most four of each fit alongside the 4096-byte header.
Writes OUT_DIR/manifest.bin (4096 bytes) and OUT_DIR/layout.json {version, image_bytes, windows, objects, entries}; the
writer then puts the manifest at byte 0 of the partition and each image at its offset (dd ... bs=1M conv=notrunc)."""
import hashlib, json, os, struct, sys
MIB = 1 << 20


def collect(specs, objects):
    entries = []
    for spec in specs:
        name, sep, path = spec.partition('=')
        if not sep or not name or '/' in name:
            sys.exit(f'{spec}: expected NAME=IMAGE')
        if not os.path.isfile(path):
            sys.exit(f'{spec}: {path} is not a regular file')
        size = os.path.getsize(path)
        block, flags = (4096, 3) if objects else (512, 1)
        if size % block or len(name.encode()) > 31:
            sys.exit(f'{spec}: size must be a multiple of {block} and the name under 32 bytes')
        h = hashlib.sha256()
        with open(path, 'rb') as f:
            for chunk in iter(lambda: f.read(8 * MIB), b''):
                h.update(chunk)
        entries.append({'name': name, 'image': os.path.abspath(path), 'size': size, 'sha256': h.hexdigest(),
                        'block_size': block, 'flags': flags})
    return entries


argv = sys.argv[1:]
out = argv[0]
objects = []
i = 1
while i < len(argv):
    if argv[i] == '--object':
        if i + 1 >= len(argv):
            sys.exit('--object needs NAME=IMAGE')
        objects.append(argv[i + 1])
        del argv[i:i + 2]
        continue
    i += 1
windows = argv[1:]
os.makedirs(out, exist_ok=True)
entries = collect(windows, False) + collect(objects, True)
if len(entries) > 8:
    sys.exit(f'{len(entries)} entries: the 4096-byte header holds at most four windows and four objects')
if len([e for e in entries if e['flags'] == 1]) > 4 or len([e for e in entries if e['flags'] == 3]) > 4:
    sys.exit('at most four windows and four objects fit the manifest')
if len(set(e['name'] for e in entries)) != len(entries):
    sys.exit('two entries share a name')
version = 2 if any(e['flags'] == 3 for e in entries) else 1
cursor = MIB
for e in entries:
    e['offset'] = cursor
    cursor += (e['size'] + MIB - 1) // MIB * MIB
    print(f"{e['name']}: {e['size']} bytes at {e['offset']:#x} sha256 {e['sha256']}", flush=True)
header = bytearray(4096)
header[0:8] = b'SHNGROOT'
struct.pack_into('<IIQ', header, 8, version, len(entries), cursor)
for i, e in enumerate(entries):
    struct.pack_into('<32sQQII32s40s', header, 24 + 128 * i, e['name'].encode(), e['offset'], e['size'],
                     e['block_size'], e['flags'], bytes.fromhex(e['sha256']), bytes(40))
header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()
open(os.path.join(out, 'manifest.bin'), 'wb').write(header)
record = {'version': version, 'image_bytes': cursor,
          'windows': [e for e in entries if e['flags'] == 1],
          'objects': [e for e in entries if e['flags'] == 3],
          # The flat list, in manifest order: what the writers of make-root-image.sh read.
          'entries': entries}
json.dump(record, open(os.path.join(out, 'layout.json'), 'w'), indent=2)
print('version', version, 'image_bytes', cursor, 'manifest', hashlib.sha256(bytes(header)).hexdigest())