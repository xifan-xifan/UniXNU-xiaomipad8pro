"""Offline transport, grader and recovery tests. No commands reach an installed device."""
import copy
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import energy_picker
import grade_energy_picker as grade
import run


class MockTransport:
    def __init__(self, source="ac", ac=2, battery=1, fault=None, profile_rule=2):
        self.source = source
        self.original = {"ac": ac, "battery": battery}
        self.prefs = self.original.copy()
        self.fault = fault
        self.profile_rule = profile_rule
        self.t = 300_000_000
        self.host_time = 0
        self.calls = []
        self.attempts = 0
        self.settings = False
        self.finishing = False
        self.journal = None

    def monotonic(self):
        return self.host_time

    def sleep(self, _seconds):
        self.host_time += 1
        self.t += 1_000_000

    def state(self):
        mode = self.prefs[self.source]
        low, high = int(mode == 1), int(mode == 2)
        profile = (2 if self.profile_rule == 2 else 0, 1, 3)[mode]
        written, cap, ext, bind = (342, 342, 3, "lpm") if low else (0, 832, 0, "skinband")
        power = "AC Power" if self.source == "ac" else "Battery Power"
        source = f"Now drawing from '{power}'"
        sample = {
            "started_us": self.t - 10_000, "source": source, "source_after": source, "profile_rule": str(self.profile_rule),
            "pmset": f"System-wide power settings:\nCurrently in use:\n powermode {mode}\n",
            "custom": f"Battery Power:\n powermode {self.prefs['battery']}\nAC Power:\n powermode {self.prefs['ac']}\n",
            "lpm": f"com.apple.system.lowpowermode {low}\n", "hpm": f"com.apple.system.highpowermode {high}\n",
            "policy_lpm": f"{low}\n", "policy_hpm": f"{high}\n",
            "cpu": f"perf=2 profile={profile} above_refused=0/0 tick_ms=20 uptime_ms={self.t // 1000 - 5}",
            "gpu": f"at_us={self.t - 100_000} lvl=2 mhz=222 cap_mhz={cap} why=demand bind={bind} ext={ext}",
            "policy": f"PianoPolicy 2 (acting); GPU ceiling {written} MHz (written {written}, client 1); sent: ...\n",
        }
        if self.attempts and not self.finishing:
            replacements = {
                "wrong-notify": ("lpm", f"com.apple.system.lowpowermode {1 - low}\n"),
                "wrong-policy": ("policy_hpm", f"{1 - high}\n"),
                "wrong-thermal": ("cpu", sample["cpu"].replace(f"profile={profile}", "profile=0")),
                "wrong-gpu": ("gpu", sample["gpu"].replace(f"ext={ext}", "ext=0")),
                "wrong-pmset": ("pmset", "Currently in use:\n powermode 2\n"),
                "stale-gpu": ("gpu", sample["gpu"].replace(str(self.t - 100_000), "299900000")),
                "source-flapped": ("source_after", "Now drawing from 'Battery Power'"),
            }
            if self.fault in replacements:
                key, value = replacements[self.fault]
                sample[key] = value
            if self.fault == "delayed" and self.t < self.pressed + 4_000_000:
                sample["lpm"] = f"com.apple.system.lowpowermode {1 - low}\n"
        return sample

    def bash(self, command, op, token, *args, timeout):
        self.calls.append((op, *args))
        self.t += 200_000
        r = {"op": op, "token": token, "t_us": self.t}
        rc = 0
        if op == "energy-state":
            if self.fault == "empty" or self.fault == "empty-after-selection" and self.attempts and not self.finishing:
                return subprocess.CompletedProcess([], 0, b"")
            r.update(self.state())
            if self.fault == "stale-token":
                r["token"] = "0" * 32
        elif op == "energy-begin":
            assert args == (self.original["ac"], self.original["battery"])
            self.journal = self.prefs.copy()
            r["armed"] = True
            if self.fault == "lost-arm":
                raise subprocess.TimeoutExpired(command, timeout)
        elif op == "energy-open":
            self.settings = True
            r.update(source=self.source, mode=self.prefs[self.source])
            if self.fault == "missing-picker":
                r.update(error="energy-picker-missing-or-ambiguous", ax=[{"depth": 0, "role": "AXWindow", "labels": ["Battery"]}])
                rc = 2
        elif op == "energy-select":
            assert self.settings and self.journal and not self.finishing
            source, before, target = args
            assert source == self.source and before == self.prefs[source] and before != target
            self.attempts += 1
            self.pressed = self.t - 50_000
            self.prefs[source] = target
            if self.fault == "other-pref-changed":
                self.prefs["battery"] = (self.prefs["battery"] + 1) % 3
            if self.fault == "lost-action":
                raise subprocess.TimeoutExpired(command, timeout)
            if self.fault == "signal":
                signal.raise_signal(signal.SIGTERM)
            if self.fault == "hangup":
                signal.raise_signal(signal.SIGHUP)
            r.update(source=source, before=before, target=target, pressed_us=self.pressed, status=0, mode=target)
            if self.fault in ("press-refused", "restore-failed"):
                r["status"] = -25206
        elif op == "energy-finish":
            self.finishing = True
            if self.fault != "close-failed":
                self.settings = False
            if self.fault != "restore-failed" and self.journal is not None:
                self.prefs = self.journal.copy()
            r.update(closed=not self.settings, restored=self.prefs == self.original and self.fault != "restore-failed")
            rc = 0 if r["closed"] and r["restored"] else 2
        else:
            raise AssertionError(op)
        return subprocess.CompletedProcess([], rc, f"ignored chatter\nGUI {json.dumps(r)}\n".encode())


class PickerWorkflow(unittest.TestCase):
    def invoke(self, status, **kwargs):
        fixture = MockTransport(**kwargs)
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            path = folder / "energy-picker.json"
            path.write_text('{"previous":"capture"}')
            old_term = signal.getsignal(signal.SIGTERM)
            try:
                with (patch.dict(os.environ, GUI_RESULTS=temp), patch.object(run, "bash", fixture.bash),
                      patch.object(run, "verdict") as verdict, patch.object(energy_picker, "time", fixture)):
                    result = run.main("energy-picker")
            finally:
                signal.signal(signal.SIGTERM, old_term)
            capture = json.loads(path.read_text())
        actual, reason = grade.grade_capture(capture)
        self.assertEqual(actual, status, reason)
        self.assertEqual(result, grade.EXIT_CODES[status])
        verdict.assert_called_once_with(status, f"Energy Mode picker: {reason} (gui-checks/energy-picker.json)")
        return fixture, capture, reason

    def test_all_transitions_starting_from_every_mode_and_both_power_sources(self):
        for source in ("ac", "battery"):
            for initial, expected in ((0, (2, 1, 0)), (1, (0, 2, 1)), (2, (1, 0, 2))):
                with self.subTest(source=source, initial=initial):
                    prefs = {"ac": (initial + 1) % 3, "battery": (initial + 1) % 3, source: initial}
                    fixture, _, _ = self.invoke("PASS", source=source, **prefs)
                    self.assertEqual([args[-1] for args in fixture.calls if args[0] == "energy-select"], list(expected))
                    self.assertEqual(fixture.calls[-2:], [("energy-finish",), ("energy-state",)])
                    self.assertEqual(fixture.prefs, prefs)
                    self.assertFalse(fixture.settings)

    def test_missing_picker_is_void_and_keeps_ax_tree(self):
        fixture, capture, reason = self.invoke("VOID", fault="missing-picker")
        self.assertIn("energy-picker-missing-or-ambiguous", reason)
        self.assertEqual(capture["open"]["receipt"]["ax"][0]["role"], "AXWindow")
        self.assertEqual(fixture.attempts, 0)
        self.assertFalse(fixture.settings)

    def test_delayed_propagation_with_the_default_thermal_rule(self):
        fixture, _, _ = self.invoke("PASS", source="battery", fault="delayed", profile_rule=1)
        self.assertGreater(fixture.host_time, 4)

    def test_propagation_mismatch_checks_every_layer(self):
        for fault, layer in (("wrong-notify", "powerd"), ("wrong-policy", "PianoPolicy"),
                             ("wrong-thermal", "PianoThermal"), ("wrong-gpu", "PianoA830"),
                             ("wrong-pmset", "pmset"), ("other-pref-changed", "per-source")):
            with self.subTest(fault=fault):
                fixture, _, reason = self.invoke("FAIL", fault=fault)
                self.assertIn(layer, reason)
                self.assertEqual(fixture.attempts, 1)
                self.assertEqual(fixture.prefs, fixture.original)

    def test_action_error_signal_and_timeout_all_restore(self):
        for fault, status in (("lost-arm", "VOID"), ("lost-action", "VOID"), ("signal", "VOID"),
                              ("hangup", "VOID"), ("press-refused", "FAIL")):
            with self.subTest(fault=fault):
                fixture, _, _ = self.invoke(status, fault=fault)
                self.assertEqual(fixture.prefs, fixture.original)
                self.assertFalse(fixture.settings)
                self.assertIn(("energy-finish",), fixture.calls)

    def test_clean_quit_and_restoration_have_independent_grades(self):
        for fault, message in (("restore-failed", "were not restored"), ("close-failed", "did not quit")):
            with self.subTest(fault=fault):
                fixture, _, reason = self.invoke("FAIL", fault=fault)
                self.assertIn(message, reason)
                self.assertTrue(fixture.finishing)

    def test_empty_and_replayed_transport_refuse_to_arm(self):
        for fault in ("empty", "stale-token"):
            with self.subTest(fault=fault):
                fixture, capture, _ = self.invoke("VOID", fault=fault)
                self.assertEqual(fixture.calls, [("energy-state",)])
                self.assertNotIn("previous", capture)

    def test_stale_missing_and_cross_source_samples_are_void(self):
        for fault in ("stale-gpu", "empty-after-selection", "source-flapped"):
            with self.subTest(fault=fault):
                fixture, _, _ = self.invoke("VOID", fault=fault)
                self.assertEqual(fixture.prefs, fixture.original)
                self.assertFalse(fixture.settings)

    def test_offline_grade_rejects_replay_and_unverified_restoration(self):
        _, capture, _ = self.invoke("PASS")
        for key in ("open", "finish", "final"):
            with self.subTest(key=key):
                altered = copy.deepcopy(capture)
                altered[key] = altered["initial"]
                self.assertEqual(grade.grade_capture(altered)[0], "VOID")
        stale = copy.deepcopy(capture)
        stale["steps"][0]["state"] = stale["initial"]
        self.assertEqual(grade.grade_capture(stale)[0], "VOID")
        malformed = copy.deepcopy(capture)
        malformed["initial"]["receipt"]["pmset"] += " powermode unknown\n"
        self.assertEqual(grade.grade_capture(malformed)[0], "VOID")
        bad = copy.deepcopy(capture)
        bad["final"]["receipt"]["custom"] = "Battery Power:\n powermode 0\nAC Power:\n powermode 2\n"
        self.assertEqual(grade.grade_capture(bad)[0], "FAIL")
        self.assertEqual(grade.grade_capture(None)[0], "VOID")

    def test_grader_cli_accepts_known_positive_and_rejects_empty_file(self):
        _, capture, _ = self.invoke("PASS")
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "capture.json"
            for text, expected, status in ((json.dumps(capture), 0, "PASS"), ("", 2, "VOID")):
                path.write_text(text)
                result = subprocess.run([sys.executable, TOOLS / "grade.py", "energy-picker", path],
                                        capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertTrue(result.stdout.startswith(status))


class SessionRecovery(unittest.TestCase):
    def test_real_restore_functions_retry_both_sources_and_preserve_the_journal(self):
        session = (TOOLS / "session.sh").read_text()
        functions = session[session.index("energy_preferences() {"):session.index("case $op in")]
        # Run only the recovery functions. Even the absolute pmset command is a Bash fixture.
        harness = r'''
            set -euo pipefail
            dir=$1
            asuser() { [[ $1 == energy-quit ]]; echo quit >> "$dir/events"; }
            function /usr/bin/pmset {
                case "$1" in
                    -g) [[ $2 == custom ]]; printf 'Battery Power:\n powermode %s\nAC Power:\n powermode %s\n' \
                            "$(cat "$dir/battery")" "$(cat "$dir/ac")" ;;
                    -c|-b)
                        [[ $2 == powermode ]]
                        echo "$1 $3" >> "$dir/events"
                        if [[ $1 == -c ]]; then
                            [[ ! -e "$dir/refuse-ac" ]] || return 1
                            echo "$3" > "$dir/ac"
                        else
                            echo "$3" > "$dir/battery"
                        fi ;;
                    *) return 99 ;;
                esac
            }
            eval "$2"
            if energy_finish; then exit 98; fi
            [[ $closed == true && $restored == false && -f "$dir/energy-original" ]]
            rm "$dir/refuse-ac"
            energy_finish
            [[ $closed == true && $restored == true && -e "$dir/energy-finished" ]]
        '''
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            for name, contents in {"energy-original": "2 1\n", "energy-opened": "", "refuse-ac": "",
                                   "ac": "1\n", "battery": "0\n"}.items():
                (folder / name).write_text(contents)
            result = subprocess.run(["/bin/bash", "-c", harness, "fixture", temp, functions],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((folder / "events").read_text().splitlines(), ["quit", "-c 2", "-b 1", "-c 2"])
            self.assertEqual((folder / "ac").read_text(), "2\n")
            self.assertEqual((folder / "battery").read_text(), "1\n")
