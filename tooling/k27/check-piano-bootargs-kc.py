#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""check-piano-bootargs-kc.py FILE [--kc] [--baseline]: k27 0076 (kern.piano_bootargs) in a built k27 kernel or kernel collection.

FILE is the unstripped native kernel (Mach-O; its symbol table is read) or, with --kc, the kernel collection a build made from it
(strings only: a KC's own symbol table is not the kernel's). Checks:
  - the sysctl's name `piano_bootargs` and its description `Piano merged boot arguments (0066 ADT tail)` are C strings in FILE (the
    description is MERGED_SYSCTL_MARKER, the string tooling/adt/boot_args.py's check_userland looks for before it lets a reader
    argument sit past the 1024-byte kern.bootargs clip);
  - k27 0066's console format `piano-boot-args: /chosen/` is there too (0076 exports 0066's merge; the known positive of the string
    search);
  - kernel only: the handler symbol _sysctl_piano_bootargs and its OID _sysctl__kern_piano_bootargs exist, next to
    _sysctl__kern_bootargs (the known positive of the symbol read).
--baseline checks a kernel without 0076 (0075 cddd1ff2, the known negative) the other way round: 0066's string and kern.bootargs
are there, the 0076 strings and symbols are not.
"""
import struct
import sys

NAME = b'piano_bootargs\0'
DESC = b'Piano merged boot arguments (0066 ADT tail)\0'
MARK_0066 = b'piano-boot-args: /chosen/'
MH_MAGIC_64 = 0xFEEDFACF
LC_SYMTAB = 0x2


def symbols(data):
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', data, 0)
    if magic != MH_MAGIC_64:
        raise SystemExit('check-piano-bootargs-kc: not a thin 64-bit Mach-O (give --kc for a kernel collection)')
    off, names = 32, set()
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == LC_SYMTAB:
            symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', data, off + 8)
            for i in range(nsyms):
                strx, ntype = struct.unpack_from('<IB', data, symoff + 16 * i)
                if ntype & 0xE0:
                    continue
                end = data.index(b'\0', stroff + strx)
                names.add(data[stroff + strx:end].decode(errors='replace'))
        off += size
    return names


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        raise SystemExit(__doc__)
    kc, baseline = '--kc' in sys.argv[1:], '--baseline' in sys.argv[1:]
    data = open(args[0], 'rb').read()
    fails = []

    def check(ok, what):
        print('  %s %s' % ('ok  ' if ok else 'FAIL', what))
        if not ok:
            fails.append(what)

    print('check-piano-bootargs-kc: %s (%s, %s)' % (args[0], 'KC strings' if kc else 'kernel',
                                                     'baseline: without 0076' if baseline else 'k27 0076'))
    check(MARK_0066 in data, 'k27 0066 console string %r (known positive)' % MARK_0066.decode())
    want = not baseline
    check((b'\0' + NAME in data) == want, 'sysctl name %r %s' % (NAME.rstrip(b'\0').decode(), 'present' if want else 'absent'))
    check((DESC in data) == want, 'sysctl description %r %s' % (DESC.rstrip(b'\0').decode(), 'present' if want else 'absent'))
    if not kc:
        syms = symbols(data)
        check('_sysctl__kern_bootargs' in syms, '_sysctl__kern_bootargs (known positive of the symbol read)')
        for s in ('_sysctl_piano_bootargs', '_sysctl__kern_piano_bootargs'):
            check((s in syms) == want, '%s %s' % (s, 'present' if want else 'absent'))
    print('check-piano-bootargs-kc: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
