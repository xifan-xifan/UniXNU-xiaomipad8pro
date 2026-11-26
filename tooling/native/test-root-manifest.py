#!/usr/bin/env python3
"""tooling/native/make-root-manifest.py, checked against what package/NativeUfs/RootLayout.h accepts.

  test-root-manifest.py

The kernel splits the manifest entries on two fields (block size 512 with flags 1 is a window it publishes; 4096 with
flags 3 is an object it reads itself, version 2 only), reads the entry array only up to the header hash, and indexes the
objects after the windows. This makes the same bytes and reads them back with those rules."""
import hashlib, json, os, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
TOOL = ROOT / 'tooling/native/make-root-manifest.py'
MIB = 1 << 20
failures = []


def check(ok, what):
    print(('  ok   ' if ok else '  FAIL ') + what)
    if not ok:
        failures.append(what)


def write(path, size, seed):
    with open(path, 'wb') as f:
        f.truncate(size)
        f.seek(0)
        f.write(bytes([seed]) * 4096)


def entries_of(manifest):
    raw = manifest.read_bytes()
    version, count, image_bytes = struct.unpack_from('<IIQ', raw, 8)
    out = []
    for i in range(count):
        at = 24 + 128 * i
        name = raw[at:at + 31].split(b'\0')[0].decode()
        offset, size, block, flags = struct.unpack_from('<QQII', raw, at + 32)
        out.append({'name': name, 'offset': offset, 'size': size, 'block_size': block, 'flags': flags,
                    'sha256': raw[at + 56:at + 88].hex()})
    return version, count, image_bytes, out


def run(work, specs, objects=()):
    out = Path(work) / 'manifest'
    cmd = [sys.executable, str(TOOL), str(out)] + specs + sum([['--object', o] for o in objects], [])
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode:
        return None, done.stderr.strip()
    return out, done.stdout


work = tempfile.mkdtemp(prefix='root-manifest-test-')
os_ = Path(work) / 'os.img'
sysos = Path(work) / 'systemos.img'
table = Path(work) / 'SystemOS.vroot'
write(os_, 3 * MIB, 1)
write(sysos, 2 * MIB + 4096, 2)
write(table, 8192, 3)
partial = Path(work) / 'partial.img'
write(partial, 4608, 4)  # a whole 512-byte sector, but not a whole 4096-byte block

print('== a version 1 manifest (windows only) keeps its shape')
out, _ = run(work, [f'OS={os_}', f'SystemOS={sysos}'])
version, count, image_bytes, entries = entries_of(out / 'manifest.bin')
layout = json.loads((out / 'layout.json').read_text())
check(version == 1 and count == 2, f'version {version} with {count} entries')
check([e['name'] for e in entries] == ['OS', 'SystemOS'], 'the names, in order')
check(entries[0]['offset'] == MIB and entries[1]['offset'] == 4 * MIB, 'the windows are 1 MiB aligned and stacked')
check(image_bytes == 7 * MIB and layout['image_bytes'] == image_bytes, 'the image size rounds up to a whole MiB')
check(all(e['block_size'] == 512 and e['flags'] == 1 for e in entries), '512-byte blocks, flags 1')
check(all(e['sha256'] == hashlib.sha256(Path(f['image']).read_bytes()).hexdigest() for e, f in zip(entries, layout['entries'])),
      'each entry hashes to its own bytes')
check(hashlib.sha256((out / 'manifest.bin').read_bytes()[:4064]).hexdigest() ==
      (out / 'manifest.bin').read_bytes()[4064:4096].hex(), 'the header hash covers the first 4064 bytes')
check(layout['objects'] == [], 'no objects')

print('== a VerifiedRoot table joins as an object, which makes the manifest version 2')
out, _ = run(work, [f'OS={os_}', f'SystemOS={sysos}'], objects=[f'SystemOS.vroot={table}', f'OS.vroot={table}'])
version, count, image_bytes, entries = entries_of(out / 'manifest.bin')
layout = json.loads((out / 'layout.json').read_text())
check(version == 2 and count == 4, f'version {version} with {count} entries')
check([e['name'] for e in entries] == ['OS', 'SystemOS', 'SystemOS.vroot', 'OS.vroot'],
      'the windows come first: findRootLayout indexes objects at prior - windows')
check(all(e['block_size'] == 4096 and e['flags'] == 3 for e in entries[2:]), '4096-byte blocks, flags 3')
check(entries[3]['offset'] == entries[2]['offset'] + 8192 or entries[2]['offset'] + 8192 <= entries[3]['offset'],
      'the objects do not overlap')
check(image_bytes >= entries[3]['offset'] + entries[3]['size'], 'the declared image covers the last object')
check([e['name'] for e in layout['objects']] == ['SystemOS.vroot', 'OS.vroot'], 'layout.json splits them out')
check([e['name'] for e in layout['entries']] == [e['name'] for e in entries],
      'layout.json keeps the flat manifest-order list the writers read')

print('== refusals')
for label, specs, objects in [
        ('two entries cannot share a name', [f'OS={os_}'], [f'OS={table}']),
        ('an object whose size is not a whole block', [f'OS={os_}'], [f'bad={partial}']),
        ('a missing file', [f'OS={work}/nope.img'], [])]:
    got, err = run(work, specs, objects)
    check(got is None and err, f'{label}: {err.splitlines()[-1] if err else "no message"}')

print('\n' + ('all checks passed (%d failures)' % len(failures) if not failures else
      'FAILURES: ' + '; '.join(failures)))
sys.exit(1 if failures else 0)