# SPDX-License-Identifier: MIT
"""Exercise the installed shell fragment with local stand-ins for the tablet."""

import subprocess
import tempfile
import time
import unittest
from pathlib import Path


SPEC = Path(__file__).with_name('installed.spec')
DRIVER = r"""
set -euo pipefail
. "$1"
fixture=$2 mode=$3
export FIXTURE_REMOTE="$fixture/remote"

rsudo() {
    local command=$1
    command=${command//\/usr\/bin\/stat/$fixture\/stat}
    command=${command//\/bin\/launchctl/$fixture\/launchctl}
    command=${command//\/usr\/bin\/sudo/$fixture\/sudo}
    printf 'rsudo %s\n' "$SSH_T" >> "$fixture/calls"
    # Mirror the text wrapper and discard the remote exit status.
    /bin/bash -c "/bin/sh -c '$command'" || true
}

rssh() {
    local command=$1
    command=${command//\/bin\/ps/$fixture\/ps}
    printf 'rssh %s %s\n' "$SSH_T" "${command%%$'\n'*}" >> "$fixture/calls"
    printf 'transport banner\r\n'
    /bin/sh -c "$command" || true
}

wait_for_file() {
    local attempt
    for ((attempt = 0; attempt < 200; attempt++)); do
        if [[ -s "$1" ]]; then return 0; fi
        /bin/sleep 0.01
    done
    return 1
}

# Advance the virtual host clock while the real local worker controls PID and rc files.
FAKE_SECONDS=0
metal_sustain_now() {
    REPLY=$FAKE_SECONDS
}

sleep() {
    printf '%s\n' "$1" >> "$fixture/sleeps"
    FAKE_SECONDS=$((FAKE_SECONDS + $1))
    wait_for_file "$FIXTURE_REMOTE/frames.pid"
    wait_for_file "$FIXTURE_REMOTE/ready"
    if [[ "$mode" = interrupted ]]; then
        kill -TERM "$$"
        return 97
    fi
    if [[ "$mode" = success || "$mode" = error ||
          ( "$mode" = at-deadline && $(wc -l < "$fixture/sleeps") -eq 36 ) ]]; then
        if [[ "$mode" = error ]]; then
            echo 7 > "$FIXTURE_REMOTE/release"
        else
            echo 0 > "$FIXTURE_REMOTE/release"
        fi
        wait_for_file "$FIXTURE_REMOTE/frames.rc"
    fi
}

if [[ "$mode" = smoke ]]; then
    metal_sustain_smoke "$FIXTURE_REMOTE"
elif [[ "$mode" = detach ]]; then
    metal_sustain_detach "$FIXTURE_REMOTE"
else
    metal_sustain_run "$FIXTURE_REMOTE"
fi
"""

BENCHMARK = r"""
set -eu
dir=${0%/*}
if read -r input; then exit 97; fi
case "$*" in
    "--onscreen --seconds 3 --width 1920 --height 1280 --draws 256")
        echo smoke-csv
        echo smoke-log >&2
        exit 0
        ;;
    "--onscreen --minutes 10 --width 1920 --height 1280 --draws 256") ;;
    *) exit 98 ;;
esac
echo test-csv
echo test-log >&2
echo "$$" > "$dir/ready"
for ((attempt = 0; attempt < 1500; attempt++)); do
    if [[ -s "$dir/release" ]]; then exit "$(< "$dir/release")"; fi
    if [[ ! -d "$dir" ]]; then exit 99; fi
    /bin/sleep 0.02
done
exit 99
"""


class InstalledTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.fixture = Path(directory.name).resolve()
        self.remote = self.fixture / 'remote'
        self.remote.mkdir()
        self.executable('stat', '[[ "$*" = "-f %u /dev/console" ]] && echo 501')
        self.executable('launchctl', '[[ "$1 $2" = "asuser 501" ]] || exit 1\nshift 2\nexec "$@"')
        self.executable('sudo', '[[ "$1 $2 $3 $4" = "-n -u #501 --" ]] || exit 1\nshift 4\nexec "$@"')
        self.executable('ps', r'''
pid=$(< "$FIXTURE_REMOTE/ready")
[[ "$*" = "-ww -p $pid -o command=" ]] || exit 1
printf '%s/piano-metal-sustain --onscreen\n' "$FIXTURE_REMOTE"
''')
        self.executable('remote/piano-metal-sustain', BENCHMARK)

    def executable(self, name, body):
        path = self.fixture / name
        path.write_text('#!/bin/bash\n' + body + '\n')
        path.chmod(0o700)

    def run_spec(self, mode):
        return subprocess.run(
            ['/bin/bash', '-c', DRIVER, 'test-installed', str(SPEC), str(self.fixture), mode],
            capture_output=True, text=True, timeout=4 if mode == 'detach' else 20,
        )

    def assert_frames(self, rc):
        # The worker can still be reaping the child when the stop probe replies.
        status = self.remote / 'frames.rc'
        deadline = time.monotonic() + 1
        while not status.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(status.read_text(), f'{rc}\n')
        self.assertEqual((self.remote / 'frames.pid').read_text(), (self.remote / 'ready').read_text())
        self.assertEqual((self.remote / 'frames.csv').read_text(), 'test-csv\n')
        self.assertEqual((self.remote / 'frames.log').read_text(), 'test-log\n')
        self.assertEqual(list(self.remote.glob('frames.*.tmp')), [])

    def test_launch_returns_while_benchmark_is_still_running(self):
        # The worker holds until released. An SSH command that waits for its
        # child or retains the output pipe will hit the four-second test limit.
        result = self.run_spec('detach')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'METAL_SUSTAIN_LAUNCH_QUEUED\n')
        self.assertEqual(result.stderr, '')
        self.assertFalse((self.remote / 'frames.rc').exists())
        (self.remote / 'release').write_text('0\n')
        self.assert_frames(0)

    def test_detached_run_hands_off_success(self):
        result = self.run_spec('success')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'METAL_SUSTAIN_REMOTE_RC=0\n')
        self.assertIn('0s elapsed', result.stderr)
        self.assertEqual((self.fixture / 'sleeps').read_text(), '20\n')
        calls = (self.fixture / 'calls').read_text().splitlines()
        self.assertEqual(calls[0], 'rsudo 4')
        self.assertEqual(len(calls), 3)
        self.assertTrue(all(call.startswith('rssh 4 ') and call.endswith('action=poll') for call in calls[1:]))
        self.assert_frames(0)

    def test_detached_run_hands_off_nonzero_rc_for_grading(self):
        result = self.run_spec('error')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'METAL_SUSTAIN_REMOTE_RC=7\n')
        self.assert_frames(7)

    def test_deadline_signals_benchmark_pid_and_fails(self):
        result = self.run_spec('timeout')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, '')
        self.assertIn('benchmark running', result.stderr)
        self.assertIn('timed out after 720s, sent SIGTERM', result.stderr)
        self.assertEqual(len((self.fixture / 'sleeps').read_text().splitlines()), 36)
        self.assertTrue((self.remote / 'frames.stop').exists())
        self.assert_frames(143)

    def test_interrupted_host_stops_detached_benchmark(self):
        result = self.run_spec('interrupted')
        self.assertTrue((self.remote / 'frames.stop').exists(), result.stderr)
        self.assertEqual(result.returncode, 143, result.stderr)
        self.assertEqual(result.stdout, '')
        self.assert_frames(143)

    def test_deadline_collects_completed_status_before_signalling(self):
        result = self.run_spec('at-deadline')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'METAL_SUSTAIN_REMOTE_RC=0\n')
        self.assertFalse((self.remote / 'frames.stop').exists())
        self.assert_frames(0)

    def test_smoke_stays_synchronous(self):
        result = self.run_spec('smoke')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'METAL_SUSTAIN_REMOTE_RC=0\n')
        self.assertEqual(result.stderr, '')
        self.assertEqual((self.fixture / 'calls').read_text(), 'rsudo 30\n')
        self.assertEqual((self.remote / 'smoke.csv').read_text(), 'smoke-csv\n')
        self.assertEqual((self.remote / 'smoke.log').read_text(), 'smoke-log\n')
        self.assertFalse((self.remote / 'frames.pid').exists())


if __name__ == '__main__':
    unittest.main()
