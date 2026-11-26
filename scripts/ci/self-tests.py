#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""The repository's self-tests: the host tests that need no Apple content, no build output (out/) and no tablet.

    python3 scripts/ci/self-tests.py [--list] [--only SUBSTR] [--jobs N] [--junit FILE]

What runs:
  - Python test scripts (test-*.py, test_*.py, run-*-tests.py) under package/, tooling/, firmware/;
  - shell test scripts (test-*.sh, run-tests.sh, run-*-host.sh, tests/run.sh) there;
  - C/C++ host tests (package/**/tests/*.c, *.cpp) whose header carries their build line
    (`c++ -std=c++20 ... NAME.cpp -o /tmp/NAME && /tmp/NAME`): compiled in their own directory, the binary in a temporary
    directory, then run.
Anything listed in scripts/ci/self-tests.skip (one path per line, `#` and a reason after it) is not run: the reason says what it
needs that a hosted runner does not have (Apple content, out/, the tablet, a Linux host). A test that is neither run nor skipped
is reported, so a new test cannot fall out of CI unnoticed.

The tests run with an empty HOME and without PIANO_* variables, so nothing local leaks in. Exit status 1 when any test fails.
"""
import argparse
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[2]
SKIP_FILE = ROOT / 'scripts/ci/self-tests.skip'
TIMEOUT = int(os.environ.get('SELFTEST_TIMEOUT', '600'))
PY_RE = re.compile(r'(^|/)(test[-_][^/]*\.py|run-[^/]*tests?\.py)$')
SH_RE = re.compile(r'(^|/)(test-[^/]*\.sh|run-tests\.sh|run-[^/]*-host\.sh|tests/run\.sh)$')
C_RE = re.compile(r'^package/.*/tests/[^/]+\.(c|cc|cpp)$')
# a build line in the first lines of a host test: `c++ ... -o /tmp/NAME && /tmp/NAME [args]`
BUILD_RE = re.compile(r'((?:xcrun\s+)?(?:c\+\+|clang\+\+|clang|cc)\s+[^\n]*?-o\s+(\S+)\s*&&\s*\2\b[^\n]*)')


def tracked():
    out = subprocess.run(['git', '-C', str(ROOT), 'ls-files'], capture_output=True, text=True, check=True).stdout
    return [p for p in out.splitlines() if p]


def load_skips():
    skips = {}
    if SKIP_FILE.exists():
        for line in SKIP_FILE.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            path, _, why = line.partition('#')
            skips[path.strip()] = why.strip() or 'no reason given'
    return skips


def build_line(path):
    head = '\n'.join((ROOT / path).read_text(errors='replace').splitlines()[:80])
    m = BUILD_RE.search(head)
    if not m:
        return None
    line = m.group(1).strip()
    line = re.sub(r'\s*\[[^\]]*\]\s*$', '', line)          # `[v]`-style optional argument notes
    line = line.rstrip(' */\\')
    return line


def collect(only):
    tests, unrunnable = [], []
    for p in tracked():
        if not p.startswith(('package/', 'tooling/', 'firmware/', 'scripts/')):
            continue
        if only and only not in p:
            continue
        if PY_RE.search(p):
            tests.append(('py', p, None))
        elif SH_RE.search(p):
            tests.append(('sh', p, None))
        elif C_RE.match(p):
            line = build_line(p)
            if line:
                tests.append(('c', p, line))
            elif re.search(r'(_host|^test_)', Path(p).stem):
                unrunnable.append(p)
    return tests, unrunnable


def env_for(home):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PIANO_', 'TURNIP_', 'K27_', 'K273_'))}
    env['HOME'] = home
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    return env


def run_one(kind, path, line):
    start = time.time()
    with tempfile.TemporaryDirectory(prefix='selftest-') as tmp:
        home = os.path.join(tmp, 'home'); os.mkdir(home)
        env = env_for(home)
        full = ROOT / path
        if kind == 'py':
            cmd, cwd = [sys.executable, str(full)], ROOT
        elif kind == 'sh':
            cmd, cwd = ['bash', str(full)], ROOT
        else:
            out = os.path.join(tmp, 'bin')
            os.mkdir(out)
            # the binary goes to the temporary directory, not /tmp: parallel runs and other users stay apart
            m = BUILD_RE.search(line)
            exe = m.group(2)
            line = line.replace(exe, os.path.join(out, os.path.basename(exe)))
            cmd, cwd = ['bash', '-c', line], full.parent
        try:
            r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, timeout=TIMEOUT,
                               stdin=subprocess.DEVNULL)
            ok, output = r.returncode == 0, (r.stdout + r.stderr)
        except subprocess.TimeoutExpired as e:
            ok, output = False, f'timeout after {TIMEOUT} s\n{(e.stdout or b"")!s}'
    return path, ok, time.time() - start, output


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--only')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    ap.add_argument('--junit')
    a = ap.parse_args()
    tests, unrunnable = collect(a.only)
    skips = load_skips()
    stale = sorted(set(skips) - {p for _, p, _ in tests} - set(unrunnable))
    run = [t for t in tests if t[1] not in skips]
    if a.list:
        for kind, p, line in tests:
            print(f'{"skip" if p in skips else "run ":4} {kind:2} {p}' + (f'  ({skips[p]})' if p in skips else ''))
        for p in unrunnable:
            print(f'none    {p}  (no build line in its header)')
        return 0
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futures = [ex.submit(run_one, *t) for t in run]
        for f in concurrent.futures.as_completed(futures):
            path, ok, secs, output = f.result()
            results.append((path, ok, secs, output))
            print(f'{"PASS" if ok else "FAIL"} {secs:6.1f}s {path}', flush=True)
            if not ok:
                print('    ' + '\n    '.join(output.strip().splitlines()[-25:]), flush=True)
    failed = [r for r in results if not r[1]]
    print(f'\n{len(results) - len(failed)} passed, {len(failed)} failed, {len(tests) - len(run)} skipped '
          f'(scripts/ci/self-tests.skip), {len(unrunnable)} C/C++ tests without a build line')
    if stale and not a.only:
        print('skip entries that name no test any more: ' + ', '.join(stale))
    if a.junit:
        cases = []
        for path, ok, secs, output in results:
            body = '' if ok else f'<failure message="failed">{escape(output[-20000:])}</failure>'
            cases.append(f'<testcase classname="selftest" name="{escape(path)}" time="{secs:.1f}">{body}</testcase>')
        for kind, p, _ in tests:
            if p in skips:
                cases.append(f'<testcase classname="selftest" name="{escape(p)}"><skipped message="{escape(skips[p])}"/></testcase>')
        Path(a.junit).write_text(f'<?xml version="1.0"?>\n<testsuite name="self-tests" tests="{len(cases)}" '
                                 f'failures="{len(failed)}">{"".join(cases)}</testsuite>\n')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
