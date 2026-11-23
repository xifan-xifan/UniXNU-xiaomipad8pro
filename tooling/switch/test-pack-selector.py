#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host tests of pack-selector.py: synthetic v4 boot images with an AVB footer built here, every refusal, the undo check, the
layout, check-signed; then, read only, the real stock android-set boot.img + macos-set-v5 kernel when both exist on this host.
  test-pack-selector.py SELECTOR_BIN [--real ANDROID_BOOT UEFI_BOOT]"""
import hashlib
import json
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


def hashdesc(name, size):
    body = struct.pack('>Q', size) + b'sha256'.ljust(32, b'\0') + struct.pack('>IIII', len(name), 32, 32, 0) + bytes(60) + name.encode()
    body += bytes(64)
    body += bytes((-len(body)) % 8)
    return struct.pack('>QQ', 2, len(body)) + body


def with_footer(img, props, rollback=1769904000, alg=2, part=100663296, name='boot'):
    aux = hashdesc(name, len(img)) + b''.join(prop(k, v) for k, v in props.items())
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
    return subprocess.run([sys.executable, '-I', PACK] + list(args), capture_output=True, text=True)


def main():
    sel = sys.argv[1]
    t = tempfile.mkdtemp(prefix='packsel-')
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
    bad = dict(PROPS, **{'com.android.build.boot.security_patch': '2025-01-01'})
    open(p('bad1.img'), 'wb').write(with_footer(out, bad))
    check(run('--check-signed', p('bad1.img'), p('android.img')).returncode != 0, 'check-signed refuses another patch level')
    open(p('bad2.img'), 'wb').write(with_footer(out, PROPS, rollback=0))
    check(run('--check-signed', p('bad2.img'), p('android.img')).returncode != 0, 'check-signed refuses rollback 0')
    check(run('--check-signed', p('out.img'), p('android.img')).returncode != 0, 'check-signed refuses an unsigned image')
    open(p('stock-signed.img'), 'wb').write(android)
    check(run('--check-signed', p('stock-signed.img'), p('android.img')).returncode != 0, 'check-signed refuses an image without a selector')

    # refusals
    def refuse(name, a, why):
        rr = run(*a)
        check(rr.returncode != 0 and why in rr.stderr, f'refuses {name} ({rr.stderr.strip()[:90]})')
    open(p('nofooter.img'), 'wb').write(boot(kern, rd))
    refuse('no AVB footer', [p('nofooter.img'), sel, p('macos.img'), '-o', p('x.img')], 'no AVB footer')
    open(p('wrapped.img'), 'wb').write(with_footer(out, PROPS))
    refuse('a second wrap', [p('wrapped.img'), sel, p('macos.img'), '-o', p('x.img')], 'already carries a selector')
    open(p('nobranch.img'), 'wb').write(with_footer(boot(kernel(code1=0xd503201f), rd), PROPS))
    refuse('code1 not a branch', [p('nobranch.img'), sel, p('macos.img'), '-o', p('x.img')], 'not a branch')
    open(p('bigk.img'), 'wb').write(with_footer(boot(kernel(image_size=250 << 20), rd), PROPS))
    refuse('a selector out of branch range', [p('bigk.img'), sel, p('macos.img'), '-o', p('x.img')], 'branch range')
    open(p('fakesel.bin'), 'wb').write(s.replace(b'PIANO-SW-SEL-V1\0', b'X' * 16))
    refuse('a foreign selector', [p('android.img'), p('fakesel.bin'), p('macos.img'), '-o', p('x.img')], 'version tag')
    open(p('tinyuefi.img'), 'wb').write(boot(b'MZ' + bytes(100)))
    refuse('no UEFI payload', [p('android.img'), sel, p('tinyuefi.img'), '-o', p('x.img')], 'no UEFI payload')
    open(p('vendor.img'), 'wb').write(with_footer(boot(kern, rd), PROPS, name='vendor_boot'))
    refuse('another partition footer', [p('vendor.img'), sel, p('macos.img'), '-o', p('x.img')], 'unexpected AVB partition')

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
                check(mj['avb']['props'].get('com.android.build.boot.security_patch') == '2026-02-01', 'real: patch level 2026-02-01 kept')
                check(mj['avb']['rollback_index'] == 1769904000, 'real: rollback index 1769904000 kept')
                check(0xA8010000 + mj['image_size_new'] <= 0xB6382000, "real: the image ends below Android's initrd 0xB6382000")
                print('INFO real layout ' + json.dumps({k: hex(v) if isinstance(v, int) else v for k, v in mj.items() if k != 'avb'}))
        else:
            print('INFO real images absent: skipped')
    print(f'{"FAIL" if fails else "PASS"}: pack-selector tests, {fails} failures')
    sys.exit(1 if fails else 0)


main()
