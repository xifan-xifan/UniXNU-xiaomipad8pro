#!/usr/bin/env python3
"""Compare ANOTHER piano unit's partition tables with the layout the install tooling is pinned to (tooling/persist/common.py), on
the host, from the unit's own GPT backups (the Ubuntu install's backup step, xiaomipad-8pro-mainline docs/INSTALL.en.md section 2,
saves every LUN's MBR + primary GPT). Read only: it opens the given files and writes nothing (unless --json).

  unit-profile.py --lun0 LUN0-HEAD.bin [--lun4 LUN4-HEAD.bin] [--json OUT.json]

LUN0-HEAD.bin is LUN 0 from byte 0: the protective MBR, the GPT header and the entry array (at least 6 x 4096 bytes); likewise LUN 4.
It reports every constant of common.py the unit differs in, and a verdict:
  SAME-LAYOUT   the partitions the install writes sit where the tooling expects (the GPT bytes still differ: the GUIDs are per unit)
  DIFFERENT     at least one LBA, size or partition index differs: maint-session.py would refuse at `map`, and must not be forced
Either way tooling/persist/maint-session.py checks the reference unit's GPT and boot-partition hashes, so a second unit needs a device
profile of its own before any write (skills/install-piano-macos/SKILL.md, Known gaps G6).
"""
import argparse
import hashlib
import importlib.util
import json
import struct
import sys
import uuid
from pathlib import Path

D = Path(__file__).resolve().parents[2]


def common():
    spec = importlib.util.spec_from_file_location('common', D / 'tooling/persist/common.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse(path):
    data = Path(path).read_bytes()
    for lba in (4096, 512):
        if data[lba:lba + 8] == b'EFI PART':
            break
    else:
        sys.exit(f'{path}: no GPT header at byte 4096 or 512')
    h = data[lba:lba + 92]
    my_lba, alt_lba, first, last = struct.unpack_from('<QQQQ', h, 24)
    disk_guid = str(uuid.UUID(bytes_le=h[56:72])).upper()
    entries_lba, count, size = struct.unpack_from('<QII', h, 72)
    parts = []
    for i in range(count):
        at = entries_lba * lba + i * size
        e = data[at:at + size]
        if len(e) < 128:
            sys.exit(f'{path}: the entry array is cut at entry {i}: give the whole head (at least 6 blocks)')
        if e[:16] == bytes(16):
            continue
        start, end = struct.unpack_from('<QQ', e, 32)
        name = e[56:128].decode('utf-16-le').rstrip('\0')
        parts.append({'number': i + 1, 'name': name, 'first': start, 'last': end})
    return {'lba': lba, 'blocks': alt_lba + 1, 'disk_guid': disk_guid, 'parts': parts,
            'primary_sha256': hashlib.sha256(data[:6 * lba]).hexdigest()}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--lun0', type=Path, required=True)
    p.add_argument('--lun4', type=Path)
    p.add_argument('--json', type=Path)
    a = p.parse_args(argv)
    c = common()
    g = parse(a.lun0)
    rows = []

    def row(what, unit, ref):
        rows.append({'what': what, 'unit': unit, 'reference': ref, 'same': unit == ref})

    row('LUN 0 logical block size', g['lba'], c.LBA)
    row('LUN 0 blocks', g['blocks'], c.LUN0_BLOCKS)
    by = {x['name']: x for x in g['parts']}
    for name, index, first, last in (('linux_rootfs', c.ROOTFS_INDEX, c.ROOTFS_FIRST, c.ROOTFS_LAST),
                                     ('linux_data', c.DATA_INDEX, c.DATA_FIRST, c.DATA_LAST)):
        x = by.get(name)
        if not x:
            row(f'{name} present (the Ubuntu install creates it)', False, True)
            continue
        row(f'{name} partition number', x['number'], index + 1)
        row(f'{name} first LBA', x['first'], first)
        row(f'{name} last LBA', x['last'], last)
    esp = by.get(c.ESP_NAME)
    row('ESP (partition 37) already present', bool(esp), False)
    row('LUN 0 primary GPT sha256 (pre-install tables)', g['primary_sha256'], c.GPT_PRIMARY_SHA256)
    if a.lun4:
        g4 = parse(a.lun4)
        b = {x['name']: x for x in g4['parts']}.get('boot_a')
        if b:
            # common.py keeps boot_a's place in 512-byte units (sysfs)
            row('boot_a start (512-byte units)', b['first'] * g4['lba'] // 512, c.BOOT_A_START_512)
            row('boot_a size (512-byte units)', (b['last'] - b['first'] + 1) * g4['lba'] // 512, c.BOOT_A_SIZE_512)
        else:
            row('boot_a present on LUN 4', False, True)
    layout = [r for r in rows if not r['same'] and 'sha256' not in r['what']]
    for r in rows:
        print(f'{"same" if r["same"] else "DIFF":4s}  {r["what"]}: unit {r["unit"]}, tooling {r["reference"]}')
    verdict = 'SAME-LAYOUT' if not layout else 'DIFFERENT'
    print(f'verdict: {verdict}. tooling/persist/maint-session.py is pinned to the reference unit (its GPT and boot-partition '
          'hashes): no write session on this unit until it has its own device profile (SKILL.md Known gaps G6).')
    if a.json:
        a.json.write_text(json.dumps({'verdict': verdict, 'disk_guid': g['disk_guid'], 'rows': rows}, indent=1) + '\n')
    return 0 if verdict == 'SAME-LAYOUT' else 2


if __name__ == '__main__':
    sys.exit(main())
