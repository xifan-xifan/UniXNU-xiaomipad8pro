#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""The userland of native macOS on piano, staged in one reproducible step (docs/userland.md): configs/userland.toml's files built
(xcrun clang, arm64/arm64e, ad hoc signed by the linker) or copied, put into a copy of the Data seed under the manifest's staging
directory with the on-tablet installer (tooling/userland/install.sh), the seed's boot job told to run it, the Mach-Os' CDHashes added
to the trust cache, and a record of every file's sha256.

    stage.py [--seed out/turnip-a830/stage/scratch-data.img] [--trust out/turnip-a830/stage/kernel.macho.trustcache]
             [--out out/userland/stage] [--manifest configs/userland.toml] [--on UNIT]... [--off UNIT]... [--force]
    stage.py --live out/userland/live [--installed-trust FILE]     the same files as a live update's bundle, no seed (docs/userland.md 7)
    stage.py --plan                                                the manifest, checked, as a table; nothing built
    --gpu-bundle BUNDLE (with any mode, repeatable)                a Metal plugin bundle as one more unit, gpu-<name>: every file of
                                                                   BUNDLE at /Library/GPUBundles/<name>.bundle/..., Mach-Os trusted
    --hal-bundle DRIVER (with any mode, repeatable)                a Core Audio HAL plug-in (AudioServerPlugIn) as one more unit,
                                                                   audio-<name>: every file of DRIVER at
                                                                   /Library/Audio/Plug-Ins/HAL/<name>.driver/..., Mach-Os trusted
In seed mode GPU bundle and HAL plug-in files are also put at their final paths before packing. WindowServer can load Metal (and
coreaudiod its HAL plug-ins) before the boot installer runs; staging an updated executable only beside install.sh would leave its
first process using the seed's old binary.
Live mode keeps the existing installer and rollback path.
PIANO_USERLAND_COMPACT=1 opts into compact-seed's free-space zero fill on the
copied seed. Its large temporary raw file goes under this worktree's out/; the
mount point stays under OUT. Only verified-zero raw ranges are made sparse.

The system layer (docs/data-store.md section 9): the files that change are installed on the running system, never staged into the
seed of an ESP that keeps the owner's Data (that seed is the store's base, tooling/persist/base-seed.py). `stage.py --live DIR
--gpu-bundle out/turnip-a830/NativeAdrenoMetal.bundle --installed-trust <the base's trust cache>` is that layer: the userland and the
plugin, and the trust cache an ESP takes with --layer DIR. install.sh keeps a copy of every file it replaces that it did not put
there itself (the base's plugin) and puts it back when the unit goes, so taking the layer out restores the base's files.

Outputs (seed mode; the input seed and trust cache are only read):
  OUT/scratch-data.img          the input seed plus STAGING (default /private/var/piano/userland: install.sh, manifest.tsv,
                                SHA256SUMS, files/<unit>/<name>) and the boot job's extra command `sh STAGING/install.sh --boot`
  OUT/kernel.macho.trustcache   the input trust cache with one more module first (stage-soft-metal.py's trust_bundle) holding the
                                CDHashes it lacks; none added when it has them all
  OUT/userland.json             the record: inputs, every file (unit, dest, owner, mode, sha256, CDHash, on), outputs, identities
  OUT/stamp                     what the outputs were made from

Idempotent: the build of a file is skipped when its stamp (its sources' bytes, the command, the toolchain) is unchanged, and the
staging when its stamp (the input seed's and trust cache's sha256, every staged file's sha256 and mode, the boot command, FORMAT) is:
then the outputs are left byte for byte as they are and the seed keeps its identity. A restage starts from the input seed again
(never from an earlier output): files of the staging directory that the manifest no longer has are removed, the boot command is
added once, CDHashes the input already has are not added again.

The seed mechanics are tooling/k27/stage-soft-metal.py's (imported, not changed: its sha256 is in build-native-image.sh's stamp):
the packed DBRDISK1 disk unpacked to a sparse raw file in OUT, attached and its APFS volume mounted in one hdiutil step (no
diskutil: this Mac's diskutil mount hangs some days; no sudo: ownership is ignored, the copies are stored as uid/gid 99 with their
modes), the files written, the disk detached and packed again. Every external command runs under a timeout (sh). The owners and
modes of configs/userland.toml are applied on the tablet by install.sh, as root,
from the boot job (launchd refuses a job file that is not root's, and this Mac cannot make one on the seed's volume).

Every run that changes the staging makes a new seed identity (APFS writes times and transaction ids): docs/data-store.md applies.
An ESP with this seed meets the installed store as another seed's (the store guard keeps it, Data runs volatile) unless the store is
flattened (then the seed is not read at all and none of these files reach Data: the live update path, docs/userland.md section 7).
"""
import argparse
import contextlib
import errno
import fcntl
import glob
import hashlib
import importlib.util
import io
import json
import os
import plistlib
import re
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:
    try:
        import tomli as tomllib
    except ModuleNotFoundError:
        # The macOS Python 3.9 distribution ships Tomli with pip.
        from pip._vendor import tomli as tomllib

D = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(D / 'tooling/persist'))
import common  # noqa: E402  (tooling/persist/common.py: seed identity, trust cache hashes, the installed seed)
sys.path.insert(0, str(D / 'tooling/adt'))
from boot_args import merged_reader_destinations  # noqa: E402

FORMAT = 3   # optional captured camera approval and signed apps exist before launchd
INSTALLER = D / 'tooling/userland/install.sh'
DIRS = {'/Library/LaunchDaemons': 'daemon', '/Library/LaunchAgents': 'agent', '/usr/local/bin': 'file', '/usr/local/libexec': 'file',
        '/usr/local/share/piano/wallpaper': 'file'}   # the last: the still wallpaper's data files (unit piano-still-wallpaper)
# A Metal plugin bundle's files (IOGPU looks for <MetalPluginName>.bundle in /Library/GPUBundles first; docs/data-store.md section 5):
# Contents/Info.plist, Contents/MacOS/<file>, Contents/Resources/<file>.
GPU_BUNDLES = '/Library/GPUBundles'
GPU_BUNDLE_FILE = re.compile(r'/Library/GPUBundles/[A-Za-z0-9_-]+\.bundle/Contents/(?:Info\.plist|(?:MacOS|Resources)/[A-Za-z0-9_+-][A-Za-z0-9._+-]*)')
# A Core Audio HAL plug-in's files (coreaudiod's driver service loads <name>.driver from /Library/Audio/Plug-Ins/HAL at its launch;
# audio/ns4's PianoMicNS.driver, "Mic (noise reduced)"): Contents/Info.plist, Contents/MacOS/<file>, Contents/Resources/<file> and
# the ad hoc signature's Contents/_CodeSignature/<file> (the driver service refuses a bundle whose seal is missing).
HAL_PLUGINS = '/Library/Audio/Plug-Ins/HAL'
HAL_BUNDLE_FILE = re.compile(r'/Library/Audio/Plug-Ins/HAL/[A-Za-z0-9_-]+\.driver/Contents/(?:Info\.plist|(?:MacOS|Resources|_CodeSignature)/[A-Za-z0-9_+-][A-Za-z0-9._+-]*)')
# Only the explicit external, version-locked Bluetooth runtime may use these Data
# destinations. Apple bytes never enter this checkout, including its ignored out/.
BT_ROOT = '/private/var/piano/bluetooth-path1'
BT_DATA = frozenset(f'{BT_ROOT}/{name}' for name in ('bin/bluetoothd', 'bin/BlueTool',
                    'bin/PianoBluetooth.runtime', 'lib/QcaBTType0.dylib', 'runtime.sha256'))
CAMERA_BUNDLE_FILE = re.compile(
    r'/Applications/(?:PianoCamera|PianoCameraCapture)\.app/Contents/'
    r'(?:Library/SystemExtensions/com\.unixnu\.PianoCamera\.Extension\.systemextension/Contents/)?'
    r'(?:Info\.plist|MacOS/[A-Za-z0-9._-]+|_CodeSignature/CodeResources)')
SYSTEM_PROGRAMS = ('/bin/', '/usr/bin/', '/sbin/', '/usr/sbin/', '/usr/libexec/')
WORD = re.compile(r'[A-Za-z0-9._+@/-]+')
MODE = re.compile(r'0[0-7]{3}')
OWNER = re.compile(r'[a-z_][a-z0-9_]*:[a-z_][a-z0-9_]*')
UNIT = re.compile(r'[a-z0-9][a-z0-9-]*')
_SSM = None


def soft_metal():
    """tooling/k27/stage-soft-metal.py as a module: cdhash, trust_bundle, data_volume, run, BOOT_JOB, SETTIME, packed_ramdisk (from
    the k27 tree: PIANO_K27_TREE)."""
    global _SSM
    if _SSM is None:
        sys.path.insert(0, str(D / 'tooling/k27'))
        spec = importlib.util.spec_from_file_location('stage_soft_metal', D / 'tooling/k27/stage-soft-metal.py')
        _SSM = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_SSM)
    return _SSM


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rel(path):
    """PATH relative to the repository when it is inside it (records and messages carry no home directories)."""
    path = Path(path).resolve()
    return path.relative_to(D).as_posix() if path.is_relative_to(D) else str(path)


def sh(command, *, timeout=600, **kw):
    """subprocess.run for every external command this tool starts, always bounded: one that stops answering (this Mac's disk tools
    have hung on some days) fails the run with a clear message instead of hanging it forever. TIMEOUT is per command."""
    try:
        return subprocess.run(command, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        raise SystemExit(f'{command[0]}: no answer within {timeout} s ({shlex.join(command)}); giving up on it') from None


# ---- the manifest ------------------------------------------------------------------------------------------------------------

def load_manifest(path, on=(), off=()):
    """configs/userland.toml as a checked plan: {'staging', 'units': [{name, about, on, planned, files: [...]}]}. SystemExit with
    every problem found."""
    try:
        doc = tomllib.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError) as e:
        raise SystemExit(f'{path}: {e}')
    problems = []
    if doc.get('format') != 1:
        problems.append(f'format {doc.get("format")!r}: this tool reads format 1')
    staging = doc.get('staging', '')
    if not (staging.startswith('/private/var/') and WORD.fullmatch(staging) and '..' not in staging.split('/')):
        problems.append(f'staging {staging!r}: an absolute path under /private/var/ (Data), plain characters')
    units, names, dests = [], set(), {}
    for u in doc.get('unit', []):
        name = u.get('name', '')
        where = f'unit {name or "?"}'
        if not UNIT.fullmatch(name) or name in names:
            problems.append(f'{where}: the name must be lower case letters, digits and dashes, and unique')
        names.add(name)
        unknown = set(u) - {'name', 'about', 'default_on', 'status', 'file'}
        if unknown:
            problems.append(f'{where}: unknown keys {sorted(unknown)}')
        if u.get('status', 'ready') not in ('ready', 'planned'):
            problems.append(f'{where}: status {u.get("status")!r} (ready or planned)')
        if not isinstance(u.get('default_on'), bool):
            problems.append(f'{where}: default_on must be true or false')
        unit = {'name': name, 'about': u.get('about', ''), 'on': bool(u.get('default_on')), 'default_on': bool(u.get('default_on')),
                'planned': u.get('status') == 'planned', 'files': []}
        for f in u.get('file', []):
            unit['files'].append(check_file(f, unit, problems))
        if not unit['files'] and not unit['planned']:
            problems.append(f'{where}: no files')
        jobs = [f for f in unit['files'] if f['kind'] != 'file']
        if len(jobs) > 1:
            problems.append(f'{where}: {len(jobs)} launchd jobs; one unit has at most one')
        if not jobs and not unit['default_on']:
            problems.append(f'{where}: default_on false needs a launchd job to turn off (a command-line tool is always installed)')
        staged_entries = set()
        for f in unit['files']:
            if f['dest'] in dests:
                problems.append(f'{where}: {f["dest"]} is also unit {dests[f["dest"]]}\'s')
            dests[f['dest']] = name
            staged = f['staged'] or f'files/{name}/{f["name"]}'
            if not isinstance(staged, str):
                continue  # check_file has already diagnosed the invalid path.
            key = staged.casefold()  # The staging volume can be case-insensitive.
            if any(p == key or p.startswith(key + '/') or key.startswith(p + '/') for p in staged_entries):
                problems.append(f'{where}: staging path conflicts with another file: {staged}')
            staged_entries.add(key)
        units.append(unit)
    for name, value in [(n, True) for n in on] + [(n, False) for n in off]:
        match = [u for u in units if u['name'] == name]
        if not match:
            problems.append(f'--{"on" if value else "off"} {name}: no such unit')
        elif not [f for f in match[0]['files'] if f['kind'] != 'file'] and not value:
            problems.append(f'--off {name}: a command-line tool has no job to turn off')
        else:
            match[0]['on'] = value
    if problems:
        raise SystemExit(f'{path}:\n  ' + '\n  '.join(problems))
    return {'path': Path(path), 'staging': staging, 'units': units, 'camera': doc.get('camera_package')}


def dest_kind(dest):
    """What a destination is: daemon, agent or file (directly in one of DIRS, or a Metal plugin bundle's file); None when refused."""
    parent, _, base = dest.rpartition('/')
    if not WORD.fullmatch(dest) or not base or base.startswith('.') or '..' in dest.split('/'):
        return None
    return DIRS.get(parent) or ('file' if GPU_BUNDLE_FILE.fullmatch(dest) or HAL_BUNDLE_FILE.fullmatch(dest) or
                                CAMERA_BUNDLE_FILE.fullmatch(dest) or dest in BT_DATA else None)


def check_file(f, unit, problems):
    where = f'unit {unit["name"]}, {f.get("dest", "?")}'
    unknown = set(f) - {'source', 'sources', 'build', 'arch', 'min_os', 'cflags', 'frameworks', 'command', 'output', 'inputs',
                        'dest', 'owner', 'mode', 'trust', 'staged'}
    if unknown:
        problems.append(f'{where}: unknown keys {sorted(unknown)}')
    dest = f.get('dest', '')
    base = dest.rpartition('/')[2]
    kind = dest_kind(dest)
    if not kind:
        problems.append(f'{where}: dest must be a file directly in {", ".join(DIRS)}, or a Metal plugin bundle\'s '
                        f'({GPU_BUNDLES}/<name>.bundle/Contents/Info.plist, MacOS/<file>, Resources/<file>), or a HAL plug-in\'s '
                        f'({HAL_PLUGINS}/<name>.driver/Contents/Info.plist, MacOS/<file>, Resources/<file>, _CodeSignature/<file>)')
        kind = 'file'
    owner, mode = f.get('owner', 'root:wheel'), f.get('mode', '')
    if not OWNER.fullmatch(owner):
        problems.append(f'{where}: owner {owner!r} (user:group)')
    if not MODE.fullmatch(mode) or int(mode, 8) & 0o022:
        problems.append(f'{where}: mode {mode!r}: four octal digits, not writable by group or others')
    if kind != 'file':
        if not base.endswith('.plist') or mode != '0644' or owner != 'root:wheel':
            problems.append(f'{where}: a launchd job is <Label>.plist, root:wheel 0644 (launchd refuses any other)')
    build = f.get('build', 'copy')
    sources = [f['source']] if 'source' in f else list(f.get('sources', []))
    if build in ('copy', 'cc'):
        if not sources or (build == 'copy' and len(sources) != 1):
            problems.append(f'{where}: build {build} needs source' + ('' if build == 'copy' else ' or sources'))
        if not unit['planned']:
            for s in sources:
                if not (D / s).is_file():
                    problems.append(f'{where}: no source {s}')
        if build == 'cc' and f.get('arch', 'arm64') not in ('arm64', 'arm64e'):
            problems.append(f'{where}: arch {f.get("arch")!r} (arm64 or arm64e)')
    elif build == 'script':
        if not f.get('command') or not isinstance(f.get('command'), list) or not f.get('output') or not f.get('inputs'):
            problems.append(f'{where}: build script needs command (a list), output and inputs')
    else:
        problems.append(f'{where}: build {build!r} (copy, cc or script)')
    staged = f.get('staged')
    prefix = f'files/{unit["name"]}/'
    if staged is not None and (
        not isinstance(staged, str) or not staged.startswith(prefix) or not WORD.fullmatch(staged)
        or any(part in ('', '.', '..') for part in staged[len(prefix):].split('/'))
    ):
        problems.append(f'{where}: staged must be a relative path under {prefix}')
    return {'unit': unit['name'], 'dest': dest, 'name': base, 'kind': kind, 'owner': owner, 'mode': mode, 'build': build,
            'staged': staged,
            'sources': sources, 'arch': f.get('arch', 'arm64'), 'min_os': f.get('min_os', '14.0'), 'cflags': list(f.get('cflags', [])),
            'frameworks': list(f.get('frameworks', [])), 'command': list(f.get('command', [])), 'output': f.get('output'),
            'inputs': list(f.get('inputs', [])), 'trust': f.get('trust', True)}


BUNDLE_KINDS = {
    # option, install directory, bundle suffix, unit prefix, what it is
    'gpu': ('--gpu-bundle', GPU_BUNDLES, '.bundle', 'gpu-', 'the Metal plugin'),
    'hal': ('--hal-bundle', HAL_PLUGINS, '.driver', 'audio-', 'the Core Audio HAL plug-in'),
}


def add_bundles(plan, bundles, kind):
    """--gpu-bundle / --hal-bundle: each plugin bundle as one more unit of PLAN, <prefix><its name in lower case>, without a job (always
    installed): every file of the bundle at <directory>/<name><suffix>/<its path>, root:wheel, 0755 for a Mach-O or an executable,
    0644 otherwise, its Mach-Os' CDHashes into the trust cache. Staged as files/<unit>/<its path in the bundle>."""
    option, directory, suffix, prefix, what = BUNDLE_KINDS[kind]
    problems = []
    dests = {f['dest']: u['name'] for u in plan['units'] for f in u['files']}
    for bundle in bundles:
        bundle = Path(bundle).resolve()
        if not re.fullmatch(r'[A-Za-z0-9_-]+' + re.escape(suffix), bundle.name) or not (bundle / 'Contents/Info.plist').is_file():
            raise SystemExit(f'{option} {bundle}: not a plugin bundle (<name>{suffix} with Contents/Info.plist)')
        name = prefix + bundle.stem.lower().replace('_', '-')
        if name in {u['name'] for u in plan['units']}:
            raise SystemExit(f'{option} {bundle}: unit {name} twice')
        unit = {'name': name, 'about': f'{what} {bundle.name} in {directory} (the system layer, docs/data-store.md section 9)',
                'on': True, 'default_on': True, 'planned': False, 'files': []}
        for path in sorted(bundle.rglob('*')):
            if path.is_symlink() or (not path.is_file() and not path.is_dir()):
                raise SystemExit(f'{option} {bundle}: {path.relative_to(bundle)} is neither a file nor a directory')
            if not path.is_file():
                continue
            inside = path.relative_to(bundle).as_posix()
            data = path.read_bytes()
            executable = data[:4] == b'\xcf\xfa\xed\xfe' or os.access(path, os.X_OK)
            f = check_file({'source': str(path), 'build': 'copy', 'dest': f'{directory}/{bundle.name}/{inside}',
                            'mode': '0755' if executable else '0644', 'staged': f'files/{name}/{inside}'}, unit, problems)
            if f['dest'] in dests:
                problems.append(f'{f["dest"]}: also unit {dests[f["dest"]]}\'s')
            dests[f['dest']] = name
            unit['files'].append(f)
        if not unit['files']:
            problems.append(f'{option} {bundle}: no files')
        plan['units'].append(unit)
    if problems:
        raise SystemExit(f'{option}:\n  ' + '\n  '.join(problems))
    return plan


def add_gpu_bundles(plan, bundles):
    """--gpu-bundle: Metal plugin bundles (add_bundles, kind gpu: /Library/GPUBundles/<name>.bundle, unit gpu-<name>)."""
    return add_bundles(plan, bundles, 'gpu')


def add_hal_bundles(plan, bundles):
    """--hal-bundle: Core Audio HAL plug-ins (add_bundles, kind hal: /Library/Audio/Plug-Ins/HAL/<name>.driver, unit audio-<name>)."""
    return add_bundles(plan, bundles, 'hal')

def add_camera_package(plan):
    """Opt-in signed apps from this commit. Copy their files byte for byte."""
    supplied = os.environ.get('PIANO_CAMERA_PACKAGE')
    if not supplied:
        if os.environ.get('PIANO_CAMERA_APPROVAL'):
            raise SystemExit('PIANO_CAMERA_APPROVAL requires a final signed PIANO_CAMERA_PACKAGE')
        return plan
    package = Path(supplied).resolve(strict=True)
    if not package.is_relative_to(D / 'out'):
        raise SystemExit('PIANO_CAMERA_PACKAGE must be built under this worktree’s out/')
    apps = (plan.get('camera') or {}).get('apps')
    if apps != ['PianoCamera.app', 'PianoCameraCapture.app']:
        raise SystemExit('The camera package needs both app bundles in the manifest')
    sh(['python3', '-I', str(D / 'tooling/camera/extension/package-camera.py'), str(package), '--verify'],
       timeout=60, check=True)
    record = json.loads((package / 'cdhashes.json').read_text())
    hashes = {f['path']: f['cdhash'] for f in record['executables']}
    unit = {'name': 'piano-camera-apps', 'about': 'signed Piano camera publisher and AVFoundation client',
            'on': True, 'default_on': True, 'planned': False, 'files': []}
    problems = []
    dests = {f['dest'] for u in plan['units'] for f in u['files']}
    found = set()
    for app in apps:
        bundle = package / app
        if bundle.is_symlink():
            raise SystemExit(f'Camera bundle is a symlink: {app}')
        for path in sorted(bundle.rglob('*')):
            inside = path.relative_to(package).as_posix()
            if path.is_symlink() or not (path.is_file() or path.is_dir()):
                raise SystemExit(f'Unsupported camera bundle entry: {inside}')
            if path.is_dir():
                continue
            dest = f'/Applications/{inside}'
            if dest in dests:
                raise SystemExit(f'Camera bundle destination already staged: {dest}')
            # The capture app uses ordinary TCC consent, which requires a non-platform identity.
            file = check_file({'source': str(path), 'dest': dest, 'build': 'copy',
                               'mode': '0755' if inside in hashes else '0644',
                               'trust': app == 'PianoCamera.app',
                               'staged': f'files/{unit["name"]}/{inside}'}, unit, problems)
            file['verified_cdhash'] = hashes.get(inside)
            if inside in hashes:
                found.add(inside)
            unit['files'].append(file)
    if problems or found != set(hashes):
        raise SystemExit('Camera bundle layout:\n  ' + '\n  '.join(problems))
    plan['units'].append(unit)
    if os.environ.get('PIANO_CAMERA_APPROVAL'):
        if (plan.get('camera') or {}).get('approval_env') != 'PIANO_CAMERA_APPROVAL':
            raise SystemExit('The manifest does not enable the private camera approval input')
        spec = importlib.util.spec_from_file_location('camera_bake', D / 'tooling/camera/extension/bake-sysext.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        plan['camera_bake'] = module.prepare(os.environ['PIANO_CAMERA_APPROVAL'], package, record)
        plan['camera_bake_module'] = module
    return plan


def add_bluetooth_runtime(plan):
    """Opt-in external artifacts built from this committed checkout; no firmware or pairing data."""
    supplied = os.environ.get('PIANO_BT_RUNTIME')
    if not supplied:
        return plan
    root = Path(supplied).resolve(strict=True)
    if root.is_relative_to(D) or any((p / '.git').exists() for p in (root, *root.parents)):
        raise SystemExit('PIANO_BT_RUNTIME must be outside every Git checkout')
    report = json.loads((root / 'report.json').read_text())
    commit = sh(['git', '-C', str(D), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    if (report.get('format') != 1 or report.get('build') != '26C5035r' or report.get('source_commit') != commit or
        report.get('native_input_sha256') != '82e244d5dcafbc150e820416ac7037debc9def106eae043ee51ed4e2c84e7721' or
        report.get('native_uuid') != '01204E5C-F295-35D8-9ED6-62ECF79D62DF' or
        report.get('native_entitlements_preserved') is not True):
        raise SystemExit('Bluetooth runtime source/build/signature provenance differs; rebuild it from this commit')
    inputs = report.get('inputs', {})
    if not inputs:
        raise SystemExit('Bluetooth runtime has no source input digests')
    source = D / 'tooling/native/piano-bluetooth'
    for name, digest in inputs.items():
        path = (source / name).resolve()
        if not path.is_relative_to(source) or not path.is_file() or sha(path.read_bytes()) != digest:
            raise SystemExit(f'Bluetooth runtime source input differs: {name}')
    destinations = {name: f'{BT_ROOT}/{name}' for name in ('bin/bluetoothd', 'bin/BlueTool',
                    'bin/PianoBluetooth.runtime', 'lib/QcaBTType0.dylib', 'runtime.sha256')}
    destinations.update({'helpers/piano-bt-runtime-mount': '/usr/local/libexec/piano-bt-runtime-mount',
                         'helpers/piano-bt-probe': '/usr/local/libexec/piano-bt-probe',
                         'scripts/piano-bluetooth-start': '/usr/local/libexec/piano-bluetooth-start',
                         'launchd/com.unixnu.piano-bluetooth-path1.plist':
                         '/Library/LaunchDaemons/com.unixnu.piano-bluetooth-path1.plist'})
    # pinned: its job file is staged byte for byte as the runtime's report records it (no BTM_LAUNCHER rewrite)
    unit = {'name': 'piano-bluetooth-path1', 'about': 'opt-in piano-bt=5 POSIX controller runtime',
            'on': True, 'default_on': True, 'planned': False, 'pinned': True, 'files': []}
    problems = []
    existing = {f['dest'] for u in plan['units'] for f in u['files']}
    for name, dest in destinations.items():
        path = root / name
        record = report.get('files', {}).get(name, {})
        if path.is_symlink() or not path.is_file() or sha(path.read_bytes()) != record.get('sha256'):
            raise SystemExit(f'Bluetooth runtime file missing or changed: {name}')
        if dest in existing:
            raise SystemExit(f'Bluetooth runtime destination already staged: {dest}')
        mode = '0755' if name.startswith(('helpers/', 'scripts/')) or 'cdhash' in record else '0644'
        unit['files'].append(check_file({'source': str(path), 'dest': dest, 'build': 'copy', 'mode': mode}, unit, problems))
    if problems:
        raise SystemExit('Bluetooth runtime:\n  ' + '\n  '.join(problems))
    plan['units'].append(unit)
    return plan


def check_jobs(plan, built):
    """Each launchd job of the plan against the plan: its Label is its file name, its program is installed by the manifest (or a
    system path), the paths under /usr/local it names are the manifest's, Disabled is not set by hand."""
    problems = []
    dests = {f['dest'] for u in plan['units'] if not u['planned'] for f in u['files']}
    for f, data in built:
        if f['kind'] == 'file':
            continue
        try:
            job = plistlib.loads(data)
        except Exception as e:  # noqa: BLE001 (plistlib raises several types)
            problems.append(f'{f["dest"]}: not a property list ({e})')
            continue
        label = job.get('Label')
        if label != f['name'][:-len('.plist')]:
            problems.append(f'{f["dest"]}: Label {label!r} is not the file name')
        argv = job.get('ProgramArguments') or ([job['Program']] if job.get('Program') else [])
        if not argv:
            problems.append(f'{f["dest"]}: no Program or ProgramArguments')
        elif argv[0] not in dests and not argv[0].startswith(SYSTEM_PROGRAMS):
            problems.append(f'{f["dest"]}: runs {argv[0]}, neither installed by the manifest nor a system program')
        for word in argv:
            if isinstance(word, str) and word.startswith('/usr/local/') and word not in dests:
                problems.append(f'{f["dest"]}: names {word}, which the manifest does not install')
        if 'Disabled' in job:
            problems.append(f'{f["dest"]}: has Disabled; on/off is default_on in the manifest (stage.py writes Disabled for off)')
        f['label'] = label or '-'
    if problems:
        raise SystemExit('launchd jobs:\n  ' + '\n  '.join(problems))


# BTM_LAUNCHER: the first words of a job whose program is one of the manifest's own files (docs/userland.md section 3a).
BTM_LAUNCHER = ['/bin/sh', '-c', 'exec "$0" "$@"']


def plan_unit_pinned(plan, f):
    """True for a file of a pinned unit (the Bluetooth runtime: staged exactly as its build report records)."""
    return any(u.get('pinned') for u in plan['units'] if u['name'] == f['unit'])


def btm_stable(job, dests):
    """A job whose program the manifest installs is started through BTM_LAUNCHER instead (in place, True when changed).

    Background Task Management (backgroundtaskmanagementd, macOS 13+) keeps one record per legacy launchd job with the plist's
    sha256 and modification date and the executable's path and modification date (Program, or ProgramArguments[0]). When either
    file changes, it logs "registerLaunchItem: plist or executable is modified", replaces the record "not notified" and shows
    "Background Items Added" again. Our programs are rebuilt and replaced by every layer update, so every update re-announced
    every changed job. With /bin/sh (on the sealed system volume) as the executable, only a change of the job file itself
    announces the job again. `exec "$0" "$@"` replaces the shell with the program: the same pid, argv and exit status as before."""
    argv = list(job.get('ProgramArguments') or [])
    program = job.get('Program') or (argv[0] if argv else None)
    if not isinstance(program, str) or program not in dests:
        return False
    job.pop('Program', None)
    job['ProgramArguments'] = BTM_LAUNCHER + [program] + argv[1:]
    return True


# ---- building ----------------------------------------------------------------------------------------------------------------

_TOOLCHAIN = None


def toolchain():
    """What a build depends on beyond its sources: clang, ld and the SDK, as their versions say."""
    global _TOOLCHAIN
    if _TOOLCHAIN is None:
        parts = []
        for command in (['xcrun', 'clang', '--version'], ['xcrun', '--show-sdk-path'], ['xcrun', '--show-sdk-version'],
                        ['xcrun', 'ld', '-v']):
            r = sh(command, timeout=60, capture_output=True, text=True)
            parts.append((r.stdout + r.stderr).strip())
        _TOOLCHAIN = '\n'.join(parts)
    return _TOOLCHAIN


def build_file(f, build_dir):
    """The bytes FILE installs: its source (copy) or its build output, rebuilt only when its stamp changed. Returns a Path."""
    if f['build'] == 'copy':
        return D / f['sources'][0]
    out = build_dir / f['unit'] / f['name']
    out.parent.mkdir(parents=True, exist_ok=True)
    stamp_file = out.with_name(out.name + '.stamp')
    if f['build'] == 'cc':
        command = ['xcrun', 'clang', '-arch', f['arch'], f'-mmacosx-version-min={f["min_os"]}', '-O2', '-Wall', '-Wextra', '-Werror',
                   *f['cflags'], *f['sources'], *[x for fw in f['frameworks'] for x in ('-framework', fw)], '-o', str(out)]
        inputs = [D / s for s in f['sources']]
        inputs += sorted({Path(p) for pattern in f['inputs'] for p in glob.glob(str(D / pattern)) if Path(p).is_file()})
    else:
        command = f['command']
        inputs = sorted({Path(p) for pattern in f['inputs'] for p in glob.glob(str(D / pattern)) if Path(p).is_file()})
        if not inputs:
            raise SystemExit(f'{f["dest"]}: no file matches its inputs {f["inputs"]}')
    h = hashlib.sha256(json.dumps([command if f['build'] == 'script' else command[:-1], toolchain()]).encode())
    for path in inputs:
        h.update(rel(path).encode() + b'\0' + path.read_bytes())
    stamp = h.hexdigest()
    try:
        old = json.loads(stamp_file.read_text())
    except (OSError, ValueError):
        old = {}
    if old.get('stamp') == stamp and out.is_file() and sha(out.read_bytes()) == old.get('sha256'):
        return out
    log = out.with_name(out.name + '.log')
    with log.open('w') as handle:
        result = sh(command, timeout=600, cwd=D, stdout=handle, stderr=subprocess.STDOUT)
    if result.returncode:
        tail = log.read_text().strip().splitlines()[-15:]
        raise SystemExit(f'{f["dest"]}: the build failed ({shlex.join(command)}); {rel(log)}:\n  ' + '\n  '.join(tail))
    if f['build'] == 'script':
        produced = D / f['output']
        if not produced.is_file():
            raise SystemExit(f'{f["dest"]}: {shlex.join(command)} made no {f["output"]}')
        shutil.copyfile(produced, out)
        os.chmod(out, 0o755)
    stamp_file.write_text(json.dumps({'stamp': stamp, 'sha256': sha(out.read_bytes())}) + '\n')
    return out


def build_all(plan, build_dir):
    """Every file of the units that are not planned: [(file, bytes)], each Mach-O's CDHash in file['cdhash'] (hex) when trusted."""
    built = []
    for unit in plan['units']:
        if unit['planned']:
            continue
        for f in unit['files']:
            path = build_file(f, build_dir)
            f['on'] = unit['on']
            f['path'] = path
            data = path.read_bytes()
            cdhash = None
            if data[:4] in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
                if data[:4] != b'\xcf\xfa\xed\xfe':
                    raise SystemExit(f'{f["dest"]}: a fat Mach-O; build one architecture (the trust cache takes a thin file\'s CDHash)')
                cdhash = soft_metal().cdhash(path).hex()
            elif f['build'] in ('cc', 'script') and f['trust']:
                raise SystemExit(f'{f["dest"]}: built, but not a Mach-O ({rel(path)})')
            if 'verified_cdhash' in f and cdhash != f['verified_cdhash']:
                raise SystemExit(f'{f["dest"]}: signing manifest and staged CodeDirectory differ')
            f['cdhash'] = cdhash if f['trust'] else None
            built.append((f, data))
    check_jobs(plan, built)
    dests = {f['dest'] for u in plan['units'] if not u['planned'] for f in u['files']}
    out = []
    for f, data in built:
        if f['kind'] != 'file':
            job = plistlib.loads(data)
            changed = not plan_unit_pinned(plan, f) and btm_stable(job, dests)
            if not f['on']:
                job['Disabled'] = True   # off in this profile: launchd loads it only once the owner says launchctl enable
                changed = True
            if changed:
                data = plistlib.dumps(job, fmt=plistlib.FMT_XML)
        f['sha256'] = sha(data)
        out.append((f, data))
    return out


# ---- the staging tree --------------------------------------------------------------------------------------------------------

def staging_tree(plan, built):
    """{path relative to the staging directory: (bytes, mode)}: install.sh, manifest.tsv, SHA256SUMS, files/<unit>/<name>."""
    tree = {'install.sh': (INSTALLER.read_bytes(), 0o755)}
    lines = ['# unit\tkind\ton\tmode\towner\tgroup\tsha256\tstaged\tdest\tlabel (configs/userland.toml, tooling/userland/stage.py)']
    for f, data in built:
        staged = f.get('staged') or f'files/{f["unit"]}/{f["name"]}'
        tree[staged] = (data, 0o755 if int(f['mode'], 8) & 0o111 else 0o644)
        user, group = f['owner'].split(':')
        fields = [f['unit'], f['kind'], '1' if f['on'] else '0', f['mode'], user, group, f['sha256'], staged, f['dest'],
                  f.get('label', '-')]
        assert all(WORD.fullmatch(x) for x in fields), fields
        lines.append('\t'.join(fields))
    tree['manifest.tsv'] = (('\n'.join(lines) + '\n').encode(), 0o644)
    sums = ''.join(f'{sha(data)}  {name}\n' for name, (data, _) in sorted(tree.items()))
    tree['SHA256SUMS'] = (sums.encode(), 0o644)
    return tree


def sync_tree(root, tree):
    """Make ROOT hold exactly TREE: files written only when their bytes differ, modes set, anything else under ROOT removed.
    Returns (written, removed) relative paths."""
    if root.is_symlink():
        raise SystemExit('staging directory must not be a symlink')
    root.mkdir(parents=True, exist_ok=True)
    written, removed = [], []
    for path in sorted(root.rglob('*'), key=lambda p: len(p.parts), reverse=True):
        name = path.relative_to(root).as_posix()
        if path.is_symlink() or (path.is_file() and name not in tree):
            path.unlink()
            removed.append(name)
        elif path.is_dir() and not any(k.startswith(name + '/') for k in tree):
            shutil.rmtree(path)
            removed.append(name + '/')
    for name, (data, mode) in sorted(tree.items()):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not (path.is_file() and path.stat().st_nlink == 1 and path.read_bytes() == data):
            # mkstemp creates the file with mode 0600 before it contains any bytes.
            fd, temporary = tempfile.mkstemp(dir=path.parent)
            try:
                with os.fdopen(fd, 'wb') as handle:
                    handle.write(data)
                    handle.flush()
                    os.fchmod(handle.fileno(), mode)
                os.replace(temporary, path)
            finally:
                Path(temporary).unlink(missing_ok=True)
            written.append(name)
        else:
            os.chmod(path, mode)
    for path in [root, *[p for p in root.rglob('*') if p.is_dir()]]:
        os.chmod(path, 0o755)
    # the build Mac's com.apple.provenance tag (its sandboxed tools write it) is not carried into the tablet's volume
    sh(['xattr', '-cr', str(root)], timeout=60, check=False)
    return written, removed


def boot_command(plan):
    return f'sh {plan["staging"]}/install.sh --boot'


def boot_wifi_profile(tree, command):
    """Optional RAM-only fixture, never a manifest/live-layer file or a logged
    credential. The source must live outside this checkout; only seed mode calls
    this. Boot fixes ownership before any Wi-Fi LaunchDaemon is loaded."""
    source = os.environ.get('PIANO_BOOT_WIFI_PROFILE')
    if not source:
        return command
    path = Path(source)
    limit = 4096
    try:
        if path.resolve(strict=True).is_relative_to(D):
            raise SystemExit('boot Wi-Fi profile must be outside the checkout')
        # Check the opened file and bound the read even if it grows after fstat.
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd, 'rb') as profile:
            st = os.fstat(profile.fileno())
            if not stat.S_ISREG(st.st_mode) or st.st_mode & 0o077 or st.st_size > limit:
                raise SystemExit('boot Wi-Fi profile must be a private regular file of at most 4 KiB')
            data = profile.read(limit + 1)
    except (OSError, RuntimeError):
        raise SystemExit('cannot open the external private boot Wi-Fi profile') from None
    if len(data) > limit:
        raise SystemExit('boot Wi-Fi profile exceeds 4 KiB')
    if not data or not data.endswith(b'\n'):
        raise SystemExit('boot Wi-Fi profile must be nonempty and newline terminated')
    tree['boot-wifi-profile'] = (data, 0o600)
    staging = command.removeprefix('sh ').removesuffix('/install.sh --boot')
    staged = shlex.quote(f'{staging}/boot-wifi-profile')
    directory = '/private/var/db/piano-wifi'
    dest = f'{directory}/profile'
    return (f'if [ -f {staged} ] && [ ! -L {staged} ]; then (umask 077; '
            f'[ ! -L {directory} ] && install -d -o root -g wheel -m 700 {directory} && '
            f'[ ! -L {dest} ] && [ ! -d {dest} ] && '
            f'install -S -o root -g wheel -m 600 {staged} {dest} && rm -f {staged}); fi; {command}')


def seed_gpu_files(mount, staging, built):
    """Put GPU files (and HAL plug-in files: coreaudiod loads them at its launch) in the loaders' paths before the first WindowServer
    and coreaudiod. Retain omitted bundle resources.
    Clone from the staged APFS files, so the installer and loader share data extents rather than doubling the RAM image.
    Other userland files still go through the root boot installer; live layers never call this function.
    """
    installed = []
    for f, data in built:
        if not (GPU_BUNDLE_FILE.fullmatch(f['dest']) or HAL_BUNDLE_FILE.fullmatch(f['dest'])):
            continue
        source = mount / staging / (f.get('staged') or f'files/{f["unit"]}/{f["name"]}')
        target = mount / f['dest'].lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        if not (target.is_file() and target.read_bytes() == data):
            temporary = target.with_name(target.name + '.userland-new')
            temporary.unlink(missing_ok=True)
            sh(['cp', '-c', str(source), str(temporary)], timeout=60, check=True)
            os.replace(temporary, target)
        os.chmod(target, int(f['mode'], 8))
        if common.sha(target) != f['sha256']:
            raise SystemExit(f'{f["dest"]}: seed GPU file differs from the staged manifest')
        sh(['xattr', '-c', str(target)], timeout=60, check=False)
        installed.append(f['dest'])
    return installed


# gpu/safari1 (2026-11-24, private/host-checks/20261124-browser-gpu/ANALYSIS.md section 10): PIANO_GPU_ADDON=copy|move (seed mode
# only) puts a clone of the seed's own Metal plugin, /Library/GPUBundles/<PIANO_GPU_ADDON_NAME, default NativeAdrenoMetal>.bundle, at
# /private/var/ngpu/addons/<name>.bundle before launchd. With boot-arg sheng-gpu-addon=1 the k27 kernel mounts that directory read-only
# as a bindfs union view over /System/Library/Extensions after the Data mount (kern_exec.c native_gpu_addon_prepare, patches 0002/0038/
# 0039): the one plugin directory WebKit's GPU process may map executable (GPUProcess.sb `file-map-executable (subpath
# "/System/Library/Extensions")`; /Library/GPUBundles has no such rule). copy keeps the /Library/GPUBundles copy, which is searched
# first (libsystem_sandbox GPU_BUNDLE_SEARCH_PATHS), so a test hides it at run time on the volatile Data for an A/B in one boot; move
# removes it from the seed, so every process, WindowServer included, loads the view. The bytes are the seed's own (clones): their
# Mach-Os' CDHashes must already be in the output trust cache, else the copy is refused. Nothing else changes; unset, nothing happens.
GPU_ADDONS = '/private/var/ngpu/addons'
# LT-B3 (ANALYSIS.md section 11): PIANO_GPU_ADDON_PLACE=app puts the bundle's real files in /private/var/ngpu/app-library/Frameworks/,
# which kernel patch 0074 shows as a union view over the App cryptex's System/Library, and makes the /S/L/E view's entry an absolute
# symlink to /System/Volumes/Preboot/Cryptexes/App/System/Library/Frameworks/<name>.bundle. Chrome's GPU sandbox maps code only from
# cryptex paths and matches a symlink's real path. Needs a 0074 kernel: without the view the symlink dangles (no Metal device).
GPU_APP_LIBRARY = '/private/var/ngpu/app-library'
GPU_APP_POINT = '/System/Volumes/Preboot/Cryptexes/App/System/Library'
# Frameworks, not Extensions (cryptex1-qual1, 11-25 10:5x: under App/System/Library/Extensions Safari's GPU process lost WebGPU): the one
# App-cryptex directory both sandboxes map code from: Chrome common.sb (subpath Preboot/Cryptexes/App/System), WebKit GPUProcess.sb
# (subpath Preboot/Cryptexes/App/System/Library/Frameworks).
GPU_APP_DIR = 'Frameworks'
GPU_ADDON_NAME = re.compile(r'[A-Za-z0-9_-]+')


# LT-B3 (gpu/cryptex2, private/host-checks/20261124-browser-gpu/ANALYSIS.md section 14): the INSTALLED layer's app2 layout, the one
# cryptex1-ctl3 qualified (seed mode PIANO_GPU_ADDON_PLACE=app2 with the /Library/GPUBundles copy gone): the whole bundle real under
# kernel 0074's view source (GPU_APP_LIBRARY/GPU_APP_DIR, shown at GPU_APP_POINT/GPU_APP_DIR), and in the /S/L/E view's source
# (GPU_ADDONS) the same bundle with its Info.plist real (what Metal's discovery and CFBundle read) and every Mach-O a link (install.sh
# kind link) to that file's App-cryptex path (what dyld maps: both browsers' GPU sandboxes allow file-map-executable there, judged on the
# view's mount path). Needs a 0074 kernel and sheng-gpu-addon=1: without the App view the links dangle (no Metal device).
GPU_LINK_DIR = 'links'


def gpu_app2_layer(unit, name, files):
    """FILES: [(path inside the bundle, bytes)] of the plugin bundle NAME (Contents/Info.plist, Contents/MacOS/<file>, ...). Returns the
    rows [(f, data)] for staging_tree: first every file real at GPU_APP_LIBRARY/GPU_APP_DIR/<name>.bundle/<path> (staged
    files/<unit>/<path>, root:wheel, 0755 for a Mach-O, 0644 otherwise), then at GPU_ADDONS/<name>.bundle/<path> the same staged file
    for a non-Mach-O and, for a Mach-O, a link to GPU_APP_POINT/GPU_APP_DIR/<name>.bundle/<path> (kind link, staged
    files/<unit>/links/<path> holding the target, its sha256 the target's)."""
    if not GPU_ADDON_NAME.fullmatch(name):
        raise SystemExit(f'gpu_app2_layer: bundle name {name!r}')
    real, links = [], []
    for inside, data in sorted(files):
        if not re.fullmatch(r'Contents/(?:Info\.plist|(?:MacOS|Resources)/[A-Za-z0-9_+-][A-Za-z0-9._+-]*)', inside):
            raise SystemExit(f'gpu_app2_layer: {inside} is not a plugin bundle file')
        macho = data[:4] == b'\xcf\xfa\xed\xfe'
        staged = f'files/{unit}/{inside}'
        mode = '0755' if macho else '0644'
        row = {'unit': unit, 'name': inside.rsplit('/', 1)[1], 'owner': 'root:wheel', 'on': True, 'label': '-'}
        real.append(({**row, 'kind': 'file', 'mode': mode, 'staged': staged, 'sha256': sha(data),
                      'dest': f'{GPU_APP_LIBRARY}/{GPU_APP_DIR}/{name}.bundle/{inside}'}, data))
        if macho:
            target = f'{GPU_APP_POINT}/{GPU_APP_DIR}/{name}.bundle/{inside}'.encode()
            links.append(({**row, 'kind': 'link', 'mode': '0755', 'staged': f'files/{unit}/{GPU_LINK_DIR}/{inside}',
                           'sha256': sha(target), 'dest': f'{GPU_ADDONS}/{name}.bundle/{inside}'}, target))
        else:
            links.append(({**row, 'kind': 'file', 'mode': mode, 'staged': staged, 'sha256': sha(data),
                           'dest': f'{GPU_ADDONS}/{name}.bundle/{inside}'}, data))
    if not any(f['kind'] == 'link' for f, _ in links):
        raise SystemExit(f'gpu_app2_layer: {name}.bundle has no Mach-O')
    return real + links


def gpu_addon_request():
    """(mode, name) from PIANO_GPU_ADDON / PIANO_GPU_ADDON_NAME; mode None when unset."""
    mode = os.environ.get('PIANO_GPU_ADDON', '')
    name = os.environ.get('PIANO_GPU_ADDON_NAME', 'NativeAdrenoMetal')
    if not mode:
        return None, name
    if mode not in ('copy', 'move'):
        raise SystemExit(f'PIANO_GPU_ADDON={mode!r}: copy or move')
    if os.environ.get('PIANO_GPU_ADDON_PLACE', 'sle') not in ('sle', 'app', 'app2', 'view3'):
        raise SystemExit('PIANO_GPU_ADDON_PLACE: sle, app, app2 or view3')
    if not GPU_ADDON_NAME.fullmatch(name):
        raise SystemExit(f'PIANO_GPU_ADDON_NAME={name!r}: letters, digits, _ and - only')
    return mode, name


def seed_gpu_addon(mount, mode, name, trust, built):
    """The PIANO_GPU_ADDON step on the mounted seed (after seed_gpu_files, so a staged --gpu-bundle is what gets copied). Returns the
    record: mode, name, every file with its sha256 (and CDHash for a Mach-O), and whether the /Library/GPUBundles copy stays."""
    place = os.environ.get('PIANO_GPU_ADDON_PLACE', 'sle')
    source = mount / GPU_BUNDLES.lstrip('/') / f'{name}.bundle'
    link = mount / GPU_ADDONS.lstrip('/') / f'{name}.bundle'
    target = mount / GPU_APP_LIBRARY.lstrip('/') / GPU_APP_DIR / f'{name}.bundle' if place in ('app', 'app2') else link
    if not (source / 'Contents/Info.plist').is_file():
        raise SystemExit(f'PIANO_GPU_ADDON: the seed has no {GPU_BUNDLES}/{name}.bundle (Contents/Info.plist)')
    reinstalled = [f['dest'] for f, _ in built if f['dest'].startswith(f'{GPU_BUNDLES}/{name}.bundle/')]
    if mode == 'move' and reinstalled:
        raise SystemExit(f'PIANO_GPU_ADDON=move: the boot installer would put {GPU_BUNDLES}/{name}.bundle back ({reinstalled[0]}); '
                         'use copy, or stage the plugin without --gpu-bundle')
    have = common.trust_hashes(trust)
    files = []
    for path in sorted(p for p in source.rglob('*') if p.is_file()):
        cd = soft_metal().cdhash(path)
        if cd is not None and bytes(cd) not in have:
            raise SystemExit(f'PIANO_GPU_ADDON: {path.relative_to(mount)} has CDHash {bytes(cd).hex()}, not in the trust cache')
        files.append({'path': path.relative_to(source).as_posix(), 'sha256': common.sha(path),
                      'cdhash': bytes(cd).hex() if cd is not None else None})
    for old in {target, link}:
        if old.is_symlink():
            old.unlink()
        elif old.exists():
            shutil.rmtree(old)
    target.parent.mkdir(parents=True, exist_ok=True)
    for directory in (target.parent.parent, target.parent):
        os.chmod(directory, 0o755)
    if place in ('app', 'app2'):
        link.parent.mkdir(parents=True, exist_ok=True)
        os.chmod(link.parent, 0o755)
    sh(['cp', '-cR', str(source), str(target)], timeout=120, check=True)
    # the seed's com.apple.macl tag stays with the original; this macOS puts its com.apple.provenance tag back on what it writes
    # (the recipe seed's plugin carries it too)
    sh(['xattr', '-cr', str(target)], timeout=60, check=False)
    for path in [target, *target.rglob('*')]:
        if path.is_dir():
            os.chmod(path, 0o755)
    for f in files:
        if common.sha(target / f['path']) != f['sha256']:
            raise SystemExit(f'PIANO_GPU_ADDON: {GPU_ADDONS}/{name}.bundle/{f["path"]} differs from the seed\'s plugin')
    record = {'mode': mode, 'name': name, 'dest': f'{GPU_ADDONS}/{name}.bundle', 'boot_arg': 'sheng-gpu-addon=1',
              'gpu_bundles_copy_kept': mode == 'copy', 'files': files, 'place': place}
    if place == 'app2':
        # cryptex1-qual2..4: with the whole bundle a symlink, late Metal clients (system_profiler, Safari) never dlopen it (discovery
        # refuses the resolved bundle). app2 keeps the bundle a real directory in the /S/L/E view (what Metal's discovery and trust check
        # see) and makes only its Mach-Os symlinks into the App-cryptex view (what dyld maps: the sandboxes' realpath rule).
        view = f'{GPU_APP_POINT}/{GPU_APP_DIR}/{name}.bundle'
        for path in sorted(p for p in target.rglob('*') if p.is_file()):
            rel = path.relative_to(target)
            dest = link / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            for d in [dest.parent, *dest.parent.parents]:
                if d == link.parent: break
                os.chmod(d, 0o755)
            if soft_metal().cdhash(path) is not None:
                os.symlink(f'{view}/{rel.as_posix()}', dest)
            else:
                shutil.copy2(path, dest); os.chmod(dest, 0o644)
        record.update({'real': f'{GPU_APP_LIBRARY}/{GPU_APP_DIR}/{name}.bundle', 'link': f'{GPU_ADDONS}/{name}.bundle: Mach-Os -> {view}',
                       'kernel': '0074 (app-library view)'})
    if place == 'app':
        view = f'{GPU_APP_POINT}/{GPU_APP_DIR}/{name}.bundle'
        os.symlink(view, link)
        if os.readlink(link) != view:
            raise SystemExit(f'PIANO_GPU_ADDON_PLACE=app: {link} is not a symlink to {view}')
        record.update({'real': f'{GPU_APP_LIBRARY}/{GPU_APP_DIR}/{name}.bundle', 'link': f'{GPU_ADDONS}/{name}.bundle -> {view}',
                       'kernel': '0074 (app-library view)'})
    if place == 'view3':
        # LT-B3 control (ANALYSIS.md 11, cryptex1-ctl2): the plugin stays a REAL bundle in the /S/L/E view (the layout Safari's WebGPU works
        # with, ctl1), and a copy goes to the app-library source so kernel 0074 still mounts its view over App/System/Library. Safari WebGPU
        # false here = the view itself (over WebKit's own directory) costs it; true = the plugin's code resolving into the view does.
        copy = mount / GPU_APP_LIBRARY.lstrip('/') / GPU_APP_DIR / f'{name}.bundle'
        if copy.exists():
            shutil.rmtree(copy)
        copy.parent.mkdir(parents=True, exist_ok=True)
        for directory in (copy.parent.parent, copy.parent):
            os.chmod(directory, 0o755)
        sh(['cp', '-cR', str(target), str(copy)], timeout=120, check=True)
        sh(['xattr', '-cr', str(copy)], timeout=60, check=False)
        for f in files:
            if common.sha(copy / f['path']) != f['sha256']:
                raise SystemExit(f'PIANO_GPU_ADDON_PLACE=view3: {copy}/{f["path"]} differs')
        record.update({'view_copy': f'{GPU_APP_LIBRARY}/{GPU_APP_DIR}/{name}.bundle (not used by Metal: the view only)',
                       'kernel': '0074 (app-library view)'})
    if mode == 'move':
        shutil.rmtree(source)
    return record


def seed_camera_apps(mount, staging, built):
    """The origin app must exist when sysextd decodes its container at startup."""
    installed = []
    applications = mount / 'Applications'
    for f, _ in built:
        if f['unit'] != 'piano-camera-apps':
            continue
        source = mount / staging / f['staged']
        target = mount / f['dest'].lstrip('/')
        parent = target.parent.relative_to(applications)
        for relative in reversed([parent, *parent.parents]):
            directory = applications / relative
            existed = directory.exists()
            directory.mkdir(exist_ok=True)
            if not stat.S_ISDIR(directory.lstat().st_mode):
                raise SystemExit(f'{directory.relative_to(mount)}: expected a camera directory')
            if directory != applications or not existed:
                directory.chmod(0o755)
        if target.is_symlink():
            raise SystemExit(f'{f["dest"]}: expected a regular camera file')
        sh(['cp', '-c', str(source), str(target)], timeout=60, check=True)
        os.chmod(target, int(f['mode'], 8))
        if sha(target.read_bytes()) != f['sha256']:
            raise SystemExit('seed camera app differs from final signed package')
        installed.append(f['dest'])
    return installed


def check_camera_seed_directories(mount, apps):
    """Verify the public path with ownership enabled, after the metadata writes."""
    applications = mount / 'Applications'
    for directory in (applications, *apps, *(p for app in apps for p in app.rglob('*') if p.is_dir())):
        st = directory.lstat()
        # The recipe already has a public /Applications with mode 0775.
        # Preserve its mode while requiring 0755 inside each signed bundle.
        modes = (0o755, 0o775) if directory == applications else (0o755,)
        if (not stat.S_ISDIR(st.st_mode) or (st.st_uid, st.st_gid) != (0, 0)
                or stat.S_IMODE(st.st_mode) not in modes):
            expected = ' or '.join(f'{mode:04o}' for mode in modes)
            raise SystemExit(f'{directory.relative_to(mount)}: expected root:wheel camera directory with mode {expected}')


def camera_seed_ownership(raw, mount):
    """Reopen only the temporary seed with ownership enabled; persist real root IDs.

    A noowners mount stores new files as uid 99. sysextd checks its staging
    area's owner/mode before serving requests, so a late boot chown is a race.
    The host already supports sudo -n (also used by warm-seed.py); never prompt.
    """
    entities = sh(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-owners', 'on', '-nobrowse',
                   '-mountpoint', str(mount), '-plist', str(raw)], timeout=120, check=True, capture_output=True).stdout
    attached = plistlib.loads(entities)['system-entities']
    whole = min((e['dev-entry'] for e in attached), key=lambda d: (len(d), d))
    try:
        if not any(e.get('mount-point') == str(mount.resolve()) for e in attached):
            raise SystemExit('camera seed ownership: wrong mounted volume')
        paths = [mount / p for p in ('Library/SystemExtensions', 'Applications/PianoCamera.app',
                                     'Applications/PianoCameraCapture.app')]
        # /Applications can also be newly created on a sparse Data seed.
        sh(['sudo', '-n', 'chown', '0:0', str(mount / 'Applications')], timeout=60, check=True)
        sh(['sudo', '-n', 'chown', '-R', '0:0', *map(str, paths)], timeout=60, check=True)
        sh(['sudo', '-n', 'xattr', '-cr', *map(str, paths)], timeout=60, check=True)
        for root in paths:
            for path in (root, *root.rglob('*')):
                st = path.lstat()
                if path.is_symlink() or (st.st_uid, st.st_gid) != (0, 0):
                    raise SystemExit('camera seed ownership did not persist as root:wheel')
        check_camera_seed_directories(mount, paths[1:])
    finally:
        sh(['hdiutil', 'detach', whole], timeout=120, check=True, stdout=subprocess.DEVNULL)


def add_boot_command(job, command, settime):
    """The seed's boot job, rewritten in place (same file, so the same owner): COMMAND right after settime.sh, once; the commands
    already there (stage-soft-metal.py --boot-run, --log-errors loops, which never end) stay, after it. Returns (script, changed)."""
    if not job.is_file():
        raise SystemExit(f'{job}: the seed has no boot job; a new plist would not be root\'s and launchd would refuse it')
    record = plistlib.loads(job.read_bytes())
    argv = record.get('ProgramArguments', [])
    if len(argv) != 3 or argv[:2] != ['/bin/sh', '-c'] or settime not in argv[2]:
        raise SystemExit(f'{job}: not the expected boot job ({argv}); check it by hand')
    if command in argv[2]:
        return argv[2], False
    script = argv[2].replace(settime, f'{settime}; {command}', 1)
    record['ProgramArguments'] = ['/bin/sh', '-c', script]
    job.write_bytes(plistlib.dumps(record, fmt=plistlib.FMT_XML))
    sh(['xattr', '-c', str(job)], timeout=60, check=False)
    return script, True


def sparsify_zero_ranges(raw):
    """Deallocate only ranges read back as entirely zero, before DBRDISK1 scans them.

    F_PUNCHHOLE's layout is the Darwin SDK's fpunchhole_t. File contents and
    logical size stay unchanged. This operates on our detached temporary raw
    image, never on the input seed or an attached device.
    """
    freed = 0
    with raw.open('r+b', buffering=0) as stream:
        for start, end in soft_metal().packed_ramdisk.data_ranges(stream, raw.stat().st_size):
            for offset in range(start, end, 8 << 20):
                stream.seek(offset)
                data = stream.read(min(8 << 20, end - offset))
                if data and not (offset % 4096 or len(data) % 4096) and data.count(0) == len(data):
                    fcntl.fcntl(stream.fileno(), 99, struct.pack('=IIqq', 0, 0, offset, len(data)))
                    freed += len(data)
        os.fsync(stream.fileno())
    return freed


def zero_seed_free_space(mount):
    """The existing compact-seed zero-fill method, inside our copied seed mount."""
    folder = next((mount / p for p in ('private/var/tmp', 'private/tmp')
                   if (mount / p).is_dir() and os.access(mount / p, os.W_OK)), None)
    if folder is None:
        raise SystemExit('seed compaction: no writable temporary directory in the copied volume')
    fill = folder / '.piano-userland-zero-fill'
    written = 0
    try:
        with fill.open('xb', buffering=0) as stream:
            zero = bytes(8 << 20)
            try:
                while True:
                    written += stream.write(zero)
            except OSError as error:
                if error.errno != errno.ENOSPC:
                    raise
            os.fsync(stream.fileno())
    finally:
        fill.unlink(missing_ok=True)
    sh(['sync'], timeout=60, check=True)
    return written


@contextlib.contextmanager
def mounted_seed(seed, out, result):
    """SEED's Data volume, mounted read-write from a sparse raw copy under OUT; packed again to OUT/scratch-data.img afterwards
    (RESULT gets packed_ramdisk.pack's metadata). stage-soft-metal.py's unpack/pack steps, in its order; the attach and mount are
    hdiutil's in one step (no diskutil: this Mac's diskutil mount hangs some days, hdiutil does not). Non-sudo like the old
    `diskutil mount`, so the volume ignores ownership the same way: the copies are stored as uid/gid 99 with their modes (inferred
    from the mount running as this uid either way; the staging's checks below would catch a mount of the wrong volume: the seed's
    boot job would be missing)."""
    ssm = soft_metal()
    logical, extents = ssm.packed_ramdisk.regions(seed.read_bytes())
    compact = os.environ.get('PIANO_USERLAND_COMPACT') == '1'
    raw_parent = D / 'out' if compact else out
    raw_parent.mkdir(parents=True, exist_ok=True)
    if compact and shutil.disk_usage(raw_parent).free < logical + (1 << 30):
        raise SystemExit('seed compaction: worktree out/ needs the logical seed size plus 1 GiB free')
    # The mount point remains under OUT (hdiutil's approved local mount path).
    # The potentially large zero-filled raw file belongs to this worktree's out/.
    with tempfile.TemporaryDirectory(prefix='seed-', dir=str(out)) as scratch, \
            tempfile.TemporaryDirectory(prefix='userland-raw-', dir=str(raw_parent)) as raw_scratch:
        raw = Path(raw_scratch) / 'seed.raw'
        with raw.open('wb') as handle:   # sparse: only the extents are written
            handle.truncate(logical)
            for offset, data in extents:
                handle.seek(offset)
                handle.write(data)
        mount = Path(scratch) / 'mnt'
        mount.mkdir()
        entities = sh(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-mountpoint', str(mount),
                       '-plist', str(raw)], timeout=300, check=True, capture_output=True).stdout
        attached = plistlib.loads(entities)['system-entities']
        disks = [e['dev-entry'] for e in attached]
        whole = min(disks, key=lambda d: (len(d), d)).removeprefix('/dev/')   # the image's disk, before its synthesized container
        detached = False
        # hdiutil reports the mount point through its realpath (/private/... for a /tmp/... request; confirmed on the host
        # 2026-11-14), so compare the resolved form of MOUNT:
        mount_real = str(mount.resolve())
        try:
            volume = next((e['dev-entry'].removeprefix('/dev/') for e in attached if e.get('mount-point') == mount_real), None)
            if volume is None:   # attached but nothing mounted here: refuse before any write
                raise SystemExit(f'{raw.name}: hdiutil attached the seed but mounted no volume at {mount}; refusing to stage')
            try:
                yield mount, volume
                if compact:
                    result['zero_filled_bytes'] = zero_seed_free_space(mount)
            finally:
                sh(['hdiutil', 'detach', '/dev/' + whole], timeout=120, check=True, stdout=subprocess.DEVNULL)
                detached = True
        finally:
            if not detached:   # the refusal above, or a detach that failed: force the image off so nothing is left attached
                sh(['hdiutil', 'detach', '-force', '/dev/' + whole], timeout=120, check=False, stdout=subprocess.DEVNULL)
        if result.get('camera_ownership'):
            camera_seed_ownership(raw, mount)
        if compact:
            result['zero_sparsified_bytes'] = sparsify_zero_ranges(raw)
        packed = out / 'scratch-data.img.new'
        packed.unlink(missing_ok=True)
        result.update(ssm.packed_ramdisk.pack(raw, packed))
        os.replace(packed, out / 'scratch-data.img')


def add_trust(trust_bytes, cdhashes):
    """The trust cache with the CDHashes it lacks as one more module first (stage-soft-metal.py's trust_bundle); unchanged when it
    has them all. Returns (bytes, added hex list)."""
    ssm = soft_metal()
    with tempfile.NamedTemporaryFile() as handle:
        handle.write(trust_bytes)
        handle.flush()
        have = common.trust_hashes(handle.name)
    need = sorted({bytes.fromhex(h) for h in cdhashes} - have)
    if not need:
        return trust_bytes, []
    return ssm.trust_bundle(need, trust_bytes), [h.hex() for h in need]


def files_record(built):
    merged_readers = merged_reader_destinations({f['dest']: data for f, data in built})
    return [{'unit': f['unit'], 'dest': f['dest'], 'kind': f['kind'], 'label': f.get('label', '-'), 'on': f['on'],
             'owner': f['owner'], 'mode': f['mode'], 'build': f['build'], 'from': rel(f['path']), 'sha256': f['sha256'],
             'cdhash': f['cdhash'],
             **({'merged_bootargs': True} if f['dest'] in merged_readers else {})} for f, _ in built]


def store_notes(identity, input_identity, seed):
    """What this seed means for the tablet's Data store (docs/data-store.md), as lines."""
    installed, source = common.installed_seed()
    lines = [f'seed identity {identity[:16]} (input {input_identity[:16]}): a new seed, as every restage makes.']
    if installed and common.same_seed(identity, installed):
        lines.append(f'it is the installed seed ({source}).')
    else:
        lines += [f'the installed seed is {(installed or "unknown")[:16]} ({source}). An ESP or RAM image with this seed:',
                  '  - store of the installed seed, not flattened: kept untouched, Data VOLATILE (these files installed every boot, '
                  'gone at the next); build-esp.py --replace-data would start the store over from this seed (the owner\'s Data '
                  'changes dropped)',
                  '  - flattened (self-contained) store: resumed, the seed NOT read: none of these files reach Data; use the live '
                  'update (docs/userland.md section 7)',
                  '  - blank store, data-store-replace, sheng-data-reset: a new store from this seed: everything installed at its '
                  'first boot and kept']
    if seed['pages'] != common.INSTALLED_STORE_PAGES:
        lines.append(f'note: {seed["pages"]} pages, not the installed store\'s {common.INSTALLED_STORE_PAGES}: build-esp.py and '
                     'build-ram-images.py refuse data-persist=2 for it (docs/data-store.md section 8)')
    return lines


# ---- the modes ---------------------------------------------------------------------------------------------------------------

def stage_seed(a, plan):
    seed_in, trust_in, out = a.seed.resolve(), a.trust.resolve(), a.out.resolve()
    if (out / 'scratch-data.img') in (seed_in,) or (out / 'kernel.macho.trustcache') in (trust_in,):
        raise SystemExit(f'--out {rel(out)} would overwrite the input seed or trust cache: stage into another directory')
    for path in (seed_in, trust_in):
        if not path.is_file():
            raise SystemExit(f'{rel(path)}: no such file')
    out.mkdir(parents=True, exist_ok=True)
    bake = plan.get('camera_bake')
    if bake and sh(['sudo', '-n', 'true'], timeout=10, capture_output=True).returncode:
        raise SystemExit('camera approval bake requires noninteractive sudo for ownership on the copied seed')
    built = build_all(plan, out.parent / 'build')
    tree = staging_tree(plan, built)
    command = boot_wifi_profile(tree, boot_command(plan))
    addon_mode, addon_name = gpu_addon_request()
    seed_info_in = common.seed_info(seed_in)
    trust_bytes = trust_in.read_bytes()
    trusted_cdhashes = sorted({f['cdhash'] for f, _ in built if f['cdhash']})
    stamp = sha(json.dumps({'format': FORMAT, 'seed': seed_info_in['file_sha256'], 'trust': sha(trust_bytes), 'boot': command,
                            'compact_free_space': os.environ.get('PIANO_USERLAND_COMPACT') == '1',
                            'camera_approval': bake['record'] if bake else None,
                            **({'camera_directory_mode': '0755'} if bake else {}),
                            'trusted_cdhashes': trusted_cdhashes,
                            **({'gpu_addon': [addon_mode, addon_name]} if addon_mode else {}),
                            'staging': plan['staging'], 'tree': {k: [sha(v), m] for k, (v, m) in sorted(tree.items())}},
                           sort_keys=True).encode())
    record_path, seed_out, trust_out = out / 'userland.json', out / 'scratch-data.img', out / 'kernel.macho.trustcache'
    try:
        record = json.loads(record_path.read_text())
    except (OSError, ValueError):
        record = {}
    if not a.force and (out / 'stamp').is_file() and (out / 'stamp').read_text().strip() == stamp and seed_out.is_file() and \
            trust_out.is_file() and common.sha(seed_out) == record.get('outputs', {}).get('seed', {}).get('file_sha256') and \
            common.sha(trust_out) == record.get('outputs', {}).get('trust', {}).get('sha256'):
        identity = record['outputs']['seed']['identity_sha256']
        print(f'{rel(seed_out)}: up to date (stamp {stamp[:16]}): seed identity {identity[:16]} unchanged; '
              f'{rel(trust_out)} unchanged')
        return record

    trust_new, added = add_trust(trust_bytes, trusted_cdhashes)
    tmp = trust_out.with_name(trust_out.name + '.new')
    tmp.write_bytes(trust_new)
    os.replace(tmp, trust_out)
    print(f'{rel(trust_out)}: ' + (f'+ {len(added)} CDHash(es) {" ".join(added)}' if added else 'the input\'s CDHashes already '
                                     'cover every Mach-O: unchanged'))

    camera_files = [f for f, _ in built if f['unit'] == 'piano-camera-apps']
    if camera_files:
        have = common.trust_hashes(trust_out)
        for f in camera_files:
            cdhash = f['verified_cdhash']
            if cdhash and (bytes.fromhex(cdhash) in have) != f['trust']:
                raise SystemExit(f'{f["dest"]}: camera CDHash has unexpected trust-cache membership')

    ssm = soft_metal()
    packed, staging = {}, plan['staging'].lstrip('/')
    (out / 'stamp').unlink(missing_ok=True)   # a run cut short below leaves no stamp: the next one restages
    with mounted_seed(seed_in, out, packed) as (mount, volume):
        written, removed = sync_tree(mount / staging, tree)
        if camera_files:
            for f in camera_files:
                if common.sha(mount / staging / f['staged']) != f['sha256']:
                    raise SystemExit(f'{f["dest"]}: staged camera bytes differ')
            print('Camera app seals, staged bytes and trust-cache policy verified')
        gpu_files = seed_gpu_files(mount, staging, built)
        addon = seed_gpu_addon(mount, addon_mode, addon_name, trust_out, built) if addon_mode else None
        camera_files = seed_camera_apps(mount, staging, built) if bake else []
        if bake:
            plan['camera_bake_module'].seed(mount, bake)
            packed['camera_ownership'] = True
        script, changed = add_boot_command(mount / ssm.BOOT_JOB, command, ssm.SETTIME)
        listing = sh(['ls', '-lnR', str(mount / staging)], timeout=60, capture_output=True, text=True).stdout
        listing = listing.replace(str(mount), '')
        print(f'{volume}: /{staging}: {len(written)} written, {len(removed)} removed{": " + " ".join(removed) if removed else ""}\n'
              f'{listing.strip()}')
        print(f'seed GPU files ready before launchd: {len(gpu_files)}')
        if addon:
            print(f'seed GPU addon ({addon["mode"]}): {addon["dest"]}, {len(addon["files"])} files; boot with '
                  f'{addon["boot_arg"]}' + ('' if addon['gpu_bundles_copy_kept'] else f'; {GPU_BUNDLES}/{addon_name}.bundle removed'))
        print(f'the boot job {"now runs" if changed else "already ran"}: {script}')
    seed_info_out = common.seed_info(seed_out)
    record = {
        'format': FORMAT, 'stamp': stamp, 'manifest': rel(plan['path']), 'staging': plan['staging'], 'boot_command': command,
        'inputs': {'seed': {'path': rel(seed_in), **{k: seed_info_in[k] for k in ('file_sha256', 'identity_sha256', 'logical_bytes')}},
                   'trust': {'path': rel(trust_in), 'sha256': sha(trust_bytes)}},
        'files': files_record(built),
        'seed_gpu_files': gpu_files,
        **({'seed_gpu_addon': addon} if addon else {}),
        'seed_camera_files': camera_files,
        'camera_approval': bake['record'] if bake else None,
        'compaction': {key: packed[key] for key in ('zero_filled_bytes', 'zero_sparsified_bytes') if key in packed},
        'planned': [u['name'] for u in plan['units'] if u['planned']],
        'outputs': {'seed': {'path': rel(seed_out), **{k: seed_info_out[k] for k in ('file_sha256', 'identity_sha256',
                                                                                     'logical_bytes', 'bytes')}},
                    'trust': {'path': rel(trust_out), 'sha256': common.sha(trust_out), 'added_cdhashes': added}},
    }
    record_path.write_text(json.dumps(record, indent=1) + '\n')
    (out / 'stamp').write_text(stamp + '\n')
    print(f'{rel(seed_out)}: {seed_info_out["bytes"]} bytes packed, logical {seed_info_out["logical_bytes"]}, identity '
          f'{seed_info_out["identity_sha256"][:16]} (input {seed_info_in["identity_sha256"][:16]})')
    print('\n'.join(store_notes(seed_info_out['identity_sha256'], seed_info_in['identity_sha256'], seed_info_out)))
    print(f'{rel(record_path)}: the record; then e.g. PIANO_SEED={rel(seed_out)} PIANO_TRUST={rel(trust_out)} '
          'tooling/k27/build-native-image.sh, or build-esp.py --seed/--trust with them')
    return record


def deterministic_tar(tree, prefix):
    """TREE as a tar archive: sorted, owner root:wheel, time 0, the modes of TREE, directories 0755."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        dirs = sorted({'/'.join(k.split('/')[:i]) for k in tree for i in range(1, k.count('/') + 1)})
        for name in ['', *dirs]:
            info = tarfile.TarInfo(f'{prefix}/{name}'.rstrip('/'))
            info.type, info.mode, info.mtime, info.uname, info.gname = tarfile.DIRTYPE, 0o755, 0, 'root', 'wheel'
            tar.addfile(info)
        for name, (data, mode) in sorted(tree.items()):
            info = tarfile.TarInfo(f'{prefix}/{name}')
            info.size, info.mode, info.mtime, info.uname, info.gname = len(data), mode, 0, 'root', 'wheel'
            tar.addfile(info, io.BytesIO(data))
    return buffer.getvalue()


APPLY = '''#!/bin/sh
# piano-userland apply-update.sh (written by tooling/userland/stage.py --live; docs/userland.md section 7): on the tablet, as root,
# with userland-update.tar next to it. Checks the archive, swaps it in as {staging} (the one before stays as .prev)
# and runs its install.sh --live detached, so a restart of the shell it came through does not stop it.
#   sh apply-update.sh [ARCHIVE]        then: cat /private/var/db/piano-userland/live.log
set -eu
A=${{1:-$(dirname "$0")/userland-update.tar}}
[ "$(id -u)" = 0 ] || {{ echo "apply-update.sh: run as root" >&2; exit 1; }}
[ "$(shasum -a 256 "$A" | cut -d' ' -f1)" = {tar_sha} ] || {{ echo "apply-update.sh: $A is not the archive this script was made for" >&2; exit 1; }}
S={staging}
rm -rf "$S.new" && mkdir -p "$S.new"
tar -xpf "$A" -C "$S.new" --strip-components 1
(cd "$S.new" && shasum -a 256 -c -s SHA256SUMS)
rm -rf "$S.prev"
[ ! -d "$S" ] || mv "$S" "$S.prev"
mv "$S.new" "$S"
mkdir -p /private/var/db/piano-userland
nohup sh {staging}/install.sh --live > /private/var/db/piano-userland/live.log 2>&1 < /dev/null &
echo "apply-update.sh: {staging} replaced; install.sh --live runs (pid $!): /private/var/db/piano-userland/live.log"
'''


def stage_live(a, plan):
    """The live update's bundle (docs/userland.md section 7): the same staging tree, as a deterministic tar and its apply script,
    and the trust check against the trust cache the running kernel has (the installed ESP's)."""
    if plan.get('camera_bake'):
        raise SystemExit('Camera approval baking is seed-only; it cannot modify the installed system through --live')
    if gpu_addon_request()[0]:
        raise SystemExit('PIANO_GPU_ADDON is seed-only (a RAM test of the /System/Library/Extensions view); not in a live layer')
    out = a.live.resolve()
    out.mkdir(parents=True, exist_ok=True)
    built = build_all(plan, out.parent / 'build')
    tree = staging_tree(plan, built)
    sync_tree(out / 'staging', tree)
    archive = deterministic_tar(tree, 'userland')
    (out / 'userland-update.tar').write_bytes(archive)
    apply = APPLY.format(staging=plan['staging'], tar_sha=sha(archive))
    (out / 'apply-update.sh').write_text(apply)
    os.chmod(out / 'apply-update.sh', 0o755)
    installed = a.installed_trust or common.installed_trust()
    report = {'format': FORMAT, 'manifest': rel(plan['path']), 'staging': plan['staging'], 'archive_sha256': sha(archive),
              'files': files_record(built), 'installed_trust': rel(installed) if installed else None,
              'installed_trust_sha256': common.sha(installed) if installed else None}
    print(f'{rel(out / "userland-update.tar")}: {len(archive)} bytes, sha256 {sha(archive)[:16]}; {rel(out / "apply-update.sh")}')
    machos = [f for f, _ in built if f['cdhash']]
    if not installed:
        print('the installed trust cache is unknown (--installed-trust, or PIANO_INSTALLED_SEED=<the ESP record>.json): which '
              'Mach-Os the running kernel trusts cannot be told; ' + ', '.join(f['dest'] for f in machos))
    else:
        have = common.trust_hashes(installed)
        new = [f for f in machos if bytes.fromhex(f['cdhash']) not in have]
        for f in machos:
            print(f'  {f["dest"]}: CDHash {f["cdhash"][:16]} ' + ('in the installed trust cache: live' if f not in new else
                                                                 'NOT in the installed trust cache'))
        report['untrusted'] = [f['dest'] for f in new]
        if new:
            trust_new, added = add_trust(installed.read_bytes(), [f['cdhash'] for f in new])
            (out / 'kernel.macho.trustcache').write_bytes(trust_new)
            report['trust'] = {'path': rel(out / 'kernel.macho.trustcache'), 'sha256': sha(trust_new), 'added_cdhashes': added}
            print(f'{rel(out / "kernel.macho.trustcache")}: the installed trust cache + {len(added)} CDHash(es). An ESP with it '
                  'and the INSTALLED seed (the store\'s base: the same identity, the store resumes) makes them trusted from its first '
                  'boot: tooling/persist/build-esp.py --profile daily --layer ' + rel(out) +
                  ' (the daily profile takes the pinned base; docs/data-store.md section 9, docs/userland.md section 7)')
    (out / 'live.json').write_text(json.dumps(report, indent=1) + '\n')
    return report


def show_plan(plan):
    print(f'{rel(plan["path"])}: staging {plan["staging"]}')
    for u in plan['units']:
        state = 'planned' if u['planned'] else ('on' if u['on'] else 'off')
        print(f'{u["name"]} [{state}]: {u["about"]}')
        for f in u['files']:
            how = {'copy': f'copy of {f["sources"][0] if f["sources"] else "?"}',
                   'cc': f'cc {f["arch"]} {" ".join(f["sources"])}' + (f' -framework {" ".join(f["frameworks"])}' if f['frameworks']
                                                                       else ''),
                   'script': f'{" ".join(f["command"])} -> {f["output"]}'}[f['build']]
            print(f'  {f["dest"]}  {f["owner"]} {f["mode"]}  {f["kind"]}  {how}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--manifest', type=Path, default=D / 'configs/userland.toml')
    p.add_argument('--seed', type=Path, default=D / 'out/turnip-a830/stage/scratch-data.img')
    p.add_argument('--trust', type=Path, default=D / 'out/turnip-a830/stage/kernel.macho.trustcache')
    p.add_argument('--out', type=Path, default=D / 'out/userland/stage')
    p.add_argument('--on', action='append', default=[], metavar='UNIT', help='this unit\'s job on in this build')
    p.add_argument('--off', action='append', default=[], metavar='UNIT', help='this unit\'s job off (Disabled) in this build')
    p.add_argument('--force', action='store_true', help='restage even when the stamp says the outputs are up to date')
    p.add_argument('--live', type=Path, metavar='DIR', help='write a live update\'s bundle to DIR instead of a seed')
    p.add_argument('--installed-trust', type=Path, help='the trust cache of the installed ESP (default: from its record)')
    p.add_argument('--plan', action='store_true', help='check the manifest and print it; build nothing')
    p.add_argument('--gpu-bundle', action='append', default=[], type=Path, metavar='BUNDLE',
                   help='a Metal plugin bundle as one more unit (/Library/GPUBundles/<name>.bundle), its Mach-Os trusted')
    p.add_argument('--hal-bundle', action='append', default=[], type=Path, metavar='DRIVER',
                   help='a Core Audio HAL plug-in as one more unit (/Library/Audio/Plug-Ins/HAL/<name>.driver), its Mach-Os trusted')
    a = p.parse_args()
    plan = add_camera_package(add_bluetooth_runtime(add_hal_bundles(add_gpu_bundles(load_manifest(a.manifest, a.on, a.off), a.gpu_bundle),
                                                                    a.hal_bundle)))
    if a.plan:
        show_plan(plan)
    elif a.live:
        stage_live(a, plan)
    else:
        stage_seed(a, plan)


if __name__ == '__main__':
    main()
