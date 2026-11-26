#!/usr/bin/env python3
"""The 27.3 line's Apple inputs, fetched from Apple's CDN on this Mac and checked against tooling/repro/pins.json at every step.

Nothing from Apple is ever committed or shipped: each user fetches the pinned IPSW's members directly from Apple. The steps, each fail
closed (the first mismatch stops the run and names the file):

  1. HEAD the pinned URL: its size and Apple's x-amz-meta-digest-sha256 must be the pins'.
  2. tooling/k273/ipsw-members.py fetch: only the K114c identity's members (13.3 GB of 27.2 GB), resumable HTTP ranges, the zip's
     CRC-32 per member; then every member is hashed again here and compared with the pin (not with the downloader's own record).
     BuildManifest.plist too.
  3. tooling/k273/decrypt-aea.py for the two AEA disk images: the key comes from Apple's key service through `ipsw fw aea --key`
     (blacktop/ipsw; the key URL is in the archive's header), decryption by macOS /usr/bin/aea; the key is never written or printed.
     The plain image's sha256 must be the pin.
  4. tooling/k273/decompress-kernelcache.py: the KC base (IM4P -> LZFSE -> MH_FILESET) with its pinned sha256.

  fetch-ipsw.py [--inputs DIR] [--pins PINS.json] [--remove-aea]     fetch what is missing, check everything
  fetch-ipsw.py --check   [--inputs DIR]                              no network, no writes: every pinned file present and equal
  fetch-ipsw.py --dry-run [--inputs DIR]                              no network, no writes: the plan, the tools, the disk space

DIR defaults to out/inputs/k273 (the members in DIR/26C5035r, the KC in DIR/kc), where tooling/k273/* read them.
--remove-aea deletes each AEA archive once its plain image is written and checked (keeps 13.9 GB less; a rerun then skips it).
Disk: about 32 GB at the peak (14 GB downloaded, 17.4 GB decrypted, the 125 MB KC); --dry-run prints the numbers and the free space.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

D = Path(__file__).resolve().parents[2]
PINS = D / 'tooling/repro/pins.json'
K273 = D / 'tooling/k273'


class Fail(SystemExit):
    def __init__(self, message):
        super().__init__(f'FAIL: {message}')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(16 << 20):
            h.update(chunk)
    return h.hexdigest()


def check_file(path, size, digest, what):
    """True when PATH is SIZE bytes with sha256 DIGEST; raises Fail when it exists and differs; False when absent."""
    if not path.is_file():
        return False
    if path.stat().st_size != size:
        raise Fail(f'{what}: {path} is {path.stat().st_size} bytes, the pin says {size} (remove it and fetch again)')
    got = sha256(path)
    if got != digest:
        raise Fail(f'{what}: {path} sha256 {got}, the pin says {digest} (remove it and fetch again)')
    print(f'ok    {what}: {path.name} {size} bytes sha256 {digest[:16]}...', flush=True)
    return True


def load_pins(path):
    pins = json.loads(Path(path).read_text())
    if pins.get('schema') != 1:
        raise Fail(f'{path}: unknown pins schema {pins.get("schema")}')
    return pins


def tools():
    """(name, path or None, why) for every external tool a real run needs."""
    return [('curl', shutil.which('curl'), 'the member downloads (ipsw-members.py)'),
            ('ipsw', shutil.which('ipsw'), 'the AEA key fetch: `ipsw fw aea --key` (brew install blacktop/tap/ipsw)'),
            ('aea', '/usr/bin/aea' if os.path.exists('/usr/bin/aea') else None, 'macOS AEA decryption (macOS 13 and later)'),
            ('compression_tool', '/usr/bin/compression_tool' if os.path.exists('/usr/bin/compression_tool') else None,
             'the kernelcache LZFSE payload (macOS)')]


def plan(pins, inputs, remove_aea):
    members = inputs / pins['ipsw']['build']
    total = sum(m['size'] for m in pins['members'])
    plain = sum(m['decrypted']['size'] for m in pins['members'] if 'decrypted' in m)
    aea = sum(m['size'] for m in pins['members'] if 'decrypted' in m)
    peak = total + plain + pins['kernelcache']['size']
    need = peak - (aea if remove_aea else 0)
    print(f'IPSW  {pins["ipsw"]["product_version"]} ({pins["ipsw"]["build"]}) identity {pins["ipsw"]["identity"]}, '
          f'{pins["ipsw"]["size"]} bytes, server sha256 {pins["ipsw"]["server_sha256"]}')
    print(f'      {pins["ipsw"]["url"]}')
    print(f'into  {members} and {inputs / "kc"}')
    for m in pins['members']:
        line = f'  fetch {m["component"]:28s} {m["path"]} ({m["size"] >> 20} MiB) sha256 {m["sha256"][:16]}...'
        if 'decrypted' in m:
            line += f'\n        decrypt -> {m["decrypted"]["file"]} ({m["decrypted"]["size"] >> 20} MiB) sha256 {m["decrypted"]["sha256"][:16]}...'
        print(line)
    print(f'  decompress {pins["kernelcache"]["from"]} -> {pins["kernelcache"]["file"]} sha256 {pins["kernelcache"]["sha256"][:16]}...')
    print(f'download {total / 1e9:.1f} GB; disk: up to {peak / 1e9:.1f} GB during the run, {need / 1e9:.1f} GB kept'
          f'{" (AEA archives removed after decryption)" if remove_aea else ""}')
    for name, path, why in tools():
        print(f'tool  {name:16s} {path or "MISSING"}  ({why})')
    free = shutil.disk_usage(inputs if inputs.exists() else inputs.parent if inputs.parent.exists() else D).free
    print(f'free  {free / 1e9:.1f} GB on the inputs volume{"" if free > peak else "  (NOT ENOUGH for a full fetch)"}')


def check_all(pins, inputs):
    """Every pinned file present and equal; returns the list of what is missing. Raises Fail on any mismatch."""
    members = inputs / pins['ipsw']['build']
    missing = []
    if not check_file_digest_only(members / 'BuildManifest.plist', pins['ipsw']['build_manifest_sha256'], 'BuildManifest.plist'):
        missing.append('BuildManifest.plist')
    for m in pins['members']:
        if 'decrypted' in m:
            d = m['decrypted']
            have_plain = check_file(members / d['file'], d['size'], d['sha256'], f'{m["component"]} (decrypted)')
            have_aea = check_file(members / m['file'], m['size'], m['sha256'], m['component'])
            if not have_plain:
                missing.append(d['file'] if have_aea else m['file'])
        elif not check_file(members / m['file'], m['size'], m['sha256'], m['component']):
            missing.append(m['file'])
    k = pins['kernelcache']
    if not check_file(inputs / k['file'], k['size'], k['sha256'], 'KC base'):
        missing.append(k['file'])
    return missing


def check_file_digest_only(path, digest, what):
    if not path.is_file():
        return False
    got = sha256(path)
    if got != digest:
        raise Fail(f'{what}: {path} sha256 {got}, the pin says {digest}')
    print(f'ok    {what}: sha256 {digest[:16]}...', flush=True)
    return True


def head(url):
    request = urllib.request.Request(url, method='HEAD')
    with urllib.request.urlopen(request, timeout=60) as response:
        return int(response.headers['Content-Length']), response.headers.get('x-amz-meta-digest-sha256')


def run(cmd):
    print('run  ', ' '.join(str(c) for c in cmd), flush=True)
    result = subprocess.run([str(c) for c in cmd])
    if result.returncode:
        raise Fail(f'{Path(str(cmd[1])).name if len(cmd) > 1 else cmd[0]} exited {result.returncode}')


def fetch(pins, inputs, remove_aea):
    for name, path, why in tools():
        if not path:
            raise Fail(f'{name} not found: {why}')
    size, server = head(pins['ipsw']['url'])
    if size != pins['ipsw']['size'] or server != pins['ipsw']['server_sha256']:
        raise Fail(f'the CDN serves {size} bytes with sha256 {server}; the pin is {pins["ipsw"]["size"]} / '
                   f'{pins["ipsw"]["server_sha256"]}: not the pinned IPSW (moved or replaced): stop')
    print(f'ok    IPSW on the CDN: {size} bytes, server sha256 {server[:16]}...')
    members = inputs / pins['ipsw']['build']
    members.mkdir(parents=True, exist_ok=True)
    (inputs / 'kc').mkdir(parents=True, exist_ok=True)
    py = [sys.executable, '-I']
    want = []
    for m in pins['members']:
        if 'decrypted' in m:
            d = m['decrypted']
            if check_file(members / d['file'], d['size'], d['sha256'], f'{m["component"]} (decrypted)'):
                continue
        elif check_file(members / m['file'], m['size'], m['sha256'], m['component']):
            continue
        want.append(m['component'])
    if want or not (members / 'BuildManifest.plist').is_file():
        # ipsw-members.py with no component would fetch its default list: always name them (BuildManifest.plist comes with any)
        run(py + [K273 / 'ipsw-members.py', 'fetch', '--url', pins['ipsw']['url'], '--identity', pins['ipsw']['identity'],
                  '--out', members] + (want or ['DeviceTree']))
    if not check_file_digest_only(members / 'BuildManifest.plist', pins['ipsw']['build_manifest_sha256'], 'BuildManifest.plist'):
        raise Fail('no BuildManifest.plist after the fetch')
    for m in pins['members']:
        if 'decrypted' in m:
            d = m['decrypted']
            if not check_file(members / d['file'], d['size'], d['sha256'], f'{m["component"]} (decrypted)'):
                if not check_file(members / m['file'], m['size'], m['sha256'], m['component']):
                    raise Fail(f'{m["component"]}: {m["file"]} missing after the fetch')
                run(py + [K273 / 'decrypt-aea.py', members / m['file']])
                if not check_file(members / d['file'], d['size'], d['sha256'], f'{m["component"]} (decrypted)'):
                    raise Fail(f'{m["component"]}: no {d["file"]} after decrypt-aea.py')
            if remove_aea and (members / m['file']).is_file():
                (members / m['file']).unlink()
                print(f'      removed {m["file"]} (its plain image is checked)')
        elif not check_file(members / m['file'], m['size'], m['sha256'], m['component']):
            raise Fail(f'{m["component"]}: {m["file"]} missing after the fetch')
    k = pins['kernelcache']
    if not check_file(inputs / k['file'], k['size'], k['sha256'], 'KC base'):
        run(py + [K273 / 'decompress-kernelcache.py', members / k['from'], inputs / k['file'], '--sha256', k['sha256']])
        if not check_file(inputs / k['file'], k['size'], k['sha256'], 'KC base'):
            raise Fail('no KC base after decompress-kernelcache.py')
    print('PASS: every pinned Apple input is present and equal to its pin')


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true')
    mode.add_argument('--dry-run', action='store_true')
    p.add_argument('--inputs', type=Path, default=D / 'out/inputs/k273')
    p.add_argument('--pins', type=Path, default=PINS)
    p.add_argument('--remove-aea', action='store_true')
    a = p.parse_args(argv)
    pins = load_pins(a.pins)
    if a.dry_run:
        plan(pins, a.inputs, a.remove_aea)
        print('dry run: nothing was fetched or written')
        return 0
    if a.check:
        missing = check_all(pins, a.inputs)
        if missing:
            print(f'FAIL: {len(missing)} pinned inputs missing: {", ".join(missing)} (run tooling/repro/fetch-ipsw.py)')
            return 1
        print('PASS: every pinned Apple input is present and equal to its pin')
        return 0
    fetch(pins, a.inputs, a.remove_aea)
    return 0


if __name__ == '__main__':
    sys.exit(main())
