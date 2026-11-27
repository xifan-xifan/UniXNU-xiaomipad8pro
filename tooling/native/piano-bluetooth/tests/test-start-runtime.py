#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang. All rights reserved.
"""Run the real startup shell with host-only replacements for tablet commands."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / 'start-runtime.sh'
with tempfile.TemporaryDirectory(prefix='bt runtime host ') as tmp:
    tmp = Path(tmp)
    for name in ('sysctl', 'launchctl', 'sleep', 'mount'):
        stub = tmp / name
        stub.write_text('''#!/bin/sh
printf '%s %s\n' "${0##*/}" "$*" >> "$BT_CALLS"
case "${0##*/}" in
sysctl)
  case "$*" in
    '-N machdep.piano_bt_boot') exit "$BT_PRESENT";;
    '-n machdep.piano_bt_boot') printf '%s\n' "$BT_BOOT"; exit 0;;
    *) exit 99;;
  esac;;
mount) exit "$BT_MOUNT";;
*) exit 0;;
esac
''')
        stub.chmod(0o755)
    script = source.read_text()
    for a, b in (('/usr/sbin/sysctl', 'sysctl'), ('/bin/launchctl', 'launchctl'),
                 ('/usr/local/libexec/piano-bt-runtime-mount', 'mount'), ('/bin/sleep', 'sleep')):
        assert a in script
        script = script.replace(a, shlex.quote(str(tmp / b)))
    runner = tmp / 'start-runtime.sh'
    runner.write_text(script)
    calls = tmp / 'calls'

    def run(present=0, boot='0', mount=0):
        calls.write_text('')
        env = dict(os.environ, BT_CALLS=str(calls), BT_PRESENT=str(present),
                   BT_BOOT=boot, BT_MOUNT=str(mount))
        result = subprocess.run(['/bin/sh', str(runner)], env=env, text=True, capture_output=True)
        return result, calls.read_text().splitlines()

    # No level5 OID: stay inert even when the runtime is staged.
    r, log = run(present=1)
    assert r.returncode == 0 and log == ['sysctl -N machdep.piano_bt_boot'] and not r.stdout
    # OID exists although kern.bootargs omits the ADT tail: activate exactly once.
    r, log = run()
    assert r.returncode == 0 and 'stock services restarted' in r.stdout
    assert not any('kern.bootargs' in s for s in log)
    assert log.index('mount ') > log.index('launchctl bootout system/com.apple.bluetoothd')
    assert log.index('mount ') < log.index('sysctl -n machdep.piano_bt_boot')
    assert log.index('sysctl -n machdep.piano_bt_boot') < log.index('launchctl bootstrap system /System/Library/LaunchDaemons/com.apple.BlueTool.plist')
    assert log.count('mount ') == log.count('sysctl -n machdep.piano_bt_boot') == 1
    assert log.count('launchctl kickstart -k system/com.apple.BlueTool') == 1
    assert log.count('launchctl kickstart -k system/com.apple.bluetoothd') == 1
    # A registered but failed/pending controller never causes service restart.
    for value in ('5', '-1'):
        r, log = run(boot=value)
        assert r.returncode == 1 and 'transport_not_ready=1' in r.stdout
        assert log.count('sysctl -n machdep.piano_bt_boot') == 4
        assert not any('bootstrap' in s or 'kickstart' in s for s in log)
    r, log = run(mount=1)
    assert r.returncode == 1 and not any('bootstrap' in s or 'kickstart' in s for s in log)
print('PASS runtime startup: kernel-only level5 gate, clipped bootargs, ordering, boot failure/pending and mount refusal')
