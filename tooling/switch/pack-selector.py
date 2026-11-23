#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build piano's combined boot_a (T-16 UEFI OS switch): the stock Android boot.img with firmware/selector placed after the kernel's
memory footprint and the UEFI payload (BootShim + FD + DTB, the kernel field of a macOS-set boot.img) after it. The kernel's
second instruction (b primary_entry) is redirected to the selector; image_size grows so ABL reserves the room. The layout and the
undo check follow piano-uefi's tools/pack.py (BSD-2-Clause-Patent, firmware/selector/NOTICE.md).

  pack-selector.py ANDROID_BOOT SELECTOR_BIN UEFI_BOOT -o OUT.img     -> OUT.img (unsigned) + OUT.img.avb.json (the stock props)
  pack-selector.py --check-signed OUT-signed.img ANDROID_BOOT         -> footer, props and rollback equal to the stock image's

Signing is tooling/bootimg/sign-on-remote.sh's job (the private key stays on the signing host), with the props and rollback of
OUT.img.avb.json: the boot patch level lives only in the stock footer, so an image without it must never be booted."""
import argparse
import hashlib
import json
import struct
import sys

PAGE = 4096
META_MAGIC = b'PIANO-UEFI-SEL1\0'
SEL_TAG = b'PIANO-SW-SEL-V1\0'
BRANCH_RANGE = 1 << 27
ABL_KERNEL_BASE = 0xA8010000
KERNEL_RESERVED_END = 0xA8000000 + (256 << 20)   # the FD's "Kernel" reservation (MemoryMapLib.c)
BOOT_PARTITION = 100663296


def fail(msg):
    sys.exit(f'pack-selector: {msg}')


def align(n, a=PAGE):
    return (n + a - 1) // a * a


def read_boot(data):
    if data[:8] != b'ANDROID!':
        fail('not an Android boot image')
    kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from('<4I', data, 8)
    version = struct.unpack_from('<I', data, 40)[0]
    if version != 4:
        fail(f'boot header v{version}; piano uses v4')
    sig_size = struct.unpack_from('<I', data, 1580)[0]
    off = PAGE
    kernel = data[off:off + kernel_size]
    off += align(kernel_size)
    ramdisk = data[off:off + ramdisk_size]
    off += align(ramdisk_size)
    return {'header': bytearray(data[:header_size]), 'kernel': kernel, 'ramdisk': ramdisk,
            'sig_size': sig_size, 'os_version': os_version, 'end': off + align(sig_size) if sig_size else off}


def kernel_info(kernel):
    if kernel[:2] != b'MZ' or kernel[56:60] != b'ARMd':
        fail('kernel is not a raw arm64 Image')
    code0, code1, text_offset, image_size, flags = struct.unpack_from('<IIQQQ', kernel)
    if code1 >> 26 != 0b000101:
        fail('kernel code1 is not a branch to primary_entry')
    imm = code1 & 0x3ffffff
    if imm & 0x2000000:
        imm -= 0x4000000
    if not 64 <= 4 + imm * 4 < len(kernel):
        fail('primary_entry outside the kernel')
    if image_size < len(kernel) or flags & 1:
        fail('unexpected image_size or a big-endian kernel')
    if META_MAGIC in kernel:
        fail('kernel already carries a selector')
    return code1, image_size


def avb_info(data):
    """The AVB footer's original size, rollback index and props (our parser; None without a footer)."""
    f = data[-64:]
    if f[:4] != b'AVBf':
        return None
    _, _, orig, vb_off, vb_size = struct.unpack_from('>IIQQQ', f, 4)
    v = data[vb_off:vb_off + vb_size]
    if v[:4] != b'AVB0':
        fail('AVB footer without vbmeta')
    fields = struct.unpack_from('>IIQQIQQQQQQQQQQQII', v, 4)
    auth, aux_size, alg, desc_off, desc_size, rollback = fields[2], fields[3], fields[4], fields[13], fields[14], fields[15]
    aux = v[256 + auth:256 + auth + aux_size]
    props, part, p = {}, None, desc_off
    while p + 16 <= min(desc_off + desc_size, len(aux)):
        tag, n = struct.unpack_from('>QQ', aux, p)
        body = aux[p + 16:p + 16 + n]
        if tag == 0:
            kl, vl = struct.unpack_from('>QQ', body, 0)
            props[body[16:16 + kl].decode()] = body[16 + kl + 1:16 + kl + 1 + vl].decode()
        elif tag == 2:
            pnl = struct.unpack_from('>I', body, 40)[0]
            part = body[116:116 + pnl].decode()
        p += 16 + n
    return {'original_size': orig, 'partition_size': len(data), 'rollback_index': rollback, 'algorithm': alg,
            'partition_name': part, 'props': props}


def wrap(kernel, selector, uefi):
    code1, image_size = kernel_info(kernel)
    m = selector.find(META_MAGIC)
    if m < 0 or selector.find(META_MAGIC, m + 1) >= 0:
        fail('selector metadata not found exactly once')
    if selector[m + 80:m + 96] != SEL_TAG:
        fail('selector.bin is not firmware/selector (version tag missing)')
    if any(selector[m + 16:m + 80]):
        fail('selector metadata already filled')
    if len(uefi) < 65536:
        fail('no UEFI payload')
    sel_off = align(image_size)
    uefi_off = align(sel_off + len(selector), 0x10000)
    end = uefi_off + len(uefi)
    total = align(end)
    if sel_off - 4 >= BRANCH_RANGE:
        fail('selector out of branch range')
    if ABL_KERNEL_BASE + total > KERNEL_RESERVED_END:
        fail(f'image_size {total:#x} leaves the FD kernel reservation')
    out = bytearray(end)
    out[:len(kernel)] = kernel
    out[sel_off:sel_off + len(selector)] = selector
    out[uefi_off:end] = uefi
    struct.pack_into('<QIIQQQQQQ', out, sel_off + m + 16, sel_off, code1, image_size & 0xffffffff, image_size,
                     uefi_off, len(uefi), len(kernel), 0, 0)
    struct.pack_into('<I', out, 4, 0x14000000 | ((sel_off - 4) // 4))
    struct.pack_into('<Q', out, 16, total)
    check = bytearray(out[:len(kernel)])
    struct.pack_into('<I', check, 4, code1)
    struct.pack_into('<Q', check, 16, image_size)
    if bytes(check) != kernel:
        fail('internal: kernel bytes changed beyond the two header fields')
    return bytes(out), {'selector_offset': sel_off, 'uefi_offset': uefi_off, 'uefi_size': len(uefi),
                        'image_size_old': image_size, 'image_size_new': total, 'kernel_size_old': len(kernel)}


def write_boot(boot, kernel):
    h = bytearray(boot['header'])
    struct.pack_into('<I', h, 8, len(kernel))
    struct.pack_into('<I', h, 1580, 0)      # the v4 boot signature no longer matches
    out = h + bytes(PAGE - len(h))
    for part in (kernel, boot['ramdisk']):
        out += part + bytes(align(len(part)) - len(part))
    return bytes(out)


def header_diff(a, b):
    """Byte offsets where two v4 headers differ, outside kernel_size (8..11) and the signature size (1580..1583)."""
    allowed = set(range(8, 12)) | set(range(1580, 1584))
    return [i for i in range(len(a)) if a[i] != b[i] and i not in allowed]


def pack(a):
    adata = open(a.android, 'rb').read()
    avb = avb_info(adata)
    if not avb:
        fail('the Android boot.img has no AVB footer: its props (boot patch level) are needed')
    if avb['partition_name'] != 'boot' or avb['partition_size'] != BOOT_PARTITION:
        fail(f"unexpected AVB partition {avb['partition_name']} / size {avb['partition_size']}")
    boot = read_boot(adata)
    uefi = read_boot(open(a.uefi, 'rb').read())['kernel']
    sel = open(a.selector, 'rb').read()
    kernel, lay = wrap(boot['kernel'], sel, uefi)
    img = write_boot(boot, kernel)
    if len(img) > BOOT_PARTITION - (1 << 20):
        fail('the combined image leaves no room for the AVB footer')
    if header_diff(boot['header'], bytearray(img[:len(boot['header'])])):
        fail('header changed beyond kernel_size and the signature size')
    if img[PAGE + align(len(kernel)):PAGE + align(len(kernel)) + len(boot['ramdisk'])] != boot['ramdisk']:
        fail('ramdisk moved wrongly')
    open(a.output, 'wb').write(img)
    meta = dict(lay, android_sha256=hashlib.sha256(adata).hexdigest(), selector_sha256=hashlib.sha256(sel).hexdigest(),
                uefi_kernel_sha256=hashlib.sha256(uefi).hexdigest(), unsigned_sha256=hashlib.sha256(img).hexdigest(),
                unsigned_bytes=len(img), avb={k: avb[k] for k in ('partition_name', 'partition_size', 'rollback_index', 'props')})
    json.dump(meta, open(a.output + '.avb.json', 'w'), indent=1, sort_keys=True)
    print(f"selector at {lay['selector_offset']:#x}, UEFI at {lay['uefi_offset']:#x}, image_size "
          f"{lay['image_size_old']:#x} -> {lay['image_size_new']:#x}; {len(img)} bytes; sha256 {meta['unsigned_sha256']}")


def check_signed(a):
    s, o = open(a.check_signed, 'rb').read(), open(a.android, 'rb').read()
    si, oi = avb_info(s), avb_info(o)
    errs = []
    if not si:
        errs.append('no AVB footer')
    else:
        for k in ('partition_name', 'partition_size', 'rollback_index', 'props'):
            if si[k] != oi[k]:
                errs.append(f'{k}: {si[k]} != stock {oi[k]}')
        if si['algorithm'] != 2:
            errs.append(f"algorithm {si['algorithm']} (want SHA256_RSA4096)")
    body = s[:si['original_size']] if si else s
    if META_MAGIC not in body:
        errs.append('no selector in the signed image')
    for e in errs:
        print('FAIL ' + e)
    print('PASS signed image matches the stock footer' if not errs else 'FAIL signed image')
    sys.exit(1 if errs else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('android')
    ap.add_argument('selector', nargs='?')
    ap.add_argument('uefi', nargs='?')
    ap.add_argument('-o', '--output')
    ap.add_argument('--check-signed', metavar='SIGNED')
    a = ap.parse_args()
    if a.check_signed:
        check_signed(a)
    if not (a.selector and a.uefi and a.output):
        fail('need ANDROID_BOOT SELECTOR_BIN UEFI_BOOT -o OUT')
    pack(a)


if __name__ == '__main__':
    main()
