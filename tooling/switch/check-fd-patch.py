#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the switcher menu patch and, with --fd, its presence in a built piano firmware volume."""
import argparse
import lzma
import sys
import uuid
from pathlib import Path

R = Path(__file__).resolve().parents[2]
ACTIONS = ('android', 'android-default', 'macos-default')
LZMA_GUID = uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf').bytes_le


def piano_firmware_volume(fd):
    """Extract the LZMA guided section in piano.fdf's FVMAIN_COMPACT."""
    guid = fd.find(LZMA_GUID)
    start = guid - 4
    if start < 0 or fd[start + 3] != 2 or guid + 20 > len(fd):
        raise ValueError('missing guided LZMA section')
    size = int.from_bytes(fd[start:start + 3], 'little')
    offset = int.from_bytes(fd[guid + 16:guid + 18], 'little')
    if not 24 <= offset < size <= len(fd) - start:
        raise ValueError('invalid guided LZMA section bounds')
    decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE, memlimit=64 << 20)
    volume = decoder.decompress(fd[start + offset:start + size], max_length=32 << 20)
    if not decoder.eof:
        raise ValueError('truncated or oversized firmware volume')
    return volume


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fd', type=Path)
    parser.add_argument('--require-efi-map', action='store_true', help='require the extra-RAM and EFI-map audit markers')
    args = parser.parse_args()
    if args.require_efi_map and args.fd is None:
        parser.error('--require-efi-map requires --fd')
    patch = (R / 'firmware/patches/unixnu/0003-UniXNULib-piano-boot-flag-menu-actions.patch').read_text()
    want = (R / 'firmware/selector/bootflag.h').read_text()
    sec = patch.split('b/package/UniXNUPkg/Library/UniXNULib/PianoBootFlag.h', 1)[1].split('\n', 1)[1]
    body = ''.join(line[1:] + '\n' for line in sec.split('\n') if line.startswith('+'))
    checks = [(body == want, 'PianoBootFlag.h in the patch = firmware/selector/bootflag.h'),
              (all(f'"{action}"' in patch for action in ACTIONS), 'the three actions parsed'),
              ('PianoBootFlagAction (PIANO_REQ_ONCE_ANDROID_VALUE)' in patch, 'android = once-Android request'),
              ('PianoBootFlag.c' in patch and 'UniXNULib.inf' in patch, 'sources listed in the inf')]
    if args.fd:
        try:
            volume = piano_firmware_volume(args.fd.read_bytes())
        except (OSError, ValueError, lzma.LZMAError) as exc:
            sys.exit(f'FAIL FD extraction: {exc}')
        checks.extend((f'action: {action}\0'.encode() in volume, f'built FD contains menu dispatch for {action}')
                      for action in ACTIONS)
        checks.extend([(b'config: volume %s' in volume, 'built FD contains RAM payload precedence'),
                       ('taken (largest run)'.encode('utf-16le') in volume, 'built FD contains the extended DarwinBoot window')])
        if args.require_efi_map:
            checks.extend([(b'piano-extra-ram\0' in volume, 'built FD contains the extra-RAM handoff'),
                           (b'piano-efi-map-audit\0' in volume, 'built FD contains the EFI-map audit handoff')])
    for ok, message in checks:
        print(('PASS ' if ok else 'FAIL ') + message)
    failures = sum(not ok for ok, _ in checks)
    print(f'{"FAIL" if failures else "PASS"}: FD patch checks, {failures} failures')
    return int(bool(failures))


if __name__ == '__main__':
    sys.exit(main())
