#!/usr/bin/env python3
"""Publish a boot_a image for a unit that does not hold the signing key (WRITE-PLAN.md, "Slot A boot triplet write").

  pad-boot-image.py [--record out/persistent-k273/boot-v5.json] [--out out/persistent] [--name NAME]

build-boot.py builds the v4/v5 container (a few MiB, no AVB footer); tooling/persist/sign.sh signs it into an exactly
BOOT_PARTITION_BYTES image on the host that holds this tablet's AVB private key, and build-boot.py --verify records that
image under the record's `signed` ledger - the only image steps boot-a and ram-boot-a will otherwise take. The signing
host is not ours (G7), so that ledger stays empty and the persistent boot stops there. Signing has two halves: pad the
container to the partition size, and seal it. This tool does the first and admits the second: it writes the container
zero-padded to exactly the partition size and a boot-v*.json-shaped record whose `padded` block names the image, its
SHA-256, the container it was padded from, and the base record's fd block so common.boot_fd_bound answers for it and the
pair rule still works. Nothing is recorded under `signed` and nothing is signed: maint-session.py accepts a `padded`
image only when told so with --allow-unsigned-boot, says so on every run, and runs the same size, hash, write and
read-back checks over it. The reason that is allowed at all is evidence rather than convenience: this unlocked ABL booted
an AVB-mismatched boot_a on 2026-11-13, and every RAM boot of this line since has gone through ABL with an unsigned
container.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

PARTITION = common.BOOT_PARTITION_BYTES


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--record', type=Path, default=Path('out/persistent-k273/boot-v5.json'),
                    help='the build-boot.py record (boot-vN.json) whose image is to be padded')
    ap.add_argument('--out', type=Path, default=Path('out/persistent'), help='where the padded image and its record go')
    ap.add_argument('--name', help='output base name (default <record image>-padded)')
    ap.add_argument('--force', action='store_true', help='overwrite an existing padded image and record')
    a = ap.parse_args()

    record = json.loads(a.record.read_text())
    images = record.get('images') or {}
    if not images:
        raise SystemExit(f'{a.record}: no images block (a build-boot.py record is expected)')
    names = [n for n in images if n.endswith('.img')]
    if a.name:
        base = next((n for n in names if n.startswith(a.name)), None)
    else:
        base = next((n for n in names if 'persistent' in n), None)
        if base is None and len(names) == 1:
            base = names[0]
    if not base:
        raise SystemExit(f'{a.record}: cannot tell which of {names} is the boot_a image (--name)')
    source = a.record.parent / base
    data = source.read_bytes()
    recorded = (images[base] or {}).get('sha256') or hashlib.sha256(data).hexdigest()
    if hashlib.sha256(data).hexdigest() != recorded:
        raise SystemExit(f'{source.name} is not the image {a.record.name} recorded ({recorded[:16]})')
    if len(data) > PARTITION:
        raise SystemExit(f'{source.name} is {len(data)} bytes, more than the {PARTITION}-byte partition')
    if len(data) == PARTITION:
        raise SystemExit(f'{source.name} is already partition-sized: nothing to pad')

    out_name = base.removesuffix('.img') + '-padded.img'
    a.out.mkdir(parents=True, exist_ok=True)
    image = a.out / out_name
    if image.exists() and not a.force:
        raise SystemExit(f'{image} exists (--force to rewrite it)')
    image.write_bytes(data + bytes(PARTITION - len(data)))

    back = image.read_bytes()
    assert len(back) == PARTITION, 'not partition-sized'
    assert back.startswith(data), 'does not start with the container byte for byte'
    assert back[len(data):] == bytes(PARTITION - len(data)), 'the padding is not zeros'
    padded_sha = hashlib.sha256(back).hexdigest()

    out_record = a.out / (a.record.stem + '-padded.json')
    out_record.write_text(json.dumps({
        'status': record.get('status', 'OFFLINE_BUILD_NOT_DEVICE_TESTED'),
        'series': record.get('series'),
        'fd': record.get('fd') or {},
        'dtb_sha256': record.get('dtb_sha256'),
        'kernel_field': record.get('kernel_field'),
        'base_record': str(a.record), 'base_record_sha256': sha(a.record),
        'images': {out_name: {'bytes': PARTITION, 'sha256': padded_sha}},
        'padded': {out_name: {
            'bytes': PARTITION, 'sha256': padded_sha, 'unsigned': True,
            'base': {'image': base, 'bytes': len(data), 'sha256': recorded},
            'padding': f'zeros to {PARTITION} bytes',
            'avb': 'none: this unit does not hold the boot_a signing key (G7); WRITE-PLAN.md, Slot A boot triplet write',
        }},
        'unsigned_evidence': 'an AVB-mismatched boot_a booted on this device family on 2026-11-13, and every RAM boot of '
                             'this line since has passed through ABL with an unsigned container',
    }, indent=2) + '\n', encoding='utf-8')

    print(f'{source.name}  {len(data)} bytes  sha256 {recorded}')
    print(f'{image.name}  {PARTITION} bytes  sha256 {padded_sha}')
    print(f"{out_record.name}: padded (unsigned), fd bound {common.boot_fd_bound(json.loads(out_record.read_text())) >> 20} MiB")
    print('not signed: maint-session.py takes this only with --allow-unsigned-boot, and says so when it does')


if __name__ == '__main__':
    main()
