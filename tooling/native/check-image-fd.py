"""Report which firmware device tree a piano boot image carries in its kernel field.

Why: firmware/patches/unixnu/0001 makes a boot image's RAM payload win over the
installed ESP.  A RAM image built with an unpatched FD boots the ESP instead of
its own payload, so every fastboot-boot experiment silently changes target.
The kernel field is BootShim + FD + DTB, so the FD is found by scanning for its
own leading bytes and the copy is confirmed by hashing it.
"""
import argparse
import hashlib
import pathlib

def sha(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()

def offsets(haystack: bytes, needle: bytes):
    start = 0
    while True:
        at = haystack.find(needle, start)
        if at < 0:
            return
        yield at
        start = at + 1

def main() -> int:
    app = argparse.ArgumentParser()
    app.add_argument('image', type=pathlib.Path)
    app.add_argument('--fd', type=pathlib.Path, action='append', default=[])
    app.add_argument('--needle', type=int, default=64)
    args = app.parse_args()
    blob = args.image.read_bytes()
    print(f'{args.image.name}: {len(blob)} bytes')
    for path in args.fd:
        firmware = path.read_bytes()
        digest = sha(firmware)
        hits = []
        for at in offsets(blob, firmware[: args.needle]):
            if at + len(firmware) <= len(blob) and sha(blob[at : at + len(firmware)]) == digest:
                hits.append(at)
        verdict = hex(hits[0]) if hits else 'absent'
        print(f'  {path.name} {digest[:12]} size={len(firmware)} at={verdict} copies={len(hits)}')
    if not args.fd:
        return 0
    return 0 if all(any(sha(blob[at : at + len(p.read_bytes())]) == sha(p.read_bytes())
                       for at in offsets(blob, p.read_bytes()[: args.needle])
                       if at + len(p.read_bytes()) <= len(blob)) for p in args.fd) else 1

if __name__ == '__main__':
    raise SystemExit(main())
