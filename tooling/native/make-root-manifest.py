#!/usr/bin/env python3
"""Build the SHNGROOT root manifest (UniXNU userdata.py layout, read by NativeUfs findRootLayout) for raw disk images that
will be written into a partition at 1 MiB-aligned windows. Runs anywhere; hashing 31 GB takes a few minutes.
   make-root-manifest.py OUT_DIR NAME=IMAGE [NAME=IMAGE ...] [--object NAME=FILE ...]
Writes OUT_DIR/manifest.bin (4096 bytes) and OUT_DIR/layout.json {name, offset, size, sha256}; the writer then puts the
manifest at byte 0 of the partition and each window or object at its listed offset.
Objects (such as verified-root hash tables) use SHNGROOT v2 and are listed separately in layout.json."""
import argparse, hashlib, json, os, struct
MIB = 1 << 20
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('out')
parser.add_argument('windows', nargs='+')
parser.add_argument('--object', action='append', default=[], dest='objects')
args = parser.parse_args()
if len(args.windows) > 4 or len(args.objects) > 4:
    parser.error('at most four windows and four objects are supported')
out = args.out
entries, cursor = [], MIB
names = set()
for spec, is_object in [(s, False) for s in args.windows] + [(s, True) for s in args.objects]:
    name, sep, path = spec.partition('=')
    if not sep or not 1 <= len(name) <= 31 or any(not 32 <= ord(c) <= 126 for c in name) or name in names:
        parser.error(f'invalid or duplicate entry: {spec}')
    names.add(name)
    size = os.path.getsize(path)
    if not size or (not is_object and size % 512):
        parser.error(f'{spec}: windows must be nonempty and sector aligned, objects must be nonempty')
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(8 * MIB), b''):
            h.update(chunk)
    entries.append({'name': name, 'image': path, 'offset': cursor, 'size': size, 'sha256': h.hexdigest(), 'object': is_object})
    print(f'{name}: {size} bytes at {cursor:#x} sha256 {h.hexdigest()}', flush=True)
    cursor += (size + MIB - 1) // MIB * MIB
os.makedirs(out, exist_ok=True)
header = bytearray(4096)
header[0:8] = b'SHNGROOT'
struct.pack_into('<IIQ', header, 8, 2 if args.objects else 1, len(entries), cursor)
for i, e in enumerate(entries):
    struct.pack_into('<32sQQII32s40s', header, 24 + 128 * i, e['name'].encode(), e['offset'], e['size'], 4096 if e['object'] else 512, 3 if e['object'] else 1,
                     bytes.fromhex(e['sha256']), bytes(40))
header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()
open(os.path.join(out, 'manifest.bin'), 'wb').write(header)
windows, objects = [], []
for entry in entries:
    (objects if entry.pop('object') else windows).append(entry)
layout = {'image_bytes': cursor, 'windows': windows}
if objects:
    layout['objects'] = objects
with open(os.path.join(out, 'layout.json'), 'w') as f:
    json.dump(layout, f, indent=2)
print('image_bytes', cursor, 'manifest', hashlib.sha256(bytes(header)).hexdigest())
