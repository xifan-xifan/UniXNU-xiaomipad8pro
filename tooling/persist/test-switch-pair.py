#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""T-16 switcher, phase 1 (private/host-checks/20261123-switcher/DESIGN.md 3.1): maint-session.py accepts Android's slot-A pair
(vendor_boot 99ef425b + init_boot dce4a752 Magisk or ae0b5153 stock) as well as the Ubuntu-era pair of macos-set-v5, refuses an
unknown pair, and refuses map / boot-a / check while boot_a holds stock Android (17e89056). It uses test-persist.py's fake tablet
with file-backed transfers and optional PIANO_TEST_PERSIST_OUT pointing to the read-only reference/GPT fixture directory."""
import contextlib
import hashlib
import importlib.util
import io
import os
import shutil
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import common  # noqa: E402

spec = importlib.util.spec_from_file_location('tp', HERE / 'test-persist.py')
tp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tp)
CB = common.BACKUP
failures = tp.failures
spec = importlib.util.spec_from_file_location('maint', HERE / 'maint-session.py')
maint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(maint)


def unit():
    print('== common.slot_a_pairs / slot_a_pair_name')
    ub = (common.VENDOR_BOOT_A_SHA256, common.INIT_BOOT_A_SHA256)
    tp.check(common.slot_a_pair_name(*ub) == 'the Ubuntu-era pair (macos-set-v5)', 'the Ubuntu-era pair is known')
    tp.check('Magisk' in (common.slot_a_pair_name(common.ANDROID_VENDOR_BOOT_SHA256, common.ANDROID_INIT_BOOT_MAGISK_SHA256) or ''),
             'Android + Magisk init_boot is known')
    tp.check('stock' in (common.slot_a_pair_name(common.ANDROID_VENDOR_BOOT_SHA256, common.ANDROID_INIT_BOOT_SHA256) or ''),
             'Android stock pair is known')
    for v, i in ((common.VENDOR_BOOT_A_SHA256, common.ANDROID_INIT_BOOT_MAGISK_SHA256),
                 (common.ANDROID_VENDOR_BOOT_SHA256, common.INIT_BOOT_A_SHA256), ('0' * 64, '0' * 64)):
        tp.check(common.slot_a_pair_name(v, i) is None, f'mixed/unknown pair {v[:8]}+{i[:8]} is not known')
    for name, path, digest in (('android-set boot', CB / 'android-set/boot.img', common.ANDROID_BOOT_A_SHA256),
                               ('android-set vendor_boot', CB / 'android-set/vendor_boot.img', common.ANDROID_VENDOR_BOOT_SHA256),
                               ('android-set init_boot', CB / 'android-set/init_boot.img', common.ANDROID_INIT_BOOT_SHA256),
                               ('android-set-root Magisk init_boot', CB / 'android-set-root/init_boot-magisk-shellgrant.img',
                                common.ANDROID_INIT_BOOT_MAGISK_SHA256)):
        tp.check(path.is_file() and common.sha(path) == digest, f'{name}: the constant {digest[:16]} = the CRITICAL-BACKUP file')


def session(tmp):
    print('== maint-session.py with Android\'s slot-A pair (a fake tablet)')
    fixtures = Path(os.environ.get('PIANO_TEST_PERSIST_OUT', str(common.DEVICE / 'out/persistent')))
    root, out, _, _ = tp.build_fake(tmp, fixtures)
    nas = tmp / 'nas'
    nas.mkdir()
    env = dict(FAKE_ROOT=str(root), PIANO_BACKUP_DIR=str(nas), PIANO_LOCALNET_PREFLIGHT='off')
    base = ['maint-session.py', '--session', str(tmp / 'session'), '--esp', 'esp-test.img',
            '--esp-final', 'esp-test.img', '--boot-image', 'boot-test-signed.img']
    served = {}

    def receive(dev, skip, count, bs, dests):
        disk, offset, length = tp.region(tp.fake_nodes(root), dev, skip, count, bs)
        digest = hashlib.sha256()
        with contextlib.ExitStack() as stack:
            src = stack.enter_context(disk.open('rb'))
            outputs = [stack.enter_context(path.open('wb')) for path in dests]
            src.seek(offset)
            left = length
            while left and (chunk := src.read(min(left, 4 * tp.MiB))):
                for dst in outputs:
                    dst.write(chunk)
                digest.update(chunk)
                left -= len(chunk)
        return length - left, digest.hexdigest()

    def run(*steps, expect=0):
        output = io.StringIO()
        with (patch.dict(os.environ, env), patch.object(maint, 'OUT', out), patch.object(maint, 'JOB_POLL', 0.001),
              patch.object(sys, 'argv', base + list(steps)), contextlib.redirect_stdout(output),
              patch.object(maint.Session, 'shell', side_effect=lambda command, timeout=120: tp.fake_shell(command)),
              patch.object(maint.Session, 'serve', side_effect=served.update),
              patch.object(maint.Session, 'receive', side_effect=receive),
              patch.object(maint.Session, 'fastboot', side_effect=AssertionError('fake tablet must never call fastboot')),
              patch.object(tp, 'http_get', side_effect=lambda port, name: served[name].read_bytes())):
            code = 0
            try:
                maint.main()
            except SystemExit as exc:
                code = exc.code
        log = output.getvalue()
        (tmp / 'session-out.txt').open('a').write(log)
        if code != expect:
            print(log[-3000:])
        tp.check(code == expect, f'maint-session.py {" ".join(steps)} -> {code} (want {expect})')
        return log

    lun4 = root / 'lun4.img'

    def put(part, path):
        data = Path(path).read_bytes()
        assert len(data) == tp.LUN4[part][2], (part, len(data))
        tp.write_at(lun4, tp.LUN4[part][1], data)

    def region(part):
        return tp.sha_region(lun4, tp.LUN4[part][1], tp.LUN4[part][2])

    log = run('map')
    tp.check('the Ubuntu-era pair (macos-set-v5)' in log, 'map: the Ubuntu-era pair named (the existing path)')
    put('vendor_boot_a', CB / 'android-set/vendor_boot.img')
    put('init_boot_a', CB / 'android-set-root/init_boot-magisk-shellgrant.img')
    log = run('map', 'backup')
    tp.check('Android\'s pair with the Magisk init_boot (macos-set-v5a)' in log, 'map: Android + Magisk pair accepted and named')
    before = region('boot_a')
    run('boot-a')        # check needs the new GPT tables (test-persist's session writes them first): not this test's subject
    tp.check(region('boot_a') == common.sha(out / 'boot-test-signed.img'), 'boot-a writes boot_a with the Android pair in place')
    run('rollback', 'boot-a')
    tp.check(region('boot_a') == before, 'rollback boot-a restores the exact verified backup')
    put('init_boot_a', CB / 'android-set/init_boot.img')
    log = run('map')
    tp.check('Android\'s stock pair (android-set)' in log, 'map: Android stock pair accepted and named')
    put('init_boot_a', fixtures / 'ref/28-init_boot_a.img')   # Ubuntu init_boot + Android vendor_boot
    log = run('map', expect=3)
    tp.check('UNKNOWN' in log, 'map refuses a mixed pair (Android vendor_boot + Ubuntu init_boot)')
    log = run('boot-a', expect=3)
    tp.check('UNKNOWN' in log, 'boot-a refuses a mixed pair')
    put('init_boot_a', CB / 'android-set-root/init_boot-magisk-shellgrant.img')
    run('map')                                   # state for check (boot_a = the test macOS image)
    put('boot_a', CB / 'android-set/boot.img')
    log = run('check', expect=3)
    tp.check('boot_a holds stock Android' in log, 'check refuses while boot_a holds stock Android')
    log = run('map', expect=3)
    tp.check('boot_a holds stock Android' in log, 'map refuses while boot_a holds stock Android')
    before = region('boot_a')
    log = run('boot-a', expect=3)
    tp.check('boot_a holds stock Android' in log and region('boot_a') == before, 'boot-a refuses (boot_a untouched) while it holds Android')


def main():
    tmp = Path(tempfile.mkdtemp(prefix='piano-switch-pair-'))
    try:
        unit()
        session(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
