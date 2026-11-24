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


def reject(fn, why):
    try:
        fn()
    except SystemExit as exc:
        check(exc.code not in (0, None), f'{why}: refused')
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
print('esp geometry: ' + ('all checks passed' if not failures else f'{len(failures)} FAILED'))
sys.exit(1 if failures else 0)