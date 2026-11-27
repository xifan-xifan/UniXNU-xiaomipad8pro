#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise preflight with a local replacement for the SSH harness and synthetic reads."""

import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

NATIVE = Path(__file__).resolve().parents[1]
CAPTURE = NATIVE / "bt-session-capture.sh"
GRADER = NATIVE / "bt-preflight-grade.py"
MAC = bytes.fromhex("02 12 34 56 78 9a")
MASK_LINE = "bt-lemask1: LE event mask 000000000000101f -> 000000000000185f"
STAGED = "/private/var/piano/bluetooth-path1"
TRACE_DIR = "/private/var/root/Library/Logs/Bluetooth"
JOBS = ("com.unixnu.piano-bluetooth-path1", "com.apple.BlueTool", "com.apple.bluetoothd")
RUNTIME = {
    "bin/bluetoothd": "/usr/sbin/bluetoothd",
    "bin/BlueTool": "/usr/sbin/BlueTool",
    "bin/PianoBluetooth.runtime": "/usr/sbin/PianoBluetooth.runtime",
    "lib/QcaBTType0.dylib": "/usr/lib/QcaBTType0.dylib",
}
HELPERS = (
    "/usr/local/libexec/piano-bluetooth-start",
    "/usr/local/libexec/piano-bt-runtime-mount",
    "/usr/local/libexec/piano-bt-probe",
    "/Library/LaunchDaemons/com.unixnu.piano-bluetooth-path1.plist",
)
REDACTOR = """\
import os, re, sys

data = sys.stdin.read()
if data and os.environ["BT_PREFLIGHT_TEST_MODE"] == "redactor-error":
    sys.stdout.write(data)
    sys.exit(7)
data = re.sub(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", "<mac>", data, flags=re.I)
sys.stdout.write(data.replace("synthetic-network", "<ssid>"))
"""


def mock_reads(counter, daemon):
    reads = {
        "/usr/bin/sw_vers": "ProductName:\tmacOS\nBuildVersion:\t26C5035r",
        "/usr/sbin/sysctl kern.bootsessionuuid": "kern.bootsessionuuid: 11111111-2222-3333-4444-555555555555",
        "/usr/sbin/sysctl -n kern.piano_bootargs": "piano-usb-net=1 piano-wifi=8 piano-bt=5 piano-bt-delay=60",
        "/usr/sbin/sysctl machdep.piano_bt_boot machdep.piano_bt_firmware machdep.piano_bt_lemask machdep.piano_bt_lerdy": (
            "machdep.piano_bt_boot: 0\nmachdep.piano_bt_firmware: Test firmware\n"
            "machdep.piano_bt_lemask: 1\nmachdep.piano_bt_lerdy: 0"
        ),
        "/bin/ls -l /dev/cu.BLTH": "crw------- 1 root wheel 0x16000000 Jan  1 01:00 /dev/cu.BLTH",
        "/usr/sbin/ioreg -r -c PianoBluetooth -d 1 -l -w0": (
            f'    "piano-bt-host-ready" = Yes\n    "piano-bt-h4-le-masks-patched" = {counter}'
        ),
        "/usr/sbin/ioreg -r -c IOBluetoothHCIController -d 1 -w0": (
            "+-o IOBluetoothHCIController  <class IOBluetoothHCIController, registered, matched, active>"
        ),
        "/usr/sbin/ioreg -r -n bluetooth -d 1 -l -w0": (
            f'+-o bluetooth  <class IOPlatformDevice, registered, matched, active>\n    "local-mac-address" = <{MAC.hex()}>'
        ),
        "/usr/sbin/ioreg -p IODeviceTree -r -n bluetooth -d 1 -l -w0": (
            '    "transport-encoding" = <01000000>\n    "transport-speed" = <00d43000>'
        ),
        "/usr/bin/shasum -a 256 -c runtime.sha256": "\n".join(f"{name}: OK" for name in RUNTIME),
        "/bin/ls -l " + " ".join(HELPERS): "\n".join(f"-rwxr-xr-x 1 root wheel 1 {path}" for path in HELPERS),
        "/sbin/mount": (
            f"{STAGED}/bin on /usr/sbin (bindfs, local, read-only, union)\n"
            f"{STAGED}/lib on /usr/lib (bindfs, local, read-only, union)"
        ),
        "/usr/local/libexec/piano-bt-probe --status": "piano-bt-path1 probe: default_object=1 power=1",
        "/usr/sbin/system_profiler -timeout 12 SPBluetoothDataType": (
            f"Bluetooth:\n      Bluetooth Controller:\n          Address: {MAC.hex(':')}\n          State: On\n"
            "          Network: synthetic-network"
        ),
        '/usr/bin/log show --last boot --style compact --info --no-pager --predicate '
        'process == "kernel" AND eventMessage CONTAINS "bt-lemask1: LE event mask"': MASK_LINE,
        "/usr/bin/defaults read com.apple.MobileBluetooth.debug": "{\n    HCIFileTraces = 0;\n}",
        f"/bin/ls -lt {TRACE_DIR}/": "total 0",
    }
    reads.update({f"/usr/bin/cmp {source} {mounted}": "" for source, mounted in RUNTIME.items()})
    for job in JOBS:
        # Finished startup jobs are valid. Nested fields are not the service's state or PID.
        status = daemon if job == "com.apple.bluetoothd" else "\tstate = exited"
        reads[f"/bin/launchctl print system/{job}"] = (
            f"system/{job} = {{\n{status}\n\tlisteners = {{\n\t\tstate = running\n\t\tpid = 999\n\t}}\n}}"
        )
    return reads


def mock_ssh():
    assert len(sys.argv) == 4 and sys.argv[2] == "--raw", sys.argv[1:3]
    source = sys.argv[3]
    directory = Path(os.environ["BT_PREFLIGHT_TEST_DIR"])
    mode = os.environ["BT_PREFLIGHT_TEST_MODE"]
    full = "'@@ sw_vers'" in source
    calls = directory / "calls"
    previous = calls.read_text().splitlines() if calls.exists() else []
    with calls.open("a") as log:
        log.write("snapshot\n" if full else "poll\n")
    if mode == "hung-poll" and not full:
        time.sleep(60)

    counter, daemon = 2, "\tstate = running\n\tpid = 123"
    if mode == "stuck" or (mode == "transition" and not previous):
        counter, daemon = 0, "\tstate = spawn scheduled"
    if mode == "nested":
        daemon = "\tstate = running"  # No top-level PID.

    # Route each tablet program through a local read-only fixture. Shell helpers
    # still execute, so the tests cover the actual filtering and section markers.
    cases = []
    reads = mock_reads(counter, daemon)
    if mode == "no-trace":
        reads["/usr/bin/defaults read com.apple.MobileBluetooth.debug"] = None
        reads[f"/bin/ls -lt {TRACE_DIR}/"] = None
    for invocation, output in reads.items():
        if output is None:
            action = "printf '%s\\n' 'trace unavailable' >&2; return 1"
        else:
            action = f"printf '%s\\n' {shlex.quote(output)}" if output else ":"
        cases.append(f"{shlex.quote(invocation)}) {action} ;;")
    fixture = (
        "set -f\nmock_read() {\ncase \"$*\" in\n" + "\n".join(cases)
        + "\n*) printf '%s\\n' \"$*\" >> unexpected; return 99 ;;\nesac\n}\n"
    )
    source = re.sub(r"(?m)^([ \t]*)(/[a-z][\w/.-]*)", r"\1mock_read \2", source)
    source = re.sub(r"(?m)^([ \t]*)mock_read (/usr/bin/(?:awk|grep|tr))\b", r"\1\2", source)
    source = source.replace(f"cd {STAGED} &&", "cd . &&")
    if mode == "incomplete":
        source = source.replace("'@@ end' BT_PREFLIGHT_DONE", "'@@ end'")
    result = subprocess.run(["/bin/bash", "-c", fixture + source], cwd=directory)
    return 7 if mode == "ssh-error" and full else result.returncode


class PreflightTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="bt-preflight-")
        self.addCleanup(tmp.cleanup)
        self.directory = Path(tmp.name)
        self.results = self.directory / "results dir"
        self.artifact = self.results / "preflight.txt"
        self.ssh = self.directory / "mock-ssh"
        python = shlex.quote(sys.executable)
        test = shlex.quote(str(Path(__file__).resolve()))
        self.ssh.write_text(f'#!/bin/bash\nexec {python} {test} --mock-ssh "$@"\n')
        self.ssh.chmod(0o700)
        self.redactor = self.directory / "redact-wifi.py"
        self.redactor.write_text(REDACTOR)

    def capture(self, mode, *options):
        env = dict(os.environ, BT_SESSION_SSH=str(self.ssh), REDACT_WIFI=str(self.redactor),
                   BT_PREFLIGHT_TEST_DIR=str(self.directory), BT_PREFLIGHT_TEST_MODE=mode)
        result = subprocess.run([str(CAPTURE), "preflight", str(self.results), *options],
                                env=env, capture_output=True, text=True, timeout=15)
        unexpected = self.directory / "unexpected"
        self.assertFalse(unexpected.exists(), unexpected.read_text() if unexpected.exists() else "")
        return result

    def calls(self):
        return (self.directory / "calls").read_text().splitlines()

    def grade(self, code, *extra):
        grade = subprocess.run([sys.executable, str(GRADER), str(self.artifact), *extra],
                               capture_output=True, text=True, timeout=5)
        self.assertEqual(grade.returncode, code, grade.stdout + grade.stderr)
        self.assertIn(f"BT_PREFLIGHT {('READY', 'NOT-READY', 'VOID')[code]}", grade.stdout)
        return grade.stdout

    def test_bash_syntax(self):
        subprocess.run(["bash", "-n", str(CAPTURE)], check=True)

    def test_immediate_ready_and_redacted_snapshot(self):
        result = self.capture("ready")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), ["poll", "snapshot"])
        data = self.artifact.read_text()
        self.assertRegex(data, r"^BT_PREFLIGHT_SETTLE status=ready elapsed_seconds=\d+ wait_seconds=90\n@@ sw_vers\n")
        self.assertTrue(data.endswith("@@ end\nBT_PREFLIGHT_DONE\n"))
        self.assertIn('"local-mac-address" = <redacted>', data)
        self.assertIn("Address: <mac>", data)
        self.assertIn("Network: <ssid>", data)
        for raw in (MAC.hex(), MAC.hex(":"), "synthetic-network"):
            self.assertNotIn(raw, data)
        self.grade(0)

    def test_waits_through_daemon_restart(self):
        result = self.capture("transition", "--wait-seconds", "08")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), ["poll", "poll", "snapshot"])
        data = self.artifact.read_text()
        self.assertRegex(data, r"^BT_PREFLIGHT_SETTLE status=ready elapsed_seconds=[1-8] wait_seconds=8\n")
        self.assertNotIn("spawn scheduled", data)
        self.assertIn('"piano-bt-h4-le-masks-patched" = 2', data)
        self.grade(0)

    def test_wait_timeout_still_captures(self):
        result = self.capture("stuck", "--wait-seconds", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), ["poll", "snapshot"])
        grade = self.grade(1)
        for gate in ("settle", "mask-counter", "bluetoothd"):
            self.assertIn(f"FAIL BT-{gate} ", grade)

    def test_nested_launchd_pid_does_not_count(self):
        result = self.capture("nested", "--wait-seconds", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        grade = self.grade(1)
        self.assertIn("FAIL BT-settle ", grade)
        self.assertIn("FAIL BT-bluetoothd ", grade)

    def test_hung_poll_is_bounded(self):
        result = self.capture("hung-poll", "--wait-seconds", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), ["poll", "snapshot"])
        grade = self.grade(1)
        self.assertIn("FAIL BT-settle ", grade)
        self.assertIn("PASS BT-bluetoothd ", grade)

    def test_incomplete_and_failed_captures_are_void(self):
        for mode, exit_code in (("incomplete", 0), ("ssh-error", 1), ("redactor-error", 1)):
            with self.subTest(mode=mode):
                result = self.capture(mode, "--wait-seconds", "0")
                self.assertEqual(result.returncode, exit_code, result.stderr)
                self.grade(2)
                self.assertNotIn(MAC.hex(":"), self.artifact.read_text())

    def test_zero_skips_polling_and_invalid_wait_is_rejected(self):
        result = self.capture("ready", "--wait-seconds", "-1")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse((self.directory / "calls").exists())
        result = self.capture("no-trace", "--wait-seconds", "0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), ["snapshot"])
        grade = self.grade(0)
        self.assertIn("INFO BT-settle status=skipped", grade)
        self.assertIn("INFO BT-trace HCI trace preference armed=0, pklg files=0", grade)

    def test_console_fallback_and_legacy_capture(self):
        result = self.capture("ready", "--wait-seconds", "0")
        self.assertEqual(result.returncode, 0, result.stderr)
        legacy = self.artifact.read_text().split("\n", 1)[1]
        missing_bits = MASK_LINE.replace("185f", "181f")
        self.artifact.write_text(legacy.replace(MASK_LINE, missing_bits))
        self.assertIn("FAIL BT-mask-line ", self.grade(1))
        console = self.directory / "boot-console"
        console.write_text(f"{missing_bits}\n{MASK_LINE}\n")
        self.assertIn("PASS BT-mask-line LE event mask bits 6 and 11 set (boot console)", self.grade(0, str(console)))

    def test_grader_checks_full_snapshot_even_after_ready(self):
        result = self.capture("ready")
        self.assertEqual(result.returncode, 0, result.stderr)
        ready = self.artifact.read_text()
        for before, after, gate in (
            ("26C5035r", "unexpected", "build"),
            ("piano-bt=5\n", "piano-bt=5\npiano-bt=4\n", "arg-bt"),
            ("piano-bt=5\n", "piano-bt=4\tpiano-bt=5\n", "arg-bt"),
            ("piano-bt=5\n", "piano-bt=5\npiano-bt-ibs=1\n", "arg-ibs"),
            ('"piano-bt-host-ready" = Yes', '"piano-bt-host-ready" = No', "host-ready"),
            ("bin/BlueTool: OK\n", "", "runtime"),
            ("(bindfs, local, read-only, union)", "(bindfs, local, union)", "mount"),
            ("\tpid = 123", "\t\tpid = 123", "bluetoothd"),
            ("power=1", "power=10", "power"),
            ("State: On", "State: Off", "profiler"),
        ):
            with self.subTest(gate=gate):
                self.artifact.write_text(ready.replace(before, after, 1))
                self.assertIn(f"FAIL BT-{gate} ", self.grade(1))
        self.artifact.write_text(ready.replace("piano-bt=5\n", "piano-bt=5\tpiano-bt=5\n", 1))
        self.grade(0)
        self.artifact.write_text(ready.replace("@@ trace\n", ""))
        self.grade(2)


if __name__ == "__main__":
    if sys.argv[1:2] == ["--mock-ssh"]:
        raise SystemExit(mock_ssh())
    unittest.main()
