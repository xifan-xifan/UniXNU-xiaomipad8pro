#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Clone a captured 27.3 camera approval into a volatile test seed.

Input is private evidence from a genuine approval, never a checked-in template.
No signing, activation, TCC, CSR or launchd override changes happen here.
stage.py calls prepare() after verifying the final signed package, then seed().
"""
import copy
import hashlib
import json
import os
import plistlib
import re
import uuid
from pathlib import Path

TEAM = 'VAELT5D9YC'
HOST = 'com.unixnu.PianoCamera'
EXT = HOST + '.Extension'
APP = '/Applications/PianoCamera.app'
ROOT = '/Library/SystemExtensions'
EXTNAME = EXT + '.systemextension'
ORIGIN = APP + '/Contents/Library/SystemExtensions/' + EXTNAME
HEADER_KEYS = {'version', 'extensions', 'extensionPolicies', 'developerMode', 'bootUUID'}
ENTRY_KEYS = {'uniqueID', 'teamID', 'identifier', 'bundleVersion', 'categories', 'originPath', 'container',
              'stagedBundleURL', 'stagedCdhashes', 'state', 'references', 'relatedKernelExtension',
              'additionalLaunchdPlistEntries', 'containingAppPath', 'stagedCdhash'}
META = re.compile(r'^(/Library/SystemExtensions(?:/[^\n|]*)?) \| size=(\d+) mode=([0-7]+) uid=(\d+) gid=(\d+) mtime=\d+$')
HEX = re.compile(r'[0-9a-f]{40}')


def require(condition, message):
    if not condition:
        raise SystemExit('camera approval: ' + message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def unarchive(data):
    """Decode only the dictionary/string/Boolean graph used by CMIO launch extras."""
    a = plistlib.loads(data)
    require(a.get('$archiver') == 'NSKeyedArchiver' and a.get('$version') == 100000, 'unknown launch archive')
    objects = a['$objects']

    def resolve(value, active=()):
        index = value.data if isinstance(value, plistlib.UID) else value.get('CF$UID') if isinstance(value, dict) else None
        if index is not None:
            require(type(index) is int and 0 <= index < len(objects) and index not in active, 'invalid archive reference')
            return resolve(objects[index], (*active, index))
        if isinstance(value, dict):
            require(set(value) == {'$class', 'NS.keys', 'NS.objects'}, 'unknown launch archive object')
            cls = value['$class']
            class_index = cls.data if isinstance(cls, plistlib.UID) else cls.get('CF$UID') if isinstance(cls, dict) else None
            require(type(class_index) is int and 0 <= class_index < len(objects) and objects[class_index] == {
                '$classes': ['NSDictionary', 'NSObject'], '$classname': 'NSDictionary'}, 'unknown archive dictionary class')
            keys, values = value['NS.keys'], value['NS.objects']
            require(len(keys) == len(values), 'archive dictionary lengths differ')
            pairs = [(resolve(k, active), resolve(v, active)) for k, v in zip(keys, values)]
            require(all(isinstance(k, str) for k, _ in pairs) and len(dict(pairs)) == len(pairs), 'archive keys invalid')
            return dict(pairs)
        require(type(value) in (str, bool), 'unknown launch archive value')
        return value

    return resolve(a['$top']['root'])


def expected_extras(info):
    service = info['CMIOExtension']['CMIOExtensionMachServiceName']
    require(service == TEAM + '.' + EXT + '.service', 'signed Mach service differs')
    return {'LaunchEvents': {'com.apple.cmio.registerassistantservice.system-extensions.matching': {
                EXT: {'CMIOExtensionBundleIdentifier': EXT}}},
            'ProcessType': 'Interactive', 'MachServices': {service: True}}


def load_capture(folder):
    """Every key is retained; unfamiliar schema or incomplete capture fails closed."""
    folder = Path(folder)
    evidence = {}
    for name, positive in [('sysext-db.xml', b'<plist'), ('sysext-staged-tree.txt', b'staged tree capture complete'),
                           ('sysext-changed-files.txt', b'post-approval changes capture complete'),
                           ('sysext-marker.txt', b'sysext marker ready'), ('sysext-before.txt', b'SystemExtensions')]:
        data = (folder / name).read_bytes()
        require(positive in data, 'incomplete capture: ' + name)
        evidence[name] = digest(data)
    db = plistlib.loads((folder / 'sysext-db.xml').read_bytes())
    require(isinstance(db, dict) and set(db) <= HEADER_KEYS, 'unknown database header keys')
    require(type(db.get('version')) is int and db['version'] == 1, 'database version is not 1')
    require(db.get('developerMode') is False, 'capture used developer mode')
    require(db.get('extensionPolicies', []) == [], 'capture contains MDM policies')
    require(len(db.get('extensions', [])) == 1, 'capture must contain only this camera extension')
    e = db['extensions'][0]
    require(set(e) <= ENTRY_KEYS, 'unknown extension keys')
    required = {'uniqueID', 'teamID', 'identifier', 'bundleVersion', 'categories', 'originPath', 'state',
                'references', 'stagedBundleURL', 'additionalLaunchdPlistEntries'}
    require(required <= set(e), 'incomplete extension record')
    require(e['teamID'] == TEAM and e['identifier'] == EXT and e['state'] == 'activated_enabled', 'capture is not approved B-TC')
    require(e['categories'] == ['com.apple.system_extension.cmio'], 'unexpected category')
    require(e['originPath'] == ORIGIN, 'origin path differs')
    require(set(e['bundleVersion']) == {'CFBundleShortVersionString', 'CFBundleVersion'}, 'unknown version fields')
    if 'container' in e:
        require(e['container'] == {'bundlePath': APP}, 'unknown container fields')
    if 'containingAppPath' in e:
        require(e['containingAppPath'] == APP, 'legacy container path differs')
    require('container' in e or 'containingAppPath' in e, 'no container app')
    require('relatedKernelExtension' not in e, 'capture has a related kernel extension')
    require(len(e['references']) == 1, 'expected one containing-app reference')
    ref = e['references'][0]
    require(set(ref) <= {'appRef', 'teamID', 'appIdentifier'} and {'teamID', 'appIdentifier'} <= set(ref), 'unknown reference fields')
    require(ref['teamID'] == TEAM and ref['appIdentifier'] == HOST, 'reference identity differs')
    require('appRef' not in ref or isinstance(ref['appRef'], str) and ref['appRef'].startswith('file:///'), 'unknown app reference')
    old_id = str(uuid.UUID(e['uniqueID'])).upper()
    # PropertyListEncoder uses URL's keyed Codable form (JSONEncoder's URL
    # string special case is different). Absolute file URLs have no base key.
    old_path = ROOT + '/' + old_id + '/' + EXTNAME
    staged_url = e['stagedBundleURL']
    require(isinstance(staged_url, dict) and set(staged_url) == {'relative'} and
            staged_url['relative'] in ('file://' + old_path, 'file://' + old_path + '/'), 'staged URL and UUID disagree')
    hashes = e.get('stagedCdhashes', {})
    if hashes:
        require(isinstance(hashes, dict) and len(hashes) == 1, 'expected one architecture')
        old_hash, arch = next(iter(hashes.items()))
        require(HEX.fullmatch(old_hash) and arch == {'cputype': 16777228, 'cpusubtype': 0}, 'expected arm64 hash record')
    else:
        old_hash = e.get('stagedCdhash', '')
        require(HEX.fullmatch(old_hash), 'no staged CDHash')
    require('stagedCdhash' not in e or e['stagedCdhash'] == old_hash, 'legacy and per-arch hashes disagree')
    text = (folder / 'sysext-staged-tree.txt').read_text()
    boots = []
    for name in ('sysext-marker.txt', 'sysext-staged-tree.txt', 'sysext-changed-files.txt'):
        ids = re.findall(r'^[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}$', (folder / name).read_text(), re.M)
        require(len(ids) == 1, 'missing/ambiguous boot identity: ' + name)
        boots.append(ids[0].upper())
    require(len(set(boots)) == 1, 'capture spans different boots')
    require('CDHash=' + old_hash in text, 'captured codesign hash differs from database')
    metadata = {}
    for line in text.splitlines():
        match = META.fullmatch(line)
        if match:
            name, size, mode, uid, gid = match.groups()
            require('..' not in Path(name).parts and (name in (ROOT, ROOT + '/.staging', ROOT + '/db.plist', ROOT + '/' + old_id)
                    or name.startswith(old_path + '/') or name == old_path), 'unexpected staged path')
            require(name not in metadata, 'duplicate tree metadata')
            metadata[name] = {'size': int(size), 'mode': int(mode, 8), 'uid': int(uid), 'gid': int(gid)}
    require(all(p in metadata for p in (ROOT, ROOT + '/.staging', ROOT + '/db.plist', old_path)), 'missing directory stat capture')
    require(all(v['uid'] == 0 and v['gid'] == 0 for v in metadata.values()), 'staged tree is not root:wheel')
    sums = {}
    for line in text.splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  (/Library/SystemExtensions/.*)', line)
        if match:
            sums[match[2]] = match[1]
    require(old_path + '/Contents/MacOS/' + EXT in sums, 'missing staged executable SHA-256')
    return db, old_path, metadata, sums, evidence


def differences(before, after, path=''):
    """A field-by-field audit. No wildcard exclusions or ignored subtrees."""
    if type(before) is not type(after):
        return [path]
    if isinstance(before, dict):
        return [p for k in sorted(before.keys() | after.keys()) for p in (
            [path + '/' + k] if k not in before or k not in after else differences(before[k], after[k], path + '/' + k))]
    if isinstance(before, list):
        if len(before) != len(after):
            return [path]
        return [p for i, (a, b) in enumerate(zip(before, after)) for p in differences(a, b, path + '/' + str(i))]
    return [] if before == after else [path]


def prepare(folder, package, package_record):
    db, old_path, metadata, sums, evidence = load_capture(folder)
    bundle = Path(package) / ORIGIN.removeprefix('/Applications/')
    info = plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())
    binaries = {e['identifier']: e for e in package_record['executables']}
    require(binaries[EXT]['team'] == TEAM and binaries[HOST]['team'] == TEAM, 'final package signer differs')
    require(binaries[EXT]['arch'] == 'arm64', 'final extension architecture differs')
    cdhash = binaries[EXT]['cdhash']
    require(HEX.fullmatch(cdhash), 'invalid final CDHash')
    executable = bundle / 'Contents/MacOS' / info.get('CFBundleExecutable', EXT)
    require(digest(executable.read_bytes()) == binaries[EXT]['sha256'], 'final executable differs from signed package record')
    original = copy.deepcopy(db)
    e = db['extensions'][0]
    require(unarchive(e['additionalLaunchdPlistEntries']) == expected_extras(info), 'launch archive differs from signed package')
    # Identity changes allocate a new staging directory. The paired fields always move together.
    new_id = str(uuid.uuid5(uuid.NAMESPACE_URL, 'unixnu-piano-camera:' + cdhash + ':' + binaries[HOST]['cdhash'])).upper()
    new_path = ROOT + '/' + new_id + '/' + EXTNAME
    e['uniqueID'] = new_id
    slash = '/' if e['stagedBundleURL']['relative'].endswith('/') else ''
    e['stagedBundleURL'] = {'relative': 'file://' + new_path + slash}
    e['bundleVersion'] = {k: info[k] for k in ('CFBundleShortVersionString', 'CFBundleVersion')}
    if 'stagedCdhashes' in e:
        e['stagedCdhashes'] = {cdhash: {'cputype': 16777228, 'cpusubtype': 0}}
    if 'stagedCdhash' in e:
        e['stagedCdhash'] = cdhash
    # File reference IDs are volume-local. The daemon's string factory resolves this
    # path URL on the target volume; a host .file/id URL must never be copied.
    if 'appRef' in e['references'][0]:
        e['references'][0]['appRef'] = 'file://' + APP + '/'
    changes = differences(original, db)
    allowed = {'/extensions/0/uniqueID', '/extensions/0/stagedBundleURL/relative', '/extensions/0/stagedCdhash',
               '/extensions/0/references/0/appRef', '/extensions/0/bundleVersion/CFBundleVersion',
               '/extensions/0/bundleVersion/CFBundleShortVersionString'}
    allowed.update('/extensions/0/stagedCdhashes/' + h for h in (original['extensions'][0].get('stagedCdhashes', {}) | e.get('stagedCdhashes', {})))
    require(set(changes) <= allowed, 'unexplained database field changes: ' + repr(set(changes) - allowed))
    tree = {}
    captured_files = {p.removeprefix(old_path + '/'): h for p, h in sums.items() if p.startswith(old_path + '/')}
    files = {p.relative_to(bundle).as_posix(): p for p in bundle.rglob('*') if p.is_file()}
    require(set(files) == set(captured_files), 'final bundle tree differs from genuine staged layout')
    root_modes = {}
    for path, meta in metadata.items():
        if path == ROOT or path == ROOT + '/.staging':
            root_modes[path] = meta['mode']
        elif path.startswith(ROOT + '/' + original['extensions'][0]['uniqueID'].upper()):
            dest = path.replace(ROOT + '/' + original['extensions'][0]['uniqueID'].upper(), ROOT + '/' + new_id, 1)
            root_modes[dest] = meta['mode']
    for inside, path in files.items():
        require(not path.is_symlink(), 'symlink in signed bundle')
        captured = metadata.get(old_path + '/' + inside)
        require(captured is not None and captured['mode'] in (0o644, 0o755), 'missing/unsafe file mode: ' + inside)
        tree[new_path + '/' + inside] = (path.read_bytes(), captured['mode'])
    tree[ROOT + '/db.plist'] = (plistlib.dumps(db, fmt=plistlib.FMT_XML, sort_keys=False), metadata.get(ROOT + '/db.plist', {}).get('mode', 0o644))
    require(tree[ROOT + '/db.plist'][1] == 0o644, 'unexpected database mode')
    return {'tree': tree, 'modes': root_modes, 'record': {'format': 1, 'captured_evidence': evidence,
            'staged_cdhash': cdhash, 'staged_path': new_path, 'derived_fields': changes,
            'preserved_fields': sorted(ENTRY_KEYS & set(e) - {p.split('/')[3] for p in changes}),
            'database_sha256': digest(tree[ROOT + '/db.plist'][0]),
            'files': {p: digest(data) for p, (data, _) in tree.items()}}}


def seed(mount, prepared):
    mount = Path(mount)
    base = mount / ROOT.lstrip('/')
    require(not (base / 'db.plist').exists(), 'input seed already has a system-extension database')
    for path, (data, mode) in prepared['tree'].items():
        dest = mount / path.lstrip('/')
        require(not any(p.is_symlink() for p in (dest, *dest.parents) if p.is_relative_to(mount)), 'symlink in seed destination')
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        os.chmod(dest, mode)
        require(digest(dest.read_bytes()) == digest(data), 'seed write differs: ' + path)
    for path, mode in prepared['modes'].items():
        dest = mount / path.lstrip('/')
        require(not dest.is_symlink(), 'symlink in seed SystemExtensions')
        if path not in prepared['tree']:
            dest.mkdir(parents=True, exist_ok=True)
        os.chmod(dest, mode)
    return prepared['record']


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    a = p.parse_args()
    db, _, _, _, evidence = load_capture(a.capture)
    print(json.dumps({'header_keys': sorted(db), 'extension_keys': sorted(db['extensions'][0]),
                      'decoded_launch_entries': unarchive(db['extensions'][0]['additionalLaunchdPlistEntries']),
                      'evidence': evidence}, indent=2))
