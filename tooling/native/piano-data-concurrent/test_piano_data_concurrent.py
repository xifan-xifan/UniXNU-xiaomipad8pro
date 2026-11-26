#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the boot daemon with stub mount, sysctl and sleep commands."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / 'piano-data-concurrent.sh'
failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what)
    if not ok:
        failures.append(what)


STUBS = {
    'sysctl': '''if [ "$1" = -w ]; then echo "sysctl $*" >> "$T/calls"; [ -f "$T/refuse" ] && exit 1
  v=${2#*=}; echo "$v" > "$T/mode"; [ "$v" = 1 ] && [ -f "$T/io" ] && sed -i.bak 's/^mode=single-gate depth=1/mode=concurrent depth=8/' "$T/io"; exit 0; fi
n=$(cat "$T/polls" 2>/dev/null || echo 0)
case "$2" in
  machdep.piano_data_concurrent)
    [ -f "$T/mode" ] && [ "$n" -ge "$(cat "$T/mode_after")" ] || exit 1
    if [ -f "$T/unreadable" ] && [ -f "$T/calls" ] && grep -q 'sysctl -w' "$T/calls"; then exit 1; fi
    cat "$T/mode" ;;
  machdep.piano_data_io)
    [ -f "$T/io" ] || exit 1
    printf '%s' "$(cat "$T/io")"
    [ ! -f "$T/state" ] || printf ' store_state=%s' "$(cat "$T/state")"
    printf '\n' ;;
  *) exit 1 ;;
esac''',
    'mount': '''n=$(cat "$T/polls" 2>/dev/null || echo 0); echo $((n + 1)) > "$T/polls"
echo "/dev/disk3s1s1 on / (apfs, sealed, local, read-only, journaled)"
if [ -f "$T/mounted" ] && [ "$n" -ge "$(cat "$T/mounted")" ] && [ "$n" -lt "$(cat "$T/unmount_after")" ]; then
  echo "/dev/disk3s5 on /System/Volumes/Data (apfs, local, journaled, nobrowse)"
fi''',
    'sleep': 'echo "sleep $1" >> "$T/calls"; exit 0',
    'mv': '[ ! -f "$T/refuse_status" ] || exit 1\nexec /bin/mv "$@"',
}


def run(mode='0', io='mode=single-gate depth=1 inflight_max=1 waits=0', state='2', mounted='0', mode_after='0',
        unmount_after='100', enable='1', refuse=False, wait='20', unreadable=False, refuse_status=False):
    with tempfile.TemporaryDirectory() as t:
        T = Path(t); b = T / 'bin'; b.mkdir()
        for k, v in STUBS.items():
            (b / k).write_text('#!/bin/sh\nT=' + t + '\n' + v + '\n'); (b / k).chmod(0o755)
        if mode is not None: (T / 'mode').write_text(mode + '\n')
        if io is not None: (T / 'io').write_text(io + '\n')
        if state is not None: (T / 'state').write_text(state)
        (T / 'mode_after').write_text(mode_after)
        (T / 'unmount_after').write_text(unmount_after)
        if mounted is not None: (T / 'mounted').write_text(mounted)
        if refuse: (T / 'refuse').write_text('')
        if unreadable: (T / 'unreadable').touch()
        if refuse_status:
            (T / 'refuse_status').touch()
            (T / 'status').write_text('previous complete result\n')
        env = {'PATH': '/usr/bin:/bin', 'PIANO_DC_PATH': f'{b}:/usr/bin:/bin', 'PIANO_DC_OUT': str(T / 'out'), 'PIANO_DC_STATUS': str(T / 'status'),
               'PIANO_DC_WAIT': wait, 'PIANO_DC_PERIOD': '2'}
        if enable is not None: env['PIANO_DATA_CONCURRENT_AT_BOOT'] = enable
        r = subprocess.run(['/bin/sh', str(SCRIPT)], env=env, capture_output=True, text=True, timeout=30)
        assert not list(T.glob('status.*')), 'temporary status files remain'
        rd = lambda n: (T / n).read_text() if (T / n).exists() else ''
        return r.returncode, rd('out'), rd('status'), rd('calls'), rd('mode').strip()


rc, out, st, calls, m = run()
check(rc == 0 and m == '1' and 'switched: machdep.piano_data_concurrent=1 (store state 2, mode=concurrent depth=8)' in out, 'state 2 on the direct store: switched to 1')
check(out.count('\n') == 1 and st.strip() == out.split('piano-data-concurrent: ', 1)[1].strip(), 'one console line, the same text in the status file')
rc, out, st, calls, m = run(mounted='4', mode_after='4')
check(m == '1' and calls.count('sleep 2') == 4 and 'after 8 s' in out, 'late publication and mount: waited, then switched')
rc, out, st, calls, m = run(unmount_after='1')
check(m == '0' and 'sysctl -w' not in calls and 'disappeared before the switch' in out, 'mount lost between checks: nothing switched')
rc, out, st, calls, m = run(enable=None)
check(m == '0' and 'sysctl -w' not in calls and 'off (PIANO_DATA_CONCURRENT_AT_BOOT is not 1)' in out, 'the plist key removed: nothing switched')
rc, out, st, calls, m = run(enable='0')
check(m == '0' and 'sysctl -w' not in calls, 'the key set to 0: nothing switched')
rc, out, st, calls, m = run(mode=None)
check('sysctl -w' not in calls and 'absent (no machdep.piano_data_concurrent' in out, 'no sysctl (no opt-in on the line): nothing switched')
rc, out, st, calls, m = run(mode='1')
check('sysctl -w' not in calls and 'already 1' in out, 'already 1: nothing written')
for s in ('0', '1', '3', '4', '5', '6', '7'):
    rc, out, st, calls, m = run(state=s)
    check(m == '0' and 'sysctl -w' not in calls and f'store state {s} (not 2' in out, f'store state {s}: nothing switched')
rc, out, st, calls, m = run(state=None, wait='6')
check(m == '0' and 'sysctl -w' not in calls and 'store state unread' in out, 'older kernel without live state: nothing switched')
rc, out, st, calls, m = run(mounted=None, wait='6')
check(m == '0' and 'sysctl -w' not in calls and 'not mounted after 6 s' in out, 'Data never mounted: nothing switched')
rc, out, st, calls, m = run(io='mode=volatile depth=1 inflight_max=0')
check(m == '0' and 'sysctl -w' not in calls and 'not the direct store in single-gate mode (mode=volatile depth=1)' in out, 'volatile Data (the RAM qual): nothing switched')
rc, out, st, calls, m = run(refuse=True)
check(m == '0' and 'switch not confirmed' in out and 'concurrent=0' in out, 'the write refused (EIO / ENXIO): logged, left serial')
rc, out, st, calls, m = run(unreadable=True)
check(m == '1' and 'concurrent=unreadable' in out, 'failed readback does not report serial mode after a successful write')
rc, out, st, calls, m = run(refuse_status=True)
check(m == '1' and 'switched:' in out and st == 'previous complete result\n', 'failed status replacement preserves the previous result and logs to console')
rc, out, st, calls, m = run()
check('=0' not in calls, 'never writes 0')
print('piano-data-concurrent host test:', 'PASS' if not failures else f'{len(failures)} FAIL')
sys.exit(1 if failures else 0)
