#!/usr/bin/env python3
"""Host-only test of maint-session.py's window-capture step (T-20 v2): the real
maint-session.py in execute mode on test-persist.py's fake tablet (a sparse LUN 0 with the real GPT), its linux_rootfs holding a SHNGROOT
manifest with two windows OS and SystemOS (made-up bytes, each starting with an APFS container superblock magic) and a root.json for
them. No tablet, no fastboot, no backup disk (a temporary directory stands in).

1. the capture: both window files = the windows' bytes; CAPTURE.json's sha256 = theirs; the pieces (1 MiB each here) recorded; nothing
   on the fake tablet changed;
2. a stream that breaks once (a flaky link): the piece is tried again and the capture passes;
3. a source that changes under the read (every send of one piece sees other bytes than the hash before it): the step stops after three
   tries (exit 3) and records no CAPTURE.json for that run; run again with the source still: it resumes (earlier pieces kept) and passes;
4. a root manifest that is not root.json's (its sha256, or a window's offset): refused before any piece is read;
5. a window without the NXSB magic: refused;
6. (--flow, needs the pool package out/pool/install) tooling/pool/pool-capture.sh end to end with POOL_FAKE=1: the windows are real
   APFS images (made by hdiutil, no root), the session is this real maint-session.py on the fake tablet, the capture then goes through
   pool-capture-check.sh (attached read-only: the volume UUIDs); the lock held refuses; nothing on the fake tablet changes.

  test-window-capture.py [--keep DIR] [--flow | --boundaries-only]
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

spec = importlib.util.spec_from_file_location('tp', HERE / 'test-persist.py')
tp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tp)
B, MiB = common.LBA, 1 << 20
failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what, flush=True)
    if not ok:
        failures.append(what)


WINDOWS = (('OS', MiB, 3 * MiB), ('SystemOS', 4 * MiB, 2 * MiB))

SHELL = r'''import os, sys
sys.path.insert(0, {here!r})
import importlib.util
spec = importlib.util.spec_from_file_location("t", {tp!r})
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
cmd = sys.argv[1]
moving = os.environ.get("FAKE_MOVING")   # SKIP: every send of the piece at MiB SKIP finds a changed byte first (a writer)
if moving and " send " in cmd and cmd.split()[4] == moving:
    lun0 = os.path.join(os.environ["FAKE_ROOT"], "lun0.img")
    nodes = m.fake_nodes(__import__("pathlib").Path(os.environ["FAKE_ROOT"]))
    _, off, _ = nodes[cmd.split()[3]]
    at = off + int(moving) * (1 << 20) + 4096 + 7
    with open(lun0, "r+b") as f:
        f.seek(at); b = f.read(1); f.seek(at); f.write(bytes([(b[0] + 1) & 255]))
sys.stdout.write(m.fake_shell(cmd))
'''


def build(tmp):
    root, out, _, _ = tp.build_fake(tmp)
    lun0 = root / 'lun0.img'
    base = common.ROOTFS_FIRST * B
    manifest = bytearray(B)
    manifest[:8] = b'SHNGROOT'
    struct.pack_into('<IIQ', manifest, 8, 1, len(WINDOWS), 6 * MiB)
    layout = []
    for i, (name, off, size) in enumerate(WINDOWS):
        data = bytearray(os.urandom(size))
        data[32:36] = b'NXSB'
        tp.write_at(lun0, base + off, bytes(data))
        digest = hashlib.sha256(data).hexdigest()
        struct.pack_into('<32sQQII32s40s', manifest, 24 + 128 * i, name.encode(), off, size, 512, 1, bytes.fromhex(digest), bytes(40))
        layout.append({'name': name, 'offset': off, 'size': size, 'sha256': digest})
    manifest[4064:] = hashlib.sha256(bytes(manifest[:4064])).digest()
    tp.write_at(lun0, base, bytes(manifest))
    rj = tmp / 'root.json'
    rj.write_text(json.dumps({'manifest_sha256': hashlib.sha256(manifest).hexdigest(), 'windows': layout}, indent=1))
    return root, out, rj


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keep', type=Path)
    modes = ap.add_mutually_exclusive_group()
    modes.add_argument('--flow', action='store_true')
    modes.add_argument('--boundaries-only', action='store_true', help='run refusals that need no backups or loopback')
    a = ap.parse_args()
    tmp = a.keep or Path(tempfile.mkdtemp(prefix='piano-capture-test-'))
    tmp.mkdir(parents=True, exist_ok=True)
    try:
        test_capture_boundaries()
        if not a.boundaries_only:
            run_tests(tmp)
        if a.flow:
            run_flow(tmp / 'flow')
    finally:
        if not a.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    sys.exit(1 if failures else 0)


def test_capture_boundaries():
    print('== window-capture refuses unsafe maps before reading a window')
    spec = importlib.util.spec_from_file_location('maint_session', HERE / 'maint-session.py')
    maint = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(maint)

    for part, node, mounted, reason in (
        ('boot_a', 'sda35', '0', 'a supported root partition'),
        ('esp', 'sda35', '0', 'a supported root partition'),
        ('linux_rootfs', 'sda35', '1', 'nothing from UFS is mounted'),
        ('linux_rootfs', 'sde13', '0', 'a mapped partition of LUN 0'),
    ):
        session = object.__new__(maint.Session)
        session.execute = True
        session.esp = {'default': 'fixture', 'cmdlines': {'fixture': f'native-ufs-root-part={part}'}}
        session.log = lambda _: None
        session.save = lambda **_: None

        def mapped(command):
            assert command == 'map'
            return {'lun0': 'sda', 'linux_rootfs': f'{node} 8192 16384', 'mounted': mounted}

        session.t = mapped
        try:
            maint.window_capture(session)
        except maint.Stop as error:
            check(reason in str(error), f'{part}, {node}, mounted={mounted}: refused before the first read')
        else:
            check(False, f'{part}, {node}, mounted={mounted}: unsafe map was accepted')


def run_tests(tmp):
    print('== maint-session.py window-capture on a fake tablet')
    root, out, rj = build(tmp)
    lun0 = root / 'lun0.img'
    nas = tmp / 'nas'
    nas.mkdir()
    (tmp / 'fake-shell.py').write_text(SHELL.format(here=str(HERE), tp=str(HERE / 'test-persist.py')))
    env = dict(os.environ, FAKE_ROOT=str(root), PIANO_PERSIST_OUT=str(out), PIANO_MAINT_HOST='127.0.0.1', PIANO_BACKUP_DIR=str(nas),
               PIANO_SERIAL='test', PIANO_MAINT_SHELL=str(tmp / 'fake-shell.py'), PYTHONDONTWRITEBYTECODE='1', PIANO_JOB_POLL='0.1')
    for k in ('FAKE_SEND_FAIL', 'FAKE_MOVING'):
        env.pop(k, None)

    def ms(session, *extra, expect=0, rjson=rj, **over):
        argv = ['python3', str(HERE / 'maint-session.py'), '--session', str(tmp / session), '--esp', 'esp-test.img',
                '--http-port', '18061', '--nc-port', '19061', '--rootfs-piece-mib', '1', '--root-json', str(rjson),
                'map', 'window-capture', *extra]
        r = subprocess.run(argv, capture_output=True, text=True, env=dict(env, **over), timeout=900)
        log = r.stdout + r.stderr
        if r.returncode != expect:
            print(log[-3000:])
        check(r.returncode == expect, f'{session}: map window-capture {" ".join(extra)} {over or ""} -> {r.returncode} (want {expect})')
        return log, (json.loads((tmp / session / 'state.json').read_text()) if (tmp / session / 'state.json').is_file() else {})

    base = common.ROOTFS_FIRST * B
    before = tp.sha_region(lun0, 0, common.LUN0_BLOCKS * B) if False else tp.sha_region(lun0, base, 8 * MiB)
    log, st = ms('s1')
    cap = st.get('window_capture') or {}
    folder = nas / st.get('backup_name', '-') / 'windows'
    ok = bool(cap)
    for name, off, size in WINDOWS:
        stem = name.lower()
        f = folder / f'{stem}.img'
        want = tp.sha_region(lun0, base + off, size)
        w = cap.get('windows', {}).get(stem, {})
        ok = ok and f.is_file() and common.sha(f) == want == w.get('sha256') and w.get('size') == size and len(w.get('pieces', [])) == size // MiB
    check(ok, 'both windows captured: each file = the window\'s bytes = CAPTURE.json\'s sha256, one piece per MiB')
    rec = json.loads((folder / 'CAPTURE.json').read_text()) if (folder / 'CAPTURE.json').is_file() else {}
    check(rec.get('root_manifest_sha256') == json.loads(rj.read_text())['manifest_sha256'] and (folder / 'SHA256SUMS').is_file(),
          'CAPTURE.json names the root manifest; SHA256SUMS written')
    check(log.count('= received = read back without cache') == 5, 'five pieces, each: the tablet\'s sha256 = received = read back')
    check(tp.sha_region(lun0, base, 8 * MiB) == before, 'nothing written on the fake tablet')

    log, st = ms('s2', FAKE_SEND_FAIL=f'once:{WINDOWS[1][1] // MiB + 1}')
    check('try 1' in log and st.get('window_capture', {}).get('windows', {}).get('systemos', {}).get('sha256') ==
          tp.sha_region(lun0, base + WINDOWS[1][1], WINDOWS[1][2]), 'a stream broken once: tried again, the capture passes')

    moving = str(WINDOWS[0][1] // MiB + 2)
    log, st = ms('s3', expect=3, FAKE_MOVING=moving)
    check('failed 3 times' in log and 'window_capture' not in st and len((st.get('window_capture_progress') or {}).get('pieces', {})) == 2,
          'a source that changes under the read: stopped after three tries, no CAPTURE record, the two earlier pieces kept')
    log, st = ms('s3')
    check(log.count('verified in an earlier run') == 2 and st.get('window_capture', {}).get('windows', {}).get('os', {}).get('sha256') ==
          tp.sha_region(lun0, base + WINDOWS[0][1], WINDOWS[0][2]), 'run again with the source still: resumed, the capture = the window now')

    bad = tmp / 'root-bad.json'
    r = json.loads(rj.read_text())
    bad.write_text(json.dumps({**r, 'manifest_sha256': '0' * 64}))
    log, st = ms('s4', expect=3, rjson=bad)
    check('root manifest of' in log and 'FAIL' in log and 'window_capture' not in st and 'piece 0' not in log,
          'a root manifest that is not root.json\'s: refused before any piece')
    r2 = {**r, 'windows': [{**r['windows'][0], 'offset': 2 * MiB}, r['windows'][1]]}
    bad.write_text(json.dumps(r2))
    log, st = ms('s5', expect=3, rjson=bad)
    check("window OS: the manifest's" in log and 'piece 0' not in log, 'a window offset that is not root.json\'s: refused')
    tp.write_at(lun0, base + WINDOWS[1][1] + 32, b'XXXX')
    log, st = ms('s6', expect=3)
    check('NXSB' in log and 'FAIL' in log and 'window_capture' not in st, 'a window without the APFS container magic: refused')


def apfs_image(path, volname, mib):
    """A raw APFS container image of MIB MiB holding one volume (hdiutil, no root): (bytes, its volume UUID)."""
    dmg = path.with_suffix('.dmg')
    subprocess.run(['hdiutil', 'create', '-quiet', '-size', f'{mib}m', '-fs', 'APFS', '-volname', volname, '-layout', 'NONE', str(dmg)],
                   check=True, capture_output=True)
    subprocess.run(['hdiutil', 'convert', '-quiet', str(dmg), '-format', 'UDTO', '-o', str(path)], check=True, capture_output=True)
    raw = path.with_suffix('.cdr').read_bytes()
    att = subprocess.run(['diskutil', 'image', 'attach', '--readOnly', '--noMount', str(path.with_suffix('.cdr'))], capture_output=True,
                         text=True).stdout
    vol = next(l.split()[0] for l in att.splitlines() if 'Apple_APFS_Volume' in l)
    info = subprocess.run(['diskutil', 'info', '-plist', vol], capture_output=True, check=True).stdout
    import plistlib
    uuid = plistlib.loads(info)['VolumeUUID']
    subprocess.run(['hdiutil', 'detach', att.split()[0]], capture_output=True)
    assert len(raw) == mib * MiB, len(raw)
    return raw, uuid


def run_flow(tmp):
    print('== pool-capture.sh (POOL_FAKE=1): the real maint-session.py on the fake tablet, real APFS images as the windows')
    D = HERE.parents[1]
    pkg = Path(os.environ.get('POOL_PACKAGE', D / 'out/pool/install'))
    if not (pkg / 'PACKAGE.sha256').is_file():
        check(False, f'no pool package {pkg} (tooling/pool/mk-pool-package.sh)')
        return
    tmp.mkdir(parents=True)
    root, out, _, _ = tp.build_fake(tmp)
    lun0 = root / 'lun0.img'
    base = common.ROOTFS_FIRST * B
    imgs = {'OS': apfs_image(tmp / 'os-win', 'Macintosh HD', 24), 'SystemOS': apfs_image(tmp / 'sys-win', 'SystemOS', 16)}
    manifest = bytearray(B)
    manifest[:8] = b'SHNGROOT'
    layout, cursor = [], MiB
    for i, name in enumerate(('OS', 'SystemOS')):
        raw = imgs[name][0]
        tp.write_at(lun0, base + cursor, raw)
        digest = hashlib.sha256(raw).hexdigest()
        struct.pack_into('<32sQQII32s40s', manifest, 24 + 128 * i, name.encode(), cursor, len(raw), 512, 1, bytes.fromhex(digest), bytes(40))
        layout.append({'name': name, 'offset': cursor, 'size': len(raw), 'sha256': digest})
        cursor += len(raw)
    struct.pack_into('<IIQ', manifest, 8, 1, 2, cursor)
    manifest[4064:] = hashlib.sha256(bytes(manifest[:4064])).digest()
    tp.write_at(lun0, base, bytes(manifest))
    rj = tmp / 'root.json'
    rj.write_text(json.dumps({'manifest_sha256': hashlib.sha256(manifest).hexdigest(), 'windows': layout}))
    rec = json.loads((out / 'esp-test.json').read_text())
    rec.update(image='esp-k273-test.img', kind='k273-only', default='macos-27.3',
               cmdlines={'macos-27.3': 'native-ufs-root=1 native-ufs-root-part=linux_rootfs data-persist=2'})
    shutil.copyfile(out / 'esp-test.img', out / 'esp-k273-test.img')
    common.write_json(out / 'esp-k273-test.json', rec)
    common.write_json(out / 'installed.json', {'image': rec['image'], 'sha256': rec['sha256'], 'seed': rec['seed']})
    ledger = tmp / 'main-ledger'
    ledger.mkdir()
    shutil.copyfile(out / 'installed.json', ledger / 'installed.json')
    nas = tmp / 'nas'
    nas.mkdir()
    (tmp / 'fake-shell.py').write_text(SHELL.format(here=str(HERE), tp=str(HERE / 'test-persist.py')))
    seams = tmp / 'seams'
    seams.mkdir()
    ms = ['python3', str(HERE / 'maint-session.py'), '--session', str(tmp / 'rounds/NAME/maintenance'), '--http-port', '18071',
          '--nc-port', '19071', '--rootfs-piece-mib', '8']
    (seams / 'ms').write_text('#!/bin/bash\nlog=$1; shift\nS=$(dirname "$log")/maintenance\nexec python3 ' + str(HERE / 'maint-session.py') +
                              ' --session "$S" --http-port 18071 --nc-port 19071 --rootfs-piece-mib 8 "$@" > "$log" 2>&1\n')
    for n, body in {'rsudo': 'exit 0', 'go-abl': 'exit 0', 'disk-boot': 'exit ${FAKE_BACK_RC:-0}', 'live-boot': 'exit 0',
                    'battery': 'echo "macOS pmset -g batt: 80 % (minimum 30 %): ok"'}.items():
        (seams / n).write_text('#!/bin/sh\n' + body + '\n')
    for f in seams.iterdir():
        f.chmod(0o755)
    del ms
    profile = tmp / 'profile-daily.args'
    profile.write_text('native-ufs-root-part=linux_rootfs data-persist=2\n')
    gate_spec = tmp / 'base-gates.spec'
    gate_spec.write_text('NEED_ARGS="data-persist=2"\n')
    lock = tmp / 'tablet.lock'
    env = dict(os.environ, FAKE_ROOT=str(root), PIANO_PERSIST_OUT=str(out), PIANO_MAINT_HOST='127.0.0.1', PIANO_BACKUP_DIR=str(nas),
               PIANO_SERIAL='test', PIANO_MAINT_SHELL=str(tmp / 'fake-shell.py'), PYTHONDONTWRITEBYTECODE='1', PIANO_JOB_POLL='0.1',
               POOL_FAKE='1', POOL_RSUDO=str(seams / 'rsudo'), POOL_GO_ABL=str(seams / 'go-abl'), POOL_MS=str(seams / 'ms'),
               POOL_DISK_BOOT=str(seams / 'disk-boot'), POOL_LIVE_BOOT=str(seams / 'live-boot'), POOL_BATTERY=str(seams / 'battery'),
               POOL_MAIN_LEDGER=str(ledger), POOL_LOCK=str(lock), POOL_RUNS=str(tmp / 'rounds'), POOL_ROOT_JSON=str(rj),
               POOL_PACKAGE=str(pkg), POOL_PROFILE_ARGS=str(profile), POOL_BASE_GATE_SPEC=str(gate_spec), POOL_EXPECT_OS_UUID=imgs['OS'][1],
               POOL_EXPECT_SYSTEMOS_UUID=imgs['SystemOS'][1])
    for k in ('FAKE_SEND_FAIL', 'FAKE_MOVING'):
        env.pop(k, None)
    before = tp.sha_region(lun0, base, cursor)

    def run(name, expect, **over):
        r = subprocess.run(['bash', str(D / 'tooling/pool/pool-capture.sh'), name], capture_output=True, text=True,
                           env=dict(env, **over), timeout=900)
        log = (tmp / 'rounds' / name / 'pool-capture.log')
        text = log.read_text() if log.is_file() else r.stdout + r.stderr
        if r.returncode != expect:
            print(text[-3000:], r.stderr[-1500:])
        check(r.returncode == expect, f'pool-capture.sh {name} {over or ""} -> {r.returncode} (want {expect})')
        return text

    lock.mkdir()
    (lock / 'owner').write_text('another job\n')
    text = run('cap-locked1', 1)
    check('is held' in text and not (tmp / 'rounds/cap-locked1/maintenance').exists(), 'the tablet lock held: refused, no session')
    shutil.rmtree(lock)
    text = run('cap-flow1', 0)
    cap = json.loads((tmp / 'rounds/cap-flow1/CAPTURE.json').read_text()) if (tmp / 'rounds/cap-flow1/CAPTURE.json').is_file() else {}
    win = Path((tmp / 'rounds/cap-flow1/windows.path').read_text().strip()) if (tmp / 'rounds/cap-flow1/windows.path').is_file() else tmp
    check(cap.get('windows', {}).get('os', {}).get('sha256') == hashlib.sha256(imgs['OS'][0]).hexdigest() and
          cap['windows']['systemos']['sha256'] == hashlib.sha256(imgs['SystemOS'][0]).hexdigest() and
          (win / 'os.img').read_bytes() == imgs['OS'][0], 'the capture = both APFS windows byte for byte')
    check('CAPTURE CHECK PASS' in text and text.count('PASS ') >= 10, 'pool-capture-check.sh: sizes, sha256, NXSB, the volume UUIDs: PASS')
    check(tp.sha_region(lun0, base, cursor) == before and not lock.exists(), 'nothing written on the fake tablet; the lock given back')
    text = run('cap-back1', 3, FAKE_BACK_RC='1')
    check('recovery hold' in text and lock.exists(), 'the installed system not back after the capture: recovery hold, the lock kept')
    shutil.rmtree(lock, ignore_errors=True)


if __name__ == '__main__':
    main()
