#!/usr/bin/env python3
"""Fetch an immutable tablet file through bounded SSH reads and verify the SSD copy."""

import argparse
import base64
import binascii
import fcntl
import hashlib
import json
import math
import os
import re
import shlex
import signal
import subprocess
import sys
import time
from pathlib import Path


CHUNK_SIZE = 65536
ATTEMPTS = 6
WIRE_BYTES_PER_MINUTE = 1_000_000
F_NOCACHE = 48  # Darwin

METADATA = """\
set -e -o pipefail
bytes=$(/usr/bin/stat -f %z "$1")
sha=$(/usr/bin/shasum -a 256 < "$1" | /usr/bin/awk '{print $1}')
printf 'FILE %s %s\\n' "$bytes" "$sha"
"""

READ_CHUNK = """\
set -e -o pipefail
data=$(/bin/dd if="$1" bs=65536 skip="$2" count=1 2>/dev/null | /usr/bin/base64 -b 76)
sha=$(printf '%s\\n' "$data" | /usr/bin/base64 -D | /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}')
printf 'CHUNK %s %s\\n%s\\nEND %s\\n' "$2" "$sha" "$data" "$2"
"""


class FetchError(Exception):
    pass


def budget(size):
    chunks = (size + CHUNK_SIZE - 1) // CHUNK_SIZE
    encoded = 4 * ((CHUNK_SIZE + 2) // 3)
    wire = chunks * (encoded + (encoded + 75) // 76 + 100)
    # Allow 2 s handshake per chunk and five minutes for retries.
    return math.ceil(60 * wire / WIRE_BYTES_PER_MINUTE) + 2 * chunks + 300


def remote(transport, script, args, timeout):
    command = shlex.join(['/bin/bash', '-c', script, 'fetch-chunks', *map(str, args)])
    # Bound the entire local SSH process group, including transport descendants
    # with stdout open. Only short, read-only remote commands use this path.
    with subprocess.Popen(
        ['/bin/bash', '-c', '"$1" "$2"', 'fetch-chunks', transport, command],
        env={**os.environ, 'SSH_T': str(math.ceil(timeout))},
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, start_new_session=True,
    ) as proc:
        try:
            data, _ = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.communicate()
            raise FetchError('transport timed out') from None
    if proc.returncode:
        raise FetchError(f'transport returned {proc.returncode}')
    return data


def retry(action, read, deadline, timeout):
    for attempt in range(1, ATTEMPTS + 1):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            return read(min(timeout, remaining))
        except FetchError as exc:
            print(f'fetch {action} attempt {attempt}/{ATTEMPTS}: {exc}', file=sys.stderr)
            if attempt < ATTEMPTS:
                time.sleep(max(0, min(attempt, 3, deadline - time.monotonic())))
    raise FetchError(f'{action} retry/deadline exhausted')


def get_metadata(transport, source, timeout):
    raw = remote(transport, METADATA, [source], timeout)
    match = re.fullmatch(rb'FILE ([0-9]{1,19}) ([0-9a-f]{64})\n', raw)
    if not match:
        raise FetchError('invalid tablet file metadata')
    return int(match[1]), match[2].decode('ascii')


def get_chunk(transport, source, index, length, timeout):
    raw = remote(transport, READ_CHUNK, [source, index], timeout)
    lines = raw.split(b'\n')
    header = re.fullmatch(rb'CHUNK ([0-9]+) ([0-9a-f]{64})', lines[0])
    number = str(index).encode('ascii')
    if not header or header[1] != number or lines[-2:] != [b'END ' + number, b'']:
        raise FetchError('missing or invalid chunk frame')
    try:
        data = base64.b64decode(b''.join(lines[1:-2]), validate=True)
    except binascii.Error:
        raise FetchError('invalid chunk encoding') from None
    if len(data) != length or hashlib.sha256(data).hexdigest().encode('ascii') != header[2]:
        raise FetchError('chunk length or SHA-256 mismatch')
    return data


def resume(stream, checkpoint, size, tablet_sha):
    digest = hashlib.sha256()
    offset = 0
    if checkpoint.exists():
        state = json.loads(checkpoint.read_text())
        offset = state['offset']
        if (state['size'] != size or state['tablet_sha'] != tablet_sha
                or not isinstance(offset, int) or not 0 <= offset <= size
                or (offset != size and offset % CHUNK_SIZE)):
            raise FetchError('checkpoint differs from current tablet file')
        for position in range(0, offset, CHUNK_SIZE):
            piece = stream.read(min(CHUNK_SIZE, offset - position))
            digest.update(piece)
        if stream.tell() != offset or digest.hexdigest() != state['prefix_sha']:
            raise FetchError('checkpoint prefix length or SHA-256 mismatch')
    # Discard bytes written before a checkpoint was published.
    stream.truncate(offset)
    return offset, digest


def fetch(transport, source, destination, expected, timeout):
    size, tablet_sha = retry('metadata', lambda t: get_metadata(transport, source, t),
                             time.monotonic() + ATTEMPTS * (timeout + 3), timeout)
    if expected is not None and expected != tablet_sha:
        raise FetchError('tablet SHA-256 differs from the backup receipt')
    seconds = budget(size)
    deadline = time.monotonic() + seconds
    part = destination.with_name(destination.name + '.part')
    checkpoint = part.with_name(part.name + '.resume')
    next_checkpoint = checkpoint.with_name(checkpoint.name + '.new')
    with part.open('a+b') as stream:
        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        stream.seek(0)
        offset, received = resume(stream, checkpoint, size, tablet_sha)
        print(f'fetch bytes={size} resume={offset} budget={seconds}s', file=sys.stderr)
        while offset < size:
            index = offset // CHUNK_SIZE
            length = min(CHUNK_SIZE, size - offset)
            data = retry(f'chunk {index}', lambda t: get_chunk(transport, source, index, length, t),
                         deadline, timeout)
            stream.write(data)
            stream.flush()
            received.update(data)
            offset += length
            next_checkpoint.write_text(json.dumps(dict(
                size=size, tablet_sha=tablet_sha, offset=offset, prefix_sha=received.hexdigest()
            )) + '\n')
            next_checkpoint.replace(checkpoint)
            if offset == size or (index + 1) % 16 == 0:
                print(f'fetch progress {offset}/{size}', file=sys.stderr)
        os.fsync(stream.fileno())

        actual = received.hexdigest()
        if actual != tablet_sha:
            raise FetchError('complete received SHA-256 differs from tablet')
        with part.open('rb', buffering=0) as disk:
            fcntl.fcntl(disk, F_NOCACHE, 1)
            disk_hash = hashlib.sha256()
            for chunk in iter(lambda: disk.read(1 << 20), b''):
                disk_hash.update(chunk)
            disk_sha = disk_hash.hexdigest()
        if disk_sha != tablet_sha:
            raise FetchError('cache-free SSD SHA-256 differs from tablet')
        part.replace(destination)
        checkpoint.unlink(missing_ok=True)
    print(f'tablet={tablet_sha}\nreceived={actual}\ncache-free-SSD={disk_sha}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('transport', choices=('rssh', 'rsudo'), help='exported wrapper function')
    parser.add_argument('source')
    parser.add_argument('destination', type=Path)
    parser.add_argument('--expected-sha256')
    parser.add_argument('--timeout', type=float, default=30, help='timeout per attempt in seconds')
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('--timeout must be positive and finite')
    try:
        fetch(args.transport, args.source, args.destination, args.expected_sha256, args.timeout)
    except (FetchError, OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(1, f'fetch: {exc}\n')


if __name__ == '__main__':
    main()
