# SPDX-License-Identifier: BSD-2-Clause-Patent
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import action
from grade import grade_phase
from test_frames import GEOMETRY, capture_events, make_bmp, make_png, scene
from test_grade import good_evidence, logs


class ActionTests(unittest.TestCase):
    def setUp(self):
        self.probe = MagicMock(spec=action.LocalProbe)
        self.addCleanup(patch.stopall)
        patch.object(action, "stop_requested", False).start()

    @patch.object(action, "version", return_value="27.3")
    def test_locked_session_never_launches(self, _version):
        self.probe.call.side_effect = action.Invalid("unlocked")
        result = action.collect(self.probe)
        self.assertEqual(result["preflight"], "void")
        self.assertEqual(result["stage"], "unlocked")
        self.probe.call.assert_called_once_with("unlocked")

    @patch.object(action, "camera_events", return_value=[])
    @patch.object(action, "crash_reports", return_value=set())
    @patch.object(action.time, "sleep")
    def test_quicktime_phase_cleans_partial_movie_after_failure(self, _sleep, _crashes, _events):
        with tempfile.TemporaryDirectory() as work:
            directory = Path(work) / "run-test"
            directory.mkdir()
            (directory / "recording.mov").write_text("partial")

            def fail(*_args):
                raise action.Invalid("qt-stop", "failed")

            with patch.object(action, "movie_dir", return_value=directory), patch.object(action, "quick_time", side_effect=fail):
                result = action.phase(self.probe, "qt")
            self.assertFalse(directory.exists())
            self.assertEqual(result["outcome"], "failed")
            self.assertTrue(result["cleanup"])
            self.probe.call.assert_any_call("quit-qt", cleanup=True)

    @patch.object(action, "camera_events", return_value=[])
    @patch.object(action, "crash_reports", return_value=set())
    @patch.object(action.time, "sleep")
    def test_refused_launch_does_not_quit_preexisting_app(self, _sleep, _crashes, _events):
        def call(op, **_):
            if op == "launch-ft":
                raise action.Invalid(op, returncode=2)

        self.probe.call.side_effect = call
        result = action.phase(self.probe, "ft")
        self.assertEqual(result["outcome"], "void")
        self.assertNotIn("quit-ft", [entry.args[0] for entry in self.probe.call.call_args_list])

    @patch.dict(action.os.environ, {"CAM_APPS_GOP_SHOT": "/usr/local/bin/gop-shot"})
    @patch.object(action, "command")
    @patch.object(action, "camera_events")
    @patch.object(action, "crash_reports", return_value=set())
    @patch.object(action.time, "sleep")
    def test_face_time_capture_brackets_two_in_memory_frames(self, sleep, _crashes, read_log, command):
        events = capture_events()
        read_log.side_effect = [events[:3], events[3:4], events]
        self.probe.read.return_value = GEOMETRY
        command.side_effect = [subprocess.CompletedProcess([], 0, image)
                               for image in (make_png(scene()), make_bmp(scene(shift=1)))]

        result = action.phase(self.probe, "ft")

        self.assertEqual(grade_phase("facetime", result)[0], "PASS")
        self.assertEqual(result["gop_shot"]["before"], events[2])
        self.assertEqual(result["gop_shot"]["after"], events[3])
        self.assertEqual(len(result["gop_shot"]["frames"]), 2)
        for sample in result["gop_shot"]["frames"]:
            self.assertEqual(set(sample), {"mean_luma", "nonblack_fraction", "luma_variance"})
        for call in command.call_args_list:
            self.assertEqual(call.args[0], ["/usr/bin/sudo", "-n", "--", "/usr/local/bin/gop-shot", "--stdout"])
            self.assertEqual(call.kwargs, {"timeout": 8, "binary": True})
        self.assertEqual(command.call_count, 2)
        sleep.assert_any_call(1.5)
        self.assertEqual(read_log.call_args_list[0].args[0], read_log.call_args_list[2].args[0])
        self.assertGreaterEqual(read_log.call_args_list[1].args[0], read_log.call_args_list[0].args[1])
        self.assertEqual([call.args[0] for call in self.probe.mock_calls],
                         ["prerequisites", "idle", "launch-ft", "window-ft", "window-ft", "geometry-ft", "geometry-ft",
                          "geometry-ft", "geometry-ft", "window-ft", "quit-ft"])
        self.probe.call.assert_called_with("quit-ft", cleanup=True)

    @patch.dict(action.os.environ, {"CAM_APPS_GOP_SHOT": "/usr/local/bin/gop-shot"})
    @patch.object(action, "command")
    @patch.object(action, "camera_events")
    @patch.object(action, "crash_reports", return_value=set())
    @patch.object(action.time, "sleep")
    def test_unavailable_capture_is_void_even_with_legacy_luma(self, _sleep, _crashes, read_log, command):
        events = logs()
        for geometry, status, stdout in (([action.Invalid("geometry-ft")], 0, b"unused"),
                                         ([GEOMETRY], 1, b""),
                                         ([GEOMETRY], 0, b""),
                                         ([GEOMETRY, dict(GEOMETRY, window=[0, 0, 10, 10])], 0, b"partial")):
            with self.subTest(geometry=geometry, status=status, stdout=stdout):
                read_log.side_effect = [events[:3], events]
                self.probe.read.side_effect = geometry
                command.return_value = subprocess.CompletedProcess([], status, stdout)
                result = action.phase(self.probe, "ft")
                self.assertEqual(grade_phase("facetime", result)[0], "VOID")
                self.assertTrue(result["cleanup"])
                self.probe.call.assert_called_with("quit-ft", cleanup=True)

    @patch.dict(action.os.environ, {"CAM_APPS_GOP_SHOT": ""})
    @patch.object(action, "camera_events")
    @patch.object(action, "crash_reports", return_value=set())
    @patch.object(action.time, "sleep")
    def test_capture_disabled_keeps_legacy_face_time_path(self, _sleep, _crashes, read_log):
        events = logs()
        read_log.side_effect = [events[:3], events]
        result = action.phase(self.probe, "ft")
        self.assertNotIn("gop_shot", result)
        self.probe.read.assert_not_called()
        self.assertEqual(grade_phase("facetime", result)[0], "PASS")

    @patch.object(action, "camera_events", return_value=[])
    @patch.object(action, "crash_reports", return_value=set())
    @patch.object(action.time, "sleep")
    def test_interruption_during_recording_still_quits_and_removes_movie(self, _sleep, _crashes, _events):
        with tempfile.TemporaryDirectory() as work:
            directory = Path(work) / "run-test"
            directory.mkdir()

            def interrupt(_probe, app, _receipt, _since, path):
                app.discard_recording = True
                path.write_text("partial")
                action.interrupted(None, None)

            with patch.object(action, "movie_dir", return_value=directory), patch.object(action, "quick_time", side_effect=interrupt):
                result = action.phase(self.probe, "qt")
            self.assertEqual(result["stage"], "interrupted")
            self.assertTrue(result["cleanup"])
            self.assertFalse(directory.exists())
            self.probe.call.assert_any_call("quit-qt-discard", cleanup=True)

    @patch.object(action, "camera_events", return_value=[])
    @patch.object(action, "crash_reports", return_value=set())
    @patch.object(action.time, "sleep")
    def test_confirmed_quit_required_even_when_movie_removed(self, _sleep, _crashes, _events):
        with tempfile.TemporaryDirectory() as work:
            directory = Path(work) / "run-test"
            directory.mkdir()

            def call(op, **_):
                if op == "quit-qt":
                    raise action.Invalid(op)

            self.probe.call.side_effect = call
            with patch.object(action, "movie_dir", return_value=directory), patch.object(action, "quick_time"):
                result = action.phase(self.probe, "qt")
            self.assertFalse(result["cleanup"])
            self.assertFalse(directory.exists())

    @patch.object(action.time, "sleep")
    def test_quicktime_operation_order(self, _sleep):
        evidence = good_evidence()["quicktime"]
        self.probe.read.side_effect = [evidence["movie"], {"time": 0.1, "playing": True}, {"time": 3.2, "playing": True}]
        app = action.App(self.probe, "qt")
        app.launch()
        receipt = {}
        with patch.object(action, "camera_events", return_value=evidence["preview"]):
            action.quick_time(self.probe, app, receipt, 0, Path("recording.mov"))
        self.assertEqual(receipt["playback"]["last"], 3.2)
        self.assertEqual(receipt["preview"], evidence["preview"])
        self.assertEqual(
            [entry.args[0] for entry in self.probe.mock_calls],
            ["launch-qt", "qt-new", "window-qt", "window-qt", "qt-start", 5, "qt-stop", "qt-finalized",
             "qt-save", "quit-qt-discard", "inspect", "launch-qt", "qt-open", "window-qt", "qt-play",
             "qt-time", 3, "qt-time"],
        )

    def test_private_staging_rejects_symlink_or_shared_directory(self):
        with tempfile.TemporaryDirectory() as work:
            target = Path(work) / "target"
            target.mkdir(mode=0o755)
            link = Path(work) / "link"
            link.symlink_to(target)
            for path in (link, target):
                with self.subTest(path=path), patch.object(action, "MOVIES", path):
                    with self.assertRaises(action.Invalid):
                        action.movie_dir()
            with patch.object(action, "MOVIES", Path(work) / "new"):
                directory = action.movie_dir()
                self.assertEqual(directory.stat().st_mode & 0o777, 0o700)

    def test_only_matching_crash_reports_are_counted(self):
        with tempfile.TemporaryDirectory() as work:
            path = Path(work)
            retired = path / "Retired"
            retired.mkdir()
            with patch.object(action, "CRASH_DIRS", (path,)):
                (path / "QuickTime Player-old.ips").touch()
                start = action.crash_reports()
                (path / "OtherApp-new.ips").touch()
                (retired / "com.unixnu.PianoCamera.Extension-new.ips").touch()
                (path / "FaceTime-symlink.ips").symlink_to(path / "OtherApp-new.ips")
                self.assertEqual(len(action.crash_reports() - start), 1)

    def test_sourceable_spec_dry_run_and_external_camera_gate(self):
        body = '''
            verdict() { printf "%s\n" "$1"; }
            source "$1"
            DRY=1 cam_apps_check_installed
            BOOT=installed KIND=observe R="$2" REC_C920=1 cam_apps_check_installed
        '''
        with tempfile.TemporaryDirectory() as work:
            result = subprocess.run(["/bin/bash", "-c", body, "spec",
                                     TOOLS / "cam-apps-check.spec", work], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("INFO cam-apps-check dry run", result.stdout)
            self.assertIn("VOID cam-apps-check", result.stdout)
            self.assertEqual(list(Path(work).iterdir()), [])
