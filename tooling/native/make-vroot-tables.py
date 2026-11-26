#!/usr/bin/env python3
"""One window's VerifiedRoot block table: the SHA-256 of every 4096-byte block, concatenated (32 bytes each).

  make-vroot-tables.py IMAGE OUT_TABLE [--offset N] [--size N] [--jobs N] [--expect-sha256 HEX]

IMAGE is the window's bytes as the root image carries them (the whole image with --offset/--size, or the window alone).
The table is what kits/VerifiedRoot/tools/manifest.py hashes into the signed header and what the kernel's vr_verify_block
compares every read against, so it is derived from the same bytes and never hand-made. A trailing partial block is hashed
over its own bytes and its offset is the last one (the kernel refuses a table whose length disagrees with the signed count).
--jobs splits the window over processes by whole blocks; the pieces are concatenated in order.
"""
import argparse, concurrent.futures as cf, hashlib, os, sys

BLOCK = 4096


def digest_range(path, offset, size):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        f.seek(offset)
        left = size
        while left:
            chunk = f.read(min(8 << 20, left))
            if not chunk:
                break
            h.update(chunk)
            left -= len(chunk)
    return h.hexdigest()


def piece(args):
    path, offset, size = args
    out = bytearray()
    with open(path, 'rb') as f:
        f.seek(offset)
        left = size
        while left:
            take = min(BLOCK, left)
            data = f.read(take)
            if len(data) != take:
                raise SystemExit(f'{path}: short read at {offset + size - left}')
            out += hashlib.sha256(data).digest()
            left -= take
    return bytes(out)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('image')
    p.add_argument('table')
    p.add_argument('--offset', type=int, default=0)
    p.add_argument('--size', type=int)
    p.add_argument('--jobs', type=int, default=1)
    p.add_argument('--expect-sha256', help='hash the window region first and require this')
    a = p.parse_args(argv)
    size = a.size or (os.path.getsize(a.image) - a.offset)
    if a.expect_sha256:
        got = digest_range(a.image, a.offset, size)
        if got != a.expect_sha256:
            raise SystemExit(f'{a.image}: region sha256 {got} is not {a.expect_sha256}')
        print(f'window sha256 ok ({got[:16]})', flush=True)
    blocks = (size + BLOCK - 1) // BLOCK
    jobs = max(1, min(a.jobs, blocks))
    per = (blocks + jobs - 1) // jobs
    spans = []
    done = 0
    while done < blocks:
        take = min(per, blocks - done)
        spans.append((a.image, a.offset + done * BLOCK, take * BLOCK if done + take < blocks or size % BLOCK == 0
                      else (blocks - 1) * BLOCK + size % BLOCK - done * BLOCK))
        done += take
    with open(a.table, 'wb') as out:
        if jobs == 1:
            out.write(piece(spans[0]))
        else:
            with cf.ProcessPoolExecutor(max_workers=jobs) as pool:
                for blob in pool.map(piece, spans):
                    out.write(blob)
    print(f'{a.table}: {blocks} blocks, {os.path.getsize(a.table)} bytes', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))