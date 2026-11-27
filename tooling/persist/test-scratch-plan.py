#!/usr/bin/env python3
"""Host-only tests of the M5 scratch-plan input (docs/tools/durability.md "Builder prerequisite"): common.scratch_window,
common.check_scratch_plan and build-ram-images.py data_store_args with PIANO_SCRATCH_PLAN. No tablet, no device.

The layout is the whole-disk one of tooling/persist/common.py (linux_rootfs ROOTFS_FIRST..ROOTFS_NEW_LAST, linux_data DATA_FIRST..
DATA_LAST); the root size, the seeds, the installed record and the collection are synthetic.
  test-scratch-plan.py
"""
import contextlib
import io
import json
import os
import struct
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import common  # noqa: E402

failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what, flush=True)
    if not ok:
        failures.append(what)


def refused(fn, *args, **kw):
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            fn(*args, **kw)
    except SystemExit as e:
        return str(e)
    return None


def packed_seed(path, logical, fill):
    head = b'DBRDISK1' + struct.pack('<IIQQ', 1, 4096, logical, 1)
    path.write_bytes(head + struct.pack('<QQ', 0, 4096) + bytes([fill]) * 4096)
    return common.seed_info(path)


def main():
    tmp = Path(tempfile.mkdtemp(prefix='scratch-plan-test.'))
    # These fixtures stop before shim compilation and FAT image writing.
    with patch.dict(sys.modules, {'unixnu.bootimg': SimpleNamespace(compile_shim=None),
                                  'unixnu.config': SimpleNamespace(load_toml=None),
                                  'unixnu.payload': SimpleNamespace(write_volume=None)}):
        ram = common.ram_images()
    rootfs = [common.ROOTFS_FIRST, common.ROOTFS_NEW_LAST - common.ROOTFS_FIRST + 1]
    data = [common.DATA_FIRST, common.DATA_LAST - common.DATA_FIRST + 1]
    root_blocks = 5111552                                    # a 19.5 GiB root image
    owner_pages = 52428800
    owner = [common.DATA_LAST + 1 - common.RESERVE_BLOCKS - common.store_blocks(owner_pages), common.store_blocks(owner_pages)]
    scratch = packed_seed(tmp / 'scratch.img', 24 << 30, 7)
    other = packed_seed(tmp / 'other.img', 24 << 30, 8)
    installed = packed_seed(tmp / 'installed.img', 200 << 30, 9)
    reserve = 1024
    end = rootfs[0] + rootfs[1] - reserve * 256
    guard_end = rootfs[0] + root_blocks + 1024
    plan = {'status': 'CONFIRMED', 'linux_rootfs': rootfs, 'approved': [guard_end, end - guard_end],
            'protected': [[rootfs[0], root_blocks + 1024], [end, reserve * 256], data, owner, [1, 5]],
            'root_blocks': root_blocks, 'seed_pages': scratch['pages'], 'reserve_mib': reserve,
            'seed': {'identity_sha256': scratch['identity_sha256']}, 'confirmed_by': {'map_set': 'test'}}

    print('== scratch_window')
    start, size = common.scratch_window(plan)
    check(size == common.store_blocks(scratch['pages']) == 1 + 192 + 6291456, f'window size {size} = 1 + bitmap + pages')
    check(start == common.store_first(common.ROOTFS_NEW_LAST, size), f'window start {start} = NativeUfs\'s place (store_first)')
    check(start >= guard_end and start + size == end, 'the window ends at the reserve and starts after the root guard')
    bad = dict(plan, approved=[guard_end - 1, end - guard_end + 1])
    check(refused(common.scratch_window, bad) is not None, 'approved interval reaching into the root guard: refused')
    bad = dict(plan, approved=[start + 1, end - start - 1])
    check(refused(common.scratch_window, bad) is not None, 'approved interval smaller than the window: refused')
    for extent in ([start - 1, 2], [start + 100, 10], [end - 1, 2], [start - 1, size + 2]):
        bad = dict(plan, protected=plan['protected'] + [extent])
        check('overlaps' in (refused(common.scratch_window, bad) or ''), f'protected extent {extent} overlapping the window: refused')
    check(refused(common.scratch_window, dict(plan, reserve_mib=100)) is not None, 'reserve below 288 MiB: refused')
    check(refused(common.scratch_window, dict(plan, protected=[])) is not None, 'no protected extents: refused')
    check(refused(common.scratch_window, dict(plan, protected=[[0, 6]])) is not None, 'an extent at LBA 0: refused (as run_durability)')
    big = packed_seed(tmp / 'big.img', 48 << 30, 7)
    check(refused(common.scratch_window, dict(plan, seed_pages=big['pages'])) is not None, 'a 48 GiB seed does not fit: refused')

    print('== check_scratch_plan')
    out = tmp / 'persistent'
    out.mkdir()
    (out / 'installed.json').write_text(json.dumps({'image': 'esp-test.img', 'seed': {'identity_sha256': installed['identity_sha256']}}))
    (out / 'esp-test.json').write_text(json.dumps({'store': {'partition': 'linux_data', 'first_lba': owner[0], 'blocks': owner[1]}}))
    kc = tmp / 'kc.bin'
    kc.write_bytes(b'\xcf\xfa\xed\xfe' + b'\0'.join(common.SCRATCH_KC_STRINGS) + b'\0')
    old_kc = tmp / 'kc-old.bin'
    old_kc.write_bytes(b'\xcf\xfa\xed\xfe data-store-lba data-store-part data-store-replace data-store-concurrent\0')
    pf = tmp / 'plan.json'
    pf.write_text(json.dumps(plan))
    args = ['data-persist=2', 'native-ufs-root=1', 'native-ufs-root-part=linux_rootfs', 'data-store-part=linux_rootfs',
            f'data-store-lba={start}', 'log-reserve=1024', 'log=0', 'data-store-concurrent=1', 'piano-durability=1',
            'data-store-write-back=1', 'data-store-queue=0', 'piano-reset-after=300', 'piano-reset-to=bootloader']
    os.environ.pop('PIANO_INSTALLED_SEED', None)
    rec = common.check_scratch_plan(pf, scratch, args, kc, out=out)
    check(rec['window'] == [start, size] and rec['owner_store'] == owner and rec['seed_identity'] == scratch['identity_sha256'],
          f'a confirmed plan, its seed and binding: accepted, window {rec["window"]}')

    def ref(what, path=pf, seed=scratch, a=args, k=kc, o=out):
        return refused(common.check_scratch_plan, path, seed, a, k, out=o) or ''

    p2 = tmp / 'plan-unconfirmed.json'
    p2.write_text(json.dumps(dict(plan, status='UNCONFIRMED')))
    check('CONFIRMED' in ref('', path=p2), 'an unconfirmed plan: refused')
    p3 = tmp / 'plan-window.json'
    p3.write_text(json.dumps(dict(plan, window=[start + 1, size])))
    check('recorded window' in ref('', path=p3), 'a recorded window other than the computed one: refused')
    check('seed' in ref('', seed=other), 'another seed than the plan\'s: refused')
    os.environ['PIANO_INSTALLED_SEED'] = scratch['identity_sha256'][:16]   # the plan's own seed declared the installed one
    check('installed store\'s seed' in ref(''), 'the installed seed as scratch seed: refused')
    os.environ.pop('PIANO_INSTALLED_SEED', None)

    def without(key, value=None):
        a = [x for x in args if not x.startswith(key + '=')]
        return a + ([f'{key}={value}'] if value is not None else [])

    check('data-store-lba' in ref('', a=without('data-store-lba')), 'no data-store-lba: refused')
    check('data-store-lba' in ref('', a=without('data-store-lba', start - 8)), 'another data-store-lba: refused')
    check('single explicit value' in ref('', a=['data-store-lba=1', *args]), 'duplicate binding: refused')
    check('data-store-part' in ref('', a=without('data-store-part', 'linux_data')), 'the store on linux_data: refused')
    check('log-reserve' in ref('', a=without('log-reserve', 512)), 'another log-reserve: refused')
    check('log must stay off' in ref('', a=without('log', 1)), 'log=1: refused')
    check('single explicit value' in ref('', a=args + ['log']), 'bare log after log=0: refused')
    check(ref('', a=without('data-store-concurrent', 2)) == '', 'data-store-concurrent=2 (armed, serial start, the run-time switch): accepted')
    check('data-store-concurrent' in ref('', a=without('data-store-concurrent', 0)), 'data-store-concurrent=0: refused')
    check('data-store-concurrent' in ref('', a=without('data-store-concurrent')), 'no data-store-concurrent: refused')
    check('piano-durability' in ref('', a=without('piano-durability')), 'no piano-durability=1: refused')
    check('piano-reset-after' in ref('', a=without('piano-reset-after')), 'no piano-reset-after: refused')
    check('data-store-replace' in ref('', a=args + ['data-store-replace=1']), 'data-store-replace: refused')
    check('data-store-flatten' in ref('', a=args + ['data-store-flatten=1']), 'data-store-flatten: refused')
    check('sheng-data-reset' in ref('', a=args + ['sheng-data-reset=1']), 'sheng-data-reset: refused')
    check('lacks' in ref('', k=old_kc), 'a collection without the M5 switches: refused')
    p5 = tmp / 'plan-owner.json'
    p5.write_text(json.dumps(dict(plan, protected=[[rootfs[0], root_blocks + 1024], [end, reserve * 256], [1, 5]])))
    check('not inside a protected' in ref('', path=p5), 'the owner store not protected: refused')
    empty = tmp / 'empty'
    empty.mkdir()
    check('installed store is unknown' in ref('', o=empty), 'no installed record: refused')

    print('== build-ram-images.py data_store_args with PIANO_SCRATCH_PLAN')

    def ds(seed_img, extra, **env):
        for k in ('PIANO_DATA_PERSIST', 'PIANO_DATA_STORE_REPLACE', 'PIANO_DATA_STORE_FLATTEN', 'PIANO_INSTALLED_SEED',
                  'PIANO_SCRATCH_PLAN', 'PIANO_BOOT_ARGS_SPLIT', 'PIANO_DATA_STORE_RELAYOUT'):
            os.environ.pop(k, None)
        os.environ.update(env, PIANO_PERSIST_OUT=str(out))
        err = io.StringIO()
        try:
            with contextlib.redirect_stderr(err):
                got, rec = ram.data_store_args(seed_img, kc, extra)
            return got, rec, err.getvalue()
        except SystemExit as e:
            return None, None, str(e)

    extra = [x for x in args if not x.startswith('data-persist=')]
    got, rec, note = ds(tmp / 'scratch.img', extra, PIANO_DATA_PERSIST='2', PIANO_SCRATCH_PLAN=str(pf), PIANO_BOOT_ARGS_SPLIT='1')
    check(got == ['data-persist=2'] and rec and rec['scratch']['window'] == [start, size] and 'SCRATCH store' in note,
          f'scratch plan: data-persist=2, the window in the build record ({got})')
    got, rec, note = ds(tmp / 'scratch.img', extra, PIANO_DATA_PERSIST='2')
    check(got is None and 'data-store-lba on linux_rootfs' in note, 'the same arguments without the plan: the size gate still refuses')
    got, rec, note = ds(tmp / 'scratch.img', extra, PIANO_DATA_PERSIST='2', PIANO_SCRATCH_PLAN=str(pf))
    check(got is None and 'PIANO_BOOT_ARGS_SPLIT' in note, 'scratch plan without PIANO_BOOT_ARGS_SPLIT=1: refused')
    got, rec, note = ds(tmp / 'scratch.img', extra, PIANO_DATA_PERSIST='0', PIANO_SCRATCH_PLAN=str(pf), PIANO_BOOT_ARGS_SPLIT='1')
    check(got is None and 'data-persist=2' in note, 'scratch plan with data-persist=0: refused')
    got, rec, note = ds(tmp / 'scratch.img', extra, PIANO_DATA_PERSIST='2', PIANO_SCRATCH_PLAN=str(pf), PIANO_BOOT_ARGS_SPLIT='1',
                        PIANO_DATA_STORE_REPLACE='1')
    check(got is None and 'REPLACE' in note, 'scratch plan with PIANO_DATA_STORE_REPLACE: refused')
    got, rec, note = ds(tmp / 'other.img', extra, PIANO_DATA_PERSIST='2', PIANO_SCRATCH_PLAN=str(pf), PIANO_BOOT_ARGS_SPLIT='1')
    check(got is None and 'seed' in note, 'scratch plan with another seed: refused')
    got, rec, note = ds(tmp / 'scratch.img', [x for x in extra if not x.startswith('data-store-lba=')] + ['data-store-lba=1'],
                        PIANO_DATA_PERSIST='2', PIANO_SCRATCH_PLAN=str(pf), PIANO_BOOT_ARGS_SPLIT='1')
    check(got is None and 'data-store-lba' in note, 'scratch plan with another data-store-lba: refused')

    print('== RAM payload with the scratch plan')
    ba = ram.boot_args_module()
    adt = tmp / 'boot.adt'
    adt.write_bytes(ba.tiny_adt())
    trust = tmp / 'trust'
    trust.write_bytes(b'test trust cache')
    kc.write_bytes(kc.read_bytes() + ba.KC_MARKER)
    env = dict(PIANO_SCRATCH_PLAN=str(pf), PIANO_BOOT_ARGS_SPLIT='1', PIANO_PERSIST_OUT=str(out),
               PIANO_DATA_PERSIST='2', PIANO_EXACT_ARGS=' '.join(args),
               PIANO_EXTRA_ARGS='native-ufs-root-part=linux_rootfs')
    with patch.dict(os.environ, env), patch.object(ram, 'boot_args_module', return_value=ba), \
            patch.object(ba, 'check_userland', wraps=ba.check_userland) as readers, patch.object(ram, 'payload'):
        _, record = ram.macos_payload(tmp / 'payload', kc, trust, tmp / 'scratch.img', adt, 0)
    effective = record['boot_args']['boot_args_8cpu'].split()
    check(record['data_store']['scratch']['window'] == [start, size] and all(effective.count(arg) == 1 for arg in args)
          and 'piano-boot-args=ram' in effective, 'RAM payload binds the same window after splitting and merging')
    check(readers.call_args.kwargs['seed'] == tmp / 'scratch.img' and not readers.call_args.kwargs['live'],
          'scratch payload checks boot-argument readers from its seed alone')
    for k in ('PIANO_DATA_PERSIST', 'PIANO_SCRATCH_PLAN', 'PIANO_BOOT_ARGS_SPLIT', 'PIANO_PERSIST_OUT', 'PIANO_DATA_STORE_REPLACE'):
        os.environ.pop(k, None)
    print('\n' + (f'{len(failures)} FAILED' if failures else 'all passed'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
