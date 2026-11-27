#!/usr/bin/env python3
# Host test for maint-session.py's gpt_set_attr_bit (step slot-success-crc), 2026-11-20.
# With real copies (argv: primary.img backup.img sgdisk-after.bin total_lbas), checks that the computed primary equals what
# `sgdisk -A N:set:54` wrote except the protective MBR (LBA 0), which must stay as backed up; that both copies' CRCs verify; that only
# entry N's attributes and the two CRCs change; and that the backup copy's entry array equals the primary's.
# Without arguments it runs the same checks on a synthetic GPT.
import importlib.util, struct, sys, zlib
from pathlib import Path

here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('ms', here / 'maint-session.py')
ms = importlib.util.module_from_spec(spec); sys.argv_backup = sys.argv; spec.loader.exec_module(ms)
B = 4096


def header_ok(blob, blob_lba, header_lba):
    h = (header_lba - blob_lba) * B
    hsize, = struct.unpack_from('<I', blob, h + 12)
    hcrc, = struct.unpack_from('<I', blob, h + 16)
    z = bytearray(blob[h:h + hsize]); struct.pack_into('<I', z, 16, 0)
    entries_lba, count, esize = struct.unpack_from('<QII', blob, h + 72)
    ecrc, = struct.unpack_from('<I', blob, h + 88)
    e = (entries_lba - blob_lba) * B
    return (zlib.crc32(bytes(z)) & 0xffffffff) == hcrc and (zlib.crc32(blob[e:e + count * esize]) & 0xffffffff) == ecrc, e, count * esize


def synthetic(total=120000, number=13):
    entries = bytearray(128 * 96)
    for i in range(96):
        struct.pack_into('<16s16sQQQ', entries, i * 128, b'T' * 16, bytes([i]) * 16, 10 + i, 11 + i, 0x003F000000000000 if i == number - 1 else 0)
    offset = (number - 1) * 128
    start = ms.common.BOOT_A_START_512 // (B // 512)
    struct.pack_into('<QQ', entries, offset + 32, start, start + ms.common.BOOT_A_SIZE_512 // (B // 512) - 1)
    entries[offset + 56:offset + 68] = 'boot_a'.encode('utf-16-le')
    def hdr(cur, alt, elba):
        h = bytearray(B); h[:8] = b'EFI PART'; struct.pack_into('<IIIIQQQQ16sQIII', h, 8, 0x10000, 92, 0, 0, cur, alt, 6, total - 8,
                                                                b'G' * 16, elba, 96, 128, zlib.crc32(entries) & 0xffffffff)
        struct.pack_into('<I', h, 16, zlib.crc32(bytes(h[:92])) & 0xffffffff); return bytes(h)
    mbr = bytearray(B); mbr[446:462] = bytes([0, 0, 1, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF]); mbr[510:512] = b'\x55\xaa'
    primary = bytes(mbr) + hdr(1, total - 1, 2) + bytes(entries) + bytes(6 * B - 2 * B - len(entries))
    backup = bytes(entries)[2 * B:] + bytes(4 * B - (len(entries) - 2 * B)) + hdr(total - 1, 1, total - 7)  # piano: entries at total-7
    return primary, backup, total, number


def check(primary, backup, total, number, after=None):
    """backup = the last 5 LBAs as backed up; the backup entry array may start before them (its header says where)."""
    new_p = ms.gpt_set_attr_bit(primary, 0, 1, number, 54)
    assert new_p[:B] == primary[:B], 'protective MBR changed'
    ok_p, ep, np_ = header_ok(new_p, 0, 1)
    assert ok_p, 'primary CRCs do not verify'
    bh = backup[4 * B:5 * B]
    be, bc, bsz = struct.unpack_from('<QII', bh, 72)
    entries = new_p[ep:ep + np_]
    new_bh = ms.gpt_backup_header_with(bh, entries)
    # assemble the backup copy as it will be on disk: entries at LBA be.., header at total-1
    span_lo = min(be, total - 5); img = bytearray((total - span_lo) * B)
    img[(total - 5 - span_lo) * B:] = backup                      # what is there in the backed-up 5 LBAs
    img[(be - span_lo) * B:(be - span_lo) * B + len(entries)] = entries
    img[(total - 1 - span_lo) * B:] = new_bh
    ok_b, eb, nb = header_ok(bytes(img), span_lo, total - 1)
    assert ok_b, 'backup CRCs do not verify'
    assert bytes(img[eb:eb + nb]) == entries, 'entry arrays of the two copies differ'
    # repair data (lun4-backup-entries-repair): the ORIGINAL primary entries match the original backup header's entry CRC
    orig_ok = struct.unpack_from('<I', bh, 88)[0] == zlib.crc32(primary[2 * B:2 * B + bc * bsz]) & 0xffffffff
    assert orig_ok, 'original backup entry CRC != crc32(original primary entries)'
    idx = number - 1
    allowed = set(range(B + 16, B + 20)) | set(range(B + 88, B + 92)) | set(range(2 * B + idx * 128 + 48, 2 * B + idx * 128 + 56))
    diff = {i for i in range(len(primary)) if primary[i] != new_p[i]}
    assert diff and diff <= allowed, f'unexpected primary bytes changed: {sorted(diff - allowed)}'
    attrs = struct.unpack_from('<Q', new_p, 2 * B + idx * 128 + 48)[0]
    assert attrs >> 54 & 1, 'bit 54 not set'
    if after is not None:
        want = primary[:B] + after[B:]
        assert new_p == want, f'differs from sgdisk output outside LBA 0 at {[i for i in range(len(want)) if want[i] != new_p[i]][:8]}'
    return len(diff), hex(attrs), f'backup entries LBA {be}'


if len(sys.argv) == 5:
    p, b, a = (Path(x).read_bytes() for x in sys.argv[1:4]); total = int(sys.argv[4])
    print('real copies:', check(p, b, total, 13, a), 'PASS (equals sgdisk output with the original protective MBR)')
else:
    primary, backup, total, number = synthetic()
    print('synthetic:', check(primary, backup, total, number), 'PASS')
    _, entries, be, nblk = ms.slot_crc_layout(primary, backup, total, number)
    assert (be, nblk, len(entries)) == (total - 7, 3, 96 * 128)

    damaged_primary = bytearray(primary)
    damaged_primary[2 * B + 10] ^= 1
    damaged_backup = bytearray(backup)
    struct.pack_into('<Q', damaged_backup, 4 * B + 72, 20)  # a valid header CRC must still not permit writes inside usable space
    new_header = ms.gpt_backup_header_with(damaged_backup[-B:], entries)
    damaged_backup[-B:] = new_header
    for head, tail, part, reason in (
        (bytes(damaged_primary), backup, number, 'primary entry array CRC'),
        (primary, bytes(damaged_backup), number, 'outside their reserved regions'),
        (primary, backup, number - 1, 'exact boot_a partition'),
    ):
        try:
            ms.slot_crc_layout(head, tail, total, part)
        except ms.Stop as exc:
            assert reason in str(exc), str(exc)
        else:
            raise AssertionError(f'accepted invalid LUN 4 GPT: {reason}')
    print('PASS: LUN 4 GPT write preflight refuses an invalid CRC, usable-space overlap or wrong entry')
