#!/usr/bin/env python3
"""Check a locally rebuilt 27.3 root image (tooling/k273/make-root-image.sh: OUT/root.img, root.json, manifest/) against
tooling/repro/pins.json. The root image is never shipped: every user rebuilds it from the pinned IPSW. Its bytes are
those of the two windows plus the SHNGROOT manifest, so:

  - the OS window is Apple's decrypted System volume container, byte for byte: its sha256 IS pinned (40d8282e...);
  - the SystemOS window is not byte-reproducible (pins.json root.systemos.why): its own record (systemos.img.json) names its
    sha256 and volume UUID, and its file-level manifest must equal the pin (tooling/repro/volume-manifest.py --compare, as root;
    run separately because it needs sudo, or give --manifest-json with that run's output);
  - the manifest at block 0 is recomputed here from the two windows' names, offsets, sizes and sha256s and must be identical;
  - the layout (offsets, sizes, total) must be the pinned one, root.json must name the same windows and UUIDs.

  verify-root.py [ROOT_DIR] [--pins PINS.json] [--quick] [--manifest-json SYSTEMOS-MANIFEST.json]
     ROOT_DIR default out/inputs/k273/root. --quick skips hashing the 20.9 GB image's two window regions (structure only).
Exit 0 with PASS, 1 with the first FAIL.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

D = Path(__file__).resolve().parents[2]
MIB = 1 << 20


def fail(message):
    print(f'FAIL: {message}')
    sys.exit(1)


def ok(message):
    print(f'ok    {message}', flush=True)


def region_sha256(path, offset, size):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        f.seek(offset)
        left = size
        while left:
            chunk = f.read(min(16 << 20, left))
            if not chunk:
                break
            h.update(chunk)
            left -= len(chunk)
    return h.hexdigest()


def manifest_bytes(windows):
    """The SHNGROOT header tooling/native/make-root-manifest.py writes for WINDOWS [{name, offset, size, sha256}]."""
    header = bytearray(4096)
    header[0:8] = b'SHNGROOT'
    total = windows[-1]['offset'] + (windows[-1]['size'] + MIB - 1) // MIB * MIB
    struct.pack_into('<IIQ', header, 8, 1, len(windows), total)
    for i, w in enumerate(windows):
        struct.pack_into('<32sQQII32s40s', header, 24 + 128 * i, w['name'].encode(), w['offset'], w['size'], 512, 1,
                         bytes.fromhex(w['sha256']), bytes(40))
    header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()
    return bytes(header), total


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('root', nargs='?', type=Path, default=D / 'out/inputs/k273/root')
    p.add_argument('--pins', type=Path, default=D / 'tooling/repro/pins.json')
    p.add_argument('--quick', action='store_true')
    p.add_argument('--manifest-json', type=Path)
    a = p.parse_args(argv)
    pins = json.loads(a.pins.read_text())['root']
    r = a.root
    for name in ('root.img', 'root.json', 'systemos.img.json', 'manifest/manifest.bin'):
        if not (r / name).is_file():
            fail(f'no {r / name} (tooling/k273/make-systemos.sh, then make-root-image.sh)')
    record = json.loads((r / 'root.json').read_text())
    sysos = json.loads((r / 'systemos.img.json').read_text())
    osw, sw = pins['os_window'], pins['systemos']
    if sysos.get('size') != sw['size']:
        fail(f'systemos.img.json size {sysos.get("size")}, the pin {sw["size"]} (make-systemos.sh SIZE_MIB must stay 6144)')
    windows = [{'name': 'OS', 'offset': osw['offset'], 'size': osw['size'], 'sha256': osw['sha256']},
               {'name': 'SystemOS', 'offset': sw['offset'], 'size': sw['size'], 'sha256': sysos['sha256']}]
    expect, total = manifest_bytes(windows)
    if total != pins['image']['size']:
        fail(f'internal: the pinned layout gives {total} bytes, the pin {pins["image"]["size"]}')
    got = [{k: w[k] for k in ('name', 'offset', 'size', 'sha256')} for w in record.get('windows', [])]
    if got != windows:
        fail(f'root.json windows {got} are not the pinned layout with this SystemOS ({windows})')
    ok('root.json: the pinned OS window and this build\'s SystemOS window, pinned offsets and sizes')
    if record.get('os_volume_uuid') != osw['volume_uuid']:
        fail(f'root.json os_volume_uuid {record.get("os_volume_uuid")}, Apple\'s is {osw["volume_uuid"]}')
    if record.get('systemos_volume_uuid') != sysos.get('volume_uuid'):
        fail(f'root.json systemos_volume_uuid {record.get("systemos_volume_uuid")} differs from systemos.img.json\'s '
             f'{sysos.get("volume_uuid")}')
    ok(f'volume UUIDs: OS {osw["volume_uuid"]} (Apple\'s), SystemOS {sysos["volume_uuid"]} (this build\'s own)')
    image = r / 'root.img'
    if image.stat().st_size != pins['image']['size']:
        fail(f'root.img is {image.stat().st_size} bytes, the pin {pins["image"]["size"]}')
    if (r / 'manifest/manifest.bin').read_bytes() != expect:
        fail('manifest/manifest.bin differs from the manifest recomputed from the windows')
    with open(image, 'rb') as f:
        if f.read(4096) != expect:
            fail('root.img block 0 is not the recomputed SHNGROOT manifest')
    if hashlib.sha256(expect).hexdigest() != record.get('manifest_sha256'):
        fail('root.json manifest_sha256 is not the recomputed manifest\'s')
    ok(f'manifest at block 0 recomputed and equal (sha256 {hashlib.sha256(expect).hexdigest()[:16]}...)')
    if a.manifest_json:
        m = json.loads(a.manifest_json.read_text())
        if m.get('digest') != sw['file_manifest']['digest']:
            fail(f'the SystemOS file manifest {m.get("digest")} is not the pin {sw["file_manifest"]["digest"]}')
        ok('SystemOS file-level manifest equals the pin')
    else:
        print('note  SystemOS file-level manifest not checked here: sudo python3 tooling/repro/volume-manifest.py --image '
              f'{r / "systemos.img"} --compare {a.pins}')
    if a.quick:
        print('PASS (quick: structure only; the window regions were not hashed)')
        return 0
    for w in windows:
        digest = region_sha256(image, w['offset'], w['size'])
        if digest != w['sha256']:
            fail(f'root.img window {w["name"]} at {w["offset"]:#x}: sha256 {digest}, expected {w["sha256"]}')
        ok(f'root.img window {w["name"]}: sha256 {digest[:16]}...')
    whole = record.get('image_sha256')
    print(f'PASS: root image {whole} (this build\'s own; the reference build was {pins["image"]["reference_build"]["sha256"][:16]}...)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
