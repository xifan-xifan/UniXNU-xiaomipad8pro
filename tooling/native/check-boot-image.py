#!/usr/bin/env python3
"""Check a piano boot image's UniXNU arguments before it is booted.

The paclr53 root image of 2026-11-23 hung and stayed hung because it was
assembled by hand from a frozen image and carried neither piano-reset-after
nor piano-boot-guard. Nothing in the build failed: the arguments were simply
absent. This reads the arguments of the live /EFI/UniXNU/unixnu.toml through
the FAT directory and cluster chain, so the mistake is caught on the host
instead of costing a physical power cycle. (A raw scan of the image cannot do
this: writing the file in place leaves orphaned copies of the old text in free
clusters.)

Usage: check-boot-image.py IMAGE [IMAGE ...]

Exit 1 for any image whose live arguments request a native UFS run
(native-ufs-root=1 or native-ufs-probe=N) without an armed reset, and for any
image whose unixnu.toml cannot be read.
"""
import struct
import sys
from pathlib import Path

BOOT = struct.Struct('<8s10I16s512s32s1024sIQII Q')  # Android boot v2 header, as the builders use it


def sector_size(bpb: bytes) -> int:
    return struct.unpack_from('<H', bpb, 11)[0]


class Fat:
    def __init__(self, data: bytes):
        self.image = data
        self.bytes_per_sector = struct.unpack_from('<H', data, 11)[0]
        self.sectors_per_cluster = data[13]
        self.reserved = struct.unpack_from('<H', data, 14)[0]
        self.fats = data[16]
        self.fat_sz = struct.unpack_from('<I', data, 36)[0]
        self.root = struct.unpack_from('<I', data, 44)[0]
        self.fat_off = self.reserved * self.bytes_per_sector
        self.data_off = (self.reserved + self.fats * self.fat_sz) * self.bytes_per_sector
        self.cluster_bytes = self.sectors_per_cluster * self.bytes_per_sector

    def chain(self, cluster: int, limit: int = 1 << 20) -> list[int]:
        out = []
        while 2 <= cluster < 0x0FFFFFF8 and len(out) < limit:
            out.append(cluster)
            entry = struct.unpack_from('<I', self.image, self.fat_off + cluster * 4)[0] & 0x0FFFFFFF
            cluster = entry
        return out

    def read_chain(self, cluster: int) -> bytes:
        parts = []
        for index in self.chain(cluster):
            at = self.data_off + (index - 2) * self.cluster_bytes
            parts.append(self.image[at:at + self.cluster_bytes])
        return b''.join(parts)

    def lookup(self, directory: bytes, short_name: bytes):
        for at in range(0, len(directory) - 31, 32):
            entry = directory[at:at + 32]
            if entry[0] in (0x00, 0xE5) or entry[11] == 0x0F:
                continue
            name = entry[0:8].rstrip(b' ')
            ext = entry[8:11].rstrip(b' ')
            short = (name + (b'.' + ext if ext else b'')).upper()
            wanted = short_name.upper()
            # a name longer than 8.3 is stored as a numbered short alias (UNIXNU~1TOM)
            alias = len(wanted) > 8 and short.startswith(wanted[:6] + b'~') and ext.upper() == wanted[7:]
            if short != wanted and not alias:
                continue
            high = struct.unpack_from('<H', entry, 20)[0]
            low = struct.unpack_from('<H', entry, 26)[0]
            size = struct.unpack_from('<I', entry, 28)[0]
            return (high << 16) | low, size
        return None, None

    def file(self, path: list[bytes]) -> bytes:
        cluster = self.root
        for index, name in enumerate(path):
            directory = self.read_chain(cluster)
            cluster, size = self.lookup(directory, name)
            if not cluster:
                raise FileNotFoundError(name.decode('ascii', 'replace'))
            if index == len(path) - 1:
                return self.read_chain(cluster)[:size]
        raise FileNotFoundError(path)


def arguments(text: bytes) -> list[str]:
    found = []
    at = 0
    marker = b'args = "'
    while True:
        at = text.find(marker, at)
        if at < 0:
            return found
        start = at + len(marker)
        end = text.find(b'"', start)
        if end < 0:
            return found
        found.append(text[start:end].decode('utf-8', 'replace'))
        at = end


def check(path: str) -> bool | None:
    data = Path(path).read_bytes()
    fields = BOOT.unpack_from(data, 0)
    kernel_size, ramdisk_size, page = fields[1], fields[3], fields[8]
    ramdisk_offset = page + -(-kernel_size // page) * page
    fat = Fat(data[ramdisk_offset:ramdisk_offset + ramdisk_size])
    try:
        text = fat.file([b'EFI', b'UNIXNU', b'UNIXNU.TOM'])
    except FileNotFoundError:
        print(f'{path}: skipped, no EFI/UniXNU/unixnu.toml in the ramdisk')
        return None
    lines = arguments(text)
    print(f'{path}: {len(data)} bytes, {len(lines)} args line(s)')
    ok = True
    if not lines:
        print('  skipped, unixnu.toml carries no args (not a k27 RAM image)')
        return None
    for index, line in enumerate(lines):
        words = line.split()
        ufs = [w for w in words if w.startswith(('native-ufs-root=', 'native-ufs-probe='))]
        guard = [w for w in words if w.startswith(('piano-reset-after=', 'piano-boot-guard='))]
        print(f'  args[{index}] ufs={ufs or "none"} backstop={guard or "none"}')
        if ufs and not guard:
            print('  FAIL native UFS run with no reset backstop')
            ok = False
        if ufs and not any(w.startswith('pstore-console=') for w in words):
            print('  FAIL native UFS run with no pstore console')
            ok = False
    return ok


def main(paths: list[str], require: bool = False) -> int:
    ok = True
    for path in paths:
        try:
            result = check(path)
            if result is None and require:
                print('  FAIL --require: image has no UniXNU arguments to check')
                result = False
            if result is False:
                ok = False
        except Exception as error:  # a malformed image is a failed check, not a crash
            print(f'{path}: FAIL {type(error).__name__}: {error}')
            ok = False
    return 0 if ok else 1


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[0])
        raise SystemExit(2)
    args = [a for a in sys.argv[1:] if a != '--require']
    raise SystemExit(main(args, '--require' in sys.argv[1:]))

