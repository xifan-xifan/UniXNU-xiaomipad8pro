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
import argparse, hashlib, json, re, struct, sys, uuid
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


def esp_entries(path):
    """What an ESP of this project asks the kernel for, read out of the image without a host FAT tool.

    The config is UniXNU's TOML menu, and each entry's `args` line is what decides **which partition holds the root and which
    holds the Data store** (`native-ufs-root-part`, `data-store-part`, `data-store-lba`, `data-persist`). A write plan that
    ignores it can only guess: on this unit the installed ESP mounts its root on linux_rootfs while `make-root-image.sh`'s own
    header describes linux_data (the reference unit's first 27.3 round), and writing the image where the installed ESP does not
    look boots nothing and says nothing (docs/results-2026-11-26-root-image-and-esp-line.md).

    Every `[[entry]]` block is reported, actions included, and `default` comes from `[boot]`, so a caller names the entry a unit
    boots instead of guessing from the first one that happens to mount a root. Refuses an ESP with no menu at all.
    """
    data = Path(path).read_bytes()
    at = data.find(b'[[entry]]')
    if at < 0:
        raise SystemExit(f'{path}: no [[entry]] block (is this an ESP image of this project?)')
    head = data[max(0, at - 8192):at].decode('utf-8', 'replace')
    m = re.search(r'^default\s*=\s*"([^"]+)"', head, re.M)
    marks, nxt = [at], data.find(b'[[entry]]', at + 1)
    while nxt >= 0:
        marks.append(nxt)
        nxt = data.find(b'[[entry]]', nxt + 1)
    entries = []
    for a in marks:
        # The TOML is a few KiB; the image pads the rest of the gigabyte with NUL, so a block is read to the next NUL at most.
        block = data[a:a + 16384].split(b'\0')[0].decode('utf-8', 'replace')
        get = lambda key: (re.search(r'^' + key + r'\s*=\s*"([^"]*)"', block, re.M) or [None, None])[1]
        args = get('args')
        tok = dict(x.split('=', 1) for x in args.split() if '=' in x) if args else {}
        entries.append({'id': get('id'), 'type': get('type'), 'action': get('action'), 'payload': get('path'),
                        'initrd': get('initrd'), 'args': args,
                        'mounts_ufs_root': tok.get('native-ufs-root') == '1',
                        'root_partition': tok.get('native-ufs-root-part'),
                        'store_partition': tok.get('data-store-part'),
                        'store_first_lba': int(tok['data-store-lba']) if tok.get('data-store-lba') else None,
                        'data_persist': int(tok.get('data-persist', 0)) if args else None})
    for e in entries:
        e['is_default'] = e['id'] == m.group(1) if m else False
        if e['mounts_ufs_root'] and not e['root_partition']:
            raise SystemExit(f'{path}: entry {e["id"]} mounts a UFS root without native-ufs-root-part, so its root partition is '
                             'a compile-time default the ESP does not record and no write plan can be derived from it')
    return {'esp_sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data), 'default': m.group(1) if m else None,
            'entries': entries}


def esp_write_target(path):
    """The entry an install boots by default, as a write target.

    Refuses when no entry carries that name, when the default entry mounts no UFS root (a fastboot-only or action default has no
    root to write), and when another entry mounts its root elsewhere - a unit with two roots is written per entry, named
    explicitly, never for whichever entry a parser happened to reach first.
    """
    esp = esp_entries(path)
    chosen = [e for e in esp['entries'] if e['is_default']]
    if not chosen:
        raise SystemExit(f'{path}: no entry is named by [boot] default ({esp["default"]})')
    e = chosen[0]
    if not e['mounts_ufs_root']:
        raise SystemExit(f'{path}: the default entry {e["id"]} does not mount a UFS root; this install has no root partition to write')
    others = {x['id']: x['root_partition'] for x in esp['entries'] if x['mounts_ufs_root'] and x['root_partition'] != e['root_partition']}
    if others:
        raise SystemExit(f'{path}: {others} mount their root on another partition than the default entry {e["id"]} '
                         f'({e["root_partition"]}); a unit with two roots needs the write plan per entry, named explicitly')
    return {'esp_sha256': esp['esp_sha256'], 'esp_bytes': esp['bytes'], 'entry': e['id'], 'payload': e['payload'],
            'root_partition': e['root_partition'], 'store_partition': e['store_partition'],
            'store_first_lba': e['store_first_lba'], 'data_persist': e['data_persist'],
            'entries': {x['id']: {'root_partition': x['root_partition'], 'default': x['is_default'], 'type': x['type'],
                                  'mounts_ufs_root': x['mounts_ufs_root'], 'data_persist': x['data_persist']}
                        for x in esp['entries']}}

def build(head, image_bytes=None, boot_esp=None):
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
        # Which partition the root belongs to is the installed ESP's decision, not the tooling's: with an ESP given, the plan
        # follows its default entry (`native-ufs-root-part`); without one it stays what the writers recorded on the reference
        # unit (linux_data, the first 27.3 round), which on another ESP would be a silent boot failure.
        name = boot_esp['root_partition'] if boot_esp else 'linux_data'
        if boot_esp:
            profile['boot_esp'] = boot_esp
        if name not in p:
            raise SystemExit(f'the ESP mounts its root on {name}, which is not a partition on this unit: no plan, no write')
        part, reserve_from = p[name], p[name]['last'] + 1 - common.RESERVE_BLOCKS
        if need > part['blocks']:
            raise SystemExit(f'the root image needs {need} blocks, {name} holds {part["blocks"]}')
        plan = {'partition': name, 'lun': 0, 'lba_bytes': profile['lba'], 'partition_first_lba': part['first'],
                'partition_blocks': part['blocks'], 'first_lba': part['first'], 'last_lba': part['first'] + need - 1,
                'blocks': need, 'free_after_root_blocks': part['blocks'] - need,
                'partition_reserve_from': reserve_from}
        if boot_esp and boot_esp['store_partition'] == name:
            # Root and store share the partition: the store's own place bounds the image, whatever the reserve says.
            store = boot_esp['store_first_lba']
            if store is None:
                raise SystemExit(f'the ESP puts root and store both on {name} but names no data-store-lba: cannot bound the image')
            plan['store_first_lba'] = store
            plan['free_before_store_blocks'] = store - plan['last_lba'] - 1
            if plan['last_lba'] >= store:
                raise SystemExit(f'the root image would reach {plan["last_lba"]}, where this unit puts its Data store at {store}')
        elif boot_esp:
            # The ESP says the store is on another partition, so this partition's tail reserve is free space - a plan that eats it
            # is recorded as such, because a later store here (or the reference unit's store scratch copy) needs that gap.
            plan['crosses_partition_reserve'] = plan['last_lba'] + 1 > reserve_from
            if plan['crosses_partition_reserve']:
                plan['reserve_blocks_consumed'] = plan['last_lba'] + 1 - reserve_from
        elif plan['last_lba'] + 1 > reserve_from:
            # No ESP given: nothing says where this unit's store is, so the tooling's own rule stands - stay under the reserve.
            raise SystemExit(f'the root image would reach {plan["last_lba"] + 1}, where this unit puts the {name} reserve at '
                             f'{reserve_from} (name the ESP with --esp when its store lives on another partition)')
        profile['root_write_plan'] = plan
    return profile


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lun0', type=Path, required=True)
    ap.add_argument('--lun4', type=Path)
    ap.add_argument('--image-bytes', type=int, help='the root image size, to turn the geometry into a write plan')
    ap.add_argument('--esp', type=Path, help='the ESP image this unit boots: the write plan then follows its default entry\'s '
                                             'native-ufs-root-part instead of the reference unit\'s linux_data')
    ap.add_argument('--json', type=Path)
    a = ap.parse_args(argv)
    profile = build(parse_head(a.lun0), a.image_bytes, boot_esp=esp_write_target(a.esp) if a.esp else None)
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