#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""The repository's self-tests: the host tests that need no Apple content, no build output (out/) and no tablet.

    python3 scripts/ci/self-tests.py [--list] [--only SUBSTR ...] [--jobs N] [--junit FILE]

What runs:
  - Python test scripts (test-*.py, test_*.py, run-*-tests.py, run-*-host.py) under package/, tooling/, firmware/, scripts/;
  - shell test scripts (test-*.sh, run-tests.sh, run-*-host.sh, tests/run.sh) there;
  - C/C++ host tests (package/**/tests/*.c, *.cpp) whose header carries their build line
    (`c++ -std=c++20 ... NAME.cpp -o /tmp/NAME && /tmp/NAME`): compiled in their own directory, the binary in a temporary
    directory, then run.
Entries in scripts/ci/self-tests.skip (one path per line, `#` and a reason after it) are excluded from standalone runs.
The reason identifies the enclosing suite or explains why a test is left out. C/C++ tests without a build line are reported
unless they have a skip entry, so a new test cannot fall out of CI unnoticed.

The tests run with an empty HOME and without PIANO_* variables, so nothing local leaks in. The summary ends with up to ten of
the slowest tests. Exit status 1 when any test fails.
Known slow suites are queued first. For wrappers that support CI_SUBJOBS, inner jobs use CPU slots left by the outer pool.
"""
import argparse
import concurrent.futures
import os
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from xml.etree import ElementTree

ROOT = Path(__file__).resolve().parents[2]
SKIP_FILE = ROOT / 'scripts/ci/self-tests.skip'
A830_HOST_TESTS = 'package/PianoA830/adreno830/tests/run-host-tests.py'
CHARGE_HOST_TESTS = 'package/PianoPower/tests/run-charge-host.sh'
VIDEO_HOST_TESTS = 'package/PianoVideo/tests/run-bringup-host.sh'
CAMERA_HOST_TESTS = 'package/PianoCamera/tests/run-camera-host.sh'
PARALLEL_SHELL_SUITES = {CHARGE_HOST_TESTS, VIDEO_HOST_TESTS, CAMERA_HOST_TESTS}
# Approximate wall times from macos-26 CI. Unmeasured suites retain discovery order.
ESTIMATED_SECONDS = {
    'tooling/persist/test-k273-only.py': 862,
    CHARGE_HOST_TESTS: 752,
    A830_HOST_TESTS: 472,
    VIDEO_HOST_TESTS: 280,
    CAMERA_HOST_TESTS: 220,
    'tooling/k27/test-cs-native4k.sh': 210,
}
SANITIZED_SHELL_SUITES = {'package/common/tests/run.sh', 'package/include/tests/run.sh'}
TIMEOUT = int(os.environ.get('SELFTEST_TIMEOUT', '600'))
PY_RE = re.compile(r'(^|/)(test[-_][^/]*\.py|run-[^/]*(?:tests?|-host)\.py)$')
SH_RE = re.compile(r'(^|/)(test-[^/]*\.sh|run-tests\.sh|run-[^/]*-host\.sh|tests/run\.sh)$')
C_RE = re.compile(r'^package/.*/tests/[^/]+\.(c|cc|cpp)$')
# a build line in the first lines of a host test: `c++ ... -o /tmp/NAME && /tmp/NAME [args]`
BUILD_RE = re.compile(
    r'((?:xcrun\s+)?(?:c\+\+|clang\+\+|clang|cc)\s+[^\n]*?-o\s+'
    r'(?P<output>\S+)\s*&&\s*(?P<run>(?P=output))(?=\s|$)[^\n]*)'
)
INVALID_XML = re.compile(r'[^\x09\x0a\x0d\x20-\ud7ff\ue000-\ufffd\U00010000-\U0010ffff]')


def tracked():
    out = subprocess.check_output(['git', '-C', str(ROOT), 'ls-files', '-z'])
    return [os.fsdecode(p) for p in out.split(b'\0') if p]


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


def collect(only, skips):
    tests, unrunnable = [], []
    for p in tracked():
        if not p.startswith(('package/', 'tooling/', 'firmware/', 'scripts/')):
            continue
        if only and not any(pattern in p for pattern in only):
            continue
        if PY_RE.search(p):
            tests.append(('py', p, None))
        elif SH_RE.search(p):
            tests.append(('sh', p, None))
        elif C_RE.match(p):
            line = build_line(p)
            if line or p in skips:
                tests.append(('c', p, line))
            elif re.search(r'(_host|^test_)', Path(p).stem):
                unrunnable.append(p)
    return tests, unrunnable


def env_for(home):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PIANO_', 'TURNIP_', 'K27_', 'K273_'))}
    env['HOME'] = home
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    return env


class SuiteProcesses:
    def __init__(self):
        self.lock = threading.Lock()
        self.active = set()
        self.stopped = False

    @staticmethod
    def kill(process):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def stop(self):
        with self.lock:
            self.stopped = True
            for process in self.active:
                self.kill(process)

    def run(self, cmd, cwd, env):
        # A suite can leave subprocesses behind even when its own process has exited.
        with self.lock:
            if self.stopped:
                return subprocess.CompletedProcess(cmd, 1, '', 'interrupted before launch')
            process = subprocess.Popen(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, encoding='utf-8', errors='replace', start_new_session=True)
            self.active.add(process)
        try:
            with process:
                try:
                    stdout, stderr = process.communicate(timeout=TIMEOUT)
                except subprocess.TimeoutExpired:
                    self.kill(process)
                    raise
            return subprocess.CompletedProcess(cmd, process.returncode, stdout, stderr)
        finally:
            with self.lock:
                self.active.remove(process)


def run_one(kind, path, line, subjob_limit=1, processes=None):
    start = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='selftest-') as tmp:
        home = os.path.join(tmp, 'home'); os.mkdir(home)
        env = env_for(home)
        full = ROOT / path
        if path == A830_HOST_TESTS:
            cmd, cwd = [sys.executable, str(ROOT / 'scripts/ci/adreno830-host.py'), tmp], ROOT
        elif kind == 'py':
            cmd, cwd = [sys.executable, str(full)], ROOT
        elif kind == 'sh':
            cmd, cwd = ['bash', str(full)], ROOT
            if path in SANITIZED_SHELL_SUITES:
                cmd.append('--sanitize')
            if path in PARALLEL_SHELL_SUITES:
                env['SELFTEST_SUBJOB_LIMIT'] = str(subjob_limit)
                env['TMPDIR'] = tmp
        else:
            out = os.path.join(tmp, 'bin')
            os.mkdir(out)
            # Relocate each use of the executable, including later invocations and fixture builders.
            output = BUILD_RE.search(line)['output']
            exe = shlex.quote(os.path.join(out, os.path.basename(output)))
            line = re.sub(r'(?<!\S)' + re.escape(output) + r'(?=$|[\s;&|)])', lambda _: exe, line)
            cmd, cwd = ['bash', '-c', line], full.parent
        try:
            r = (processes or SuiteProcesses()).run(cmd, cwd, env)
            ok, output = r.returncode == 0, (r.stdout + r.stderr)
        except subprocess.TimeoutExpired as e:
            partial = b'\n'.join(stream for stream in (e.stdout, e.stderr) if stream)
            ok, output = False, f'timeout after {TIMEOUT} s\n' + partial.decode('utf-8', errors='replace')
    return path, ok, time.monotonic() - start, output


def write_junit(path, results, skipped):
    suite = ElementTree.Element('testsuite', name='self-tests', tests=str(len(results) + len(skipped)),
                                failures=str(sum(not ok for _, ok, _, _ in results)))
    for test, ok, secs, output in results:
        case = ElementTree.SubElement(suite, 'testcase', classname='selftest',
                                      name=INVALID_XML.sub('\ufffd', test), time=f'{secs:.1f}')
        if not ok:
            failure = ElementTree.SubElement(case, 'failure', message='failed')
            failure.text = INVALID_XML.sub('\ufffd', output[-20000:])
    for test, reason in skipped:
        case = ElementTree.SubElement(suite, 'testcase', classname='selftest', name=INVALID_XML.sub('\ufffd', test))
        ElementTree.SubElement(case, 'skipped', message=INVALID_XML.sub('\ufffd', reason))
    ElementTree.ElementTree(suite).write(path, encoding='utf-8', xml_declaration=True)


def main():
    cpu_count = os.cpu_count() or 2
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--only', '--match', action='append', metavar='SUBSTR', help='select paths matching any supplied substring')
    ap.add_argument('--jobs', type=int, default=cpu_count)
    ap.add_argument('--junit')
    a = ap.parse_args()
    if a.jobs < 1:
        ap.error('--jobs must be at least 1')
    skips = load_skips()
    tests, unrunnable = collect(a.only, skips)
    stale = sorted(set(skips) - {p for _, p, _ in tests}) if not a.only else []
    stale_message = 'skip entries that name no test any more: ' + ', '.join(stale) if stale else ''
    run = [t for t in tests if t[1] not in skips]
    if a.list:
        for kind, p, line in tests:
            print(f'{"skip" if p in skips else "run ":4} {kind:2} {p}' + (f'  ({skips[p]})' if p in skips else ''))
        for p in unrunnable:
            print(f'none    {p}  (no build line in its header)')
        if stale_message:
            print(stale_message)
        return 0
    run.sort(key=lambda test: -ESTIMATED_SECONDS.get(test[1], 0))
    workers = min(a.jobs, len(run))
    parallel_suites = min(workers, sum(path in PARALLEL_SHELL_SUITES for _, path, _ in run))
    # Each active suite already has one slot. Divide any spare slots among the wrappers that can use them.
    subjob_limit = 1 + max(0, cpu_count - workers) // max(1, parallel_suites)
    results = []
    processes = SuiteProcesses()
    interrupted = 0

    def interrupt(signum, _frame):
        nonlocal interrupted
        if not interrupted:
            interrupted = signum
            raise KeyboardInterrupt

    handlers = {sig: signal.signal(sig, interrupt) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            try:
                futures = [ex.submit(run_one, *t, subjob_limit, processes) for t in run]
                for f in concurrent.futures.as_completed(futures):
                    path, ok, secs, output = f.result()
                    results.append((path, ok, secs, output))
                    print(f'{"PASS" if ok else "FAIL"} {secs:6.1f}s {path}', flush=True)
                    if not ok:
                        print('    ' + '\n    '.join(output.strip().splitlines()[-25:]), flush=True)
            except BaseException:
                processes.stop()
                ex.shutdown(cancel_futures=True)
                raise
    except KeyboardInterrupt:
        if not interrupted:
            raise
        print(f'Interrupted by {signal.Signals(interrupted).name}', file=sys.stderr)
        return 128 + interrupted
    finally:
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
    failed = [r for r in results if not r[1]]
    print(f'\n{len(results) - len(failed)} passed, {len(failed)} failed, {len(tests) - len(run)} skipped '
          f'(scripts/ci/self-tests.skip), {len(unrunnable)} C/C++ tests without a build line')
    if stale_message:
        print(stale_message)
    if a.junit:
        write_junit(a.junit, results, [(p, skips[p]) for _, p, _ in tests if p in skips])
    if results:
        slowest = sorted(results, key=lambda result: (-result[2], result[0]))[:10]
        print(f'\nSlowest {len(slowest)} tests:')
        for path, _, secs, _ in slowest:
            print(f'{secs:6.1f}s {path}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
