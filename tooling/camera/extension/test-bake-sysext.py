#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Host contract checks with synthetic approval data; not tablet qualification.

With --capture DIR --package DIR, also compare the genuine approval against
the final signed package, field by field, and write the audit to --record.
"""
import argparse
import copy
import importlib.util
import json
import plistlib
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('bake', HERE / 'bake-sysext.py')
bake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bake)


def archive(value):
    objects = ['$null']
    objects.append({'$classes': ['NSDictionary', 'NSObject'], '$classname': 'NSDictionary'})

    def add(value):
        index = len(objects)
        objects.append(None)
        if isinstance(value, dict):
            objects[index] = {'$class': plistlib.UID(1), 'NS.keys': [add(k) for k in value],
                              'NS.objects': [add(v) for v in value.values()]}
        else:
            objects[index] = value
        return plistlib.UID(index)

    root = add(value)
    return plistlib.dumps({'$archiver': 'NSKeyedArchiver', '$version': 100000, '$top': {'root': root},
                           '$objects': objects}, fmt=plistlib.FMT_BINARY)


def fixture(root):
    capture, package = root / 'capture', root / 'package'
    capture.mkdir()
    bundle = package / bake.ORIGIN.removeprefix('/Applications/')
    (bundle / 'Contents/MacOS').mkdir(parents=True)
    info = {'CFBundleShortVersionString': '1.1', 'CFBundleVersion': '4',
            'CMIOExtension': {'CMIOExtensionMachServiceName': bake.TEAM + '.' + bake.EXT + '.service'}}
    (bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    (bundle / 'Contents/MacOS' / bake.EXT).write_bytes(b'synthetic final executable')
    old_hash = 'a' * 40
    old_id = '00000000-0000-4000-8000-000000000001'
    old_path = bake.ROOT + '/' + old_id + '/' + bake.EXTNAME
    db = {'version': 1, 'developerMode': False, 'extensionPolicies': [], 'bootUUID': 'old-test-boot', 'extensions': [{
        'uniqueID': old_id, 'teamID': bake.TEAM, 'identifier': bake.EXT,
        'bundleVersion': {'CFBundleShortVersionString': '1.1', 'CFBundleVersion': '3'},
        'categories': ['com.apple.system_extension.cmio'], 'originPath': bake.ORIGIN,
        'container': {'bundlePath': bake.APP}, 'stagedBundleURL': {'relative': 'file://' + old_path + '/'},
        'stagedCdhashes': {old_hash: {'cputype': 16777228, 'cpusubtype': 0}}, 'stagedCdhash': old_hash,
        'state': 'activated_enabled', 'references': [{'appRef': 'file:///.file/id=synthetic-old-volume/',
                'teamID': bake.TEAM, 'appIdentifier': bake.HOST}],
        'additionalLaunchdPlistEntries': archive(bake.expected_extras(info))}]}
    (capture / 'sysext-db.xml').write_bytes(plistlib.dumps(db))
    boot = '00000000-0000-4000-8000-000000000002'
    lines = [boot, 'CDHash=' + old_hash]
    paths = {bake.ROOT: 0o755, bake.ROOT + '/.staging': 0o700, bake.ROOT + '/db.plist': 0o644,
             bake.ROOT + '/' + old_id: 0o755, old_path: 0o755, old_path + '/Contents': 0o755,
             old_path + '/Contents/MacOS': 0o755, old_path + '/Contents/Info.plist': 0o644,
             old_path + '/Contents/MacOS/' + bake.EXT: 0o755}
    for path, mode in paths.items():
        lines.append(f'{path} | size=1 mode={mode:o} uid=0 gid=0 mtime=1')
    for path in ('Contents/Info.plist', 'Contents/MacOS/' + bake.EXT):
        lines.append('0' * 64 + '  ' + old_path + '/' + path)
    lines.append('staged tree capture complete')
    (capture / 'sysext-staged-tree.txt').write_text('\n'.join(lines))
    for name, text in [('sysext-changed-files.txt', 'post-approval changes capture complete'),
                       ('sysext-marker.txt', 'sysext marker ready'), ('sysext-before.txt', 'SystemExtensions')]:
        (capture / name).write_text((boot + '\n' if name != 'sysext-before.txt' else '') + text)
    record = {'executables': [{'identifier': bake.EXT, 'team': bake.TEAM, 'arch': 'arm64', 'cdhash': 'b' * 40,
                              'sha256': bake.digest((bundle / 'Contents/MacOS' / bake.EXT).read_bytes())},
                             {'identifier': bake.HOST, 'team': bake.TEAM, 'arch': 'arm64', 'cdhash': 'c' * 40}]}
    return capture, package, db, record


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture', type=Path)
    p.add_argument('--package', type=Path)
    p.add_argument('--record', type=Path)
    a = p.parse_args()
    if a.capture or a.package:
        if not (a.capture and a.package and a.record):
            p.error('--capture, --package and --record are required together')
        package_spec = importlib.util.spec_from_file_location('package', HERE / 'package-camera.py')
        package_module = importlib.util.module_from_spec(package_spec)
        package_spec.loader.exec_module(package_module)
        package_record = package_module.package_record(a.package)
        assert package_record == json.loads((a.package / 'cdhashes.json').read_text())
        prepared = bake.prepare(a.capture, a.package, package_record)
        a.record.write_text(json.dumps(prepared['record'], indent=2) + '\n')
        print('PASS genuine capture: every preserved field and every derived field checked')
        return
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        capture, package, db, record = fixture(root)
        prepared = bake.prepare(capture, package, record)
        output = plistlib.loads(prepared['tree'][bake.ROOT + '/db.plist'][0])
        e = output['extensions'][0]
        assert output['bootUUID'] == db['bootUUID']
        assert e['stagedCdhash'] == record['executables'][0]['cdhash']
        assert list(e['stagedCdhashes']) == [e['stagedCdhash']]
        assert e['references'][0]['appRef'] == 'file://' + bake.APP + '/'
        assert e['bundleVersion']['CFBundleVersion'] == '4'
        assert bake.prepare(capture, package, record)['tree'] == prepared['tree']
        changed_record = copy.deepcopy(record)
        changed_record['executables'][0]['cdhash'] = 'd' * 40
        changed = bake.prepare(capture, package, changed_record)
        assert changed['record']['staged_path'] != prepared['record']['staged_path']
        assert changed['record']['staged_cdhash'] == 'd' * 40
        mount = root / 'seed'
        bake.seed(mount, prepared)
        assert (mount / bake.ROOT.lstrip('/') / 'db.plist').read_bytes() == prepared['tree'][bake.ROOT + '/db.plist'][0]
        for path, (data, mode) in prepared['tree'].items():
            dest = mount / path.lstrip('/')
            assert dest.read_bytes() == data and dest.stat().st_mode & 0o777 == mode
        try:
            bake.seed(mount, prepared)
        except SystemExit:
            pass
        else:
            raise AssertionError('seed database overwrite accepted')
        for label, mutate in [
            ('unknown header', lambda d: d.update(checksum='unexpected')),
            ('wrong version', lambda d: d.update(version=2)),
            ('developer mode', lambda d: d.update(developerMode=True)),
            ('unknown entry', lambda d: d['extensions'][0].update(surprise=1)),
            ('disabled', lambda d: d['extensions'][0].update(state='activated_disabled')),
            ('wrong team', lambda d: d['extensions'][0].update(teamID='OTHERTEAM')),
            ('hash disagreement', lambda d: d['extensions'][0].update(stagedCdhash='f' * 40)),
            ('incomplete hash', lambda d: d['extensions'][0].pop('stagedCdhashes') or d['extensions'][0].pop('stagedCdhash')),
            ('stale staged URL', lambda d: d['extensions'][0].update(stagedBundleURL='file:///wrong/')),
            ('wrong archive', lambda d: d['extensions'][0].update(additionalLaunchdPlistEntries=archive({'MachServices': {}}))),
        ]:
            bad = copy.deepcopy(db)
            mutate(bad)
            if label == 'incomplete hash':
                bad['extensions'][0].pop('stagedCdhash', None)
            (capture / 'sysext-db.xml').write_bytes(plistlib.dumps(bad))
            try:
                bake.prepare(capture, package, record)
            except SystemExit:
                pass
            else:
                raise AssertionError('accepted ' + label)
        print('PASS synthetic clone: sign/version rebasing, every unchanged field, deterministic output, modes, and 11 refusals')


if __name__ == '__main__':
    main()
