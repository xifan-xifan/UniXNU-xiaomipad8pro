#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""check-uva47-kc.py KERNEL [--baseline]: k27 0075 "user VA 47-bit" in a built k27 kernel (Mach-O, unstripped), no KC needed.

Reads the kernel's own symbol table and data (no external tools):
  - pmap_pt_attr_4k_u47 exists: root level 0, commpage level 1, max level 3, 4 KiB pages, valid mask 0x7fffffffffff; the native
    pmap_pt_attr_4k next to it unchanged (root 1, mask 0x7fffffffff) - the native one is the field layout's known positive;
  - the initial arm64_pmap_max_offset_default and piano_uva_max_offset are 0x7ffffe000000, piano_uva47 starts true;
  - machdep.virtual_address_size's handler loads 0x2f (47), not 0x27;
  - the console strings piano-uva47: ... (the NEED_KC_STRINGS of a RAM test) and the boot-arg name are in the kernel.
--baseline checks a kernel without 0075 (0074, the known negative) the other way round: no u47 attribute, 0x7ffe000000, 0x27.
"""
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2


class MachO:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        magic, cputype, _, filetype, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', d, 0)
        if magic != MH_MAGIC_64:
            raise SystemExit('check-uva47-kc: %s is not a thin 64-bit Mach-O' % path)
        self.segs, self.sects, self.syms = [], [], {}
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', d, off)
            if cmd == LC_SEGMENT_64:
                segname = d[off + 8:off + 24].rstrip(b'\0').decode()
                vmaddr, vmsize, fileoff, filesize, _, _, nsects, _ = struct.unpack_from('<QQQQiiII', d, off + 24)
                self.segs.append((segname, vmaddr, vmsize, fileoff, filesize))
                so = off + 72
                for _ in range(nsects):
                    sectname = d[so:so + 16].rstrip(b'\0').decode()
                    addr, ssize, soff = struct.unpack_from('<QQI', d, so + 32)
                    self.sects.append((segname, sectname, addr, ssize, soff))
                    so += 80
            elif cmd == LC_SYMTAB:
                symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', d, off + 8)
                for i in range(nsyms):
                    strx, ntype, nsect, _, value = struct.unpack_from('<IBBhQ', d, symoff + 16 * i)
                    if ntype & 0xE0:     # stab
                        continue
                    if (ntype & 0x0E) != 0x0E:   # N_SECT only
                        continue
                    end = d.index(b'\0', stroff + strx)
                    name = d[stroff + strx:end].decode(errors='replace')
                    self.syms.setdefault(name, value)
            off += size
        self.sorted_syms = sorted(set(self.syms.values()))

    def fileoff(self, vmaddr):
        for name, va, vs, fo, fs in self.segs:
            if va <= vmaddr < va + fs:
                return fo + (vmaddr - va)
        raise KeyError(hex(vmaddr))

    def read(self, vmaddr, n):
        o = self.fileoff(vmaddr)
        return self.data[o:o + n]

    def sym(self, name):
        return self.syms.get('_' + name)

    def func_words(self, name):
        a = self.sym(name)
        nxt = next((v for v in self.sorted_syms if v > a), a + 256)
        n = min(nxt - a, 1024)
        b = self.read(a, n)
        return struct.unpack('<%dI' % (len(b) // 4), b[:len(b) // 4 * 4])

    def has_string(self, s):
        # a C string, alone or as a printf line (the format's newline before the NUL)
        return (s.encode() + b'\0') in self.data or (s.encode() + b'\n\0') in self.data


def movz32_imms(words):
    """imm16 of every 32-bit MOVZ (mov wN, #imm) in the words."""
    return [(w >> 5) & 0xFFFF for w in words if (w & 0xFFE00000) == 0x52800000]


ATTR_ROOT, ATTR_COMMPAGE, ATTR_MAX, ATTR_PAGE, ATTR_PAGEZERO, ATTR_SHIFT, ATTR_MASK = 64, 68, 72, 88, 96, 104, 112


def attr(k, name):
    a = k.sym(name)
    if a is None:
        return None
    b = k.read(a, 120)
    root, cp, mx = struct.unpack_from('<III', b, ATTR_ROOT)
    page, pz, shift, mask = struct.unpack_from('<QQQQ', b, ATTR_PAGE)
    return dict(root=root, commpage=cp, max=mx, page=page, pagezero=pz, shift=shift, mask=mask)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path, baseline = sys.argv[1], '--baseline' in sys.argv[2:]
    k = MachO(path)
    fails = []

    def check(ok, what):
        print('  %s %s' % ('ok  ' if ok else 'FAIL', what))
        if not ok:
            fails.append(what)

    print('check-uva47-kc: %s (%s)' % (path, 'baseline: a kernel without 0075' if baseline else 'k27 0075'))
    native = attr(k, 'pmap_pt_attr_4k')
    check(native is not None and native['root'] == 1 and native['commpage'] == 1 and native['max'] == 3 and native['page'] == 4096
          and native['shift'] == 12 and native['mask'] == (1 << 39) - 1,
          'pmap_pt_attr_4k (native, the layout known-positive): %s' % native)
    u47 = attr(k, 'pmap_pt_attr_4k_u47')
    mo = k.sym('arm64_pmap_max_offset_default')
    max_default = struct.unpack('<Q', k.read(mo, 8))[0] if mo is not None else None
    vas = k.sym('virtual_address_size')
    imms = movz32_imms(k.func_words('virtual_address_size')) if vas is not None else []
    check(vas is not None, 'virtual_address_size handler found (movz immediates %s)' % [hex(i) for i in imms])
    strings = ['piano-uva47: user VA 47 bits (T0SZ 17, L0 root), map end 0x%llx', 'piano-uva47: user VA 39 bits (piano-uva47=0), map end 0x%llx', 'piano-uva47']
    if not baseline:
        check(u47 is not None and u47['root'] == 0 and u47['commpage'] == 1 and u47['max'] == 3 and u47['page'] == 4096
              and u47['pagezero'] == 4096 and u47['shift'] == 12 and u47['mask'] == (1 << 47) - 1,
              'pmap_pt_attr_4k_u47: %s' % u47)
        check(max_default == 0x7FFFFE000000, 'arm64_pmap_max_offset_default initial = %s' % (hex(max_default) if max_default else None))
        pm = k.sym('piano_uva_max_offset')
        pmv = struct.unpack('<Q', k.read(pm, 8))[0] if pm is not None else None
        check(pmv == 0x7FFFFE000000, 'piano_uva_max_offset initial = %s' % (hex(pmv) if pmv else None))
        pu = k.sym('piano_uva47')
        puv = k.read(pu, 1)[0] if pu is not None else None
        check(puv == 1, 'piano_uva47 initial = %s' % puv)
        check(0x2F in imms and 0x27 not in imms, 'virtual_address_size returns 0x2f (47)')
        for s in strings:
            check(k.has_string(s), 'string %r' % s)
    else:
        check(u47 is None, 'no pmap_pt_attr_4k_u47')
        check(max_default == 0x7FFE000000, 'arm64_pmap_max_offset_default initial = %s' % (hex(max_default) if max_default else None))
        check(0x27 in imms and 0x2F not in imms, 'virtual_address_size returns 0x27 (39)')
        check(b'piano-uva47: user VA' not in k.data, 'no piano-uva47 console string')
    if fails:
        print('check-uva47-kc: FAIL (%d)' % len(fails))
        return 1
    print('check-uva47-kc: PASS%s' % ('' if baseline else '; NEED_KC_STRINGS token: piano-uva47:'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
