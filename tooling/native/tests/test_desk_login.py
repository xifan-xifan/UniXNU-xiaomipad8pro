#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Replay the real login script with offline HID, session and camera transports."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

W = Path(__file__).resolve().parents[3]
CLEAR_AND_SUBMIT = ['key cmd+a', 'key backspace', 'type --stdin --enter']


class LoginTests(unittest.TestCase):
    def run_case(self, mode, expected, transactions, frame=None):
        out = W / 'out'
        out.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out) as directory:
            p = Path(directory)
            secret = p / 'dummy.secret'
            secret.write_text('fixture-only\n')
            console = p / 'console'
            console.write_text('PianoA830: SecurityAgent opens the accelerator\n'
                               'NativeGopDisplay: last frame 99.4% non-black\n')
            (p / 'clock').write_text(str(int(time.time()) + 2))
            trace = p / 'trace'
            commands = {
                'hid': """echo "$*" >> "$TRACE"
case "$1" in
  status) [ "$MODE" != noinput ];;
  type) test "$(cat)" = fixture-only;;
esac
""",
                'probe': """test "$1" = 501 || exit 2
n=$(grep -c '^type ' "$TRACE" || :)
case "$MODE:$n" in
  already:*|ready:[12]|retry:2) printf '501\n81\n';;
  oldfinder:0) printf '0\n80\n';;
  oldfinder:1) printf '501\n80\n';;
  wronguid:1) printf '502\n81\n';;
  slowfinder:1) echo 501;;
  *) echo 0;;
esac
""",
                'date': 'cat "$STATE/clock"\n',
                'sleep': 'echo $(( $(cat "$STATE/clock") + 3 * $1 )) > "$STATE/clock"\n',
                'imagesnap': """for photo in "$@"; do :; done
echo "camera ${photo##*/}" >> "$TRACE"
if [ -n "$FRAME" ]; then cp "$FRAME" "$photo"; else echo image > "$photo"; fi
epoch=$(cat "$STATE/clock")
case "$MODE:${photo##*/}" in
  stale:*|retrystale:after-wake-2.jpg) epoch=$((epoch - 300));;
esac
touch -t "$(/bin/date -r "$epoch" +%Y%m%d%H%M.%S)" "$photo"
""",
            }
            if frame is None:
                commands['ffmpeg'] = """y=130
[ "$MODE" != dark ] || y=8
echo "lavfi.signalstats.YAVG=$y"
"""
            for name, body in commands.items():
                tool = p / name
                tool.write_text('#!/bin/sh\n' + body)
                tool.chmod(0o700)
            env = dict(os.environ, PATH=str(p) + ':' + os.environ['PATH'],
                       DESK_LOGIN_INPUT=str(p / 'hid'), DESK_LOGIN_PROBE=str(p / 'probe'),
                       DESK_LOGIN_UID='501', TRACE=str(trace), STATE=str(p), MODE=mode,
                       FRAME=str(frame) if frame else '')
            r = subprocess.run(['/bin/bash', str(W / 'tooling/native/desk-login.sh'),
                                '--console', str(console), '--timeout', '300', '--secret', str(secret),
                                '--webcam-dir', str(p / 'camera')], env=env, capture_output=True,
                               text=True, timeout=20)
            calls = trace.read_text().splitlines()
            self.assertEqual(r.returncode, expected, (r.stdout, r.stderr, calls))
            self.assertNotIn('fixture-only', r.stdout + r.stderr)
            self.assertEqual(calls.count('type --stdin --enter'), transactions)
            if transactions:
                keys = [line for line in calls if not line.startswith(('status', 'camera '))]
                self.assertEqual(keys[:4 * transactions], (['key shift'] + CLEAR_AND_SUBMIT) * transactions)
                for attempt in range(1, transactions + 1):
                    self.assertIn(f'camera after-wake-{attempt}.jpg', calls)
                    self.assertIn(f'fresh lit frame and HID field clear complete for attempt {attempt}', r.stdout)
            if expected == 0 and transactions:
                self.assertIn('new PID and console user confirmed', r.stdout)
            return r.stdout

    def test_pre_type_gates(self):
        for mode in ('stale', 'dark', 'noinput'):
            with self.subTest(mode=mode):
                self.run_case(mode, 2, 0)

    def test_process_and_console_proof(self):
        self.run_case('ready', 0, 1)
        self.run_case('already', 0, 0)
        for mode in ('oldfinder', 'wronguid', 'slowfinder'):
            with self.subTest(mode=mode):
                self.run_case(mode, 1, 1)

    def test_retry_requires_another_lit_frame(self):
        self.run_case('retry', 0, 2)
        self.run_case('retrystale', 2, 1)
        self.run_case('noauth', 1, 2)


if __name__ == '__main__':
    unittest.main()
