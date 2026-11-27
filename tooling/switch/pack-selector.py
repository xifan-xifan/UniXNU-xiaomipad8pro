#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build piano's combined boot_a (T-16 UEFI OS switch): the stock Android boot.img with firmware/selector placed after the kernel's
memory footprint and the UEFI payload (BootShim + FD + DTB, the kernel field of a persistent macOS-set v4 boot.img) after it.
The Android ramdisk is the only ramdisk carried. The kernel's second instruction now branches to the selector, and image_size
grows so ABL reserves the room. The layout and undo check follow piano-uefi's tools/pack.py (BSD-2-Clause-Patent,
firmware/selector/NOTICE.md).

  pack-selector.py ANDROID_BOOT SELECTOR_BIN UEFI_BOOT -o OUT.img     -> OUT.img (unsigned) + OUT.img.avb.json (the stock props)
  pack-selector.py --check-signed OUT-signed.img ANDROID_BOOT [--unsigned OUT.img]
                      -> footer, props and rollback equal to the stock image's, optionally bind the exact packed payload

Both output paths must be new host files. Signing is tooling/bootimg/sign-on-remote.sh's job (the private key stays on the
signing host), with the props and rollback of OUT.img.avb.json. The boot patch level lives only in the stock footer."""
import argparse
import hashlib
import hmac
import json
import os
import stat
import struct
import sys

PAGE = 4096
V4_HEADER_SIZE = 1584
META_MAGIC = b'PIANO-UEFI-SEL1\0'
SEL_TAG = b'PIANO-SW-SEL-V1\0'
SELECTOR_ARGS = b'bootmonitor.bootmode=recovery\0bootmonitor.bootmode=poweroff_cg\0'
SELECTOR_STACK = 8192
BRANCH_RANGE = 1 << 27
ABL_KERNEL_BASE = 0xA8010000
KERNEL_RESERVED_END = 0xA8000000 + (256 << 20)   # the FD's "Kernel" reservation (MemoryMapLib.c)
BOOT_PARTITION = 100663296
UNSIGNED_LIMIT = BOOT_PARTITION - (1 << 20)
SELECTOR_LIMIT = 65536
PATCH_LEVEL = 'com.android.build.boot.security_patch'


def fail(msg):
    sys.exit(f'pack-selector: {msg}')


def align(n, a=PAGE):
    return (n + a - 1) // a * a


def read_input(path, limit):
    # Nonblocking open lets fstat reject a FIFO without waiting for a writer.
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as f:
        info = os.fstat(f.fileno())
        if not stat.S_ISREG(info.st_mode):
            fail('input must be a regular file')
        if info.st_size > limit:
            fail(f'input exceeds {limit} bytes')
        data = f.read(limit + 1)
        if len(data) > limit:
            fail(f'input exceeds {limit} bytes')
        return data


def read_boot(data):
    if data[:8] != b'ANDROID!':
        fail('not an Android boot image')
    if len(data) < PAGE:
        fail('truncated boot header')
    kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from('<4I', data, 8)
    version = struct.unpack_from('<I', data, 40)[0]
    if version != 4:
        fail(f'boot header v{version}; piano uses v4')
    if header_size != V4_HEADER_SIZE:
        fail(f'boot v4 header size {header_size}; expected {V4_HEADER_SIZE}')
    sig_size = struct.unpack_from('<I', data, V4_HEADER_SIZE - 4)[0]
    ramdisk_offset = PAGE + align(kernel_size)
    end = ramdisk_offset + align(ramdisk_size) + align(sig_size)
    if end > len(data):
        fail('truncated boot image sections')
    kernel = data[PAGE:PAGE + kernel_size]
    ramdisk = data[ramdisk_offset:ramdisk_offset + ramdisk_size]
    return {'header': bytearray(data[:header_size]), 'kernel': kernel, 'ramdisk': ramdisk,
            'sig_size': sig_size, 'os_version': os_version, 'end': end}


def kernel_info(kernel):
    if len(kernel) < 64 or kernel[:2] != b'MZ' or kernel[56:60] != b'ARMd':
        fail('kernel is not a raw arm64 Image')
    code1, _, image_size, flags = struct.unpack_from('<IQQQ', kernel, 4)
    if code1 >> 26 != 0b000101:
        fail('kernel code1 is not a branch to primary_entry')
    imm = code1 & 0x3ffffff
    if imm & 0x2000000:
        imm -= 0x4000000
    if not 64 <= 4 + imm * 4 <= len(kernel) - 4:
        fail('primary_entry outside the kernel')
    if image_size < len(kernel) or flags & 1:
        fail('unexpected image_size or a big-endian kernel')
    if META_MAGIC in kernel:
        fail('kernel already carries a selector')
    return code1, image_size


def avb_info(data):
    """Read AVB metadata for signing and comparison. The signer handles AVB authentication."""
    f = data[-64:]
    if f[:4] != b'AVBf':
        return None
    if len(f) != 64:
        fail('truncated AVB footer')
    _, _, orig, vb_off, vb_size = struct.unpack_from('>IIQQQ', f, 4)
    if not (0 < orig <= vb_off and 256 <= vb_size and vb_off + vb_size <= len(data) - 64):
        fail('invalid AVB image bounds')
    v = data[vb_off:vb_off + vb_size]
    if v[:4] != b'AVB0':
        fail('AVB footer without vbmeta')
    fields = struct.unpack_from('>IIQQIQQQQQQQQQQQII', v, 4)
    auth, aux_size, alg, desc_off, desc_size, rollback = fields[2], fields[3], fields[4], fields[13], fields[14], fields[15]
    if 256 + auth + aux_size > vb_size:
        fail('invalid AVB block bounds')
    if desc_off + desc_size > aux_size or desc_off % 8 or desc_size % 8:
        fail('invalid AVB descriptor bounds')
    p = 256 + auth + desc_off
    end = p + desc_size
    props, part = {}, None
    while p < end:
        if end - p < 16:
            fail('invalid AVB descriptor bounds')
        tag, n = struct.unpack_from('>QQ', v, p)
        p += 16
        if n % 8 or n > end - p:
            fail('invalid AVB descriptor bounds')
        body = v[p:p + n]
        p += n
        if tag == 0:
            if n < 16:
                fail('invalid AVB property descriptor')
            kl, vl = struct.unpack_from('>QQ', body, 0)
            key_end = 16 + kl
            value_end = key_end + 1 + vl
            if not kl or value_end >= n or body[key_end] or body[value_end]:
                fail('invalid AVB property descriptor')
            try:
                key = body[16:key_end].decode()
                value = body[key_end + 1:value_end].decode()
            except UnicodeDecodeError:
                fail('invalid UTF-8 in AVB property descriptor')
            if key in props or '\0' in key or '\0' in value:
                fail('ambiguous AVB property descriptor')
            props[key] = value
        elif tag == 2:
            if n < 116 or part is not None:
                fail('invalid or duplicate AVB hash descriptor')
            image_size = struct.unpack_from('>Q', body)[0]
            name_len, salt_len, digest_len = struct.unpack_from('>III', body, 40)
            name_end = 116 + name_len
            salt_end = name_end + salt_len
            digest_end = salt_end + digest_len
            hash_name = body[8:40].split(b'\0', 1)[0]
            if image_size != orig or not name_len or digest_end > n or hash_name not in (b'sha256', b'sha512'):
                fail('invalid AVB hash descriptor')
            try:
                part = body[116:name_end].decode()
            except UnicodeDecodeError:
                fail('invalid UTF-8 in AVB hash descriptor')
            hasher = hashlib.new(hash_name.decode(), body[name_end:salt_end])
            if digest_len != hasher.digest_size:
                fail('invalid AVB hash descriptor digest size')
            hasher.update(memoryview(data)[:orig])
            if not hmac.compare_digest(hasher.digest(), body[salt_end:digest_end]):
                fail('AVB payload digest mismatch')
    return {'original_size': orig, 'partition_size': len(data), 'rollback_index': rollback, 'algorithm': alg,
            'partition_name': part, 'props': props}


def wrap(kernel, selector, uefi, ramdisk_size):
    code1, image_size = kernel_info(kernel)
    if not SELECTOR_STACK < len(selector) <= SELECTOR_LIMIT or len(selector) % 4:
        fail('invalid selector size')
    m = selector.find(META_MAGIC)
    if m < 0 or selector.find(META_MAGIC, m + 1) >= 0:
        fail('selector metadata not found exactly once')
    if selector[m + 80:m + 96] != SEL_TAG:
        fail('selector.bin is not firmware/selector (version tag missing)')
    if any(selector[m + 16:m + 80]):
        fail('selector metadata already filled')
    args_end = m + 96 + len(SELECTOR_ARGS)
    stack = align(args_end, 16)
    if m % 16 or stack + SELECTOR_STACK + 4 > len(selector):
        fail('invalid selector metadata or stack bounds')
    if selector[m + 96:args_end] != SELECTOR_ARGS or any(selector[stack:stack + SELECTOR_STACK]):
        fail('invalid selector boot arguments or stack contents')
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
    if PAGE + total + align(ramdisk_size) > UNSIGNED_LIMIT:
        fail('the combined image leaves no room for the AVB footer')
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


def stock_avb(adata):
    avb = avb_info(adata)
    if not avb:
        fail('the Android boot.img has no AVB footer: its props (boot patch level) are needed')
    if avb['partition_name'] != 'boot' or avb['partition_size'] != BOOT_PARTITION:
        fail(f"unexpected AVB partition {avb['partition_name']} / size {avb['partition_size']}")
    if not avb['props'].get(PATCH_LEVEL) or not avb['rollback_index']:
        fail('the stock AVB footer needs a boot patch level and positive rollback index')
    return avb


def write_outputs(path, image, meta):
    # Create both host files exclusively, including the sidecar, before writing either.
    created = []
    try:
        with open(path, 'xb') as out:
            created.append(path)
            sidecar = path + '.avb.json'
            with open(sidecar, 'x', encoding='utf-8') as info:
                created.append(sidecar)
                out.write(image)
                json.dump(meta, info, indent=1, sort_keys=True)
    except OSError as e:
        for filename in created:
            os.unlink(filename)
        if isinstance(e, FileExistsError):
            fail('output already exists: ' + e.filename)
        raise


def pack(a):
    adata = read_input(a.android, BOOT_PARTITION)
    avb = stock_avb(adata)
    boot = read_boot(adata[:avb['original_size']])
    uefi_data = read_input(a.uefi, BOOT_PARTITION)
    uefi_avb = avb_info(uefi_data)
    uefi_boot = read_boot(uefi_data[:uefi_avb['original_size']] if uefi_avb else uefi_data)
    if uefi_boot['ramdisk']:
        fail('UEFI input carries a ramdisk; use a persistent macOS-set v4 image')
    uefi = uefi_boot['kernel']
    sel = read_input(a.selector, SELECTOR_LIMIT)
    kernel, lay = wrap(boot['kernel'], sel, uefi, len(boot['ramdisk']))
    img = write_boot(boot, kernel)
    if header_diff(boot['header'], bytearray(img[:len(boot['header'])])):
        fail('header changed beyond kernel_size and the signature size')
    if img[PAGE + align(len(kernel)):PAGE + align(len(kernel)) + len(boot['ramdisk'])] != boot['ramdisk']:
        fail('ramdisk moved wrongly')
    meta = dict(lay, android_sha256=hashlib.sha256(adata).hexdigest(), selector_sha256=hashlib.sha256(sel).hexdigest(),
                uefi_kernel_sha256=hashlib.sha256(uefi).hexdigest(), unsigned_sha256=hashlib.sha256(img).hexdigest(),
                unsigned_bytes=len(img), avb={k: avb[k] for k in ('partition_name', 'partition_size', 'rollback_index', 'props')})
    write_outputs(a.output, img, meta)
    print(f"selector at {lay['selector_offset']:#x}, UEFI at {lay['uefi_offset']:#x}, image_size "
          f"{lay['image_size_old']:#x} -> {lay['image_size_new']:#x}; {len(img)} bytes; sha256 {meta['unsigned_sha256']}")


def check_signed(a):
    s = read_input(a.check_signed, BOOT_PARTITION)
    oi = stock_avb(read_input(a.android, BOOT_PARTITION))
    si = avb_info(s)
    errs = []
    if not si:
        errs.append('no AVB footer')
    else:
        for k in ('partition_name', 'partition_size', 'rollback_index', 'props'):
            if si[k] != oi[k]:
                errs.append(f'{k}: {si[k]} != stock {oi[k]}')
        if si['algorithm'] != 2:
            errs.append(f"algorithm {si['algorithm']} (want SHA256_RSA4096)")
        if a.unsigned and s[:si['original_size']] != read_input(a.unsigned, UNSIGNED_LIMIT):
            errs.append('signed payload differs from the packed unsigned image')
        body = read_boot(s[:si['original_size']])['kernel']
        m = body.find(META_MAGIC)
        if m < 0 or body[m + 80:m + 96] != SEL_TAG:
            errs.append('no selector in the signed image kernel')
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
    ap.add_argument('--unsigned', metavar='PACKED', help='with --check-signed, compare the full payload with the packed image')
    a = ap.parse_args()
    try:
        if a.check_signed:
            check_signed(a)
        if a.unsigned:
            ap.error('--unsigned requires --check-signed')
        if not (a.selector and a.uefi and a.output):
            fail('need ANDROID_BOOT SELECTOR_BIN UEFI_BOOT -o OUT')
        pack(a)
    except OSError as e:
        fail(str(e))


if __name__ == '__main__':
    main()
