#!/usr/bin/env python3
"""out/inputs/piano-boot.dtb (the boot DTB every RAM image and boot_a carry: tooling/bootimg/build-ram-images.py --dtb) from the public
Ubuntu v1.1.0 release's maintenance RAM image, whose v2 dtb section is that DTB byte for byte. Both are checked against
tooling/repro/pins.json (maintenance_image.sha256, boot_dtb.sha256); fail closed.

  boot-dtb.py MAINTENANCE.img [OUT.dtb]      default OUT: out/inputs/piano-boot.dtb
  boot-dtb.py --check [OUT.dtb]              no writes: OUT exists and equals the pin

MAINTENANCE.img is boot/boot-ram-maintenance.img of piano-ubuntu-v1.1.0-boot.zip (github.com/yzddmr6/xiaomipad-8pro-mainline,
release v1.1.0, checked with its SHA256SUMS.assets); the same file tooling/persist/maint-session.py boots for every write session.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

D = Path(__file__).resolve().parents[2]
PINS = json.loads((D / 'tooling/repro/pins.json').read_text())
HEADER = struct.Struct('<8s10I16s512s32s1024sIQIIQ')   # boot_img_hdr_v2


def dtb_of(image):
    f = HEADER.unpack_from(image, 0)
    if f[0] != b'ANDROID!' or f[9] != 2:
        sys.exit('FAIL: not an Android boot image v2')
    page = f[8]
    up = lambda n: (n + page - 1) // page * page
    at = page + up(f[1]) + up(f[3]) + up(f[5]) + up(f[15])
    return image[at:at + f[18]]


def main():
    args = sys.argv[1:]
    want = PINS['boot_dtb']['sha256']
    if args[:1] == ['--check']:
        out = Path(args[1]) if len(args) > 1 else D / 'out/inputs/piano-boot.dtb'
        got = hashlib.sha256(out.read_bytes()).hexdigest() if out.is_file() else None
        print(f'{"ok  " if got == want else "FAIL"}  {out}: {got or "missing"} (pin {want[:16]}...)')
        return 0 if got == want else 1
    if not args:
        sys.exit(__doc__)
    image = Path(args[0]).read_bytes()
    got = hashlib.sha256(image).hexdigest()
    if got != PINS['maintenance_image']['sha256']:
        sys.exit(f'FAIL: {args[0]} sha256 {got} is not the pinned maintenance image {PINS["maintenance_image"]["sha256"]}')
    dtb = dtb_of(image)
    if dtb[:4] != b'\xd0\x0d\xfe\xed' or hashlib.sha256(dtb).hexdigest() != want:
        sys.exit(f'FAIL: the dtb section ({len(dtb)} bytes, sha256 {hashlib.sha256(dtb).hexdigest()}) is not the pin {want}')
    out = Path(args[1]) if len(args) > 1 else D / 'out/inputs/piano-boot.dtb'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(dtb)
    print(f'ok    {out}: {len(dtb)} bytes, sha256 {want}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
