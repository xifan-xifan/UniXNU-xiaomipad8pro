#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Privacy scan for the public repository: generic pattern classes only.

    python3 scripts/ci/privacy-scan.py                 # every tracked text file
    python3 scripts/ci/privacy-scan.py --staged        # only additions to the index
    python3 scripts/ci/privacy-scan.py --diff BASE     # only lines added since BASE (and the commit messages BASE..HEAD)
    python3 scripts/ci/privacy-scan.py --staged        # only lines added to the index

The maintainer's own pre-publish audit has a private list of literal values; it is not here and must never be. This scan only
knows shapes: a user's home path, the maintainer's private notes folder, RFC 1918 LAN addresses, host names on local
domains, MAC addresses, e-mail addresses that are not no-reply ones, secret-looking strings, and file types that would be
Apple payloads or build outputs. A hit fails the scan; a line that is meant (an example, a pattern) carries the marker
`privacy-scan: ok` in the same line, or its file:pattern goes into scripts/ci/privacy-scan.allow with a reason.
"""
import argparse
import fnmatch
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ALLOW_FILE = ROOT / 'scripts/ci/privacy-scan.allow'
MARK = 'privacy-scan: ok'

HOME_OK = {'Shared', 'Guest', 'runner', 'user', 'u', 'me', 'NAME', 'name', 'USER', 'you', 'example'}
EMAIL_OK = re.compile(
    r'[^@]+@(?:users\.noreply\.github\.com|[A-Za-z0-9.-]+\.(?:invalid|example|test)|example\.(?:com|org|net)|com\.apple\.[A-Za-z0-9.-]+)'
    r'|(?:noreply|no-reply)@[^@]+', re.I,
)
HUNK = re.compile(r'^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@')

CLASSES = [
    # (name, regex, filter(match, line, path) -> bool: True = a real hit)
    ('home path', re.compile(r'(?<![\w.])/(Users|home)/([A-Za-z0-9][A-Za-z0-9._-]*)'),
     lambda m, line, path: m.group(2) not in HOME_OK and not m.group(2).startswith('$')),
    ('maintainer private notes*', re.compile(r'(?<![\w/.~-])private/(host-checks|harness|helper-queue|charging|login\.secret|'
                                            r'publish|QUEUE|HANDOFF|HW-INVENTORY|TICKETS)'), None),
    ('LAN address', re.compile(r'(?<![\d.])(10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|'
                               r'172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})(?![\d.]*\d)'),
     lambda m, line, path: not re.search(r'\d\.\d+\.\d+\.\d+\.\d', line[m.start():m.end() + 2])),
    ('local host name', re.compile(r'(?<![\w.-])[A-Za-z0-9][A-Za-z0-9-]*\.(lan|home\.arpa|internal|private|localdomain)\b(?![.\w-])'),
     None),
    ('MAC address', re.compile(r'(?<![0-9A-Fa-f:])([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:])'),
     lambda m, line, path: m.group(0).lower() not in ('00:00:00:00:00:00', 'ff:ff:ff:ff:ff:ff')),
    ('e-mail address', re.compile(r'[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}'),
     lambda m, line, path: not EMAIL_OK.fullmatch(m.group(0)) and not _email_in_header(line, path)),
    ('private key', re.compile(r'-----BEGIN ([A-Z]+ )*PRIVATE KEY-----'), None),
    ('access token', re.compile(r'\b(ghp_[A-Za-z0-9]{36}|gho_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{40,}|AKIA[0-9A-Z]{16}|'
                                r'xox[abprs]-[A-Za-z0-9-]{10,}|sk-[A-Za-z0-9]{32,}|AIza[0-9A-Za-z_-]{35})\b'), None),
    ('inline secret', re.compile(r'(?i)\b(password|passwd|passphrase|secret|api[_-]?key|token)\b["\']?\s*[:=]\s*["\']([^"\'\s$<{]{6,})["\']'),
     lambda m, line, path: not re.fullmatch(r'(x+|\*+|REDACTED|changeme|example\w*|placeholder)', m.group(2), re.I)),
    ('password piped to sudo', re.compile(r'echo\s+["\']?[^\s|"\'$]{4,}["\']?\s*\|\s*sudo\s+-S'),
     lambda m, line, path: '[REDACTED]' not in m.group(0)),
]

# file types that would be Apple payloads, encrypted blobs or build outputs (a PR adding one fails)
BAD_FILES = ['*.ipsw', '*.dmg', '*.aea', '*.kc', '*.macho', '*.img', '*.fd', '*.enc', '*.zst', '*.p12', '*.mobileprovision',
             '*.kdk', 'out/*', '*/out/*', '*.secret', '*.psk', '*.pem', '*.key']
BAD_BINARIES = ['*kernelcache*', '*.bin', '*.o', '*.a', '*.dylib']   # refused when binary (a script with the name is fine)
BAD_FILES_OK = ['*.tbd']   # IOGPU.tbd and other text stubs stay (owner decision)


def _email_in_header(line, path):
    """An author/copyright header names people by design: a patch's From:, Co-authored-by trailers, AUTHORS/NOTICE files."""
    if re.match(r'[+ -]?(From|Signed-off-by|Co-authored-by|Reported-by|Reviewed-by):', line.strip(), re.I):   # also a patch in a patch
        return True
    name = Path(path).name.upper()
    return name.startswith(('AUTHORS', 'NOTICE', 'COPYING', 'LICENSE', 'CREDITS', 'MAINTAINERS', 'UPSTREAM'))


def load_allow():
    allow = []
    if ALLOW_FILE.exists():
        for raw in ALLOW_FILE.read_text().splitlines():
            line = raw.split('  #', 1)[0].strip()
            if line and not line.startswith('#'):
                glob, _, pat = line.partition(' ')
                allow.append((glob, re.compile(pat.strip()) if pat.strip() else None))
    return allow


def allowed(allow, path, text):
    for glob, pat in allow:
        if fnmatch.fnmatch(path, glob) and (pat is None or pat.search(text)):
            return True
    return False


def git(*args):
    return subprocess.run(['git', '-C', str(ROOT), *args], capture_output=True, check=True).stdout


def is_text(data):
    return b'\0' not in data[:8192]


def changed_blobs(*revisions):
    # NUL-delimited raw records preserve all path names and give us the blob IDs.
    records = iter(git('diff', '--raw', '-z', '--no-abbrev', '--find-renames', '--no-ext-diff',
                       '--diff-filter=AMRT', *revisions, '--').split(b'\0')[:-1])
    for header, path in zip(records, records):
        old_mode, new_mode, old_oid, new_oid, status = header.decode('ascii').split()
        if status.startswith('R'):
            path = next(records)
        old = old_oid if old_mode in (':100644', ':100755', ':120000') else None
        new = new_oid if new_mode in ('100644', '100755', '120000') else None
        yield os.fsdecode(path), old, new


def added_lines(old, new, data):
    if old is None:
        yield from enumerate(data.decode('utf-8', errors='replace').splitlines(), 1)
        return
    if old == new:
        return
    # Diff the blobs directly so attributes, diff drivers and the working tree cannot hide changes.
    patch = git('diff', '--no-color', '--no-ext-diff', '--no-textconv', '--text', '-U0', old, new, '--')
    lineno = None
    for line in patch.decode('utf-8', errors='replace').split('\n'):
        hunk = HUNK.match(line)
        if hunk:
            lineno = int(hunk[1])
        elif lineno is not None:
            if line.startswith('+'):
                yield lineno, line[1:]
            if line.startswith(('+', ' ')):
                lineno += 1


def scan_changed_files(revisions, allow, hits, bad):
    for path, old, new in changed_blobs(*revisions):
        data = git('cat-file', 'blob', new) if new else None
        text = data is not None and is_text(data)
        lower = path.lower()
        if any(fnmatch.fnmatch(lower, g) for g in BAD_FILES) and not any(fnmatch.fnmatch(lower, g) for g in BAD_FILES_OK):
            bad.append(path)
        elif any(fnmatch.fnmatch(lower, g) for g in BAD_BINARIES) and not text:
            bad.append(path)
        if text:
            for n, line in added_lines(old, new, data):
                scan_line(path, n, line, allow, hits, diff=True)


def scan_commit_messages(base, allow, hits):
    log = git('log', '--no-notes', '--no-show-signature', '--format=%H%n%B%x00', f'{base}..HEAD', '--')
    for msg in log.decode('utf-8', errors='replace').split('\0'):
        if msg.strip():
            sha, _, body = msg.lstrip('\n').partition('\n')
            for n, line in enumerate(body.splitlines(), 1):
                scan_line(f'commit {sha[:10]} message', n, line, allow, hits, diff=True)


def scan_worktree(allow, hits):
    for name in git('ls-files', '-z').split(b'\0'):
        if not name:
            continue
        path = os.fsdecode(name)
        file = ROOT / path
        try:
            data = os.fsencode(os.readlink(file)) if file.is_symlink() else file.read_bytes()
        except (FileNotFoundError, IsADirectoryError):
            continue
        if is_text(data):
            for n, line in enumerate(data.decode('utf-8', errors='replace').splitlines(), 1):
                scan_line(path, n, line, allow, hits)


def scan_line(path, lineno, line, allow, hits, diff=False):
    if MARK in line:
        return
    for name, rx, keep in CLASSES:
        if name.endswith('*') and not diff:
            continue   # a class marked * is checked on added lines only: the published tree has audited references
        for m in rx.finditer(line):
            if keep and not keep(m, line, path):
                continue
            if allowed(allow, path, m.group(0)):
                continue
            hits.append((path, lineno, name, m.group(0)))


def redact(s):
    return s if len(s) <= 6 else s[:3] + '…' + s[-2:]


def main():
    ap = argparse.ArgumentParser()
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--diff', metavar='BASE', help='scan only what BASE..HEAD adds')
    mode.add_argument('--staged', action='store_true', help='scan only additions to the index')
    ap.add_argument('--show', action='store_true', help='print the matched text in full (local use only, never in CI logs)')
    a = ap.parse_args()
    allow = load_allow()
    hits, bad = [], []
    if a.diff is not None:
        try:
            base = git('rev-parse', '--verify', '--end-of-options', f'{a.diff}^{{commit}}').decode('ascii').strip()
        except subprocess.CalledProcessError:
            ap.error('--diff requires a valid base commit')
        try:
            merge_base = git('merge-base', base, 'HEAD').decode('ascii').strip()
        except subprocess.CalledProcessError:
            ap.error('--diff base and HEAD have no common ancestor')
        scan_changed_files((merge_base, 'HEAD'), allow, hits, bad)
        scan_commit_messages(base, allow, hits)
    elif a.staged:
        scan_changed_files(('--cached',), allow, hits, bad)
    else:
        scan_worktree(allow, hits)
    for path, n, name, text in hits:
        print(f'{path}:{n}: {name}: {text if a.show else redact(text)}')
    for f in bad:
        print(f'{f}: file type not allowed in the public repository (Apple payload, encrypted blob or build output)')
    suffix = f' (lines added since {a.diff})' if a.diff else ' (staged changes)' if a.staged else ''
    print(f'privacy scan: {len(hits)} hits, {len(bad)} refused files{suffix}')
    return 1 if hits or bad else 0


if __name__ == '__main__':
    sys.exit(main())
