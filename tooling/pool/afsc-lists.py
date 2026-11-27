#!/usr/bin/env python3
"""Split a SystemOS manifest (tooling/pool/pool-manifest) into aa include-path lists by the filesystem-compression family of each
compressed file, so `aa extract` can recompress each file with its source's algorithm (aa keeps only the UF_COMPRESSED flag, not the
algorithm). decmpfs types: 3/4 zlib, 7/8 lzvn, 9/10 copy (stored uncompressed in decmpfs), 11/12 lzfse, 13/14 lzbitmap.

  afsc-lists.py MANIFEST OUTDIR      writes OUTDIR/afsc-<family>.list (raw paths, one per line) and OUTDIR/afsc-lists.json
"""
import json
import re
import sys
from pathlib import Path

FAMILY = {3: 'zlib', 4: 'zlib', 7: 'lzvn', 8: 'lzvn', 9: 'copy', 10: 'copy', 11: 'lzfse', 12: 'lzfse', 13: 'lzbitmap', 14: 'lzbitmap'}


def unescape(s):
    return re.sub(rb'\\x([0-9a-f]{2})', lambda m: bytes([int(m.group(1), 16)]), s.encode()).decode('utf-8', 'surrogateescape')


def main():
    manifest, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    lists, unknown = {}, []
    for line in manifest.read_text().splitlines():
        if line.startswith('#'):
            continue
        f = line.split('\t')
        if len(f) != 14 or f[1] != 'f' or f[13] == '-':
            continue
        t = int(f[13].split(':')[0])
        fam = FAMILY.get(t)
        if fam is None:
            unknown.append((f[0], t))
            continue
        lists.setdefault(fam, []).append(unescape(f[0]))
    if unknown:
        raise SystemExit(f'compression types this tool does not know: {sorted({t for _, t in unknown})} ({len(unknown)} files)')
    for fam, paths in lists.items():
        if any('\n' in p for p in paths):
            raise SystemExit(f'a path with a newline in family {fam}: a list file cannot hold it')
        (out / f'afsc-{fam}.list').write_text(''.join(p + '\n' for p in sorted(paths)))
    summary = {fam: len(paths) for fam, paths in sorted(lists.items())}
    (out / 'afsc-lists.json').write_text(json.dumps({'manifest': manifest.name, 'families': summary}, indent=1) + '\n')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
