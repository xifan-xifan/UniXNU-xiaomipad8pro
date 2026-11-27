#!/usr/bin/env python3
"""The installed ESP plus one entry, macos-27.3-pool, the new default (T-20, the unified System and Data layout).
The pool entry boots the same kernel collection, trust cache, Data seed and command line as the base's 27.3 entry. Only its ADT
differs, in exactly two places: the System volume's UUID
inside /chosen/root-matching (the root XNU waits for) and /chosen/owner-systemos-uuid (the SystemOS union): the UUIDs of the two
copies tooling/pool/pool-device.sh made and verified in the owner's Data container, read from that run's state file, never typed in.
Every file of the base stays byte for byte (the base's macos-27.3 entry is the rollback: one menu choice, or the base ESP written
back), unixnu.toml gains the entry (first, the default) and nothing else.

  build-pool-esp.py --base OUT/esp-k273-v131y10.json --state WORK/state.env [--name esp-k273-v131y10-pool] [--out out/persistent]
  build-pool-esp.py --base ... --placeholder OS_UUID SYSTEMOS_UUID --out DIR   (a test artifact: refused under out/persistent, its
                                                                                record says TEST_PLACEHOLDER_NOT_FOR_INSTALL)

DarwinBoot loads PATH, PATH.adt and PATH.trustcache per entry, so the pool entry's payload is a directory of its own,
Payloads/k273p/: kernel.macho and kernel.macho.trustcache are copies of the base entry's (115 MB: the ESP has room), kernel.macho.adt
is the patched one; its initrd is the base entry's file itself. The image is read back by the FAT reader and by mtools.
"""
import argparse
import hashlib
import importlib.util
import json
import re
import sys
import tempfile
from pathlib import Path

D = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(D / 'tooling/persist'))
import common  # noqa: E402

spec = importlib.util.spec_from_file_location('boot_args', D / 'tooling/adt/boot_args.py')
boot_args = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_args)
spec = importlib.util.spec_from_file_location('build_esp', D / 'tooling/persist/build-esp.py')
build_esp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_esp)

ROOT = 'EFI/UniXNU/'
TOML = ROOT + 'unixnu.toml'
POOL_DIR = 'Payloads/k273p/'
OS_OLD = '84581E99-85BE-4957-AC53-2947E887B3E4'        # 27.3's System volume (configs/k273-adt.toml root_match_volume_uuid)
SYSTEMOS_OLD = '07AD53B3-7565-4004-B574-F45FAC3753FB'  # 27.3's SystemOS volume (out/inputs/k273/root/systemos.img.json)
UUID_RE = re.compile(r'[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}')
ROOT_MATCHING = ('<dict><key>IOProviderClass</key><string>IOMedia</string><key>IOPropertyMatch</key><dict><key>UUID</key><string>'
                 '{}</string></dict></dict>')
# /chosen property -> (its whole value with the old UUID, the volume): each must be exactly that, once
PROPS = {'root-matching': (ROOT_MATCHING.format(OS_OLD), 'os', OS_OLD),
         'owner-systemos-uuid': (SYSTEMOS_OLD, 'systemos', SYSTEMOS_OLD)}
NAMES = {'os': 'PianoOS', 'systemos': 'PianoSystemOS'}


def h(data):
    return hashlib.sha256(data).hexdigest()


def with_pool_uuids(adt, new):
    """ADT bytes with the System UUID in /chosen/root-matching and /chosen/owner-systemos-uuid replaced by NEW {'os': UUID,
    'systemos': UUID}. Each property must be exactly its expected value with the old UUID (once in the whole ADT); the result is
    the same size and differs only inside the two 36-byte UUID spans (checked byte by byte)."""
    for vol in ('os', 'systemos'):
        if not UUID_RE.fullmatch(new.get(vol, '')) or new[vol] in (OS_OLD, SYSTEMOS_OLD):
            raise SystemExit(f'{vol}: {new.get(vol)!r} is not an upper-case UUID other than the old volumes\'')
    if new['os'] == new['systemos']:
        raise SystemExit('the two new UUIDs are the same')
    root = boot_args.load(adt)
    chosen = boot_args.chosen(root)
    spans = []
    for prop, (value, vol, old) in PROPS.items():
        props = [p for p in chosen[0] if p[0] == prop]
        if len(props) != 1:
            raise SystemExit(f'the ADT has {len(props)} /chosen/{prop} properties, not one')
        p = props[0]
        if p[1] & 0x80000000:
            raise SystemExit(f'/chosen/{prop}: a placeholder property (DarwinBoot fills it)')
        if p[2] != value.encode() + b'\0':
            raise SystemExit(f'/chosen/{prop} is {p[2][:120]!r}, not the expected value with {old}')
        if adt.count(old.encode()) != 1:
            raise SystemExit(f'{old} is {adt.count(old.encode())} times in the ADT, not once')
        at = adt.find(value.encode() + b'\0') + value.index(old)
        spans.append((at, at + len(old), prop))
        p[2] = p[2].replace(old.encode(), new[vol].encode(), 1)
    out = boot_args.serialize(root)
    if len(out) != len(adt):
        raise SystemExit('the patched ADT changed size')
    diff = [i for i in range(len(adt)) if adt[i] != out[i]]
    if not diff or any(not any(lo <= i < hi for lo, hi, _ in spans) for i in diff):
        raise SystemExit(f'the patched ADT differs outside the two UUID spans (bytes {diff[:4]}...)')
    for lo, hi, prop in spans:
        if out[lo:hi].decode() not in (new['os'], new['systemos']):
            raise SystemExit(f'/chosen/{prop}: the span does not hold a new UUID')
    if boot_args.sets(out) != boot_args.sets(adt):
        raise SystemExit('the patched ADT\'s piano-boot-args tails changed')
    return out, {'spans': [{'property': prop, 'offset': lo, 'length': hi - lo} for lo, hi, prop in spans], 'bytes_changed': len(diff)}


def read_state(path):
    state = {}
    for line in Path(path).read_text().splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            state[k] = v
    if state.get('undone') == 'yes':
        raise SystemExit(f'{path}: the copies were undone')
    new = {}
    for vol in ('os', 'systemos'):
        if state.get(f'{vol}_verified') != 'yes' or state.get(f'{vol}_extracted') != 'yes' or state.get(f'{vol}_preflight') != 'ok':
            raise SystemExit(f'{path}: the {vol} copy has not passed preflight, extract and verify')
        new[vol] = state.get(f'{vol}_volume_uuid', '')
    if state.get('fstab') != 'yes':
        raise SystemExit(f'{path}: the fstab step has not run (the old windows would be automounted read-write in the pool entry)')
    if state.get('root_systemos_at_preflight') != SYSTEMOS_OLD:
        raise SystemExit(f'{path}: preflight did not run on the old SystemOS ({state.get("root_systemos_at_preflight")})')
    return state, new


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--base', type=Path, required=True, help='the installed ESP\'s build record (its image beside it)')
    p.add_argument('--entry', default='macos-27.3', help='the base entry the pool entry copies (and stays as the rollback)')
    p.add_argument('--pool-entry', default='macos-27.3-pool')
    p.add_argument('--state', type=Path, help='pool-device.sh\'s state.env of the verified copies')
    p.add_argument('--placeholder', nargs=2, metavar=('OS_UUID', 'SYSTEMOS_UUID'),
                   help='a test artifact only (no state): refused under out/persistent')
    p.add_argument('--name')
    p.add_argument('--out', type=Path, default=common.DEVICE / 'out/persistent')
    a = p.parse_args()
    if bool(a.state) == bool(a.placeholder):
        raise SystemExit('one of --state (the verified copies) or --placeholder (a test artifact)')
    if a.placeholder:
        if (common.DEVICE / 'out/persistent').resolve() in [a.out.resolve(), *a.out.resolve().parents]:
            raise SystemExit('a placeholder build never goes under out/persistent (maint-session.py would take it)')
        uuids, state, state_sha = {'os': a.placeholder[0].upper(), 'systemos': a.placeholder[1].upper()}, None, None
    else:
        state, uuids = read_state(a.state)
        state_sha = common.sha(a.state)

    base = json.loads(a.base.read_text())
    base_img = common.checked_esp_base(a.base, base)
    name = a.name or (Path(base['image']).stem + ('-pool' if state else '-pool-TEST'))
    if not state and not name.endswith('-TEST'):
        raise SystemExit('a placeholder build\'s name ends in -TEST')
    with common.stage_esp_output(a.out, name, base_img, a.base) as image:
        build(a, base, base_img, image, state, uuids, state_sha)


def build(a, base, base_img, image, state, uuids, state_sha):
    fat = common.fat32_read(base_img)
    if fat['problems']:
        raise SystemExit(f'{base_img}: the FAT reader finds {fat["problems"]}')
    files = fat['files']
    if set(files) != set(base['files']) or any(h(files[n]) != v for n, v in base['files'].items() if v != 'private'):
        raise SystemExit(f'{base_img}: its files differ from its record {a.base.name}')
    text = files[TOML].decode()
    if text != base['unixnu_toml']:
        raise SystemExit(f'{base_img}: unixnu.toml differs from its record')
    config = common.toml_loads(text)
    if config['boot']['default'] != a.entry:
        raise SystemExit(f'{base_img}: its default is {config["boot"]["default"]}, not {a.entry}')
    entry = next((e for e in config['entry'] if e['id'] == a.entry), None)
    if not entry or entry.get('type') != 'xnu':
        raise SystemExit(f'{base_img}: no xnu entry {a.entry}')
    if any(e['id'] == a.pool_entry for e in config['entry']):
        raise SystemExit(f'{base_img}: an entry {a.pool_entry} exists already')
    kc, adt, trust = ROOT + entry['path'], ROOT + entry['path'] + '.adt', ROOT + entry['path'] + '.trustcache'
    seed = ROOT + entry['initrd']
    for path in (kc, adt, trust, seed):
        if path not in files:
            raise SystemExit(f'{base_img}: {a.entry}\'s {path} is not on the ESP')
    pool_kc = ROOT + POOL_DIR + 'kernel.macho'
    if any(n.startswith(ROOT + POOL_DIR) for n in files):
        raise SystemExit(f'{base_img}: {POOL_DIR} exists already')
    new_adt, adt_diff = with_pool_uuids(files[adt], uuids)

    # the menu: the pool entry first (the default), then the base's text as it is
    pool = {'id': a.pool_entry, 'title': 'macOS 27.3 (System and Data in one pool)', 'type': 'xnu',
            'path': POOL_DIR + 'kernel.macho', 'initrd': entry['initrd'], 'args': entry['args']}
    block = '[[entry]]\n' + ''.join(f'{k} = "{v}"\n' for k, v in pool.items())
    default_line = f'default = "{a.entry}"\n'
    if text.count(default_line) != 1 or text.count('[[entry]]\n') < 1:
        raise SystemExit('the base unixnu.toml has not one default line or no entry')
    head, rest = text.split('[[entry]]\n', 1)
    new_text = (head.replace(default_line, f'default = "{a.pool_entry}"\n') +
                f'# T-20 (tooling/k273/build-pool-esp.py): {a.pool_entry} = {a.entry} with System {uuids["os"]} and SystemOS '
                f'{uuids["systemos"]} in the Data container; {a.entry} is the rollback\n' + block + '[[entry]]\n' + rest)
    parsed = common.toml_loads(new_text)
    if (parsed['boot']['default'] != a.pool_entry or parsed['entry'][0] != pool or parsed['entry'][1:] != config['entry'] or
            {k: v for k, v in parsed['boot'].items() if k != 'default'} != {k: v for k, v in config['boot'].items() if k != 'default'} or
            parsed.get('kits') != config.get('kits')):
        raise SystemExit('the new unixnu.toml is not the base\'s plus the pool entry as the default')
    line = pool['args']
    limit = boot_args.line_max(line.split())
    if len(line.encode()) > limit or any(c in line for c in ('"', '\\', '\0')):
        raise SystemExit(f'the pool entry\'s line does not fit the {limit}-byte loader layout')

    want_files = dict(files)
    want_files[TOML] = new_text.encode()
    want_files[pool_kc] = files[kc]
    want_files[pool_kc + '.adt'] = new_adt
    want_files[pool_kc + '.trustcache'] = files[trust]
    with tempfile.TemporaryDirectory(prefix='piano-pool-esp-') as scratch:
        scratch = Path(scratch)
        mt = scratch / 'base-mtools'
        mt.mkdir()
        build_esp.mtools('mcopy', '-i', str(base_img), '-s', '-n', '::/EFI', str(mt))
        mfiles = {str(f.relative_to(mt)): h(f.read_bytes()) for f in sorted(mt.rglob('*')) if f.is_file()}
        if mfiles != {n: h(b) for n, b in files.items()}:
            raise SystemExit(f'{base_img}: mtools and the FAT reader read different files')
        stage = scratch / 'stage'
        for path, data in want_files.items():
            (stage / path).parent.mkdir(parents=True, exist_ok=True)
            (stage / path).write_bytes(data)
        for path in [stage, *stage.rglob('*')]:
            build_esp.os.utime(path, (build_esp.EPOCH, build_esp.EPOCH))
        build_esp.mtools('mformat', '-C', '-F', '-S', '5', '-M', '4096', '-T', str(common.ESP_BLOCKS), '-h', '1', '-s', '1', '-c', '1',
                         '-H', str(common.ESP_FIRST), '-v', build_esp.LABEL, '-N', build_esp.SERIAL, '-i', str(image), '::')
        build_esp.mtools('mcopy', '-i', str(image), '-s', '-Q', '-m', str(stage / 'EFI'), '::/')
        minfo = build_esp.mtools('minfo', '-i', str(image), '::').stdout
        back = scratch / 'back'
        back.mkdir()
        build_esp.mtools('mcopy', '-i', str(image), '-s', '-n', '::/EFI', str(back))
        mback = {str(f.relative_to(back)): h(f.read_bytes()) for f in sorted(back.rglob('*')) if f.is_file()}

    new = common.fat32_read(image)
    want = {n: h(b) for n, b in want_files.items()}
    got = {n: h(b) for n, b in new['files'].items()}
    if new['problems']:
        raise SystemExit(f'{image}: the FAT reader finds {new["problems"]}')
    problems = []
    if image.stat().st_size != common.ESP_BLOCKS * common.LBA:
        problems.append('image size')
    if new['sector_bytes'] != 4096 or new['sectors_per_cluster'] != 1 or new['total_sectors'] != common.ESP_BLOCKS:
        problems.append('geometry')
    if new['hidden_sectors'] != common.ESP_FIRST or new['label'] != build_esp.LABEL or new['serial'] != build_esp.SERIAL:
        problems.append('hidden sectors, label or serial')
    if got != want or mback != want:
        problems.append('read back differs from the stage')
    if 'sector size: 4096 bytes' not in minfo:
        problems.append('minfo sector size')
    for path, data in files.items():
        if path != TOML and new['files'].get(path) != data:
            problems.append(f'{path} is not the base\'s byte for byte')
    if new['files'].get(pool_kc) != files[kc] or new['files'].get(pool_kc + '.trustcache') != files[trust]:
        problems.append('the pool payload is not the base entry\'s')
    if problems:
        raise SystemExit(f'{image}: ' + '; '.join(problems))

    record = dict(base)
    record.update({
        'status': 'OFFLINE_BUILD_NOT_DEVICE_TESTED' if state else 'TEST_PLACEHOLDER_NOT_FOR_INSTALL',
        'kind': 'k273-pool', 'image': image.name, 'bytes': image.stat().st_size, 'sha256': common.sha(image),
        'about': f'{base["image"]} + the {a.pool_entry} entry (the default): {a.entry}\'s payload and line, the ADT with System '
                 f'{uuids["os"]} and SystemOS {uuids["systemos"]} (tooling/k273/build-pool-esp.py, T-20 plan A)',
        'base_esp': {'image': base['image'], 'sha256': base['sha256'], 'record': a.base.name, 'entry': a.entry},
        'default': a.pool_entry,
        'cmdlines': {a.pool_entry: pool['args'], **(base.get('cmdlines') or {})},
        'inputs': {role: dict(v, path=f'{image.name}:' + v['path'].split(':', 1)[1]) if isinstance(v, dict) and ':' in v.get('path', '')
                   else v for role, v in (base.get('inputs') or {}).items()},
        'files': {n: ('private' if base['files'].get(n) == 'private' else v) for n, v in want.items()},
        'unixnu_toml': new_text,
        'fat': {k: v for k, v in new.items() if k not in ('files', 'problems')},
        'pool': {'entry': a.pool_entry, 'rollback_entry': a.entry, 'payload_dir': POOL_DIR,
                 'os_uuid_old': OS_OLD, 'os_uuid_new': uuids['os'], 'systemos_uuid_old': SYSTEMOS_OLD, 'systemos_uuid_new': uuids['systemos'],
                 'volume_names': NAMES, 'state': str(a.state) if a.state else None, 'state_sha256': state_sha,
                 'copy_manifest_sha256': {v: state.get(f'{v}_copy_manifest_sha256') for v in NAMES} if state else None,
                 'adt_base_sha256': h(files[adt]), 'adt_pool_sha256': h(new_adt), 'adt_diff': adt_diff,
                 'placeholder': not state},
    })
    common.write_json(image.with_suffix('.json'), record)
    print(f'{image.name}: {record["bytes"]} bytes sha256 {record["sha256"]}; {a.pool_entry} (default) = {a.entry} with System '
          f'{uuids["os"]}, SystemOS {uuids["systemos"]} ({adt_diff["bytes_changed"]} ADT bytes in two UUID spans); every base file byte '
          'for byte'
          + ('' if state else '; TEST PLACEHOLDER, not for install'))


if __name__ == '__main__':
    main()
