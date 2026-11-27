#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host tests of pack-selector.py: synthetic v4 boot images with an AVB footer built here, every refusal, the undo check, the
layout, check-signed; then, read only, the real stock android-set boot.img + macos-set-v5 kernel when both exist on this host.
  test-pack-selector.py SELECTOR_BIN [--real ANDROID_BOOT UEFI_BOOT]"""
import hashlib
import json
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.join(HERE, 'pack-selector.py')
PAGE = 4096
fails = 0


def check(c, msg):
    global fails
    print(('PASS ' if c else 'FAIL ') + msg)
    fails += 0 if c else 1


def kernel(size=3 << 20, image_size=None, code1=0x14000010):
    k = bytearray(os.urandom(size))
    struct.pack_into('<IIQQQ', k, 0, 0xfa405a4d, code1, 0, image_size or size + (1 << 20), 0x0a)
    k[0:2] = b'MZ'
    k[56:60] = b'ARMd'
    return bytes(k)


def boot(kern, ramdisk=b'', sig=0):
    h = bytearray(1584)
    h[:8] = b'ANDROID!'
    struct.pack_into('<4I', h, 8, len(kern), len(ramdisk), 0, 1584)
    struct.pack_into('<I', h, 40, 4)
    h[44:50] = b'piano!'
    struct.pack_into('<I', h, 1580, sig)
    out = h + bytes(PAGE - len(h))
    for p in (kern, ramdisk, bytes(sig)):
        if p:
            out += p + bytes((-len(p)) % PAGE)
    return bytes(out)


def prop(k, v):
    body = struct.pack('>QQ', len(k), len(v)) + k.encode() + b'\0' + v.encode() + b'\0'
    body += bytes((-len(body)) % 8)
    return struct.pack('>QQ', 0, len(body)) + body


def hashdesc(name, img):
    salt = bytes(range(32))
    digest = hashlib.sha256(salt + img).digest()
    body = struct.pack('>Q', len(img)) + b'sha256'.ljust(32, b'\0')
    body += struct.pack('>IIII', len(name), len(salt), len(digest), 0) + bytes(60) + name.encode() + salt + digest
    body += bytes((-len(body)) % 8)
    return struct.pack('>QQ', 2, len(body)) + body


def with_footer(img, props, rollback=1769904000, alg=2, part=100663296, name='boot'):
    aux = hashdesc(name, img) + b''.join(prop(k, v) for k, v in props.items())
    dsize = len(aux)
    aux += bytes((-len(aux)) % 64)
    auth = 0
    hdr = bytearray(256)
    hdr[:4] = b'AVB0'
    struct.pack_into('>IIQQIQQQQQQQQQQQII', hdr, 4, 1, 0, auth, len(aux), alg, 0, 0, 0, 0, 0, 0, 0, 0, 0, dsize, rollback, 0, 0)
    vb = bytes(hdr) + aux
    vb_off = len(img) + ((-len(img)) % PAGE)
    out = bytearray(part)
    out[:len(img)] = img
    out[vb_off:vb_off + len(vb)] = vb
    f = bytearray(64)
    f[:4] = b'AVBf'
    struct.pack_into('>IIQQQ', f, 4, 1, 0, len(img), vb_off, len(vb))
    out[-64:] = f
    return bytes(out)


PROPS = {'com.android.build.boot.os_version': '15', 'com.android.build.boot.security_patch': '2026-02-01'}


def run(*args):
    return subprocess.run([sys.executable, '-I', PACK] + list(args), capture_output=True, text=True, timeout=30)


def main(t):
    sel = sys.argv[1]
    p = lambda n: os.path.join(t, n)
    kern = kernel()
    rd = os.urandom(70000)
    android = with_footer(boot(kern, rd, sig=4096), PROPS)
    open(p('android.img'), 'wb').write(android)
    uefi = os.urandom(3293356)
    open(p('macos.img'), 'wb').write(boot(uefi))

    r = run(p('android.img'), sel, p('macos.img'), '-o', p('out.img'))
    print(r.stderr[-1500:]) if r.returncode else None
    check(r.returncode == 0, 'pack synthetic: ' + (r.stdout + r.stderr).strip()[:160])
    out = open(p('out.img'), 'rb').read()
    meta = json.load(open(p('out.img.avb.json')))
    kn = struct.unpack_from('<I', out, 8)[0]
    k2 = out[PAGE:PAGE + kn]
    code1 = struct.unpack_from('<I', k2, 4)[0]
    sel_off = meta['selector_offset']
    check(code1 >> 26 == 0b000101 and (code1 & 0x3ffffff) * 4 + 4 == sel_off, 'code1 branches to the selector')
    check(sel_off == (struct.unpack_from('<Q', kern, 16)[0] + PAGE - 1) // PAGE * PAGE, 'selector at align(image_size)')
    undo = bytearray(k2[:len(kern)])
    struct.pack_into('<I', undo, 4, 0x14000010)
    struct.pack_into('<Q', undo, 16, struct.unpack_from('<Q', kern, 16)[0])
    check(bytes(undo) == kern, 'undoing the two header fields gives the stock kernel')
    s = open(sel, 'rb').read()
    m = s.find(b'PIANO-UEFI-SEL1\0')
    f = struct.unpack_from('<QIIQQQQQQ', k2, sel_off + m + 16)
    check(f[0] == sel_off and f[1] == 0x14000010 and f[4] == meta['uefi_offset'] and f[5] == len(uefi) and f[6] == len(kern) and f[7] == f[8] == 0,
          'metadata filled (selector_offset, code1, uefi offset/size, kernel size)')
    check(k2[meta['uefi_offset']:meta['uefi_offset'] + len(uefi)] == uefi and meta['uefi_offset'] % 0x10000 == 0,
          'UEFI payload placed, 64 KiB aligned')
    check(struct.unpack_from('<Q', k2, 16)[0] == meta['image_size_new'] >= meta['uefi_offset'] + len(uefi), 'image_size covers the payload')
    check(struct.unpack_from('<I', out, 1580)[0] == 0, 'v4 boot signature size zeroed')
    roff = PAGE + (kn + PAGE - 1) // PAGE * PAGE
    check(out[roff:roff + len(rd)] == rd, 'ramdisk carried')
    check(out[:8] == android[:8] and out[12:1580] == android[12:1580], 'header unchanged except kernel_size and sig size')
    check(meta['avb']['props'] == PROPS and meta['avb']['rollback_index'] == 1769904000, 'stock props and rollback recorded')
    check(out[-64:-60] != b'AVBf', 'output is unsigned (signing is sign-on-remote.sh)')

    # check-signed: a footer with the stock props passes, a wrong patch level / rollback / no footer fails
    open(p('good.img'), 'wb').write(with_footer(out, PROPS))
    check(run('--check-signed', p('good.img'), p('android.img')).returncode == 0, 'check-signed accepts the stock props')
    check(run('--check-signed', p('good.img'), p('android.img'), '--unsigned', p('out.img')).returncode == 0,
          'check-signed accepts the exact packed payload')
    other = bytearray(out)
    other[PAGE + 128] ^= 1
    open(p('other-signed.img'), 'wb').write(with_footer(other, PROPS))
    r = run('--check-signed', p('other-signed.img'), p('android.img'), '--unsigned', p('out.img'))
    check(r.returncode != 0 and 'differs from the packed unsigned image' in r.stdout,
          'check-signed refuses a different payload even with matching AVB properties and digest')
    bad = dict(PROPS, **{'com.android.build.boot.security_patch': '2025-01-01'})
    open(p('bad1.img'), 'wb').write(with_footer(out, bad))
    check(run('--check-signed', p('bad1.img'), p('android.img')).returncode != 0, 'check-signed refuses another patch level')
    open(p('bad2.img'), 'wb').write(with_footer(out, PROPS, rollback=0))
    check(run('--check-signed', p('bad2.img'), p('android.img')).returncode != 0, 'check-signed refuses rollback 0')
    check(run('--check-signed', p('out.img'), p('android.img')).returncode != 0, 'check-signed refuses an unsigned image')
    open(p('stock-signed.img'), 'wb').write(android)
    check(run('--check-signed', p('stock-signed.img'), p('android.img')).returncode != 0, 'check-signed refuses an image without a selector')
    open(p('marker-in-ramdisk.img'), 'wb').write(with_footer(boot(kern, rd + b'PIANO-UEFI-SEL1\0'), PROPS))
    check(run('--check-signed', p('marker-in-ramdisk.img'), p('android.img')).returncode != 0,
          'check-signed requires the selector marker inside the kernel')

    # refusals
    def refuse(name, a, why):
        try:
            rr = run(*a)
        except subprocess.TimeoutExpired:
            check(False, f'refuses {name} without blocking')
            return
        check(rr.returncode != 0 and why in rr.stderr and 'Traceback' not in rr.stderr,
              f'refuses {name} ({rr.stderr.strip()[:90]})')

    def pack_refusal(name, selector=sel, uefi_path=p('macos.img'), android_path=p('android.img'), *, why):
        refuse(name, [android_path, selector, uefi_path, '-o', p('x.img')], why)
        for suffix in ('', '.avb.json'):
            if os.path.isfile(p('x.img' + suffix)):
                check(False, f'{name} created an output{suffix}')
                os.unlink(p('x.img' + suffix))

    for name, data, why in (
            ('truncated selector', s[:m + 96], 'selector size'),
            ('selector truncated inside its stack', s[:m + 8192], 'selector metadata or stack bounds'),
            ('selector with corrupt boot mode strings', s[:m + 96] + b'X' + s[m + 97:], 'selector boot arguments'),
            ('selector cut in an instruction', s[:-1], 'selector size'),
            ('oversized selector', s.ljust(65537, b'\0'), 'input exceeds')):
        with open(p('malformed-selector.bin'), 'wb') as f:
            f.write(data)
        pack_refusal(name, p('malformed-selector.bin'), why=why)

    vb_off = struct.unpack_from('>Q', android, len(android) - 44)[0]
    desc = vb_off + 256
    property_desc = desc + 16 + struct.unpack_from('>Q', android, desc + 8)[0]
    for name, off, content, why in (
            ('AVB payload with a flipped byte', PAGE + 128, bytes([android[PAGE + 128] ^ 1]), 'AVB payload digest mismatch'),
            ('AVB auxiliary block outside vbmeta', vb_off + 20, struct.pack('>Q', 1 << 63), 'AVB block bounds'),
            ('AVB descriptor table outside the auxiliary block', vb_off + 104, struct.pack('>Q', 1 << 63), 'AVB descriptor bounds'),
            ('AVB hash descriptor extending past its table', desc + 8, struct.pack('>Q', 1 << 63), 'AVB descriptor bounds'),
            ('AVB hash name extending past its body', desc + 16 + 40, struct.pack('>I', 1 << 31), 'AVB hash descriptor'),
            ('AVB hash image size differing from its footer', desc + 16, struct.pack('>Q', 1), 'AVB hash descriptor'),
            ('AVB property with a truncated body', property_desc + 8, struct.pack('>Q', 8), 'AVB property descriptor'),
            ('AVB property value extending past its body', property_desc + 16 + 8, struct.pack('>Q', 1 << 63), 'AVB property descriptor'),
            ('AVB property with invalid UTF-8', property_desc + 32, b'\xff', 'AVB property descriptor')):
        with open(p('android.img'), 'r+b') as f:
            f.seek(off)
            original = f.read(len(content))
            f.seek(off)
            f.write(content)
        pack_refusal(name, why=why)
        with open(p('android.img'), 'r+b') as f:
            f.seek(off)
            f.write(original)

    with open(p('macos.img'), 'rb') as f:
        macos = f.read()
    os.link(p('macos.img'), p('alias.img'))
    refuse('an output hard-linked to the UEFI input',
           [p('android.img'), sel, p('macos.img'), '-o', p('alias.img')], 'output already exists')
    check(open(p('macos.img'), 'rb').read() == macos, 'UEFI input is unchanged')
    with open(p('macos.img'), 'wb') as f:
        f.write(macos)
    os.symlink(p('android.img'), p('sidecar.img.avb.json'))
    refuse('a sidecar linked to the Android input',
           [p('android.img'), sel, p('macos.img'), '-o', p('sidecar.img')], 'output already exists')
    check(open(p('android.img'), 'rb').read() == android, 'Android input is unchanged')
    check(not os.path.exists(p('sidecar.img')), 'sidecar refusal leaves no image')
    with open(p('android.img'), 'wb') as f:
        f.write(android)
    with open(p('oversized.img'), 'wb') as f:
        f.truncate(len(android) + 1)
    pack_refusal('an oversized input file', uefi_path=p('oversized.img'), why='input exceeds')
    os.mkfifo(p('pipe.img'))
    pack_refusal('a non-regular input file', uefi_path=p('pipe.img'), why='regular file')
    refuse('a stock reference without its AVB footer',
           ['--check-signed', p('good.img'), p('out.img')], 'no AVB footer')
    with open(p('good.img'), 'r+b') as f:
        f.seek(PAGE + 128)
        byte = f.read(1)[0]
        f.seek(PAGE + 128)
        f.write(bytes([byte ^ 1]))
    refuse('a signed image with a corrupted payload',
           ['--check-signed', p('good.img'), p('android.img')], 'AVB payload digest mismatch')
    with open(p('no-patch-level.img'), 'wb') as f:
        f.write(with_footer(boot(kern, rd), {'com.android.build.boot.os_version': '15'}))
    pack_refusal('stock AVB metadata without a boot patch level', android_path=p('no-patch-level.img'), why='boot patch level')
    open(p('nofooter.img'), 'wb').write(boot(kern, rd))
    refuse('no AVB footer', [p('nofooter.img'), sel, p('macos.img'), '-o', p('x.img')], 'no AVB footer')
    open(p('truncated-android.img'), 'wb').write(with_footer(boot(kern, rd)[:-PAGE], PROPS))
    refuse('a stock ramdisk truncated before AVB padding',
           [p('truncated-android.img'), sel, p('macos.img'), '-o', p('x.img')], 'truncated boot image sections')
    open(p('wrapped.img'), 'wb').write(with_footer(out, PROPS))
    refuse('a second wrap', [p('wrapped.img'), sel, p('macos.img'), '-o', p('x.img')], 'already carries a selector')
    open(p('nobranch.img'), 'wb').write(with_footer(boot(kernel(code1=0xd503201f), rd), PROPS))
    refuse('code1 not a branch', [p('nobranch.img'), sel, p('macos.img'), '-o', p('x.img')], 'not a branch')
    open(p('bigk.img'), 'wb').write(with_footer(boot(kernel(image_size=250 << 20), rd), PROPS))
    refuse('a selector out of branch range', [p('bigk.img'), sel, p('macos.img'), '-o', p('x.img')], 'branch range')
    open(p('bigk.img'), 'wb').write(with_footer(boot(kernel(image_size=99 << 20), rd), PROPS))
    pack_refusal('an image within branch range but over the partition limit', android_path=p('bigk.img'),
                 why='no room for the AVB footer')
    open(p('bad-entry.img'), 'wb').write(with_footer(boot(kernel(size=69), rd), PROPS))
    pack_refusal('a partial primary_entry instruction', android_path=p('bad-entry.img'), why='primary_entry outside')
    open(p('fakesel.bin'), 'wb').write(s.replace(b'PIANO-SW-SEL-V1\0', b'X' * 16))
    refuse('a foreign selector', [p('android.img'), p('fakesel.bin'), p('macos.img'), '-o', p('x.img')], 'version tag')
    open(p('tinyuefi.img'), 'wb').write(boot(b'MZ' + bytes(100)))
    refuse('no UEFI payload', [p('android.img'), sel, p('tinyuefi.img'), '-o', p('x.img')], 'no UEFI payload')
    open(p('truncated-uefi.img'), 'wb').write(boot(uefi)[:-PAGE])
    refuse('a truncated UEFI payload', [p('android.img'), sel, p('truncated-uefi.img'), '-o', p('x.img')],
           'truncated boot image sections')
    open(p('truncated-signed-uefi.img'), 'wb').write(with_footer(boot(uefi)[:-PAGE], PROPS, part=8 << 20))
    refuse('a UEFI payload truncated before AVB padding',
           [p('android.img'), sel, p('truncated-signed-uefi.img'), '-o', p('x.img')], 'truncated boot image sections')
    with open(p('truncated-signed-uefi.img'), 'r+b') as f:
        f.seek(-52, os.SEEK_END)  # original image size in the AVB footer
        f.write(struct.pack('>Q', 8 << 20))
    refuse('an AVB original size extending past vbmeta',
           [p('android.img'), sel, p('truncated-signed-uefi.img'), '-o', p('x.img')], 'invalid AVB image bounds')
    open(p('uefi-ram.img'), 'wb').write(boot(uefi, rd))
    refuse('a UEFI RAM image whose ramdisk would be dropped',
           [p('android.img'), sel, p('uefi-ram.img'), '-o', p('x.img')], 'UEFI input carries a ramdisk')
    open(p('vendor.img'), 'wb').write(with_footer(boot(kern, rd), PROPS, name='vendor_boot'))
    refuse('another partition footer', [p('vendor.img'), sel, p('macos.img'), '-o', p('x.img')], 'unexpected AVB partition')
    check(not os.path.exists(p('x.img')) and not os.path.exists(p('x.img.avb.json')), 'refused inputs create no output')

    # determinism
    run(p('android.img'), sel, p('macos.img'), '-o', p('out2.img'))
    check(open(p('out2.img'), 'rb').read() == out, 'deterministic output')

    if '--real' in sys.argv:
        i = sys.argv.index('--real')
        ra, ru = sys.argv[i + 1], sys.argv[i + 2]
        if os.path.exists(ra) and os.path.exists(ru):
            r = run(ra, sel, ru, '-o', p('real.img'))
            check(r.returncode == 0, 'pack the real stock boot + macos-set kernel: ' + (r.stdout + r.stderr).strip()[:200])
            if r.returncode == 0:
                mj = json.load(open(p('real.img.avb.json')))
                spec = importlib.util.spec_from_file_location('packsel',
                        os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pack-selector.py'))
                packs = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(packs)
                stock = packs.avb_info(open(ra, 'rb').read())
                check(mj['avb']['props'] == stock['props'], 'real: the stock props kept, patch level '
                      + str(stock['props'].get('com.android.build.boot.security_patch')))
                check(mj['avb']['rollback_index'] == stock['rollback_index'],
                      'real: the stock rollback index ' + str(stock['rollback_index']) + ' kept')
                check(0xA8010000 + mj['image_size_new'] <= 0xB6382000, "real: the image ends below Android's initrd 0xB6382000")
                print('INFO real layout ' + json.dumps({k: hex(v) if isinstance(v, int) else v for k, v in mj.items() if k != 'avb'}))
        else:
            print('INFO real images absent: skipped')
    print(f'{"FAIL" if fails else "PASS"}: pack-selector tests, {fails} failures')
    sys.exit(1 if fails else 0)


with tempfile.TemporaryDirectory(prefix='packsel-') as work:
    main(work)
