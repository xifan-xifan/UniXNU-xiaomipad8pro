#!/usr/bin/env python3
"""Seal a build-boot.py boot_a container with this host's own AVB key and publish it as a `signed` boot image.

  seal-boot-image.py [--record out/persistent-k273/boot-v5.json] [--out out/persistent]
                     [--props-from out/switch/boot-a-combined-20261126.img.avb.json] [--salt HEX]

pad-boot-image.py only pads: a unit without the signing host has an AVB-mismatched container and nothing sealed, and
maint-session.py takes that only with --allow-unsigned-boot. Signing has two halves, and this tool does both with a key
this host holds (out/keys/piano-boot-v4-local.pem since 2026-11-25): avbtool add_hash_footer over the container, then a
boot-v*.json record whose `signed` ledger names the image, so steps boot-a and ram-boot-a accept it without the flag.

The seal is SELF-CONSISTENT, NOT TRUSTED: this host's key is not the key of this tablet's stock boot_a (whose AVB public
key sha1 is de5be2a57e0cfbb83ec5c9523806eb6b7a72f809 on OS3.0.309) and not the key of the maintainer's installed Ubuntu
set (f5151930dc69791637b2a48505cb175f5f78a522, held by their signing host). vbmeta is never touched, so ABL sees an
AVB-mismatched boot_a either way; what a footer changes is that ABL's verifier gets a structurally valid vbmeta instead of
nothing, which is the difference between the orange-state path and dropping to fastboot. The record says all of this, and
so does every line this tool prints.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import common  # noqa: E402

PARTITION = common.BOOT_PARTITION_BYTES
STOCK_KEY_SHA1 = 'de5be2a57e0cfbb83ec5c9523806eb6b7a72f809'   # this tablet's own OS3.0.309 boot_a, read by avbtool
UPSTREAM_KEY_SHA1 = 'f5151930dc69791637b2a48505cb175f5f78a522'  # the maintainer's key, not held here


def run(argv):
    p = subprocess.run(argv, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit(f'{argv[1] if len(argv) > 1 else argv[0]} failed ({p.returncode}): '
                         f'{(p.stdout + p.stderr).strip().splitlines()[-1] if (p.stdout + p.stderr).strip() else "no output"}')
    return (p.stdout or '') + (p.stderr or '')


def info_field(text, label):
    m = re.search(rf'^{re.escape(label)}\s+(.*)$', text, re.M)
    return m.group(1).strip() if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--record', type=Path, default=Path('out/persistent-k273/boot-v5.json'),
                    help='the build-boot.py record (boot-vN.json) whose container is to be sealed')
    ap.add_argument('--out', type=Path, default=Path('out/persistent'), help='where the sealed image and its record go')
    ap.add_argument('--key', type=Path, default=Path('out/keys/piano-boot-v4-local.pem'), help='the AVB private key on this host')
    ap.add_argument('--pubkey', type=Path, help='public half used for verify_image (default <key>.pub.pem, else .pem -> .pub.pem)')
    ap.add_argument('--avbtool', type=Path, default=Path('out/avbtool.py'), help='the avbtool to seal with')
    ap.add_argument('--props-from', type=Path, help='a pack-selector .avb.json: rollback index, props and partition size of the stock boot')
    ap.add_argument('--salt', help='64 hex digits; without it avbtool draws its own salt')
    ap.add_argument('--force', action='store_true', help='overwrite an existing sealed image and record')
    a = ap.parse_args()

    for path in (a.record, a.key, a.avbtool):
        if not path.is_file():
            raise SystemExit(f'{path} is missing')
    pub = a.pubkey or a.key.with_suffix('.pub.pem')
    if not pub.is_file():
        raise SystemExit(f'{pub} is missing: verify_image needs the public half next to the key')

    record = json.loads(a.record.read_text())
    images = record.get('images') or {}
    names = [n for n in images if n.endswith('.img') and 'persistent' in n] or list(images)
    if len(names) != 1:
        raise SystemExit(f'{a.record}: cannot tell which of {list(images)} is the boot_a image')
    base = names[0]
    container = a.record.parent / base
    data = container.read_bytes()
    recorded = (images[base] or {}).get('sha256') or hashlib.sha256(data).hexdigest()
    if hashlib.sha256(data).hexdigest() != recorded:
        raise SystemExit(f'{container.name} is not the image {a.record.name} recorded ({recorded[:16]})')
    if len(data) >= PARTITION:
        raise SystemExit(f'{container.name} is {len(data)} bytes, not less than the {PARTITION}-byte partition')

    if a.props_from:
        avb = json.loads(a.props_from.read_text())['avb']
        if avb['partition_name'] != 'boot' or int(avb['partition_size']) != PARTITION or int(avb['rollback_index']) <= 0:
            raise SystemExit(f'{a.props_from} is not a boot-partition props file')
        rollback = int(avb['rollback_index'])
        props = dict(avb['props'])
        props_source = f'{a.props_from.name} (the stock boot_a of this unit)'
    else:
        profile = json.loads((Path(os.environ.get('PIANO_V110') or '.') / 'release/boot/boot-profile.json').read_text())['boot']
        rollback = int(profile['rollback_index'])
        props = dict(profile['avb_properties'])
        props_source = 'boot-profile.json of the v1.1.0 release'
    if not props:
        raise SystemExit('no com.android.build.boot.* properties to seal with')

    out_name = base.removesuffix('.img') + '-sealed.img'
    a.out.mkdir(parents=True, exist_ok=True)
    image = a.out / out_name
    if image.exists() and not a.force:
        raise SystemExit(f'{image} exists (--force to rewrite it)')

    # avbtool verify_image finds the partition's image as boot.img next to it, so one scratch directory per seal.
    with tempfile.TemporaryDirectory(prefix='piano-seal-') as work:
        boot = Path(work) / 'boot.img'
        boot.write_bytes(data)
        argv = [sys.executable, str(a.avbtool), 'add_hash_footer', '--image', str(boot), '--partition_name', 'boot',
                '--partition_size', str(PARTITION), '--algorithm', 'SHA256_RSA4096', '--key', str(a.key),
                '--rollback_index', str(rollback)]
        if a.salt:
            if not re.fullmatch(r'[0-9a-fA-F]{64}', a.salt):
                raise SystemExit('--salt wants 64 hex digits')
            argv += ['--salt', a.salt.lower()]
        for k, v in sorted(props.items()):
            argv += ['--prop', f'{k}:{v}']
        run(argv)
        verified = run([sys.executable, str(a.avbtool), 'verify_image', '--image', str(boot), '--key', str(pub)])
        info = run([sys.executable, str(a.avbtool), 'info_image', '--image', str(boot)])
        sealed = boot.read_bytes()

    if len(sealed) != PARTITION or not sealed.startswith(data):
        raise SystemExit('the sealed image is not the container padded to the partition size')
    if 'Successfully verified' not in verified:
        raise SystemExit(f'verify_image did not pass: {verified.strip().splitlines()[-1:]}')
    sealed_sha = hashlib.sha256(sealed).hexdigest()
    key_sha1 = info_field(info, 'Public key (sha1):')
    image.write_bytes(sealed)

    out_record = a.out / (a.record.stem + '-sealed.json')
    out_record.write_text(json.dumps({
        'status': record.get('status', 'OFFLINE_BUILD_NOT_DEVICE_TESTED'),
        'series': record.get('series'),
        'fd': record.get('fd') or {},
        'dtb_sha256': record.get('dtb_sha256'),
        'kernel_field': record.get('kernel_field'),
        'base_record': str(a.record),
        'base_record_sha256': hashlib.sha256(a.record.read_bytes()).hexdigest(),
        'images': {out_name: {'bytes': PARTITION, 'sha256': sealed_sha}},
        'signed': {out_name: {
            'bytes': PARTITION, 'sha256': sealed_sha,
            'base': {'image': base, 'bytes': len(data), 'sha256': recorded},
            'avb': {
                'tool': f'{a.avbtool.name} {info_field(info, "Release String:") or "?"}',
                'algorithm': info_field(info, 'Algorithm:'),
                'rollback_index': rollback,
                'props': props, 'props_source': props_source,
                'salt': a.salt.lower() if a.salt else 'drawn by avbtool',
                'key': str(a.key),
                'key_sha1': key_sha1,
                'key_note': f'self-consistent seal by this host, not the stock key (stock {STOCK_KEY_SHA1}) and not '
                            f'upstream'"'"'s (upstream {UPSTREAM_KEY_SHA1}); vbmeta is unchanged, so ABL sees an '
                            f'AVB-mismatched boot_a either way and the footer is what makes the vbmeta structurally valid',
            },
        }},
    }, indent=2) + '\n', encoding='utf-8')

    print(f'{container.name}  {len(data)} bytes  sha256 {recorded}')
    print(f'{image.name}  {PARTITION} bytes  sha256 {sealed_sha}')
    print(f'seal: {info_field(info, "Algorithm:")} rollback {rollback} key sha1 {key_sha1}, salt {a.salt.lower() if a.salt else "avbtool"}')
    print(f'props from {props_source}: ' + ', '.join(sorted(props)))
    print(f'{out_record.name}: the image is under the signed ledger; fd bound '
          f'{common.boot_fd_bound(json.loads(out_record.read_text())) >> 20} MiB')
    print('untrusted key: ABL still reports the orange state for boot_a, and vbmeta was not touched')


if __name__ == '__main__':
    main()
