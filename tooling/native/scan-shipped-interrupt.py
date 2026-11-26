"""Which shipped or retained boot images carry the interrupt path, including inside a compressed payload.

Why: docs/results-2026-11-25-interrupt-kernel-provenance.md section 5 - a byte scan of the release images for
piano_gicv3_init and friends gave zero, but their payloads are compressed, so that was absence of evidence.
This carves the boot-image sections (tooling/persist/build-boot.py's own readers), decompresses what is
gzip/lz4/zstd/lzma, and scans raw and decompressed bytes. Host only; nothing is sent to the tablet.
"""
import gzip, pathlib, struct, sys, bz2, lzma, zlib
sys.path.insert(0, str(pathlib.Path('tooling/persist').resolve()))
sys.stdout.reconfigure(encoding='utf-8')

NEEDLES = [b'piano_gicv3_init', b'piano_gic_irq_handler', b'piano_delay_timer_service', b'piano-gic',
           b'piano_gicv3_timer_selftest', b'piano_dbg_kv', b'_piano_stage_slots', b'piano_boot_guard']

def try_decompress(name, blob):
    out = []
    if blob[:2] == b'\x1f\x8b':
        try: out.append(('gzip', gzip.decompress(blob)))
        except Exception as e: out.append(('gzip-failed', b''))
    if blob[:4] == b'\x04\x22\x4d\x18':
        try:
            import lz4.frame
            out.append(('lz4', lz4.frame.decompress(blob)))
        except Exception:
            out.append(('lz4-no-decoder', b''))
    if blob[:4] == b'\x28\xb5\x2f\xfd':
        try:
            import zstandard
            out.append(('zstd', zstandard.ZstdDecompressor().decompress(blob)))
        except Exception:
            out.append(('zstd-no-decoder', b''))
    if blob[:6] == b'\xfd7zXZ':
        try: out.append(('lzma', lzma.decompress(blob)))
        except Exception as e: out.append(('lzma-failed', b''))
    if blob[:3] == b'BZh':
        try: out.append(('bzip2', bz2.decompress(blob)))
        except Exception: out.append(('bzip2-failed', b''))
    # gzip/lz4 members not at offset 0 (a FAT or cpio holding a compressed file): find them.
    return out

def scan(label, blob):
    hits = {n.decode(): blob.count(n) for n in NEEDLES if blob.count(n)}
    print(f'    {label}: {len(blob)} bytes {blob[:8].hex()} hits={hits if hits else "none"}')
    return hits

def main():
    images = [pathlib.Path(p) for p in sys.argv[1:]]
    for img in images:
        if not img.is_file():
            print(img, 'ABSENT'); continue
        data = img.read_bytes()
        print('##', img.name, len(data), data[:8])
        sections = {}
        if data[:8] == b'ANDROID!':
            magic, kernel, ramdisk, os_version, header = struct.unpack_from('<8sIIII', data, 0)
            version = struct.unpack_from('<I', data, 40)[0] if len(data) >= 44 else None
            if version in (3, 4):
                page = 4096
                off = -(-header // page) * page
                sections['kernel'] = data[off:off + kernel]
                sections['ramdisk'] = data[off + -(-kernel // page) * page:][:ramdisk]
                print(f'  v{version}: kernel {kernel} ramdisk {ramdisk} header {header}')
            else:
                f = struct.unpack_from('<8s10I', data, 0)
                page = f[8] or 2048
                o = -(-struct.calcsize('<8s10I') // page) * page if f[9] == 0 else f[5]
                kernel_size, ramdisk_size = f[1], f[3]
                o = f[5]
                sections['kernel'] = data[o:o + kernel_size]; o += -(-kernel_size // page) * page
                sections['ramdisk'] = data[o:o + ramdisk_size]; o += -(-ramdisk_size // page) * page
                dtb_size = struct.unpack_from('<I', data, 1648)[0]
                sections['dtb'] = data[o:o + dtb_size]
                print(f'  v{f[9]}: kernel {kernel_size} ramdisk {ramdisk_size} dtb {dtb_size} page {page}')
        elif data[:8] == b'VNDRBOOT':
            # vendor_boot v4: sections after the 4096-byte header, page 4096, no sizes in the simple read here
            sections['head'] = data[:65536]
        else:
            sections['whole'] = data
        for name, blob in sections.items():
            if not blob: continue
            scan(name, blob)
            for how, out in try_decompress(name, blob):
                print(f'    {name}: {how} -> {len(out)} bytes')
                if out:
                    scan(f'{name}/{how}', out)
                    for sub, sub_out in try_decompress(f'{name}/{how}', out):
                        print(f'    {name}/{how}: {sub} -> {len(sub_out)} bytes')
                        if sub_out: scan(f'{name}/{how}/{sub}', sub_out)
        # AVB adds no compressed payload; the UEFI container inside a kernel field is a FAT, already scanned raw.
        # A FAT may hold a gzipped kernel: scan the whole image's decompressible members.
        at = data.find(b'\x1f\x8b\x08')
        found = 0
        while at >= 0 and found < 5:
            found += 1
            try:
                out = gzip.decompress(data[at:])
                print(f'  embedded gzip @{at} -> {len(out)} bytes')
                scan(f'gzip@{at}', out)
            except Exception:
                pass
            at = data.find(b'\x1f\x8b\x08', at + 1)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())