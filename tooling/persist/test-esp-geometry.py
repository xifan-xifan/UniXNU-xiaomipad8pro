#!/usr/bin/env python3
"""Host-only test of build-esp.py's ESP geometry: the FAT32 hidden-sector field, the record's LBA and dd seek, the LUN 0 bound,
and the refusal of an overlap with the linux_data extent or a rootfs that contains the ESP. No image is built, no device touched."""
import argparse
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import common  # noqa: E402

spec = importlib.util.spec_from_file_location('build_esp', HERE / 'build-esp.py')
build_esp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_esp)

failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what, flush=True)
    if not ok:
        failures.append(what)


def parser():
    return argparse.ArgumentParser(prog='build-esp.py')


def reject(fn, why, exc=(SystemExit,)):
    try:
        fn()
    except exc as caught:
        code = getattr(caught, 'code', 1)
        check(code not in (0, None), f'{why}: refused')
        return
    check(False, f'{why}: not refused')


base = build_esp.esp_geometry(parser(), common.ESP_FIRST)
check(base == {'first_lba': common.ESP_FIRST, 'blocks': common.ESP_BLOCKS}, 'default is the historical layout')
device = build_esp.esp_geometry(parser(), 124289792)
check(device == {'first_lba': 124289792, 'blocks': 262144}, 'device layout accepted')
check(device['first_lba'] * common.LBA >> 20 == 485507, 'device dd seek is 485507 MiB')
reject(lambda: build_esp.esp_geometry(parser(), 124289792 + 128), 'not 1 MiB aligned')
reject(lambda: build_esp.esp_geometry(parser(), 0), 'LBA 0')
reject(lambda: build_esp.esp_geometry(parser(), 2**32), 'past the FAT32 hidden-sector field')
reject(lambda: build_esp.esp_geometry(parser(), common.LUN0_BLOCKS - 1), 'past LUN 0')

check_ok = build_esp.check_esp_layout
check_ok(parser(), device, [107512576, 124289791], 107512575)
check(True, 'device ESP above the device linux_data extent: allowed')
reject(lambda: check_ok(parser(), device, [124289792, 124289792 + 262143], None), 'ESP overlapping linux_data')
reject(lambda: check_ok(parser(), device, [107512576, 124289791 + 262144], None), 'data extent reaching into the ESP')
reject(lambda: check_ok(parser(), device, [2048, 1024], None), 'data extent FIRST past LAST')
reject(lambda: check_ok(parser(), device, None, 124289792 + 5), 'rootfs last inside the ESP')
reject(lambda: check_ok(parser(), device, None, -1), 'negative rootfs last')
seed = {'store_blocks': 6291649}
check(common.data_store_lba(seed, [107512576, 124289791]) == 117735999,
      'the 24 GiB store binds to LBA 117735999, below linux_data\'s 1 GiB reserve')
tight_last = 107512576 + common.RESERVE_BLOCKS + 6291649 - 1
check(common.data_store_lba(seed, [107512576, tight_last]) == 107512576,
      'an extent of exactly the store plus its reserve binds the store at its first LBA')
reject(lambda: common.data_store_lba(seed, [107512576, tight_last - 1]), 'an extent one block short of the store and reserve')
mspec = importlib.util.spec_from_file_location('maint_session', HERE / 'maint-session.py')
maint = importlib.util.module_from_spec(mspec)
mspec.loader.exec_module(maint)
device_record = {'partition': {'lun': 0, 'index': 37, 'first_lba': 124289792, 'blocks': 262144}}
check(maint.check_esp_placement(device_record)['first_lba'] == 124289792, 'maintenance accepts the device placement')
check(maint.check_esp_placement({})['first_lba'] == common.ESP_FIRST, 'no placement means the historical layout')
reject(lambda: maint.check_esp_placement({'partition': {'lun': 1, 'index': 37, 'first_lba': 124289792, 'blocks': 262144}}),
       'a placement on another LUN', exc=(SystemExit, maint.Stop))
reject(lambda: maint.check_esp_placement({'partition': {'lun': 0, 'index': 37, 'first_lba': 124289792, 'blocks': 1024}}),
       'a placement with another size', exc=(SystemExit, maint.Stop))
reject(lambda: maint.check_esp_placement({'partition': {'lun': 0, 'index': 37, 'first_lba': 124289793, 'blocks': 262144}}),
       'an unaligned maintenance placement', exc=(SystemExit, maint.Stop))
check((common.CANDIDATE_ESP_FIRST, common.CANDIDATE_ESP_LAST) == (124289792, 124551935), 'candidate ESP constants')
check(common.CANDIDATE_ESP_LAST - common.CANDIDATE_ESP_FIRST + 1 == common.ESP_BLOCKS, 'candidate ESP is 1 GiB')
class BlankStoreSession:
    execute = True
    nodes = {'linux_data': 'sda36'}
    sizes = {'linux_data': 68719476736}
    starts = {'linux_data': 107512576}
    state = {}

    def t(self, action, node, first, offset, count):
        return {'hex': '00' * 128}

    def save(self, **kw):
        self.state.update(kw)

seed = {'identity_sha256': '7e' * 32, 'store_blocks': 6291649}
record = {'store': {'partition': 'linux_data', 'first_lba': 117735999}, 'seed': seed}
blank = BlankStoreSession()
check(maint.bound_store(blank, record, fresh_ok=True)[0] is True,
      'a blank pre-install store window is accepted for its first data-persist=2 boot')
check(blank.state['esp_store'].get('fresh') is True, 'the fresh store state is recorded')
check(maint.bound_store(BlankStoreSession(), record)[0] is False,
      'a blank store is still refused after installation')
print('esp geometry: ' + ('all checks passed' if not failures else f'{len(failures)} FAILED'))
sys.exit(1 if failures else 0)