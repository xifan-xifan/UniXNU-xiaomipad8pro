#!/usr/bin/env python3
"""Host-only tests of the T-20 install (the System and SystemOS volumes into the Data container).
The ESPs and tablet are simulated. The install flow uses the local pool package.

1. tooling/k273/build-pool-esp.py's ADT patch (with_pool_uuids): only the two UUID spans change; its refusals.
2. build-pool-esp.py on a made-up installed 27.3 ESP (test-k273-only.py's fixtures, an ADT with /chosen/root-matching and
   /chosen/owner-systemos-uuid): an independent read of both images (every base file byte for byte, the pool payload = the base
   entry's, the ADT = the base's but for the two spans, the menu = the base's + the pool entry as the default); refusals (a state with
   one copy unverified, without fstab, undone, preflight on another SystemOS; a placeholder under out/persistent; a base that is not
   its record's).
3. With --flow, tooling/pool/pool-install.sh, wired to the pool package (out/pool/install: install-functions.sh and install-gate.sh,
   pool-host.sh), with POOL_FAKE=1 replacing only the device seams (ssh, sudo, go_abl, the maintenance session's transport is the
   real maint-session.py on test-persist.py's fake tablet, the disk boot, live_boot, the battery, the desktop gates) and
   tooling/pool/mock-tablet.sh as the running system: the lock held -> exit 1; an inventory gate FAIL -> exit 1 before any device
   step; a device refusal -> exit 1, nothing left; a gate FAIL -> rollback to the base entry (exit 2, the base ESP back, / on the old
   windows, the copies deleted); the pool entry not booting -> rollback (exit 2); then the install (exit 0, the pool ESP installed
   and published to the fixture main ledger, / on both copies, every gate PASS).
   --real-base RECORD also checks a placeholder build from a real installed ESP (no file of out/persistent is written).

  test-pool-session.py [--keep DIR] [--real-base OUT/esp-....json]
"""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import common  # noqa: E402

D = HERE.parents[1]
PY = sys.executable


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


tk = load('test_k273_only', HERE / 'test-k273-only.py')
tp = tk.tp
bp = load('build_pool_esp', D / 'tooling/k273/build-pool-esp.py')
ba = bp.boot_args
B = common.LBA
OS_OLD, SYS_OLD = bp.OS_OLD, bp.SYSTEMOS_OLD
NEW = {'os': '5E6F7A8B-1111-2222-3333-444455556666', 'systemos': '9A0B0C0D-1111-2222-3333-444455556666'}   # mock-tablet.sh's
failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what, flush=True)
    if not ok:
        failures.append(what)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def pool_adt():
    chosen = tk.adt_node([('name', b'chosen\0'), ('darwinboot-memory-size', struct.pack('<Q', tk.WINDOW13)),
                          ('root-matching', bp.ROOT_MATCHING.format(OS_OLD).encode() + b'\0'),
                          ('owner-systemos-uuid', SYS_OLD.encode() + b'\0')], [tk.adt_node([('name', b'memory-map\0')])])
    return tk.adt_node([('name', b'device-tree\0')], [chosen])


def refuses(fn, *args):
    try:
        fn(*args)
    except SystemExit as e:
        return str(e)
    return None


def verify_pool(base_img, pool_img, uuids, entry='macos-27.3', pool_entry='macos-27.3-pool'):
    """An independent read of BASE and POOL: the problems found (empty = the pool ESP is the base's + the pool entry, nothing else)."""
    problems = []
    fb, fp = common.fat32_read(base_img), common.fat32_read(pool_img)
    b, p = fb['files'], fp['files']
    root = 'EFI/UniXNU/'
    toml = root + 'unixnu.toml'
    cb, cp = common.toml_loads(b[toml].decode()), common.toml_loads(p[toml].decode())
    e = next(x for x in cb['entry'] if x['id'] == entry)
    kc, pk = root + e['path'], root + 'Payloads/k273p/kernel.macho'
    if set(p) != set(b) | {pk, pk + '.adt', pk + '.trustcache'}:
        problems.append(f'file set: + {sorted(set(p) - set(b))}, - {sorted(set(b) - set(p))}')
    problems += [f'{n} differs' for n in b if n != toml and p.get(n) != b[n]]
    if p.get(pk) != b[kc] or p.get(pk + '.trustcache') != b[kc + '.trustcache']:
        problems.append('the pool collection or trust cache is not the base entry\'s')
    a0, a1 = b[kc + '.adt'], p.get(pk + '.adt', b'')
    if len(a0) != len(a1):
        problems.append('the ADT size changed')
    else:
        spans = []
        for old, new in ((OS_OLD, uuids['os']), (SYS_OLD, uuids['systemos'])):
            at = a0.find(old.encode())
            spans.append((at, at + 36))
            if a1[at:at + 36] != new.encode():
                problems.append(f'the ADT holds {a1[at:at + 36]!r} where {old} was, not {new}')
        diff = [i for i in range(len(a0)) if a0[i] != a1[i]]
        if any(not any(lo <= i < hi for lo, hi in spans) for i in diff):
            problems.append('the ADT differs outside the two UUID spans')
        ch = {q[0]: q[2] for q in ba.chosen(ba.load(a1))[0]}
        if ch.get('root-matching') != bp.ROOT_MATCHING.format(uuids['os']).encode() + b'\0' or \
                ch.get('owner-systemos-uuid') != uuids['systemos'].encode() + b'\0':
            problems.append('the pool ADT\'s /chosen values are not the new UUIDs')
    if cp['boot']['default'] != pool_entry or cp['entry'][1:] != cb['entry'] or cp['entry'][0]['id'] != pool_entry:
        problems.append('the menu is not the base\'s + the pool entry as the default')
    pe = cp['entry'][0]
    if {k: pe[k] for k in ('type', 'initrd', 'args')} != {k: e[k] for k in ('type', 'initrd', 'args')} or pe['path'] != 'Payloads/k273p/kernel.macho':
        problems.append('the pool entry is not the base entry with its own payload path')
    if {k: v for k, v in cp['boot'].items() if k != 'default'} != {k: v for k, v in cb['boot'].items() if k != 'default'}:
        problems.append('the [boot] table changed beyond the default')
    for k in ('sector_bytes', 'sectors_per_cluster', 'total_sectors', 'hidden_sectors', 'label', 'serial'):
        if fb[k] != fp[k]:
            problems.append(f'FAT {k}')
    return problems


def write_state(path, **over):
    s = {'container': 'disk3', 'root_systemos_at_preflight': SYS_OLD, 'fstab': 'yes'}
    for v in ('os', 'systemos'):
        s.update({f'{v}_preflight': 'ok', f'{v}_extracted': 'yes', f'{v}_verified': 'yes', f'{v}_volume_uuid': NEW[v]})
    s.update(over)
    path.write_text(''.join(f'{k}={v}\n' for k, v in s.items()))
    return path


def builder(*args):
    r = subprocess.run([PY, str(D / 'tooling/k273/build-pool-esp.py'), *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def test_adt_patch():
    print('== with_pool_uuids')
    a = pool_adt()
    out, info = bp.with_pool_uuids(a, NEW)
    diff = [i for i in range(len(a)) if a[i] != out[i]]
    spans = [(s['offset'], s['offset'] + s['length']) for s in info['spans']]
    check(len(out) == len(a) and diff and all(any(lo <= i < hi for lo, hi in spans) for i in diff),
          f'only the two UUID spans change ({len(diff)} bytes)')
    check(refuses(bp.with_pool_uuids, a, dict(NEW, os=OS_OLD)) is not None, 'refused: the new System UUID = the old one')
    check(refuses(bp.with_pool_uuids, a, dict(NEW, systemos=NEW['os'])) is not None, 'refused: the two new UUIDs the same')
    check(refuses(bp.with_pool_uuids, a, dict(NEW, os=NEW['os'].lower())) is not None, 'refused: a lower-case UUID')
    two = tk.adt_node([('name', b'device-tree\0')], [tk.adt_node([('name', b'chosen\0'), ('owner-systemos-uuid', SYS_OLD.encode() + b'\0'),
                       ('owner-systemos-uuid', SYS_OLD.encode() + b'\0'), ('root-matching', bp.ROOT_MATCHING.format(OS_OLD).encode() + b'\0')])])
    check('properties, not one' in (refuses(bp.with_pool_uuids, two, NEW) or ''), 'refused: two owner-systemos-uuid properties')
    other = tk.adt_node([('name', b'device-tree\0')], [tk.adt_node([('name', b'chosen\0'), ('owner-systemos-uuid', SYS_OLD.encode() + b'\0'),
                         ('root-matching', b'<dict><key>UUID</key><string>' + OS_OLD.encode() + b'</string></dict>\0')])])
    check('not the expected value' in (refuses(bp.with_pool_uuids, other, NEW) or ''), 'refused: a root-matching of another shape')
    none = tk.adt_node([('name', b'device-tree\0')], [tk.adt_node([('name', b'chosen\0'), ('owner-systemos-uuid', SYS_OLD.encode() + b'\0')])])
    check('0 /chosen/root-matching' in (refuses(bp.with_pool_uuids, none, NEW) or ''), 'refused: no root-matching')


def make_base(tmp, *, long_layout=False):
    out = tmp / 'out'
    out.mkdir()
    line = tk.line_273(long=not long_layout)
    if long_layout:
        line += ' data-store-concurrent=2 piano-long-boot-args=1 piano-gpu=2 audit-pad='
        kept, _ = ba.fit(line.split(), 'macos', force=True)
        line += 'a' * (ba.LONG_LINE_MAX - len(' '.join(kept).encode()))
    _, inputs, _ = tk.make_dual(out, 'esp-dual', line)
    inputs['adt273'] = pool_adt()
    if long_layout:
        inputs['kc273'] += ba.KC_MARKER
    dual, inputs, seed273 = tk.make_dual(out, 'esp-dual', line, inputs)
    rc, log = tk.build(out, 'esp-dual', *(['--split'] if long_layout else []))
    check(rc == 0, 'build-k273-esp.py: the made-up installed 27.3 ESP (esp-k273-only)')
    return out, dual, seed273


def test_builder(tmp, out):
    print('== build-pool-esp.py on a made-up installed ESP')
    st = write_state(tmp / 'state.env')
    rc, log = builder('--base', str(out / 'esp-k273-only.json'), '--state', str(st), '--out', str(out))
    rec = json.loads((out / 'esp-k273-only-pool.json').read_text()) if rc == 0 else {}
    check(rc == 0 and rec.get('default') == 'macos-27.3-pool' and rec.get('pool', {}).get('os_uuid_new') == NEW['os'], f'built: {log.strip()[-160:]}')
    probs = verify_pool(out / 'esp-k273-only.img', out / 'esp-k273-only-pool.img', NEW) if rc == 0 else ['not built']
    check(not probs, f'independent read: the base + the pool entry, nothing else {probs[:3]}')
    check(rec.get('sha256') == common.sha(out / 'esp-k273-only-pool.img'), 'the record names the image')
    for what, over in (('the systemos copy not verified', {'systemos_verified': 'no'}), ('no fstab step', {'fstab': 'no'}),
                       ('undone', {'undone': 'yes'}), ('preflight on another SystemOS', {'root_systemos_at_preflight': NEW['systemos']})):
        bad = write_state(tmp / 'state-bad.env', **over)
        rc, log = builder('--base', str(out / 'esp-k273-only.json'), '--state', str(bad), '--out', str(tmp / 'refused'), '--name', 'x')
        check(rc != 0 and not (tmp / 'refused' / 'x.img').exists(), f'refused: {what}')
    rc, log = builder('--base', str(out / 'esp-k273-only.json'), '--placeholder', NEW['os'], NEW['systemos'], '--out', str(D / 'out/persistent'))
    check(rc != 0 and 'never goes under out/persistent' in log, 'refused: a placeholder under out/persistent')
    rc, log = builder('--base', str(out / 'esp-k273-only.json'), '--placeholder', NEW['os'], NEW['systemos'],
                      '--out', str(tmp), '--name', '../escaped-TEST')
    check(rc != 0 and 'single file stem' in log, 'refused: a placeholder name that escapes its output directory')
    rc, log = builder('--base', str(out / 'esp-k273-only.json'), '--state', str(st), '--out', str(out), '--name', 'esp-k273-only')
    check(rc != 0 and 'overwrite the rollback base' in log, 'refused: the output would replace the rollback base')
    placeholder = tmp / 'placeholder'
    rc, log = builder('--base', str(out / 'esp-k273-only.json'), '--placeholder', NEW['os'], NEW['systemos'], '--out', str(placeholder))
    check(rc == 0, 'a placeholder can be built into a separate host output directory')
    if rc == 0:
        env = dict(os.environ, PIANO_PERSIST_OUT=str(placeholder), PIANO_LOCALNET_PREFLIGHT='off')
        image = 'esp-k273-only-pool-TEST.img'
        for step in ('esp-final', 'esp-delta'):
            result = subprocess.run([PY, str(HERE / 'maint-session.py'), '--dry-run', '--session', str(tmp / 'placeholder-session'),
                                     '--esp', image, '--esp-final', image, step], env=env, capture_output=True, text=True)
            check(result.returncode == 3 and 'a placeholder pool ESP cannot be installed' in result.stdout,
                  f'{step} refuses a placeholder in a custom PIANO_PERSIST_OUT')
    bad = json.loads((out / 'esp-k273-only.json').read_text())
    bad['sha256'] = '0' * 64
    (tmp / 'badbase').mkdir()
    common.write_json(tmp / 'badbase/esp-k273-only.json', bad)
    os.link(out / 'esp-k273-only.img', tmp / 'badbase/esp-k273-only.img')
    rc, log = builder('--base', str(tmp / 'badbase/esp-k273-only.json'), '--state', str(st), '--out', str(tmp / 'refused'), '--name', 'y')
    check(rc != 0 and 'not the image its record' in log, 'refused: a base image that is not its record\'s')


def test_long_pool(tmp):
    print('== pool ESP from a long-args daily base')
    tmp.mkdir()
    out, _, _ = make_base(tmp, long_layout=True)
    base = json.loads((out / 'esp-k273-only.json').read_text())
    state = write_state(tmp / 'state.env')
    rc, log = builder('--base', str(out / 'esp-k273-only.json'), '--state', str(state), '--out', str(out))
    check(rc == 0, f'pool build with the 1008-byte opt-in loader line: {log.strip()[-160:]}')
    if rc != 0:
        return
    pool = json.loads((out / 'esp-k273-only-pool.json').read_text())
    line = pool['cmdlines']['macos-27.3-pool']
    check(line == base['cmdlines']['macos-27.3'] and len(line.encode()) == ba.LONG_LINE_MAX and
          'data-store-concurrent=2' in line.split(), 'the pool line preserves the opt-in layout and mounted-store concurrency switch')
    check(pool['boot_args_split'] == base['boot_args_split'] and 'piano-gpu=2' in pool['boot_args_split']['tail'].split() and
          all(pool[k] == base[k] for k in ('seed', 'store', 'data_store', 'firmware')),
          'the pool record preserves the ADT tail, bound Data store and firmware pins')
    probs = verify_pool(out / base['image'], out / pool['image'], NEW)
    check(not probs, f'long-layout read-back keeps the base and ADT tail byte for byte outside the two UUIDs {probs[:3]}')


def test_publish_ledger(tmp):
    print('== publish_ledger verifies the destination before updating installed.json')
    source, target = tmp / 'source', tmp / 'target'
    source.mkdir(parents=True)
    target.mkdir()
    image, record = 'esp-pool.img', 'esp-pool.json'
    payload = b'verified ESP fixture'
    digest = sha(payload)
    ledger = json.dumps({'image': image, 'sha256': digest})
    (source / image).write_bytes(payload)
    (source / record).write_text(ledger)
    (source / 'installed.json').write_text(ledger)
    installed = target / 'installed.json'
    old = json.dumps({'image': 'esp-base.img', 'sha256': '0' * 64})
    installed.write_text(old)

    def publish():
        env = dict(os.environ, PIANO_PERSIST_OUT=str(source), POOL_MAIN_LEDGER=str(target))
        return subprocess.run(['bash', '-uo', 'pipefail', '-c', '''
source "$1"
cache_sha() { shasum -a 256 "$1" | awk '{print $1}'; }
publish_ledger "$2" "$3"
''', 'test', str(D / 'tooling/pool/pool-host.sh'), image, digest], env=env, capture_output=True).returncode

    (target / image).write_bytes(b'wrong image')
    check(publish() != 0 and installed.read_text() == old, 'a destination image with the wrong SHA leaves the ledger unchanged')
    (target / image).write_bytes(payload)
    (target / record).write_text('{}')
    check(publish() != 0 and installed.read_text() == old, 'a different destination record leaves the ledger unchanged')
    (target / record).write_text(ledger)
    (source / 'installed.json').write_text(old)
    check(publish() != 0 and installed.read_text() == old, 'a stale source ledger leaves the destination ledger unchanged')
    (source / 'installed.json').write_text(ledger)
    check(publish() == 0 and installed.read_text() == ledger, 'verified image and record publish the matching ledger')


def test_gate_args(tmp):
    spec, profile = tmp / 'base-gates.spec', tmp / 'profile.args'
    profile.write_text('piano-wifi=6\n')
    env = dict(os.environ, POOL_BASE_GATE_SPEC=str(spec), POOL_PROFILE_ARGS=str(profile))
    command = ['bash', '-uo', 'pipefail', '-c', 'source "$1"; pool_gate_args',
               'test', str(D / 'tooling/pool/pool-host.sh')]
    check(subprocess.run(command, env=env, capture_output=True).returncode != 0, 'a missing base gate spec refuses')
    spec.write_text('NEED_ARGS="data-persist=2 piano-wifi=5"\n')
    result = subprocess.run(command, env=env, capture_output=True, text=True)
    check(result.returncode == 0 and set(result.stdout.split()) == {'data-persist=2', 'piano-wifi=6', 'piano-touch=2'},
          'the supplied files retain the base gate args and apply profile overrides')


def small_volume(tmp, mock, vol, old, disk, tool):
    """A tiny volume's archive folder as make-volume-archive.sh makes one, and its old window's raw bytes in the mock."""
    src, a = tmp / f'src-{vol}', tmp / f'arch-{vol}'
    (src / 'usr/lib').mkdir(parents=True)
    a.mkdir()
    for i in (1, 2, 3):
        (src / f'usr/lib/lib{i}.dylib').write_bytes(os.urandom(5000 * i))
    (src / 'usr/lib/Localizable.strings').write_text('s')
    (src / 'usr/lib/Localizable.stringsdict').write_text('d')
    (a / 'expected.manifest').write_text(subprocess.run([str(tool), str(src)], capture_output=True, text=True, check=True).stdout)
    subprocess.run(['aa', 'archive', '-d', str(src), '-o', str(a / 'volume.aar'), '-include-field', 'attr,xat,acl,sh2'], check=True)
    raw = os.urandom(1 << 20)
    (mock / f'raw-{disk}').write_bytes(raw)
    (a / 'archive.env').write_text(f'archive_sha256={common.sha(a / "volume.aar")}\nmanifest_file_sha256={common.sha(a / "expected.manifest")}\n'
                                   f'source_sha256={sha(raw)}\nsource_volume_uuid={old}\nsource_personality=APFS\nfamilies=\n')
    return a


SEAMS = {
    'rssh': '#!/bin/bash\nexport PATH="$MOCKD/bin:$PATH"\nexec /bin/bash -c "$1"\n',
    'rsudo': '#!/bin/bash\nexport PATH="$MOCKD/bin:$PATH" SUDO_UID=$(id -u)\nexec /bin/bash -c "$1" < /dev/null\n',
    'go-abl': '#!/bin/sh\nexit 0\n',
    'gui-gates': '#!/bin/sh\nexit ${FAKE_GUI_RC:-0}\n',
    'live-boot': '#!/bin/sh\nexit ${FAKE_LIVE_RC:-0}\n',
    'battery': '#!/bin/sh\necho "macOS pmset -g batt: 80 % (minimum 30 %): ok"\n',
}


def inventory(path, os_sha, sys_sha, **over):
    """A complete inventory folder as INVENTORY-READS.sh writes one (the files pool-inventory-check.sh reads)."""
    path.mkdir(parents=True, exist_ok=True)
    data_uuid = 'DA7A0000-0000-4000-8000-000000000001'
    files = {'RESULT.txt': 'T-20 inventory (read only)\nUSED os x\nINVENTORY DONE 2026-11-23 20:00:00: x\n',
             'window-sha-os.txt': f'store=disk4\n{os_sha}  -\nseconds=60\nRC=0\n',
             'window-sha-systemos.txt': f'store=disk5\n{sys_sha}  -\nseconds=25\nRC=0\n',
             'csr.txt': 'System Integrity Protection status: disabled.\nRC=0\n',
             'info-data.txt': f'   Volume UUID:               {data_uuid}\n'
                              '   Container Free Space:      190.0 GB (190000000000 Bytes)\nRC=0\n',
             'apfs-list.txt': '+-- Container disk3\n    APFS Container Reference:     disk3\n    +-> Volume disk3s1\n'
                              f'        Volume UUID:               {data_uuid}\n',
             'adt-chosen.txt': '"owner-systemos-uuid" = <"07AD53B3-7565-4004-B574-F45FAC3753FB">\n', 'preboot.txt': 'x\n'}
    files.update(over)
    for n, t in files.items():
        (path / n).write_text(t)
    return path


DISK_BOOT = r'''#!/usr/bin/env python3
"""The fake disk boot: the default entry of the ESP on the fake tablet decides which volumes / runs on (its ADT's root-matching and
owner-systemos-uuid); a volume the mock does not have = no root (exit 1, as the boot guard's reset into ABL)."""
import os, re, sys, tempfile
sys.path.insert(0, {persist!r})
import common
B = common.LBA
mock = os.environ['MOCKD']
with open(os.path.join(os.environ['FAKE_ROOT'], 'lun0.img'), 'rb') as f:
    f.seek(common.ESP_FIRST * B)
    region = f.read(common.ESP_BLOCKS * B)
with tempfile.NamedTemporaryFile() as t:
    t.write(region); t.flush()
    files = common.fat32_read(t.name)['files']
cfg = common.toml_loads(files['EFI/UniXNU/unixnu.toml'].decode())
entry = next(e for e in cfg['entry'] if e['id'] == cfg['boot']['default'])
adt = files['EFI/UniXNU/' + entry['path'] + '.adt']
root = re.search(rb'<key>UUID</key><string>([0-9A-F-]{{36}})</string>', adt).group(1).decode()
sysos = re.search(rb'([0-9A-F-]{{36}})\0', adt[adt.find(b'owner-systemos-uuid'):]).group(1).decode()
vols = {{'84581E99-85BE-4957-AC53-2947E887B3E4': 'disk4s1', '07AD53B3-7565-4004-B574-F45FAC3753FB': 'disk5s1'}}
for n in ('PianoOS', 'PianoSystemOS'):
    p = os.path.join(mock, f'vol-{{n}}.uuid')
    if os.path.exists(p):
        vols[open(p).read().strip()] = open(os.path.join(mock, f'vol-{{n}}.disk')).read().strip()
if root not in vols or sysos not in vols or (os.environ.get('FAKE_BOOT_FAIL') == 'pool' and entry['id'].endswith('-pool')):
    print(f'fake boot of {{entry["id"]}}: no root {{root}} / SystemOS {{sysos}}: the boot guard resets into ABL'); sys.exit(1)
open(os.path.join(mock, 'root.uuid'), 'w').write(sysos + '\n')
open(os.path.join(mock, 'mounts.txt'), 'w').write(f'/dev/{{vols[root]}} on / (apfs, local, read-only, journaled)\n'
                                                f'/dev/{{vols[sysos]}} on / (apfs, local, read-only, union)\n')
with open(sys.argv[1], 'w') as c:
    for d in ('/private', '/Applications', '/Library', '/Users', '/Volumes', '/cores', '/opt', '/usr/local', '/System/Library/Caches',
              '/System/Volumes/Hardware'):
        c.write(f'k27: firmlink {{d}} -> /System/Volumes/Data{{d}}: 0\n')
    c.write('NativeRamBlock: Data store state 2\n')
print(f'fake boot of {{entry["id"]}}: / on {{root}} + SystemOS {{sysos}}')
'''


def test_session(tmp, out, dual, seed273, pkg):
    print('== pool-install.sh (POOL_FAKE=1) on the fake tablet, the real maint-session.py for the ESP')
    root = tk.build_tablet(tmp, out, dual, seed273)
    lun0, lun4 = root / 'lun0.img', root / 'lun4.img'
    nas, home = tmp / 'nas', tmp / 'home'
    nas.mkdir()
    home.mkdir()
    (tmp / 'fake-shell.py').write_text(f'import sys; sys.path.insert(0, {str(HERE)!r}); import importlib.util\n'
                                       f'spec = importlib.util.spec_from_file_location("t", {str(HERE / "test-persist.py")!r})\n'
                                       'm = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)\n'
                                       'sys.stdout.write(m.fake_shell(sys.argv[1]))\n')
    maint = HERE / 'maint-session.py'
    (tmp / 'maint-wrap.py').write_text(f'import os, runpy, sys\nsys.path.insert(0, {str(HERE)!r})\nimport common\n'
                                       'common.VENDOR_BOOT_A_SHA256 = os.environ["FAKE_VENDOR_BOOT_A_SHA256"]\n'
                                       'common.INIT_BOOT_A_SHA256 = os.environ["FAKE_INIT_BOOT_A_SHA256"]\n'
                                       f'sys.argv[0] = {str(maint)!r}\nrunpy.run_path({str(maint)!r}, run_name="__main__")\n')
    env = dict(os.environ, FAKE_ROOT=str(root), PIANO_PERSIST_OUT=str(out), PIANO_MAINT_HOST='127.0.0.1', PIANO_BACKUP_DIR=str(nas),
               PIANO_SERIAL='test', PIANO_MAINT_SHELL=str(tmp / 'fake-shell.py'), PYTHONDONTWRITEBYTECODE='1', HOME=str(home),
               PIANO_JOB_POLL='0.2',
               FAKE_VENDOR_BOOT_A_SHA256=tp.sha_region(lun4, tp.LUN4['vendor_boot_a'][1], tp.LUN4['vendor_boot_a'][2]),
               FAKE_INIT_BOOT_A_SHA256=tp.sha_region(lun4, tp.LUN4['init_boot_a'][1], tp.LUN4['init_boot_a'][2]))
    for k in [k for k in env if k.startswith('PIANO_') and k not in ('PIANO_PERSIST_OUT', 'PIANO_MAINT_HOST', 'PIANO_BACKUP_DIR',
                                                                       'PIANO_SERIAL', 'PIANO_MAINT_SHELL', 'PIANO_JOB_POLL')]:
        env.pop(k)
    session = tmp / 'session'
    base_ms = [PY, str(tmp / 'maint-wrap.py'), '--session', str(session), '--http-port', '18031', '--nc-port', '19031']

    def ms(*steps, expect=0):
        r = subprocess.run(base_ms + list(steps), capture_output=True, text=True, env=env, timeout=3600)
        if r.returncode != expect:
            print((r.stdout + r.stderr)[-3000:])
        check(r.returncode == expect, f'maint-session.py {" ".join(steps)} -> {r.returncode}')

    def region():
        return tp.sha_region(lun0, common.ESP_FIRST * B, common.ESP_BLOCKS * B)

    E = 'esp-k273-only.img'
    ms('map', 'backup', '--esp', E, '--esp-final', E, '--new-backup')
    ms('map', 'esp-final', 'check', '--esp', E, '--esp-final', E)
    base_sha = json.loads((out / 'esp-k273-only.json').read_text())['sha256']
    check(region() == base_sha and json.loads((out / 'installed.json').read_text())['image'] == E, 'the made-up 27.3 ESP installed (the base)')
    mock = tmp / 'mock'
    subprocess.run(['bash', str(D / 'tooling/pool/mock-tablet.sh'), str(mock)], check=True)
    a_os = small_volume(tmp, mock, 'os', OS_OLD, 'disk4', pkg / 'pool-manifest')
    a_sys = small_volume(tmp, mock, 'systemos', SYS_OLD, 'disk5', pkg / 'pool-manifest')
    seams = tmp / 'seams'
    seams.mkdir()
    for n, text in SEAMS.items():
        (seams / n).write_text(text)
    (seams / 'disk-boot').write_text(DISK_BOOT.format(persist=str(HERE)))
    (seams / 'ms').write_text('#!/bin/bash\nlog=$1; shift\nexec ' + ' '.join(f"'{x}'" for x in base_ms) + ' "$@" > "$log" 2>&1\n')
    for f in seams.iterdir():
        f.chmod(0o755)
    fstab = tmp / 'fstab'
    fstab.write_text('LABEL=Owner none hfs rw,noauto\n')
    fstab0 = fstab.read_bytes()
    renv = dict(env, POOL_FAKE='1', POOL_RSSH=str(seams / 'rssh'), POOL_RSUDO=str(seams / 'rsudo'), POOL_GO_ABL=str(seams / 'go-abl'),
                POOL_MS=str(seams / 'ms'), POOL_DISK_BOOT=str(seams / 'disk-boot'), POOL_GUI_GATES=str(seams / 'gui-gates'),
                POOL_ARCHIVE_OS=str(a_os), POOL_ARCHIVE_SYSTEMOS=str(a_sys), POOL_RUNS=str(tmp / 'rounds'), POOL_ACCEPT_UNRESTRICTED='1',
                MOCKD=str(mock), POOL_DISKUTIL=str(mock / 'bin/diskutil'), POOL_MOUNT=str(mock / 'bin/mount'),
                POOL_UMOUNT=str(mock / 'bin/umount'), POOL_CHFLAGS=str(mock / 'bin/chflags'), POOL_ID=str(mock / 'bin/id'),
                POOL_SWVERS=str(mock / 'bin/sw_vers'), POOL_FSTAB=str(fstab), POOL_RAWDEV_PREFIX=str(mock / 'raw-'),
                MOCK_ROOT_UUID=SYS_OLD, POOL_MS_BOOT='', POOL_MS_EXIT='')

    lock = tmp / 'tablet.lock'
    ledger_main = tmp / 'main-ledger'
    ledger_main.mkdir()
    for f in out.iterdir():
        if f.name in ('installed.json', E, 'esp-k273-only.json'):
            shutil.copyfile(f, ledger_main / f.name)
    renv.update(POOL_GO_ABL=str(seams / 'go-abl'), POOL_LIVE_BOOT=str(seams / 'live-boot'), POOL_BATTERY=str(seams / 'battery'),
                POOL_MAIN_LEDGER=str(ledger_main), POOL_LOCK=str(lock), POOL_PACKAGE=str(pkg), POOL_POLL='0.2')
    renv.pop('POOL_MS_BOOT', None)
    renv.pop('POOL_MS_EXIT', None)
    good_inv = inventory(tmp / 'inv-good', sha((mock / 'raw-disk4').read_bytes()), sha((mock / 'raw-disk5').read_bytes()))

    def capture(path, os_sha, sys_sha):
        """A windows/ folder as maint-session.py window-capture writes one (CAPTURE.json; T-20 v2)."""
        path.mkdir(parents=True, exist_ok=True)
        (path / 'CAPTURE.json').write_text(json.dumps({'windows': {'os': {'sha256': os_sha}, 'systemos': {'sha256': sys_sha}}}))
        return path
    good_cap = capture(tmp / 'cap-good', sha((mock / 'raw-disk4').read_bytes()), sha((mock / 'raw-disk5').read_bytes()))
    renv['POOL_LIVE_ROOT'] = str(tmp / 'src-systemos')   # the live `/` of the mock: SystemOS's names cover all of OS's here
    renv['POOL_PROFILE_ARGS'] = str(tmp / 'profile-daily.args')
    (tmp / 'profile-daily.args').write_text('piano-usb=2 piano-usb-net=1 piano-touch=2\n')
    renv['POOL_BASE_GATE_SPEC'] = str(tmp / 'base-gates.spec')
    (tmp / 'base-gates.spec').write_text('NEED_ARGS="data-persist=2"\n')
    renv['POOL_DATA_UUID'] = 'DA7A0000-0000-4000-8000-000000000001'

    def install(name, expect, inv=good_inv, cap=good_cap, **extra):
        (mock / 'root.uuid').unlink(missing_ok=True)
        (mock / 'mounts.txt').unlink(missing_ok=True)
        calls0 = (mock / 'calls').read_text() if (mock / 'calls').exists() else ''
        r = subprocess.run(['bash', str(D / 'tooling/pool/pool-install.sh'), name, '--inventory', str(inv), '--capture', str(cap)], capture_output=True, text=True,
                           env=dict(renv, POOL_REMOTE=str(tmp / f'remote-{name}'), **extra), timeout=3600)
        log = (tmp / 'rounds' / name / 'pool-install.log')
        text = log.read_text() if log.exists() else r.stdout + r.stderr
        if r.returncode != expect:
            print(text[-4000:], r.stderr[-2000:])
        check(r.returncode == expect, f'pool-install.sh {name} {extra or ""} -> {r.returncode} (want {expect})')
        calls = ((mock / 'calls').read_text() if (mock / 'calls').exists() else '')[len(calls0):]
        return text, calls

    def root_now():
        return (mock / 'root.uuid').read_text().strip() if (mock / 'root.uuid').exists() else SYS_OLD

    def no_copies():
        return not list(mock.glob('vol-*.uuid'))

    lock.mkdir()
    (lock / 'owner').write_text('another job\n')
    log, calls = install('pool-locked1', 1)
    check('lock' in log and not calls and region() == base_sha, 'the tablet lock held: refused, no device call')
    shutil.rmtree(lock)
    bad_cap = capture(tmp / 'cap-bad', '0' * 64, sha((mock / 'raw-disk5').read_bytes()))
    log, calls = install('pool-r1afail1', 1, cap=bad_cap)
    check('R1a: the os archive' in log and not calls and region() == base_sha and not lock.exists(),
          'R1a: an archive not made from the capture: exit 1 before any device step, the lock given back')
    bad_inv = inventory(tmp / 'inv-bad', '0' * 64, sha((mock / 'raw-disk5').read_bytes()))
    log, calls = install('pool-invrec1', 1, inv=bad_inv, MOCK_FREE='1000')
    check('RECORDED R1 os' in log and 'refused: device step preflight' in log,
          'the inventory\'s live raw window read is only recorded under T-20 v2 (the run goes on to the device preflight)')
    live = tmp / 'src-systemos/usr/lib/lib2.dylib'
    keep, st0 = live.read_bytes(), live.stat()
    live.write_bytes(keep[:10] + b'X' + keep[11:])
    log, calls = install('pool-r1bfail1', 1)
    check('R1b: the live old volumes are not the capture' in log and 'addVolume' not in calls and no_copies() and region() == base_sha,
          'R1b: a content change in the live SystemOS: refused before any volume is created')
    live.write_bytes(keep)
    os.utime(live, ns=(st0.st_atime_ns, st0.st_mtime_ns))
    low_inv = inventory(tmp / 'inv-low', sha((mock / 'raw-disk4').read_bytes()), sha((mock / 'raw-disk5').read_bytes()),
                        **{'info-data.txt': '   Container Free Space:      12.0 GB (12000000000 Bytes)\n'})
    log, calls = install('pool-invlow1', 1, inv=low_inv)
    check('R4' in log and 'below 30 GB' in log and not calls, 'an inventory gate FAIL (R4 Data free below 30 GB): exit 1, no device call')
    log, calls = install('pool-refused1', 1, MOCK_FREE='1000')
    check(region() == base_sha and 'refused: device step preflight' in log and no_copies(), 'a device refusal (no room): exit 1, nothing left')
    log, calls = install('pool-gatefail1', 2, FAKE_GUI_RC='1')
    check(region() == base_sha and json.loads((out / 'installed.json').read_text())['image'] == E and root_now() == SYS_OLD and
          'rolled back' in log, 'a gate FAIL: rolled back, the base ESP verified back, / on the old windows')
    check(no_copies() and fstab.read_bytes() == fstab0 and 'deleted by UUID' in log, 'the rollback deletes both copies by UUID, fstab as before')
    check(json.loads((ledger_main / 'installed.json').read_text())['image'] == E, 'the fixture main ledger names the base again')
    log, calls = install('pool-bootfail1', 2, FAKE_BOOT_FAIL='pool')
    check(region() == base_sha and 'FAIL  G1' in log and no_copies(), 'the pool entry not booting: G1 FAIL, rolled back, copies deleted')
    log, calls = install('pool-install1', 0)
    pool = json.loads((out / 'esp-k273-only-pool.json').read_text())
    check(region() == pool['sha256'] and json.loads((out / 'installed.json').read_text())['image'] == 'esp-k273-only-pool.img',
          'the install: the pool ESP on the tablet, installed.json follows')
    check(json.loads((ledger_main / 'installed.json').read_text())['image'] == 'esp-k273-only-pool.img' and
          (ledger_main / 'esp-k273-only-pool.img').is_file(), 'the pool ESP published to the (fixture) main ledger')
    check(root_now() == NEW['systemos'] and log.count('  PASS  ') == 9 and '  FAIL  ' not in log,
          'the pool entry: / on both copies, nine gates PASS (G1 G2 P1-P6, the desktop gates)')
    check('R1b' in log and 'EQUAL' in log and '"verdict": "EQUAL"' in (tmp / 'rounds/pool-install1/r1b.json').read_text(),
          'the install ran R1b on its own boot: EQUAL')
    check(not verify_pool(out / E, out / 'esp-k273-only-pool.img', NEW), 'the installed pool ESP = the base + the pool entry (independent read)')
    st = (tmp / 'rounds/pool-install1/state.env').read_text()
    check('os_verified=yes' in st and 'systemos_verified=yes' in st and 'fstab=yes' in st and fstab.read_text().count('ro,noauto') == 4,
          'the round\'s state: both copies verified, the fstab block in place')
    check(not (tmp / 'remote-pool-install1/pool/os/volume.aar').exists() and (tmp / 'remote-pool-install1/pool/os/expected.manifest').exists(),
          'after the install the pushed archives are removed from the tablet (the manifests kept)')
    check(not lock.exists(), 'the lock given back')


def test_real(real, tmp):
    print(f'== a placeholder pool ESP from the real {real.name}')
    rc, log = builder('--base', str(real), '--placeholder', NEW['os'], NEW['systemos'], '--out', str(tmp / 'real'))
    img = next((tmp / 'real').glob('*-pool-TEST.img'), None) if rc == 0 else None
    check(rc == 0 and img is not None, f'built: {log.strip()[-200:]}')
    base = json.loads(real.read_text())
    probs = verify_pool(real.with_name(base['image']), img, NEW) if img else ['not built']
    check(not probs, f'independent read of the real base and the placeholder: the base + the pool entry, nothing else {probs[:3]}')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--keep', type=Path)
    p.add_argument('--real-base', type=Path)
    p.add_argument('--flow', action='store_true', help='replay the install on loopback using a built pool package')
    a = p.parse_args()
    if a.flow:
        pkg = Path(os.environ.get('POOL_PACKAGE', D / 'out/pool/install'))
        if not all((pkg / name).is_file() for name in ('PACKAGE.sha256', 'PACKAGE.tsv', 'pool-manifest')):
            p.error('--flow needs a built pool package (POOL_PACKAGE or out/pool/install)')
    tmp = Path(tempfile.mkdtemp(prefix='piano-pool-test-')) if not a.keep else a.keep
    tmp.mkdir(parents=True, exist_ok=True)
    try:
        test_adt_patch()
        test_publish_ledger(tmp / 'ledger-test')
        test_gate_args(tmp)
        out, dual, seed273 = make_base(tmp)
        test_builder(tmp, out)
        test_long_pool(tmp / 'long-layout')
        if a.flow:
            test_session(tmp, out, dual, seed273, pkg)
        if a.real_base:
            test_real(a.real_base, tmp)
    finally:
        if not a.keep:
            subprocess.run(['chmod', '-R', 'u+w', str(tmp)], capture_output=True)
            shutil.rmtree(tmp, ignore_errors=True)
    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
