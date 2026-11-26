#!/usr/bin/env python3
"""Host test of the second-unit profile (tooling/persist/unit_profile.py, common.apply_unit_profile): SKILL.md gap G6.

Synthetic GPT heads (built here, no device, no backup): the tooling's own unit's geometry, another unit's, and the shapes a
profile must refuse. Run: python3 tooling/persist/test-unit-profile.py
"""
import hashlib, json, struct, subprocess, sys, tempfile, uuid
from pathlib import Path

D = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(D / 'tooling' / 'persist'))
import common, unit_profile

LBA = 4096
FAILS = []


def check(what, ok, note=''):
    print(f'{"PASS" if ok else "FAIL":4}  {what}{": " + note if note else ""}')
    if not ok:
        FAILS.append(what)


def head(parts, blocks=124552192, lba=LBA, cut_entries=False):
    """A LUN head as unit_profile.parse_head reads it: MBR, primary header, entry array. PARTS is
    (entry index, name, first LBA, last LBA); the reference unit's esp is carved, so it has no esp entry at all."""
    entries_lba, count, size = 2, 128, 128
    h = bytearray(6 * lba)
    h[510:512] = b'\x55\xAA'                       # the MBR's signature; the GPT starts at LBA 1
    h[lba:lba + 8] = b'EFI PART'
    struct.pack_into('<QQQQ', h, lba + 24, 1, blocks - 1, 2, blocks - 2)   # my_lba 1, the backup header at the last LBA
    struct.pack_into('<16s', h, lba + 56, uuid.UUID('11111111-2222-3333-4444-555555555555').bytes_le)
    struct.pack_into('<QII', h, lba + 72, entries_lba, count, size)
    for index, name, first, last in parts:
        at = entries_lba * lba + index * size
        struct.pack_into('<QQ', h, at + 32, first, last)
        h[at + 56:at + 128] = name.encode('utf-16-le')[:72].ljust(72, b'\0')   # a short slice would resize the bytearray
        h[at:at + 16] = uuid.uuid5(uuid.NAMESPACE_URL, 'type').bytes_le   # an all-zero type GUID would read as an empty entry
        h[at + 16:at + 32] = uuid.uuid5(uuid.NAMESPACE_DNS, name).bytes_le
    return bytes(h[:5 * lba] if cut_entries else h)


def write(tmp, name, data):
    p = tmp / name
    p.write_bytes(data)
    return p


OURS = [(33, 'userdata', 3632128, 102269695), (34, 'linux_rootfs', 102269696, 107512575),
        (35, 'linux_data', 107512576, 124289791), (36, 'esp', 124289792, 124551935)]
REFERENCE = [(33, 'userdata', 3632128, 56060927), (34, 'linux_rootfs', 56060928, 71789567),
             (35, 'linux_data', 71789568, 124552186)]
ROOT_IMG = 20936916992                      # this project's 27.3 root image (out/inputs/k273/root/root.json)

with tempfile.TemporaryDirectory() as td:
    tmp = Path(td)
    ours = write(tmp, 'ours.bin', head(OURS))
    ref = write(tmp, 'ref.bin', head(REFERENCE))

    h = unit_profile.parse_head(ours)
    check('parse_head reads the synthetic head', h['lba'] == LBA and set(h['parts']) == {n for _, n, _, _ in OURS},
          f"{len(h['parts'])} partitions, {h['blocks']} blocks")

    p = unit_profile.build(unit_profile.parse_head(ref), ROOT_IMG)
    check("the tooling's own geometry needs no profile override", p['data_first'] == common.REFERENCE['DATA_FIRST'],
          str(p['data_first']))

    q = unit_profile.build(unit_profile.parse_head(ours), ROOT_IMG)
    plan = q['root_write_plan']
    check('our unit: linux_data is elsewhere', (q['data_first'], q['data_last']) == (107512576, 124289791))
    check('our unit: the ESP is not carved from linux_rootfs', q['esp_preexisting'] and q['esp_first'] == 124289792
          and q['rootfs_new_last'] == 107512575)
    check('our unit: the root image plan starts at linux_data, not inside userdata',
          (plan['first_lba'], plan['last_lba'], plan['blocks']) == (107512576, 112624127, 5111552),
          f"{plan['free_after_root_blocks']} blocks free after it")
    check('the reference plan would land in our userdata (the hazard this module exists to stop)',
          OURS[0][2] < common.REFERENCE['DATA_FIRST'] < OURS[0][3],
          f"{common.REFERENCE['DATA_FIRST']} is inside our userdata ({OURS[0][2]}..{OURS[0][3]})")

    # An image that would run under the Data reserve is refused, not written.
    big = (q['data_reserve_from'] - q['data_first'] + 1) * LBA
    try:
        unit_profile.build(unit_profile.parse_head(ours), big)
        check('a root image reaching the Data reserve is refused', False)
    except SystemExit as e:
        check('a root image reaching the Data reserve is refused', 'reserve' in str(e), str(e)[:70])

    for what, parts, why in (
            ('a unit without linux_data is refused', OURS[:2], 'linux_data'),
            ('overlapping rootfs/data is refused', [(34, 'linux_rootfs', 100, 2000), (35, 'linux_data', 1000, 3000)],
             'end before'),
            ('an esp of another size is refused', OURS[:-1] + [(36, 'esp', 124289792, 124290047)], 'blocks'),
            ('an esp of another entry index is refused', OURS[:-1] + [(37, 'esp', 124289792, 124551935)], 'entry index'),
            ('an esp overlapping linux_data is refused', OURS[:-1] + [(36, 'esp', 120000000, 120262143)], 'overlaps'),
    ):
        try:
            unit_profile.build(unit_profile.parse_head(write(tmp, f'{abs(hash(what))}.bin', head(parts))))
            check(what, False)
        except SystemExit as e:
            check(what, why in str(e), str(e)[:70])

    try:
        unit_profile.parse_head(write(tmp, 'cut.bin', head(OURS, cut_entries=True)))
        check('a cut entry array is refused', False)
    except SystemExit as e:
        check('a cut entry array is refused', 'cut' in str(e) or '6 blocks' in str(e), str(e)[:70])

    # common: unset keeps every reference constant; a profile rebinds the places and refuses to rebind the rules.
    before = {n: getattr(common, n) for n in ('LBA', 'LUN0_BLOCKS', 'DATA_FIRST', 'DATA_LAST', 'ROOTFS_FIRST',
                                              'ROOTFS_LAST', 'ESP_FIRST', 'ESP_BLOCKS', 'ESP_TYPE', 'GPT_PRIMARY_SHA256')}
    check('nothing applied: the reference constants stand', common.UNIT_PROFILE is None
          and common.DATA_FIRST == common.REFERENCE['DATA_FIRST'])

    profile_file = tmp / 'profile.json'
    profile_file.write_text(json.dumps(unit_profile.build(unit_profile.parse_head(ours), ROOT_IMG), indent=1))
    got = common.apply_unit_profile(profile_file)
    check('apply_unit_profile rebinds the places', (common.DATA_FIRST, common.DATA_LAST, common.ESP_FIRST,
                                                    common.ROOTFS_LAST) == (107512576, 124289791, 124289792, 107512575))
    check('apply_unit_profile keeps the tooling\'s rules', (common.LBA, common.ESP_BLOCKS, common.ESP_TYPE)
          == (before['LBA'], before['ESP_BLOCKS'], before['ESP_TYPE']))
    check('the reference values stay readable for logs', common.REFERENCE['DATA_FIRST'] == before['DATA_FIRST'])
    common.clear_unit_profile()
    after = {n: getattr(common, n) for n in before}
    check('clear_unit_profile restores every constant', after == before)

    bad = tmp / 'bad.json'
    bad.write_text(json.dumps({**json.loads(profile_file.read_text()), 'lun0_blocks': 1}))
    try:
        common.apply_unit_profile(bad)
        check('a profile disagreeing about the disk length is refused', False)
    except SystemExit as e:
        check('a profile disagreeing about the disk length is refused', 'LUN0_BLOCKS' in str(e), str(e)[:70])
    notprofile = tmp / 'notprofile.json'
    notprofile.write_text('{"hello": 1}')
    try:
        common.apply_unit_profile(notprofile)
        check('a file that is not a profile is refused', False)
    except SystemExit as e:
        check('a file that is not a profile is refused', 'not a unit profile' in str(e), str(e)[:70])
    try:
        common.apply_unit_profile(tmp / 'missing.json')
        check('a missing profile is refused', False)
    except SystemExit as e:
        check('a missing profile is refused', 'no such profile' in str(e), str(e)[:70])

    # The command line: exit 2 = DIFFERENT, and the JSON it writes is what common accepts.
    out = tmp / 'cli.json'
    r = subprocess.run([sys.executable, str(D / 'tooling/persist/unit_profile.py'), '--lun0', str(ours),
                        '--image-bytes', str(ROOT_IMG), '--json', str(out)], capture_output=True, text=True)
    check('the CLI exits 2 (DIFFERENT) for another unit', r.returncode == 2, f'rc={r.returncode}')
    check('the CLI wrote a profile common accepts', json.loads(out.read_text())['root_write_plan']['first_lba'] == 107512576
          and common.apply_unit_profile(out) is not None)

    # --- the installed ESP decides where the root goes (its UniXNU menu, read out of the image) -----------------------------
    def esp_image(entries, default='macos', tail=0):
        """A synthetic ESP: UniXNU's TOML menu as the real image holds it, the [boot] block above the entries, then NUL padding -
        a builder that read only the first KiB would miss nothing, and one that sliced to the end of the last block would read a
        gigabyte of padding (the real image pads 1,073,741,824 bytes; this fixture keeps both bounds honest)."""
        text = ('version = 1\n[boot]\ntimeout = 3\n' + (f'default = "{default}"\n' if default else '')
                + '[kits]\nautoload = false\n')
        for e in entries:
            text += f'[[entry]]\nid = "{e["id"]}"\ntype = "{e.get("type", "xnu")}"\n'
            text += (f'action = "{e["action"]}"\n' if 'action' in e else
                     f'path = "Payloads/kernel.macho"\ninitrd = "Payloads/scratch-data.img"\nargs = "{e["args"]}"\n')
        return (text + '\0' * (tail or 4096)).encode()

    root_args = 'native-ufs-root=1 native-ufs-root-part=%s data-store-part=%s data-store-lba=%d data-persist=2'
    fastboot_args = 'native-ufs-root=0 data-persist=0'
    good = esp_image([{'id': 'macos', 'args': root_args % ('linux_rootfs', 'linux_data', 117735999)},
                      {'id': 'macos-diag', 'args': root_args % ('linux_rootfs', 'linux_data', 117735999)},
                      {'id': 'fastboot', 'args': fastboot_args},
                      {'id': 'reset', 'type': 'action', 'action': 'reset'}], tail=65536)
    esp_ok = write(tmp, 'esp-good.img', good)
    e = unit_profile.esp_entries(esp_ok)
    check('esp_entries reads the whole menu, its default and each entry',
          e['default'] == 'macos' and [x['id'] for x in e['entries']] == ['macos', 'macos-diag', 'fastboot', 'reset']
          and e['entries'][0]['root_partition'] == 'linux_rootfs' and e['entries'][2]['mounts_ufs_root'] is False
          and e['entries'][3]['action'] == 'reset' and e['entries'][3]['args'] is None,
          f"{[x['id'] for x in e['entries']]}, default {e['default']}")
    t = unit_profile.esp_write_target(esp_ok)
    check('the write target is the default entry, not the first entry that mounts a root',
          t['entry'] == 'macos' and t['root_partition'] == 'linux_rootfs' and t['store_first_lba'] == 117735999
          and t['data_persist'] == 2 and t['esp_sha256'] == hashlib.sha256(good).hexdigest())
    check('an entry with no args (an action) is not a kernel line', 'reset' in t['entries']
          and t['entries']['reset']['root_partition'] is None)

    for what, blob, why in (
            ('an ESP whose default is an action is refused', esp_image(
                [{'id': 'macos', 'args': root_args % ('linux_rootfs', 'linux_data', 100)},
                 {'id': 'reset', 'type': 'action', 'action': 'reset'}], default='reset'), 'does not mount a UFS root'),
            ('an ESP mounting a root without naming its partition is refused', esp_image(
                [{'id': 'macos', 'args': 'native-ufs-root=1 data-store-part=linux_data'}]), 'without native-ufs-root-part'),
            ('an ESP whose entries disagree about the root partition is refused', esp_image(
                [{'id': 'macos', 'args': root_args % ('linux_rootfs', 'linux_data', 100)},
                 {'id': 'macos-273', 'args': root_args % ('linux_data', 'linux_data', 100)}]), 'another partition'),
            ('an ESP whose default mounts no UFS root is refused', esp_image([{'id': 'fastboot', 'args': fastboot_args}], default='fastboot'),
             'does not mount a UFS root'),
            ('an ESP whose [boot] default names no entry is refused',
             esp_image([{'id': 'macos', 'args': root_args % ('linux_rootfs', 'linux_data', 100)}], default='nothing-here'),
             'no entry is named by'),
            ('an ESP with no menu is refused', b'\0' * 4096, 'no [[entry]] block'),
    ):
        try:
            unit_profile.esp_write_target(write(tmp, f'esp-{abs(hash(what))}.img', blob))
            check(what, False)
        except SystemExit as ex:
            check(what, why in str(ex), str(ex)[:70])

    q = unit_profile.build(unit_profile.parse_head(write(tmp, 'ours-esp.bin', head(OURS))), ROOT_IMG,
                           boot_esp=unit_profile.esp_write_target(esp_ok))
    plan = q['root_write_plan']
    check('with the ESP given, the plan lands on the partition its default entry mounts the root from',
          (plan['partition'], plan['first_lba'], plan['last_lba'], plan['blocks']) ==
          ('linux_rootfs', 102269696, 107381247, 5111552), f"{plan['partition']} {plan['first_lba']}..{plan['last_lba']}")
    check('a root image crossing its partition tail reserve says so (the store is elsewhere, so it is allowed)',
          plan['crosses_partition_reserve'] and plan['reserve_blocks_consumed'] == 130816
          and plan['free_after_root_blocks'] == 131328, f"{plan.get('reserve_blocks_consumed')} reserve blocks consumed")
    check('the ESP is recorded with the plan (a write binds to the menu that asked for it)',
          q['boot_esp']['esp_sha256'] == hashlib.sha256(good).hexdigest() and q['boot_esp']['entry'] == 'macos')
    check('without an ESP the plan stays the reference unit partition choice (linux_data)',
          unit_profile.build(unit_profile.parse_head(write(tmp, 'ours-noesp.bin', head(OURS))),
                             ROOT_IMG)['root_write_plan']['partition'] == 'linux_data')

    esp_same = write(tmp, 'esp-same.img', esp_image([{'id': 'macos', 'args': root_args % ('linux_data', 'linux_data', 107600000)}]))
    try:
        unit_profile.build(unit_profile.parse_head(write(tmp, 'ours-same.bin', head(OURS))), ROOT_IMG,
                           boot_esp=unit_profile.esp_write_target(esp_same))
        check('a root image reaching the store on its own partition is refused', False)
    except SystemExit as ex:
        check('a root image reaching the store on its own partition is refused', 'Data store at 107600000' in str(ex), str(ex)[:70])
    try:
        unit_profile.build(unit_profile.parse_head(write(tmp, 'ours-big.bin', head(OURS))), 5242881 * LBA,
                           boot_esp=unit_profile.esp_write_target(esp_ok))
        check('a root image larger than the ESP root partition is refused', False)
    except SystemExit as ex:
        check('a root image larger than the ESP root partition is refused', 'linux_rootfs holds 5242880' in str(ex), str(ex)[:70])

    # The CLI takes the ESP too, and the plan it writes agrees with the library.
    esp_cli = tmp / 'cli-esp.json'
    r = subprocess.run([sys.executable, str(D / 'tooling/persist/unit_profile.py'), '--lun0', str(ours),
                        '--image-bytes', str(ROOT_IMG), '--esp', str(esp_ok), '--json', str(esp_cli)],
                       capture_output=True, text=True)
    w = json.loads(esp_cli.read_text())['root_write_plan']
    check('the CLI with --esp writes the plan the ESP asks for', r.returncode == 2 and w['partition'] == 'linux_rootfs'
          and w['first_lba'] == 102269696 and json.loads(esp_cli.read_text())['boot_esp']['entry'] == 'macos', f'rc={r.returncode}')
    # --- both GPT copies and this unit's own Android pair: the two facts a second unit cannot borrow from another -------
    def tail(back=2, blocks_back=5, lba=LBA):
        """The LUN's last blocks as tooling/native/fetch-lun0-tail.py hands them over, with the backup header BACK blocks
        from the end. parse_tail's hash is over the whole region, which is what maint-session.py compares the medium's
        s.region_sha(lun0, TAIL_LBA, 5) against, so a fixture has to keep the region length honest."""
        t = bytearray(blocks_back * lba)
        at = (blocks_back - back) * lba
        t[at:at + 8] = b'EFI PART'
        return bytes(t)

    for back in (2, 1, 5):
        p = write(tmp, f'tail-{back}.bin', tail(back))
        g = unit_profile.parse_head(p, tail=True)
        check(f'parse_head(tail=True) finds the backup header {back} blocks from the end',
              g['blocks_back'] == back and g['backup_sha256'] == hashlib.sha256(p.read_bytes()).hexdigest(),
              f"blocks_back={g['blocks_back']}")
    try:
        unit_profile.parse_tail('zeros.bin', b'\0' * (5 * LBA))
        check('a tail with no GPT header is refused', False)
    except SystemExit as ex:
        check('a tail with no GPT header is refused', 'no GPT header' in str(ex), str(ex)[:70])

    tail_bin = write(tmp, 'ours-tail.bin', tail())
    tail_sha = hashlib.sha256(tail_bin.read_bytes()).hexdigest()
    q = unit_profile.build(unit_profile.parse_head(ours), ROOT_IMG, tail=unit_profile.parse_head(tail_bin, tail=True))
    check('a profile names the backup table read off this unit\'s own disk',
          q['unit_gpt_backup_sha256'] == tail_sha, str(q['unit_gpt_backup_sha256'])[:16])
    check('without --lun0-tail the profile cannot name it (None, never a guess)',
          unit_profile.build(unit_profile.parse_head(ours), ROOT_IMG)['unit_gpt_backup_sha256'] is None)

    VENDOR, INIT = 'a' * 64, 'b' * 64
    pairp = tmp / 'slot-a.json'
    pairp.write_text(json.dumps({'vendor_boot_a_sha256': VENDOR, 'init_boot_a_sha256': INIT}))
    both = tmp / 'cli-both.json'
    r = subprocess.run([sys.executable, str(D / 'tooling/persist/unit_profile.py'), '--lun0', str(ours),
                        '--lun0-tail', str(tail_bin), '--image-bytes', str(ROOT_IMG), '--esp', str(esp_ok),
                        '--android-slot-a', str(pairp), '--json', str(both)], capture_output=True, text=True)
    w = json.loads(both.read_text())
    check('the CLI takes --lun0-tail and --android-slot-a together',
          r.returncode == 2 and w['unit_gpt_backup_sha256'] == tail_sha
          and w['android_slot_a']['init_boot_a_sha256'] == INIT, f'rc={r.returncode}')
    bad = tmp / 'slot-a-bad.json'
    bad.write_text(json.dumps({'vendor_boot_a_sha256': VENDOR}))
    r = subprocess.run([sys.executable, str(D / 'tooling/persist/unit_profile.py'), '--lun0', str(ours),
                        '--android-slot-a', str(bad)], capture_output=True, text=True)
    check('a slot-A file missing one of the two digests is refused',
          r.returncode != 0 and 'no init_boot_a_sha256' in r.stderr, (r.stderr or r.stdout)[-70:])

    common.clear_unit_profile()
    common.apply_unit_profile(both)
    check('apply_unit_profile rebinds the backup table with the primary', common.GPT_BACKUP_SHA256 == tail_sha)
    named = common.slot_a_pair_name(VENDOR, INIT)
    check('slot_a_pairs accepts the pair this unit was built from', named is not None and 'this unit' in named, str(named)[:60])
    check('one digest of the pair swapped is still unknown', common.slot_a_pair_name(VENDOR, common.INIT_BOOT_A_SHA256) is None)
    probe = ('import sys; sys.path.insert(0, {path!r}); import common;'
             'print(common.slot_a_pair_name({v!r}, {i!r}) or "UNKNOWN")').format(path=str(D / 'tooling' / 'persist'), v=VENDOR, i=INIT)
    r = subprocess.run([sys.executable, '-c', probe], capture_output=True, text=True, cwd=str(D),
                       env={**__import__('os').environ, 'PIANO_UNIT_PROFILE': str(both)})
    check('a profile applied at import reaches slot_a_pairs (the pair is known without an explicit apply)',
          'this unit' in r.stdout, (r.stdout + r.stderr)[-70:])
    common.clear_unit_profile()
    check('clear_unit_profile forgets the unit pair and both tables',
          common.UNIT_SLOT_A_PAIR is None and common.slot_a_pair_name(VENDOR, INIT) is None
          and (common.GPT_PRIMARY_SHA256, common.GPT_BACKUP_SHA256) ==
              (common.REFERENCE['GPT_PRIMARY_SHA256'], common.REFERENCE['GPT_BACKUP_SHA256']))
print(f'\n{"FAILURES: " + ", ".join(FAILS) if FAILS else "all checks passed"} ({0 if not FAILS else len(FAILS)})')
sys.exit(1 if FAILS else 0)