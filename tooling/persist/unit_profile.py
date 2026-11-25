#!/usr/bin/env python3
"""A second unit's own LUN 0 geometry, read from its own GPT heads (skills/install-piano-macos/SKILL.md gap G6).

tooling/persist/common.py's constants are the reference unit's (its LBAs, its GPT hashes, the ESP it carved out of
linux_rootfs). Every write step reads those constants, so on another unit they are wrong: on ours, following the
reference plan would write the 27.3 root image across Android's userdata and never touch linux_data
(docs/results-2026-11-26-root-image-built-and-write-plan-hazard.md). This module turns a unit's own GPT head into the
profile tooling/persist/common.py's apply_unit_profile() installs, and it refuses rather than guesses:

    tooling/persist/unit_profile.py --lun0 HEAD.bin [--lun4 HEAD.bin] [--image-bytes N] [--json OUT.json]

HEAD is LUN n from byte 0 (protective MBR, primary header, entry array; at least 6 blocks). Read only: nothing is
written except --json. A profile never relaxes a reference-unit identity check (the GPT hashes, the boot-partition
hashes): it only says where this unit's partitions are, and only when the layout has the shape the write tooling
assumes (linux_rootfs before linux_data, the ESP either absent or a partition named esp of the tooling's size).
"""
import argparse, hashlib, json, struct, sys, uuid
from pathlib import Path

D = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(D / 'tooling' / 'persist'))
import common  # noqa: E402


def parse_head(path):
    data = Path(path).read_bytes()
    for lba in (4096, 512):
        if data[lba:lba + 8] == b'EFI PART':
            break
    else:
        raise SystemExit(f'{path}: no GPT header at byte 4096 or 512 (give the LUN from byte 0, at least 6 blocks)')
    if len(data) < 6 * lba:
        raise SystemExit(f'{path}: {len(data)} bytes is less than the 6 blocks a GPT head needs')
    h = data[lba:lba + 96]
    my, alt, _, _ = struct.unpack_from('<QQQQ', h, 24)
    entries, count, size = struct.unpack_from('<QII', h, 72)
    parts = {}
    for i in range(count):
        at = entries * lba + i * size
        if at + size > len(data):
            raise SystemExit(f'{path}: the entry array is cut at entry {i}: give the whole head')
        e = data[at:at + size]
        if e[:16] == bytes(16):
            continue
        first, last = struct.unpack_from('<QQ', e, 32)
        name = e[56:128].decode('utf-16-le').rstrip('\0')
        parts[name] = {'index': i, 'number': i + 1, 'first': first, 'last': last, 'blocks': last - first + 1}
    return {'path': str(path), 'lba': lba, 'blocks': alt + 1, 'header_lba': my,
            'disk_guid': str(uuid.UUID(bytes_le=h[56:72])).upper(),
            'primary_sha256': hashlib.sha256(data[:6 * lba]).hexdigest(), 'parts': parts}


def build(head, image_bytes=None):
    """The profile of one unit, or a refusal naming what is not the expected shape."""
    if head['lba'] != common.LBA:
        raise SystemExit(f"LUN 0 logical block size {head['lba']} is not the tooling's {common.LBA}")
    p = head['parts']
    for name in ('linux_rootfs', 'linux_data'):
        if name not in p:
            raise SystemExit(f'{name} is not on this unit (the Ubuntu install creates it): no profile, no write')
    rootfs, data = p['linux_rootfs'], p['linux_data']
    if rootfs['last'] >= data['first']:
        raise SystemExit('linux_rootfs does not end before linux_data starts: not the layout the write tooling assumes')
    if data['last'] >= head['blocks']:
        raise SystemExit(f'linux_data ends at {data["last"]} at or past the end of LUN 0 ({head["blocks"]} blocks)')
    profile = {
        'unit_gpt_primary_sha256': head['primary_sha256'],
        'lun0_blocks': head['blocks'], 'lba': head['lba'], 'disk_guid': head['disk_guid'],
        'rootfs_index': rootfs['index'], 'rootfs_first': rootfs['first'], 'rootfs_last': rootfs['last'],
        'data_index': data['index'], 'data_first': data['first'], 'data_last': data['last'],
        'data_blocks': data['blocks'],
        'reference': {'data_first': common.DATA_FIRST, 'rootfs_first': common.ROOTFS_FIRST,
                      'gpt_primary_sha256': common.GPT_PRIMARY_SHA256},
    }
    # The reference unit's ESP was carved out of the end of linux_rootfs; a unit that already has a partition named esp
    # keeps it (ours sits after linux_data) and the carve rule then does not apply to this unit.
    esp = p.get(common.ESP_NAME)
    if esp:
        if esp['blocks'] != common.ESP_BLOCKS:
            raise SystemExit(f"esp is {esp['blocks']} blocks, the tooling expects {common.ESP_BLOCKS}")
        if esp['index'] != common.ESP_INDEX:
            raise SystemExit(f"esp is entry index {esp['index']}, the tooling expects {common.ESP_INDEX}")
        if esp['first'] <= data['last']:
            raise SystemExit('esp overlaps linux_data: refuse')
        profile.update({'esp_index': esp['index'], 'esp_first': esp['first'], 'esp_last': esp['last'],
                        'esp_preexisting': True, 'rootfs_new_last': rootfs['last']})
    else:
        profile.update({'esp_index': common.ESP_INDEX, 'esp_first': rootfs['last'] + 1 - common.ESP_BLOCKS,
                        'esp_last': rootfs['last'], 'esp_preexisting': False,
                        'rootfs_new_last': rootfs['last'] - common.ESP_BLOCKS})
    # NativeUfs's Data store sits below the reserve at the end of the partition it opens the store on
    # (native_ufs_store_open); the root image must stay under both reserves.
    profile['rootfs_reserve_from'] = profile['rootfs_new_last'] + 1 - common.RESERVE_BLOCKS
    profile['data_reserve_from'] = profile['data_last'] + 1 - common.RESERVE_BLOCKS
    if image_bytes is not None:
        need = (image_bytes + profile['lba'] - 1) // profile['lba']
        if need > profile['data_blocks']:
            raise SystemExit(f'the root image needs {need} blocks, linux_data holds {profile["data_blocks"]}')
        if profile['data_first'] + need > profile['data_reserve_from']:
            raise SystemExit(f'the root image would reach {profile["data_first"] + need}, where this unit puts the Data reserve at '
                             f'{profile["data_reserve_from"]}')
        profile['root_write_plan'] = {'partition': 'linux_data', 'lun': 0, 'lba_bytes': profile['lba'],
                                      'partition_first_lba': profile['data_first'], 'partition_blocks': profile['data_blocks'],
                                      'first_lba': profile['data_first'], 'last_lba': profile['data_first'] + need - 1,
                                      'blocks': need, 'free_after_root_blocks': profile['data_blocks'] - need}
    return profile


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lun0', type=Path, required=True)
    ap.add_argument('--lun4', type=Path)
    ap.add_argument('--image-bytes', type=int, help='the root image size, to turn the geometry into a write plan')
    ap.add_argument('--json', type=Path)
    a = ap.parse_args(argv)
    profile = build(parse_head(a.lun0), a.image_bytes)
    if a.lun4:
        head4 = parse_head(a.lun4)
        boot = head4['parts'].get('boot_a')
        if not boot:
            raise SystemExit('boot_a is not on the given LUN 4 head')
        profile['lun4'] = {'lba': head4['lba'], 'blocks': head4['blocks'], 'primary_sha256': head4['primary_sha256'],
                           'boot_a_index': boot['index'], 'boot_a_first': boot['first'], 'boot_a_last': boot['last'],
                           'boot_a_start_512': boot['first'] * head4['lba'] // 512,
                           'boot_a_size_512': boot['blocks'] * head4['lba'] // 512,
                           'reference_boot_a_start_512': common.BOOT_A_START_512,
                           'reference_boot_a_size_512': common.BOOT_A_SIZE_512}
    print(json.dumps(profile, indent=1))
    same = profile['data_first'] == common.DATA_FIRST and profile['rootfs_first'] == common.ROOTFS_FIRST
    print(f'verdict: {"SAME-LAYOUT" if same else "DIFFERENT"}; write by this profile only with PIANO_UNIT_PROFILE naming it',
          file=sys.stderr)
    if a.json:
        a.json.parent.mkdir(parents=True, exist_ok=True)
        a.json.write_text(json.dumps(profile, indent=1) + '\n')
    return 0 if same else 2


if __name__ == '__main__':
    sys.exit(main())