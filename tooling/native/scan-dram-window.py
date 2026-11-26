"""What is actually inside a dumped DRAM window? Host only.

Why: docs/results-2026-11-26-dump-channel-cold-only-and-payload-has-interrupts.md - a cold read of a page we
thought held piano_dbg_kv's table returned something else entirely, so a window has to be *identified*, not
assumed. This reports the Mach-O magics (i.e. a kernel or kext still in DRAM), the collection's own symbols,
and every piano_dbg_kv-shaped slot run (one u64 repeated in all four words, code in 0x0b..0xca).
"""
import re, struct, sys
sys.stdout.reconfigure(encoding='utf-8')

path, base = sys.argv[1], int(sys.argv[2], 16)
blob = open(path, 'rb').read()
print(f'{path}: {len(blob)} bytes, base {hex(base)}')
for name, magic in [('Mach-O 64 (MH_MAGIC_64)', b'\xcf\xfa\xed\xfe'), ('Mach-O fileset', b'\xcf\xfa\xed\x01'),
                    ('FAT superblock', b'\xca\xfe\xba\xbe'), ('ASCII "UNIXNU"', b'UNIXNU'),
                    ('kernel fileset name', b'com.apple.kernel'), ('GIC code', b'piano_gicv3_init'),
                    ('probe sink', b'piano_dbg_kv'), ('slot table symbol', b'_piano_stage_slots')]:
    hits, at = [], blob.find(magic)
    while at >= 0 and len(hits) < 8:
        hits.append(hex(base + at)); at = blob.find(magic, at + 1)
    n = blob.count(magic)
    print(f'  {name}: {n} hits {hits[:6]}{" ..." if n > 6 else ""}')
runs, best = 0, []
for off in range(0, len(blob) - 32, 8):
    w = struct.unpack_from('<4Q', blob, off)[0]
    if w and w == struct.unpack_from('<Q', blob, off + 8)[0] == struct.unpack_from('<Q', blob, off + 16)[0] == struct.unpack_from('<Q', blob, off + 24)[0]:
        code = w & 0xFF
        if 0x0B <= w <= 0xCA or 0x0B <= code <= 0xCA:
            runs += 1
            if len(best) < 12: best.append((hex(base + off), hex(w)))
print(f'  piano_dbg_kv-shaped entries (one u64 in four words, code 0x0b..0xca): {runs} {best[:8]}')
nz = sum(1 for b in blob if b)
print(f'  nonzero bytes {nz} of {len(blob)} ({100.0*nz/len(blob):.1f}%)')
# entropy-ish: count distinct 4 KiB blocks that are all zero
zero = sum(1 for i in range(0, len(blob) - 4096, 4096) if blob[i:i+4096] == bytes(4096))
print(f'  all-zero 4 KiB blocks {zero} of {len(blob)//4096}')