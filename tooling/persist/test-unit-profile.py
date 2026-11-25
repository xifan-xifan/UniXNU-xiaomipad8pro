#!/usr/bin/env python3
"""Host test of the second-unit profile (tooling/persist/unit_profile.py, common.apply_unit_profile): SKILL.md gap G6.

Synthetic GPT heads (built here, no device, no backup): the tooling's own unit's geometry, another unit's, and the shapes a
profile must refuse. Run: python3 tooling/persist/test-unit-profile.py
"""
import json, struct, subprocess, sys, tempfile, uuid
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

print(f'\n{"FAILURES: " + ", ".join(FAILS) if FAILS else "all checks passed"} ({0 if not FAILS else len(FAILS)})')
sys.exit(1 if FAILS else 0)