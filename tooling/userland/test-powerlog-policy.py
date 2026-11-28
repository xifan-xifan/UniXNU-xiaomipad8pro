#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host test of piano-powerlog-policy.sh against a launchctl stub on a temporary fake root; no tablet and no Apple files."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / 'piano-powerlog-policy.sh'
LABEL = 'com.apple.PerfPowerServices'
TARGET = f'system/{LABEL}'

STUB = '''#!/bin/sh
state=$STUB_STATE
log() { printf '%s\\n' "$*" >> "$state/calls"; }
has() { grep -qxF "$1" "$state/$2" 2> /dev/null; }
add() { mkdir -p "$state"; printf '%s\n' "$1" >> "$state/$2"; }
del() { [ -f "$state/$2" ] && grep -vxF "$1" "$state/$2" > "$state/$2.new" 2> /dev/null; mv -f "$state/$2.new" "$state/$2" 2> /dev/null || : > "$state/$2"; }
cmd=$1; shift
case "$cmd" in
  print) [ -f "$state/loaded" ] && has "$1" loaded && exit 0; exit 3 ;;
  print-disabled) printf '%s\\n' '{' '  "'"$TARGET_LABEL"'" = false;' '}' > /dev/null
    if has "$TARGET_LABEL" disabled; then printf '"%s" => true\\n' "$TARGET_LABEL"; else printf '"%s" => false\\n' "$TARGET_LABEL"; fi
    exit 0 ;;
  disable) log "disable system/$TARGET_LABEL"; add "$TARGET_LABEL" disabled; del "$TARGET_LABEL" enabled ;;
  enable) log "enable system/$TARGET_LABEL"; add "$TARGET_LABEL" enabled; del "$TARGET_LABEL" disabled ;;
  bootout) log "bootout $1"; del "$1" loaded ;;
  kickstart) exit 0 ;;
  *) log "unexpected $cmd $*"; exit 0 ;;
esac
'''


def main():
    assert SCRIPT.is_file(), SCRIPT
    with tempfile.TemporaryDirectory(prefix='powerlog-policy-') as temporary:
        tmp = Path(temporary)
        root = tmp / 'root'
        state = tmp / 'stub'
        state.mkdir()
        bin_ = tmp / 'bin'
        bin_.mkdir()
        stub = bin_ / 'launchctl'
        stub.write_text(STUB.replace('$TARGET_LABEL', LABEL))
        stub.chmod(0o755)
        env = {**os.environ, 'PATH': f'{bin_}:{os.environ["PATH"]}', 'STUB_STATE': str(state),
               'PIANO_USERLAND_ROOT': str(root), 'PIANO_POWERLOG_POLICY_NOCHOWN': '1'}
        receipt = root / 'private/var/db/piano-userland/powerlog-policy'

        def run(*args, expect=0):
            p = subprocess.run(['/bin/sh', str(SCRIPT), *args], env=env, capture_output=True, text=True)
            out = p.stdout + p.stderr
            assert p.returncode == expect, f'{args}: rc {p.returncode}\n{out}'
            return out

        def load_it():
            (state / 'loaded').write_text(TARGET + '\n')

        def disabled():
            return (state / 'disabled').is_file() and LABEL in (state / 'disabled').read_text()

        def calls():
            return (state / 'calls').read_text() if (state / 'disabled').exists() or (state / 'calls').exists() else ''

        # 1. a fresh machine: an adaptive PerfPowerServices that is loaded and not disabled by anyone
        load_it()
        out = run('--install')
        assert 'PASS' in out, out
        assert disabled(), 'the job was not disabled'
        assert 'bootout system/com.apple.PerfPowerServices' in calls(), calls()
        assert (receipt / 'original').read_text().strip() == 'false', 'the owner state was not recorded'
        assert 'PASS disabled=true loaded=false policy=powerlog1' in (receipt / 'status').read_text()

        # 2. idempotent, and --check agrees
        out = run('--apply')
        assert 'PASS' in out, out
        run('--check')

        # 3. launchd brings the job back (an adaptive job can): --apply boots it out again
        load_it()
        run('--apply')
        assert not (state / 'loaded').exists() or TARGET not in (state / 'loaded').read_text()
        run('--check')

        # 4. --remove puts the owner's own state back, and a pre-existing disable is respected
        out = run('--remove')
        assert 'RESTORED' in out, out
        assert 'enable system/com.apple.PerfPowerServices' in calls(), calls()   # the owner's own state handed back
        load_it()
        run('--apply')
        (receipt / 'original').write_text('true\n')
        out = run('--remove')
        assert 'RESTORED' in out, out
        assert calls().rstrip().endswith('disable system/com.apple.PerfPowerServices'), calls()

        # 5. --remove on a machine that never had the policy is a no-op, and --check fails while the job runs
        shutil.rmtree(receipt)
        run('--remove')
        load_it()
        out = run('--check', expect=1)
        assert 'FAIL' in out, out

    print('PASS: applied and idempotent; reloaded job booted out again; --check honest; owner state recorded and restored')


if __name__ == '__main__':
    main()