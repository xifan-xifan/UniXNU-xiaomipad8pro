#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Checks of the built selector: one allocated section, no relocations, the entry at offset 0 saves x0..x3/sp/DAIF first,
the metadata block once with our version tag, the bootargs strings present, size bounded, no FP/SIMD, all addresses in range."""
import re
import subprocess
import sys

LLVM = '/opt/homebrew/opt/llvm/bin'
obj, binf = sys.argv[1], sys.argv[2]
data = open(binf, 'rb').read()
fails = 0


def check(cond, msg):
    global fails
    print(('PASS ' if cond else 'FAIL ') + msg)
    fails += 0 if cond else 1


rel = subprocess.run([f'{LLVM}/llvm-objdump', '-r', obj], capture_output=True, text=True).stdout
check('R_AARCH64' not in rel, 'no relocations')
syms = subprocess.run([f'{LLVM}/llvm-objdump', '-t', obj], capture_output=True, text=True).stdout
start = [l for l in syms.splitlines() if l.endswith(' _start')]
check(len(start) == 1 and start[0].startswith('0000000000000000'), '_start at offset 0')
check(not re.search(r'\bg\s+(F\s+)?\.text', syms), 'no global symbols (every reference resolved locally)')
words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, 28, 4)]
# mov x19,x0; mov x20,x1; mov x21,x2; mov x22,x3; mov x23,sp; mrs x24,daif; msr daifset,#15
want = [0xaa0003f3, 0xaa0103f4, 0xaa0203f5, 0xaa0303f6, 0x910003f7, 0xd53b4238, 0xd5034fdf]
check(words == want, 'entry saves x0..x3, sp, DAIF and masks interrupts first')
magic = b'PIANO-UEFI-SEL1\0'
check(data.count(magic) == 1, 'metadata magic exactly once')
m = data.find(magic)
check(m % 16 == 0, 'metadata 16-byte aligned')
check(data[m + 80:m + 96] == b'PIANO-SW-SEL-V1\0', 'our version tag after the 80-byte metadata')
check(all(b == 0 for b in data[m + 16:m + 80]), 'metadata fields zero before packing')
for s in (b'bootmonitor.bootmode=recovery\0', b'bootmonitor.bootmode=poweroff_cg\0'):
    check(data.count(s) == 1, f'bootargs string {s[:-1].decode()} present')
check(8192 < len(data) <= 65536, f"size {len(data)} bytes (8 KiB stack included, under 64 KiB)")
dis = subprocess.run([f'{LLVM}/llvm-objdump', '-d', obj], capture_output=True, text=True).stdout
check(not re.search(r'\s(ldr|str|ldp|stp)\s+q|\sv[0-9]+\.', dis), 'no FP/SIMD instructions')
check('smc\t#0' in dis or 'smc #0' in dis, 'PSCI smc present (fastboot path)')
check('dc\tcivac' in dis or 'dc civac' in dis, 'splash framebuffer clean present')
# every PC-relative adr/b/bl target stays inside the binary
bad = 0
for line in dis.splitlines():
    mm = re.match(r'\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(adr|b|bl|cbz|cbnz|b\.\w+|tbz|tbnz)\s.*?0x([0-9a-f]+)', line)
    if mm and int(mm.group(3), 16) >= len(data):
        bad += 1
check(bad == 0, 'all PC-relative targets inside the binary')
print(f'{"FAIL" if fails else "PASS"}: selector binary checks, {fails} failures')
sys.exit(1 if fails else 0)
