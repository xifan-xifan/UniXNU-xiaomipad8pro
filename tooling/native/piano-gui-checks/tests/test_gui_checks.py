import json
import os
import platform
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

FIXTURES = Path(__file__).resolve().parent
sys.path.insert(0, str(FIXTURES.parent))
from grade import brightness_at, parse_luma, parse_panel, visible_change, wifi_off, wifi_transition
import run


class RecordedEvidence(unittest.TestCase):
    def setUp(self):
        self.registry = (FIXTURES / "recorded-backlight.txt").read_text()
        self.luma = (FIXTURES / "recorded-luma.txt").read_text()

    def test_installed_panel_snapshot(self):
        self.assertEqual(parse_panel(self.registry), (178, 2293, True))
        self.assertFalse(parse_panel(self.registry.replace('secondary-code" = 178', 'secondary-code" = 177'))[2])
        self.assertFalse(parse_panel(self.registry.replace('primary-lit" = Yes', 'primary-lit" = No'))[2])
        with self.assertRaises(ValueError):
            parse_panel(self.registry.split("IODisplayParameters")[0])
        with self.assertRaises(ValueError):
            parse_panel(self.registry * 2)

    def test_recorded_c920_change(self):
        before, low, high = [parse_luma(line) for line in self.luma.splitlines()
                             if line.startswith("lavfi.")]
        self.assertEqual((before, low, high), (10.4933, 24.8314, 122.34))
        self.assertTrue(visible_change((.30, low), (.70, high), (.09, before), (.09, before)))
        self.assertFalse(visible_change((.30, low), (.70, low + 1), (.09, before), (.09, before)))
        self.assertFalse(visible_change((.30, low), (.70, high), (.09, high), (.09, before)))
        self.assertTrue(visible_change((.30, low), (.70, high), (.70, high), (.70, high)))
        self.assertFalse(visible_change((.30, low), (.70, high), (.84, low), (.84, high)))
        with self.assertRaises(ValueError):
            parse_luma(self.luma)

    def test_core_and_panel_agree_at_user_target(self):
        panel = parse_panel(self.registry)
        self.assertTrue(brightness_at(.30, .25, .35, 2.293, panel))
        for slider, core, nits, reading in [
            (.249, .30, 2.293, panel),
            (.30, .351, 2.293, panel),
            (.30, .30, 3.293, panel),
            (.30, .30, 2.293, (178, 2293, False)),
        ]:
            self.assertFalse(brightness_at(.30, slider, core, nits, reading))


class WorkflowReceipts(unittest.TestCase):
    def test_complete_and_duplicate_receipts(self):
        receipt = 'GUI {"menu":true,"row":true,"rowCount":3}'
        self.assertTrue(run.parse_reply(f"ssh chatter\n{receipt}\n")["row"])
        for text in ("GUI {", f"{receipt}\n{receipt}", 'GUI []', ""):
            with self.assertRaises(ValueError):
                run.parse_reply(text)

    def test_wifi_uses_preclick_baseline_and_deadline(self):
        before = {"power": True, "associated": False, "target": False, "active": False, "lease": False, "t": 99}
        ready = {"power": True, "associated": True, "target": True, "active": True, "lease": True}
        self.assertTrue(wifi_transition(before, [(130, ready)], 100))
        for sample in [(130.001, ready), (99.9, ready), (102, dict(ready, target=False)),
                       (103, dict(ready, lease=False)), (103, dict(ready, associated=False))]:
            self.assertFalse(wifi_transition(before, [sample], 100))
        self.assertFalse(wifi_transition(dict(before, target=True), [(103, ready)], 100))
        self.assertFalse(wifi_transition(dict(before, t=102), [(103, ready)], 100))
        self.assertFalse(wifi_transition({}, [(103, ready)], 100))

    def test_off_requires_power_and_link_drop(self):
        off = {"power": False, "associated": False, "target": False, "active": False, "lease": False}
        self.assertTrue(wifi_off(off))
        self.assertFalse(wifi_off(dict(off, active=True)))
        self.assertFalse(wifi_off(dict(off, power=True)))

    def test_ssid_file_keeps_exact_bytes(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "fixture"
            path.write_bytes("ssid= Café's Wi-Fi \npass=offline-placeholder\n".encode())
            self.assertEqual(run.ssid_bytes(path), " Café's Wi-Fi ".encode())
            path.write_bytes(b"  test-fixture  \n")
            self.assertEqual(run.ssid_bytes(path), b"  test-fixture  ")
            for contents in (b"ssid=one\nssid=two\n", b"ssid=\n", b"x" * 33, b"ssid=\xff\n"):
                path.write_bytes(contents)
                with self.assertRaises(run.CheckError):
                    run.ssid_bytes(path)

    def test_target_staging_requires_receipt_and_sends_only_ssid_on_stdin(self):
        target = b"offline fixture"
        dest = "/test-staging/target"
        with (patch.dict(os.environ, GUI_TARGET=dest, GUI_SSID_FILE="/test-fixture"),
              patch.object(run, "ssid_bytes", return_value=target),
              patch.object(run, "bash", return_value=subprocess.CompletedProcess([], 0, b"no answer")) as helper):
            with self.assertRaisesRegex(run.CheckError, "could not stage"):
                run.stage_target()
        self.assertEqual(helper.call_args.kwargs["body"], target)
        self.assertNotIn(target.decode(), helper.call_args.args[1])

    def test_wifi_passes_only_scalar_evidence_to_redactor(self):
        receipt = {"row": True, "owner": "synthetic-private-text",
                   "before": {"target": False, "t": 12.0, "ssid": "synthetic-private-text"}}
        raw = b"synthetic-private-text\nGUI " + json.dumps(receipt).encode() + b"\n"
        with tempfile.TemporaryDirectory() as d:
            folder = Path(d)
            redactor = folder / "redact.py"
            redactor.write_text("import sys\nfor line in sys.stdin: print(line.replace('12.0', '0.0'), end='')\n")
            with (patch.dict(os.environ, GUI_RESULTS=d, GUI_REDACT=str(redactor)),
                  patch.object(run, "bash", return_value=subprocess.CompletedProcess([], 0, raw))):
                info = run.remote("wifi-click", artifact="wifi-click")
            self.assertEqual(info, {"row": True, "before": {"target": False, "t": 12.0}})
            self.assertEqual(json.loads((folder / "wifi-click.txt").read_text()),
                             {"row": True, "before": {"target": False, "t": 0.0}})

    def test_wifi_failure_does_not_echo_transport_error(self):
        raw = b'GUI {"error":"synthetic-private-text"}\n'
        with patch.object(run, "bash", return_value=subprocess.CompletedProcess([], 0, raw)):
            with self.assertRaisesRegex(run.CheckError, "^wifi-menu: action failed or receipt missing$"):
                run.remote("wifi-menu")

    def test_wifi_power_is_restored_on_hangup(self):
        driver = '''
import signal
import run

def remote(op, **kwargs):
    if op == "wifi-off":
        signal.raise_signal(signal.SIGHUP)
    if op == "wifi-on":
        print("restored Wi-Fi power", flush=True)
    return {}

run.wifi_check = run.wifi_toggle
run.remote = remote
run.wifi_state = lambda label: {"power": False}
run.wait_wifi = lambda *args: [(0, {"power": True})]
try:
    run.main("wifi")
except KeyboardInterrupt:
    raise SystemExit(129)
'''
        result = subprocess.run([sys.executable, '-c', driver], cwd=FIXTURES.parent,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.stdout, 'restored Wi-Fi power\n', result.stderr)
        self.assertEqual(result.returncode, 129)

    def test_hid_drag_keeps_button_held_during_move(self):
        geometry = {"x": 100, "y": 30, "width": 220, "height": 20, "slider": .30}
        history = []
        port = Mock()
        port.send.side_effect = lambda line: history.append(line)

        def move(_port, x, y, held=False):
            history.append((x, y, held))

        with (patch.object(run, "remote", side_effect=[geometry, dict(geometry, slider=.70)]) as probe,
              patch.object(run, "move_pointer", side_effect=move)):
            run.drag_on_port(port, .70, "fixture")
        self.assertEqual(history, [(170, 40, False), "P 1 0 0", "P 1 36 0",
                                   (250, 40, True), "P 0 0 0"])
        self.assertTrue(all(item.args == ("display-read",) for item in probe.call_args_list))

    def test_hid_port_releases_on_interrupted_drag(self):
        source = Mock()
        source.InputError = RuntimeError
        port = source.Port.return_value
        with (patch.object(run, "load_input", return_value=source),
              patch.object(run, "drag_on_port", side_effect=RuntimeError("HID refused"))):
            with self.assertRaisesRegex(run.CheckError, "HID refused"):
                run.drag(.70, "fixture")
        port.send.assert_called_once_with("R")
        port.close.assert_called_once()


class InstalledSpec(unittest.TestCase):
    """Mock the installed actions environment without loading the tablet runner."""

    harness = r"""
        set -euo pipefail
        BOOT=installed KIND=observe REC_C920=0 DRY=0
        R=$2/results GUI_BIN=$2/gui GUI_SSID_FILE=$2/ssid GUI_REDACT=$2/redact.py
        export MOCK_EVENTS=$2/events MOCK_CASE=$3
        mkdir "$R"
        if [[ "$MOCK_CASE" == dry ]]; then
            DRY=1
            unset GUI_BIN GUI_SSID_FILE GUI_REDACT
        fi
        [[ "$MOCK_CASE" != recording ]] || REC_C920=1
        check=
        if [[ "$MOCK_CASE" == energy-* ]]; then
            check=energy-picker
            unset GUI_SSID_FILE GUI_REDACT
        fi
        event() { printf '%s\n' "$1" >> "$MOCK_EVENTS"; }
        export -f event
        trap 'event parent-exit' EXIT
        steps() { event caller-steps; }
        verdict() { printf '%s\n' "$1" >> "$R/verdict.txt"; }
        fexpect() { event "fexpect:$1"; grep -Eq -- "$2" "$R/$1"; }
        shot() { event "shot:$1"; return 99; }
        rssh() {
            case "$1" in
                *kern.bootsessionuuid*) event identity
                    echo 'GUI_SESSION 00000000-0000-0000-0000-000000000001 501' ;;
                *GUI_STAGING_READY*) event staging
                    [[ "$MOCK_CASE" != staging-lost ]] && echo GUI_STAGING_READY || true ;;
                *GUI_STAGE_SHA256*)
                    case "$1" in *'/session.sh'*) event stage-session ;; *) event stage-binary ;; esac
                    if [[ "$MOCK_CASE" == bad-sha && "$1" == *'/session.sh'* ]]; then
                        cat >/dev/null
                        echo 'GUI_STAGE_SHA256 mismatched'
                    else
                        command python3 -c 'import sys, hashlib, base64; print("GUI_STAGE_SHA256", hashlib.sha256(base64.b64decode(sys.stdin.buffer.read())).hexdigest())'
                    fi ;;
                *GUI_STAGING_REMOVED*) event staging-cleanup; echo GUI_STAGING_REMOVED ;;
                *) event unexpected-rssh; return 99 ;;
            esac
        }
        rsudo() {
            case "$1" in
                *GUI_INSTALL_OK*) event root-install
                    [[ "$MOCK_CASE" != root-lost ]] && echo GUI_INSTALL_OK || true ;;
                *GUI_CLEANUP_OK*) event remote-cleanup
                    case "$MOCK_CASE" in
                        cleanup-lost) ;;
                        energy-recovery-lost) echo GUI_ENERGY_RECOVERY_UNCONFIRMED; return 2 ;;
                        *) echo GUI_CLEANUP_OK ;;
                    esac ;;
                *'session.sh ax-trust '*) event remote-ax-trust; echo 'GUI {"trusted":true}' ;;
                *) event unexpected-rsudo; return 99 ;;
            esac
        }
        python3() {
            [[ "$1" == */run.py ]] || return 99
            event "phase:$2"
            case "$2" in
                ax-trust) command python3 "$@" ;;
                wifi)
                    case "$MOCK_CASE" in
                        wifi-hangup) command python3 -c 'import os, signal; os.kill(os.getppid(), signal.SIGHUP)' ;;
                        wifi-terminated) command python3 -c 'import os, signal; os.kill(os.getppid(), signal.SIGTERM)' ;;
                    esac
                    [[ "$MOCK_CASE" != wifi-failure ]] ;;
                brightness-ax|brightness-hid) ;;
                energy-picker) [[ "$MOCK_CASE" != energy-void ]] || return 2 ;;
                *) return 99 ;;
            esac
        }
        source "$1"
        rc=0
        gui_checks_installed "$check" || rc=$?
        steps
        exit "$rc"
    """
    staging = ["identity", "staging", "stage-binary", "stage-session"]
    phases = ["root-install", "phase:ax-trust", "remote-ax-trust", "fexpect:gui-checks/ax-trust.txt",
              "phase:wifi", "phase:brightness-ax", "phase:brightness-hid"]
    cleanup = ["remote-cleanup", "staging-cleanup"]

    def invoke(self, scenario, status, events):
        with tempfile.TemporaryDirectory() as d:
            folder = Path(d)
            binary = folder / "gui"
            binary.write_bytes(b"offline-probe")
            binary.chmod(0o700)
            (folder / "ssid").write_text("offline-network\n")
            (folder / "redact.py").touch()
            result = subprocess.run(
                ["/bin/bash", "-c", self.harness, "fixture", FIXTURES.parent / "gui-checks.spec", d, scenario],
                text=True, capture_output=True, timeout=15,
            )
            self.assertEqual(result.returncode, status, result.stdout + result.stderr)
            self.assertEqual((folder / "events").read_text().splitlines(), [*events, "caller-steps", "parent-exit"])
            return (folder / "results" / "verdict.txt").read_text()

    def test_dry_records_info_without_accessing_tablet_or_fixtures(self):
        self.assertEqual(self.invoke("dry", 0, []), "INFO gui-checks dry run\n")

    def test_installed_actions_harness(self):
        verdicts = self.invoke("normal", 0, self.staging + self.phases + self.cleanup)
        self.assertIn("PASS gui-checks console-user Accessibility verified", verdicts)
        self.assertIn("INFO gui-checks staging cleanup confirmed", verdicts)

    def test_sha_mismatch_refuses_root_install(self):
        verdicts = self.invoke("bad-sha", 2, self.staging + ["staging-cleanup"])
        self.assertIn("staged session.sh SHA-256 differs", verdicts)

    def test_lost_staging_receipt_still_cleans_candidate(self):
        self.invoke("staging-lost", 2, ["identity", "staging", "staging-cleanup"])

    def test_lost_install_receipt_still_cleans_both_locations(self):
        self.invoke("root-lost", 2, self.staging + ["root-install"] + self.cleanup)

    def test_wifi_failure_still_runs_both_brightness_phases_and_cleanup(self):
        self.invoke("wifi-failure", 1, self.staging + self.phases + self.cleanup)

    def test_wifi_interruption_cleans_the_temporary_session(self):
        for scenario, status in (("wifi-hangup", 129), ("wifi-terminated", 143)):
            with self.subTest(scenario=scenario):
                self.invoke(scenario, status, self.staging + self.phases[:-2] + self.cleanup)

    def test_failed_remote_cleanup_still_attempts_staging_cleanup(self):
        verdicts = self.invoke("cleanup-lost", 2, self.staging + self.phases + self.cleanup)
        self.assertIn("FAIL HARNESS gui-checks remote cleanup unconfirmed", verdicts)

    def test_continuous_recording_refused_before_remote_access(self):
        self.assertIn("requires camera stills", self.invoke("recording", 2, []))

    def test_energy_opt_in_has_no_wifi_fixtures_or_other_phases(self):
        events = self.staging + self.phases[:-3] + ["phase:energy-picker"] + self.cleanup
        for scenario, status in (("energy-normal", 0), ("energy-void", 2), ("energy-recovery-lost", 1)):
            with self.subTest(scenario=scenario):
                self.invoke(scenario, status, events)

    def test_fragment_has_no_tablet_lock_access(self):
        lock_path = "/tmp/piano-tablet." + "lock"
        tools = FIXTURES.parent
        self.assertFalse((tools / ("run-" + "installed.sh")).exists())
        for name in ("build.sh", "gui-checks.spec", "session.sh", "run.py", "grade.py"):
            with self.subTest(name=name):
                self.assertNotIn(lock_path, (tools / name).read_text())


@unittest.skipUnless(platform.system() == "Darwin" and platform.machine() == "arm64", "arm64 macOS SDK required")
class NativeParser(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = tempfile.TemporaryDirectory()
        cls.addClassCleanup(scratch.cleanup)
        cls.binary = Path(scratch.name) / "gui"
        subprocess.run(["/bin/bash", FIXTURES.parent / "build.sh", cls.binary], check=True)

    def test_energy_finish_receipt(self):
        token = "0" * 32
        result = subprocess.run([self.binary, "energy-finish", token, "true", "false"],
                                text=True, capture_output=True, check=True)
        receipt = run.parse_reply(result.stdout)
        self.assertEqual(receipt["token"], token)
        self.assertEqual(receipt["op"], "energy-finish")
        self.assertIs(receipt["closed"], True)
        self.assertIs(receipt["restored"], False)
        self.assertGreater(receipt["t_us"], 0)

    def test_ipconfig_lease_fixture(self):
        packet = (FIXTURES / "dhcp-ack.txt").read_text()
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "packet"
            for address, output, expected in [
                ("192.0.2.10", packet, True),
                ("192.0.2.10", packet.replace("ACK 0x5", "0x5"), True),
                ("192.0.2.11", packet, False),
                ("192.0.2.10", packet.replace("ACK 0x5", "NAK 0x6"), False),
                ("192.0.2.10", packet.replace("0x15180", "0x0"), False),
                ("192.0.2.10", packet.replace("0x15180", ""), False),
                ("169.254.0.10", packet.replace("192.0.2.10", "169.254.0.10"), False),
            ]:
                with self.subTest(address=address, output=output):
                    path.write_text(output)
                    result = subprocess.run([self.binary, "fixture-lease", address, path],
                                            text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0 if expected else 2)
                    self.assertIs(json.loads(result.stdout.removeprefix("GUI "))["lease"], expected)


if __name__ == "__main__":
    unittest.main()
