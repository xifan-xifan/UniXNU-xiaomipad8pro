#!/usr/bin/env python3
"""A canonical, file-level manifest of an APFS volume (the 27.3 line's SystemOS window), for checking a local rebuild against the pin
when the image bytes cannot match: newfs_apfs gives every new container a random volume UUID, and APFS records creation times and
allocation order, so two runs of tooling/k273/make-systemos.sh never give the same image sha256. What they do give is the same tree.

  volume-manifest.py MOUNTPOINT [--json OUT.json] [--lines OUT.txt]
  volume-manifest.py --image IMAGE.img [--json OUT.json] [--lines OUT.txt]    attach IMAGE read-only (hdiutil, ownership on), walk it,
                                                                               detach; the image file is never written
  volume-manifest.py ... --compare PINS.json      exit 1 unless the digest equals PINS.json's systemos.file_manifest (per-group
                                                  digests name where a mismatch is)

One line per entry, sorted by path (relative to the volume root, never following links):
  d MODE UID GID FLAGS PATH            directory
  f MODE UID GID FLAGS SIZE SHA256 PATH  regular file (content as read: APFS/decmpfs compression is transparent)
  l MODE UID GID FLAGS TARGET PATH     symbolic link
  o MODE UID GID FLAGS PATH            anything else
MODE is st_mode & 0o7777 (octal), FLAGS is st_flags without UF_COMPRESSED (ditto --hfsCompression decides that per host), in hex.
Times, inode numbers, link counts, extended attributes and the root's host metadata (HOST_METADATA: .fseventsd ...) are left
out. The digest is the sha256 of all lines joined by '\n'; each group
(the first three path components) gets its own digest. Reading every file needs root (some are 0600 root): run with sudo, or the tool
stops with the count of unreadable files (a partial manifest is never reported as a digest).
"""
import argparse
import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

UF_COMPRESSED = 0x20
# Host metadata macOS writes at a volume's root whenever it was mounted writable on the building Mac: not part of the window's tree.
HOST_METADATA = {'.fseventsd', '.Spotlight-V100', '.Trashes', '.TemporaryItems', '.DocumentRevisions-V100'}
GROUP_DEPTH = 3


def esc(path):
    return path.replace('\\', '\\\\').replace('\n', '\\n').replace(' ', '\\s')


def walk(root):
    root = Path(root)
    lines, unreadable = [], []
    stack = ['']
    while stack:
        rel = stack.pop()
        full = root / rel if rel else root
        try:
            names = sorted(os.listdir(full))
        except OSError:
            unreadable.append(rel or '.')
            continue
        for name in names:
            if not rel and name in HOST_METADATA:
                continue
            r = f'{rel}/{name}' if rel else name
            p = full / name
            st = os.lstat(p)
            head = f'{stat.S_IMODE(st.st_mode):o} {st.st_uid} {st.st_gid} {getattr(st, "st_flags", 0) & ~UF_COMPRESSED:x}'
            if stat.S_ISDIR(st.st_mode):
                lines.append(f'd {head} {esc(r)}')
                stack.append(r)
            elif stat.S_ISREG(st.st_mode):
                h = hashlib.sha256()
                try:
                    with open(p, 'rb') as f:
                        while chunk := f.read(8 << 20):
                            h.update(chunk)
                except OSError:
                    unreadable.append(r)
                    continue
                lines.append(f'f {head} {st.st_size} {h.hexdigest()} {esc(r)}')
            elif stat.S_ISLNK(st.st_mode):
                lines.append(f'l {head} {esc(os.readlink(p))} {esc(r)}')
            else:
                lines.append(f'o {head} {esc(r)}')
    lines.sort(key=lambda line: line.rsplit(' ', 1)[1])
    return lines, unreadable


def summarize(lines):
    groups = {}
    for line in lines:
        path = line.rsplit(' ', 1)[1]
        key = '/'.join(path.split('/')[:GROUP_DEPTH])
        groups.setdefault(key, []).append(line)
    counts = {'d': 0, 'f': 0, 'l': 0, 'o': 0}
    for line in lines:
        counts[line[0]] += 1
    size = sum(int(line.split(' ')[5]) for line in lines if line[0] == 'f')
    return {'digest': hashlib.sha256('\n'.join(lines).encode()).hexdigest(), 'entries': len(lines), 'counts': counts,
            'file_bytes': size,
            'groups': {k: hashlib.sha256('\n'.join(v).encode()).hexdigest() for k, v in sorted(groups.items())}}


def attach(image):
    """Attach IMAGE read-only without mounting, mount its first APFS volume read-only with ownership; return (mountpoint, disk)."""
    diskof = [sys.executable, '-I', str(Path(__file__).resolve().parents[1] / 'k273/disk-of.py')]
    subprocess.run(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-readonly', '-nomount', '-nobrowse',
                    str(image)], capture_output=True, check=True)
    whole = subprocess.run(diskof + ['whole', str(image)], capture_output=True, text=True, check=True).stdout.strip()
    got = subprocess.run(diskof + ['volume', whole], capture_output=True, text=True)
    volume = got.stdout.strip()
    if got.returncode or not volume:
        subprocess.run(['hdiutil', 'detach', whole], capture_output=True)
        sys.exit(f'{image}: no APFS volume on {whole}')
    mnt = tempfile.mkdtemp(prefix='volume-manifest.')
    subprocess.run(['diskutil', 'mount', 'readOnly', '-mountPoint', mnt, volume], capture_output=True, check=True)
    subprocess.run(['diskutil', 'enableOwnership', mnt], capture_output=True)
    return mnt, whole


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('mountpoint', nargs='?')
    p.add_argument('--image', type=Path)
    p.add_argument('--json', type=Path)
    p.add_argument('--lines', type=Path)
    p.add_argument('--compare', type=Path)
    a = p.parse_args()
    if bool(a.mountpoint) == bool(a.image):
        sys.exit('give MOUNTPOINT or --image IMAGE')
    whole = None
    mnt = a.mountpoint
    if a.image:
        mnt, whole = attach(a.image)
    try:
        lines, unreadable = walk(mnt)
    finally:
        if whole:
            subprocess.run(['diskutil', 'unmount', mnt], capture_output=True)
            subprocess.run(['hdiutil', 'detach', whole], capture_output=True)
            os.rmdir(mnt)
    if unreadable:
        sys.exit(f'{len(unreadable)} entries unreadable (first: {unreadable[0]}): run as root; no digest from a partial walk')
    s = summarize(lines)
    if a.lines:
        a.lines.write_text('\n'.join(lines) + '\n')
    if a.json:
        a.json.write_text(json.dumps(s, indent=1) + '\n')
    print(f'manifest: {s["entries"]} entries {s["counts"]}, {s["file_bytes"]} file bytes, digest {s["digest"]}')
    if a.compare:
        pin = json.loads(a.compare.read_text())['systemos']['file_manifest']
        if s['digest'] != pin['digest']:
            bad = sorted(k for k in set(pin['groups']) | set(s['groups']) if pin['groups'].get(k) != s['groups'].get(k))
            print(f'MISMATCH: digest {s["digest"]}, the pin is {pin["digest"]}; groups that differ: {", ".join(bad[:20])}')
            sys.exit(1)
        print('ok: the file-level manifest equals the pin')


if __name__ == '__main__':
    main()
