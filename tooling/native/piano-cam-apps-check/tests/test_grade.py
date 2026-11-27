# SPDX-License-Identifier: BSD-2-Clause-Patent
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from grade import grade, reduce_log


def rows(sensor=2, *, luma=True, pid=320):
    messages = [
        f"camera stream start sensor={sensor} index=1 duration=1/30 source_fps=15",
        f"camera output sensor={sensor} index=1 sent=30 sources=18 repeated=12 dropped=0 stale_ticks=0",
        f"camera output sensor={sensor} index=1 sent=60 sources=35 repeated=25 dropped=0 stale_ticks=0",
        f"camera output stop sensor={sensor} sent=150 sources=81 repeated=69 dropped=0 stale_ticks=0",
        f"camera stream stop sensor={sensor} result=0 state=3 clean=1 guard=0 smmu=0 capture_dropped=0",
    ]
    if luma:
        for index, count in ((1, 16), (2, 39)):
            messages[index] += f" nonblack={count}"
    return [
        {"subsystem": "com.unixnu.PianoCamera", "category": "extension", "processID": pid,
         "timestamp": f"1970-01-01 00:00:0{index}.123456789+0000", "eventMessage": message}
        for index, message in enumerate(messages)
    ]


def logs(*args, **kwargs):
    return reduce_log(rows(*args, **kwargs), 0, 10)


def good_evidence():
    events = logs(1, luma=False)
    return {
        "schema": 1, "macos": "27.3.1", "preflight": "ready",
        "facetime": {"cleanup": True, "crashes": 0, "outcome": "observed", "window": True, "events": logs()},
        "quicktime": {"cleanup": True, "crashes": 0, "outcome": "observed", "window": True,
                      "events": events, "preview": copy.deepcopy(events[:3]),
                      "movie": {"valid": True, "bytes": 50000, "duration": 5.03, "video_span": 4.96, "decoded": 75,
                                "nonblack": 73, "frame_hashes": ["1" * 64, "2" * 64, "3" * 64],
                                "container": "quicktime", "codec": "avc1"},
                      "playback": {"first": 0.1, "last": 3.2, "elapsed": 3.15, "playing": True}},
    }


class GradeTests(unittest.TestCase):
    def assertGrade(self, evidence, ft, qt):
        result = grade(evidence)
        self.assertEqual({key: value[0] for key, value in result.items()}, {"facetime": ft, "quicktime": qt})

    def test_known_positive(self):
        self.assertGrade(good_evidence(), "PASS", "PASS")

    def test_only_inside_window_with_expected_subsystem_category(self):
        good = rows()
        foreign = [
            dict(good[0], subsystem="com.example.fixture", eventMessage="unrelated private text"),
            dict(good[0], category="other"),
            dict(good[0], timestamp="invalid"),
            dict(good[0], timestamp="1969-12-31 23:59:59.950000+0000"),
            dict(good[0], timestamp="1970-01-01 00:00:11+0000"),
            dict(good[0], processID=True),
            dict(good[0], eventMessage="client unrelated private text connected"),
        ]
        self.assertEqual(reduce_log(foreign + good[::-1], 0, 10), logs())
        self.assertNotIn("private text", str(reduce_log(foreign + good, 0, 10)))

    def test_facetime_requires_front_sensor_and_nonblack_frames(self):
        evidence = good_evidence()
        for events, expected in ((logs(1), "FAIL"), (logs(0), "FAIL"), (logs(luma=False), "VOID")):
            with self.subTest(expected=expected):
                evidence["facetime"]["events"] = events
                self.assertGrade(evidence, expected, "PASS")
        events = logs()
        events[1]["nonblack"] = events[2]["nonblack"] = 0
        evidence["facetime"]["events"] = events
        self.assertGrade(evidence, "FAIL", "PASS")

    def test_missing_or_contradictory_evidence_never_passes(self):
        for events in ([], logs()[:-1], logs()[:1] + logs()[2:], logs() + logs(pid=321), [None]):
            with self.subTest(events=events):
                evidence = good_evidence()
                evidence["facetime"]["events"] = events
                self.assertGrade(evidence, "VOID", "PASS")

    def test_stream_faults_and_stalled_source_fail(self):
        evidence = good_evidence()
        events = evidence["facetime"]["events"]
        events[2]["sources"] = events[1]["sources"]
        self.assertGrade(evidence, "FAIL", "PASS")
        evidence["facetime"]["events"] = logs()
        evidence["facetime"]["events"][-1]["clean"] = False
        self.assertGrade(evidence, "FAIL", "PASS")
        raw_fault = dict(rows()[0], eventMessage="camera stream start failed sensor=2 step=abi2-start result=0xe00002d5")
        evidence["facetime"]["events"] = reduce_log([raw_fault], 0, 1)
        self.assertGrade(evidence, "FAIL", "PASS")

    def test_pre_recording_preview_must_advance_and_match_recorded_session(self):
        evidence = good_evidence()
        evidence["quicktime"]["preview"] = logs(2, luma=False)[:3]
        self.assertGrade(evidence, "PASS", "VOID")
        evidence["quicktime"]["preview"] = logs(1, luma=False)[:2]
        self.assertGrade(evidence, "PASS", "VOID")

    def test_bad_video_stats(self):
        for override, expected in (
            ({"valid": False}, "FAIL"),
            ({"bytes": 0}, "FAIL"),
            ({"codec": "hvc1"}, "FAIL"),
            ({"duration": 3.5}, "FAIL"),
            ({"video_span": 1}, "FAIL"),
            ({"nonblack": 0, "frame_hashes": []}, "FAIL"),
            ({"container": "mp4"}, "FAIL"),
            ({"frame_hashes": ["1" * 64]}, "VOID"),
            ({"frame_hashes": ["account text"]}, "VOID"),
            ({"duration": float("nan")}, "VOID"),
            ({"decoded": True}, "VOID"),
        ):
            with self.subTest(override=override):
                evidence = good_evidence()
                evidence["quicktime"]["movie"].update(override)
                self.assertGrade(evidence, "PASS", expected)

    def test_playback_needs_positive_clock_advance_and_playing_state(self):
        for patch, expected in (
            ({"last": 1.6}, "FAIL"),
            ({"last": 0.1}, "FAIL"),
            ({"last": 7}, "FAIL"),
            ({"elapsed": 1}, "FAIL"),
            ({"playing": False}, "FAIL"),
            ({"last": None}, "VOID"),
            ({"last": float("inf")}, "VOID"),
        ):
            with self.subTest(patch=patch):
                evidence = good_evidence()
                evidence["quicktime"]["playback"].update(patch)
                self.assertGrade(evidence, "PASS", expected)

    def test_setup_cleanup_and_crashes(self):
        for patch, expected in (
            ({"macos": "27.2.1"}, "VOID"),
            ({"schema": True}, "VOID"),
            ({"preflight": "void"}, "VOID"),
        ):
            evidence = good_evidence()
            evidence.update(patch)
            self.assertGrade(evidence, expected, expected)
        for patch, expected in (
            ({"crashes": 1}, "FAIL"),
            ({"crashes": None}, "VOID"),
            ({"cleanup": False}, "VOID"),
            ({"outcome": "failed"}, "FAIL"),
            ({"outcome": "void"}, "VOID"),
            ({"window": False}, "FAIL"),
            ({"window": None}, "VOID"),
        ):
            with self.subTest(patch=patch):
                evidence = good_evidence()
                evidence["quicktime"].update(patch)
                self.assertGrade(evidence, "PASS", expected)

    def test_cli_pass_fail_void_exit_status(self):
        with tempfile.TemporaryDirectory() as work:
            path = Path(work) / "evidence.json"
            evidence = good_evidence()
            for status, label in ((0, "PASS"), (1, "FAIL"), (2, "VOID")):
                with self.subTest(status=status):
                    evidence["quicktime"]["crashes"] = (0, 2, None)[status]
                    path.write_text(json.dumps(evidence))
                    result = subprocess.run([sys.executable, TOOLS / "grade.py", path], capture_output=True, text=True)
                    self.assertEqual(result.returncode, status)
                    self.assertIn(f"{label} Cameras: QuickTime", result.stdout)
            path.write_text("not JSON")
            result = subprocess.run([sys.executable, TOOLS / "grade.py", path], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
