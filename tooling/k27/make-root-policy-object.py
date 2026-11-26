#!/usr/bin/env python3
"""Turn a VerifiedRoot policy directory into the object the kernel collection links as the root policies.

  make-root-policy-object.py POLICY_DIR --out OUT [--tree TREE] [--compiler CC] [--name NAME ...]

POLICY_DIR holds one directory per root window, each made by the VerifiedRoot kit's tools/manifest.py (through the UniXNU
scripts/make-root-policies.py wrapper): manifest.header.bin (VR_HEADER_BYTES + VR_SIGNATURE_BYTES, i.e. the signed envelope
without the block table, which travels as the NAME.vroot object of the root image), trusted-key.bin (the 395-byte owner
anchor) and verification.json. Every window must carry the same anchor: NativeUfs keeps one key for the set.

The output is an arm64e relocatable defining the three symbols package/NativeUfs/NativeUfs.cpp reads under
NATIVE_VERIFIED_BLOCK (verified_root_key, verified_root_policies, verified_root_policy_count), so the windows are only
published after vr_open authenticates their signed header and every served block is checked against its table.
`--name` selects the windows and their order (default: every directory of POLICY_DIR, sorted); a window whose signed
capacity matches no root window is refused by NativeUfs at runtime, so name them the same as the manifest's windows.
"""
import argparse, hashlib, json, os, struct, subprocess, sys, tempfile
from pathlib import Path

HEADER_BYTES, SIGNATURE_BYTES, KEY_BYTES, BLOCK_BYTES = 160, 384, 395, 4096
KEY_MAGIC = b'VROOTKEY'
HEADER_MAGIC = b'VROOT01\0'


def check_window(window_dir, key):
    header = (window_dir / 'manifest.header.bin').read_bytes()
    anchor = (window_dir / 'trusted-key.bin').read_bytes()
    record = json.loads((window_dir / 'verification.json').read_text())
    if anchor != key:
        raise SystemExit(f'{window_dir.name}: its trusted-key.bin is not the anchor of the set')
    if len(header) != HEADER_BYTES + SIGNATURE_BYTES:
        raise SystemExit(f'{window_dir.name}: manifest.header.bin is {len(header)} bytes, '
                         f'{HEADER_BYTES + SIGNATURE_BYTES} is the header-only envelope')
    if header[:8] != HEADER_MAGIC:
        raise SystemExit(f'{window_dir.name}: the header does not start with {HEADER_MAGIC!r}')
    image_bytes = struct.unpack_from('<Q', header, 24)[0]
    block_count = struct.unpack_from('<Q', header, 32)[0]
    image_sha256 = header[56:88].hex()
    if record['image_sha256'] != image_sha256:
        raise SystemExit(f'{window_dir.name}: the header signs {image_sha256}, verification.json says {record["image_sha256"]}')
    if int(record['blocks_verified']) != block_count:
        raise SystemExit(f'{window_dir.name}: the header signs {block_count} blocks, verification.json says {record["blocks_verified"]}')
    if block_count * BLOCK_BYTES < image_bytes or (block_count - 1) * BLOCK_BYTES >= image_bytes:
        raise SystemExit(f'{window_dir.name}: {block_count} blocks cannot cover {image_bytes} bytes')
    if not record.get('real_c_signature_and_block_verification'):
        raise SystemExit(f'{window_dir.name}: verification.json does not record a real signature and block verification')
    return {'name': window_dir.name, 'header': header, 'image_bytes': image_bytes, 'blocks': block_count,
            'image_sha256': image_sha256, 'volume_uuid': record['volume_uuid']}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('policy_dir', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--tree', type=Path, help='the k27 tree whose recipe names the compiler and target (default tooling/k27/k27_tree.py)')
    p.add_argument('--compiler')
    p.add_argument('--obj', default='k27-native-obj')
    p.add_argument('--name', action='append', default=[], metavar='WINDOW')
    a = p.parse_args(argv)
    if not a.policy_dir.is_dir():
        raise SystemExit(f'{a.policy_dir}: no such policy directory')
    names = a.name or sorted(d.name for d in a.policy_dir.iterdir() if (d / 'manifest.header.bin').is_file())
    if not names:
        raise SystemExit(f'{a.policy_dir}: no window directory with manifest.header.bin')
    key = (a.policy_dir / names[0] / 'trusted-key.bin').read_bytes()
    if len(key) != KEY_BYTES or not key.startswith(KEY_MAGIC):
        raise SystemExit(f'{names[0]}: trusted-key.bin is not a {KEY_BYTES}-byte {KEY_MAGIC.decode()} anchor')
    windows = [check_window(a.policy_dir / name, key) for name in names]
    blob_dir = a.out
    blob_dir.mkdir(parents=True, exist_ok=True)
    for w in windows:
        (blob_dir / f'{w["name"]}.header-only.bin').write_bytes(w['header'])
    assembly = blob_dir / 'root-policies.S'
    lines = ['// Generated from a VerifiedRoot policy directory; the block tables stay in the root image (NAME.vroot).',
             '.section __DATA_CONST,__const', '.balign 8', '.globl _verified_root_key', '_verified_root_key:',
             f'.incbin "{(blob_dir / "trusted-key.bin").resolve()}"', '.balign 8',
             '.globl _verified_root_policies, _verified_root_policy_count', '_verified_root_policies:']
    for w in windows:
        path = (blob_dir / f'{w["name"]}.header-only.bin').resolve()
        (blob_dir / f'{w["name"]}.header-only.bin').write_bytes(w['header'])
        lines.append(f'.quad _verified_root_{w["name"]}_manifest, {HEADER_BYTES + SIGNATURE_BYTES}')
        lines.append(f'_verified_root_{w["name"]}_manifest:')
        lines.append(f'.incbin "{path}"')
        lines.append('.balign 8')
    lines.append('_verified_root_policy_count:')
    lines.append(f'.quad {len(windows)}')
    (blob_dir / 'trusted-key.bin').write_bytes(key)
    assembly.write_text('\n'.join(lines) + '\n')
    compiler = a.compiler
    target = None
    if a.tree or compiler is None:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'k27'))
        import k27_tree  # noqa: E402
        tree = a.tree or k27_tree.k27_tree()
        recipe = tree / 'out/x12377/build/xnu' / a.obj / 'DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json'
        record = (json.JSONDecoder().raw_decode(recipe.read_text())[0])
        compiler = compiler or record['arguments'][0]
        target = next((a[len('--target='):] for a in record['arguments'] if a.startswith('--target=')), None)
    out = blob_dir / 'root-policies.o'
    command = [compiler] + ([f'--target={target}'] if target else []) + ['-c', str(assembly), '-o', str(out)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)
    for w in windows:
        print(f'{w["name"]}: {w["blocks"]} blocks over {w["image_bytes"]} bytes, image {w["image_sha256"][:16]}, '
              f'volume {w["volume_uuid"]}')
    print(f'{out}: {os.path.getsize(out)} bytes, anchor sha256 {hashlib.sha256(key).hexdigest()[:16]}, '
          f'{len(windows)} policies (manifestBytes {HEADER_BYTES + SIGNATURE_BYTES}: header-only)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))