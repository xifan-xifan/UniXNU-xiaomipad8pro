#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import grade


def pipe_line(**fields):
    totals = dict.fromkeys(grade.PIPE_TOTALS, 0) | fields
    return "NativeGopDisplay: pipe120: " + " ".join(f"{k}={v}" for k, v in totals.items()) + " (cumulative)"


def notification(at_us, valid=0, missing=0, **counts):
    return {
        "version": 1, "abi": 2, "hz": 120, "at_us": at_us, "frame_info_enabled": 1,
        **dict.fromkeys(grade.FI_TOTALS, 0),
        "programs": valid + missing, "complete_hooks": valid + missing,
        "frame_info_valid": valid, "frame_info_missing": missing, "frame_info_packets": valid,
        "type3_attempts": valid, "type3_accepted": valid, "frame_info_audit_enabled": 1,
        "frame_info_audit": {
            "version": 1, "events": valid + missing, "valid": valid, "missing": missing,
            "reasons": dict.fromkeys(grade.AUDIT_TOTALS, 0) | counts,
        },
    }


def queue(head=41, **fields):
    return dict(ticks=12, current=39, created=1, pending=2, head=head,
                layers=1, ready=1, drops=8, main_surface=1) | fields


def pipe(swap=41, **fields):
    return {"m120_diag": 1, "kind": "pipe", "swap": swap, "worker": 17,
            "preparation": 2, "take": 2, "ticket": 0, "failure": 0, "key_matches": 0,
            "prepare": queue(swap), "program": queue(swap)} | fields


def flush(generation=5, **fields):
    return {"m120_diag": 1, "kind": "flush", "generation": generation, "worker": 17,
            "selected_irq": 8, "armed": 10, "enabled": 11, "flushed": 13, "latched": 16,
            "disabled": 17, "disarmed": 18, "end": 19, "mask": 3, "latch_ctl": 0,
            "frame1": 9, "line1": 2200, "frame2": 0xffffffff, "line2": 2199} | fields


def irq(generation=8, **fields):
    return {"m120_diag": 1, "kind": "irq", "generation": generation, "armed_flush": 5,
            "ticks": 14, "status": 1 << 27, "ctl": 0,
            "before1": 9, "after1": 9, "line1": 4, "before2": 0, "after2": 0, "line2": 5} | fields


class GradeTest(unittest.TestCase):
    def test_historical_populations(self):
        final_fi = notification(92_870_001, 99, 3187, same_frame=3186, before_flush=1, other_frame=1)
        final_fi.update(type3_last_mask=0x80000003, type3_last_slots=6, frame_info_last_delta_raw=67_000_775)
        console = [
            "trailing unrelated line" + pipe_line(captured=1716, used=1715, fallback=2403),
            "NativeGopDisplay: pacing120 at-us 252133064 window-us 10008020: replaced 0",
            "NativeGopDisplay: pacing120 program: events 511 gaps 511",
            "NativeGopDisplay: pacing120 complete-hook: events 511 gaps 511",
            "NativeGopDisplay: pacing120 stage-fence: n 511 sum 5487141 min 6894 mean 10738 max 12219 us;",
            pipe_line(captured=1716, used=1715, fallback=2914),
            "machdep.native_gop_notifications: " + json.dumps(notification(1)),
            json.dumps(final_fi),
        ]
        report = grade.analyze(console)
        self.assertEqual(report["malformed_lines"], [])
        self.assertEqual(report["fix_image"], "HOLD")
        legacy = report["legacy"]
        self.assertIsNone(legacy["pipe_reports"][0]["delta"])
        self.assertEqual(legacy["pipe_reports"][1]["delta"]["fallback"], 511)
        self.assertEqual(legacy["pacing_windows"][0]["events"]["program"], 511)
        self.assertEqual(legacy["pacing_windows"][0]["stages"]["stage-fence"]["mean_us"], 10738)
        self.assertEqual(legacy["notifications"][1]["type3_last_mask"], 0x80000003)
        self.assertEqual(legacy["notifications"][1]["frame_info_last_delta_raw"], 67_000_775)
        delta = legacy["notifications"][1]["delta"]
        self.assertEqual(delta["programs"], 3286)
        self.assertEqual(delta["audit"]["missing"], 3187)
        self.assertEqual(sum(delta["audit"]["reasons"].values()), 3188)
        self.assertEqual(delta["audit"]["reasons"]["same_frame"], 3186)
        self.assertEqual(report["raw"], {"pipe": None, "physical": None})

    def test_baselines_and_decreasing_counters(self):
        lines = [pipe_line(fallback=3), pipe_line(fallback=1), pipe_line(fallback=2),
                 json.dumps(notification(33, missing=3)), json.dumps(notification(4, missing=1))]
        legacy = grade.analyze(lines)["legacy"]
        self.assertIsNone(legacy["pipe_reports"][1]["delta"])
        self.assertEqual(legacy["pipe_reports"][2]["delta"]["fallback"], 1)
        self.assertIsNone(legacy["notifications"][1]["delta"])

    def test_wrapped_rollups_start_a_new_baseline(self):
        top = grade.UINT64_MAX
        lines = [pipe_line(captured=top), pipe_line(captured=0), pipe_line(captured=1)]
        result = grade.analyze(lines)
        reports = result["legacy"]["pipe_reports"]
        self.assertIsNone(reports[1]["delta"])
        self.assertEqual(reports[2]["delta"]["captured"], 1)
        self.assertEqual(result["fix_image"], "HOLD")

    def test_every_pipe_bucket(self):
        cases = {
            "unobserved": {"take": 0},
            "used": {"take": 1, "ticket": 1, "key_matches": 1},
            "no_preparation": {"preparation": 0},
            "power_off": {"preparation": 1},
            "empty_head": {"prepare": queue(0)},
            "head_replaced": {"prepare": queue(40, ready=0)},
            "ineligible_head": {"prepare": queue(41, layers=0x10001, ready=2, drops=9, main_surface=0)},
            "key_mismatch": {"ticket": 2},
            "capture_failed": {"ticket": 3, "key_matches": 1, "failure": 1},
            "poll_failed": {"ticket": 3, "key_matches": 1, "failure": 2},
            "deadline": {"ticket": 3, "key_matches": 1, "failure": 3},
            "failed_unknown": {"ticket": 3, "key_matches": 1},
            "no_ticket": {},
            "inconsistent": {"ticket": 1, "key_matches": 1},
        }
        for label, fields in cases.items():
            with self.subTest(bucket=label):
                report = grade.analyze([json.dumps(pipe(**fields))], details=True)
                raw = report["raw"]
                self.assertEqual(raw["pipe"]["take_buckets"], {label: 1})
                self.assertEqual(raw["records"]["pipe"][0]["program_rejections"], [])
                if label == "ineligible_head":
                    self.assertEqual(raw["pipe"]["prepare_rejections"],
                                     dict.fromkeys(["layers", "not_ready", "drops", "no_surface"], 1))

    def test_physical_samples_join_only_on_exact_identity(self):
        rows = [irq(7, ticks=12, after1=10), flush(6, worker=21, selected_irq=42),
                irq(9, armed_flush=4, ticks=20, status=0, ctl=1, after1=10, before2=2, after2=2),
                flush(7, worker=21, selected_irq=9, latch_ctl=2), flush(qualifier_reasons=32),
                irq(), pipe(fence={"program_target": 0, "retired": 0}, feedback={"disposition": 1})]
        raw = grade.analyze(map(json.dumps, rows), details=True)["raw"]
        self.assertEqual(raw["physical"]["intf1"], {"missing": 1, "unstable": 1, "same": 1})
        self.assertEqual(raw["physical"]["intf2"], {"missing": 1, "other": 1, "next": 1})
        missing, stale, selected = raw["records"]["flush"]
        self.assertEqual(missing["observations"], ["no_irq"])
        self.assertEqual(set(stale["observations"]), {
            "latch_ctl_pending", "arm_mismatch", "no_intf1_vsync", "after_end",
            "irq_ctl_pending", "after_disable", "after_disarm",
        })
        self.assertIsNone(stale["swap"])
        self.assertEqual(selected["swap"], 41)
        self.assertEqual(selected["irq_rows_armed_for_flush"], 2)
        self.assertEqual(raw["pipe"]["fence_rows"], 1)
        self.assertEqual(raw["pipe"]["feedback"], {"suppressed": 1})

    def test_malformed_and_ambiguous_data(self):
        valid = pipe()
        bad_field = deepcopy(valid)
        bad_field["prepare"]["ready"] = 1 << 32
        rows = [valid, valid, bad_field, pipe(42, unexpected="discard this"),
                irq(generation=0), flush(selected_irq=0)]
        result = grade.analyze([*map(json.dumps, rows), '{"m120_diag":1,"m120_diag":1}'], details=True)
        self.assertEqual(result["malformed_lines"], [2, 3, 4, 5, 7])
        self.assertEqual(result["raw"]["physical"]["observations"], {"no_irq": 1})
        self.assertNotIn("discard this", json.dumps(result))
        result = grade.analyze(map(json.dumps, [valid, pipe(42), flush(), irq()]), details=True)
        self.assertIsNone(result["raw"]["records"]["flush"][0]["swap"])

    def test_unsigned_record_boundaries(self):
        for invalid in (-1, 1 << 32, float("nan"), True):
            with self.subTest(invalid=invalid):
                rows = [irq(before2=invalid), irq(ticks=grade.UINT64_MAX)]
                result = grade.analyze(map(json.dumps, rows))
                self.assertEqual(result["malformed_lines"], [1])
                self.assertEqual(result["raw"]["physical"]["irq_rows"], 1)
                self.assertEqual(result["fix_image"], "HOLD")

    def test_cli(self):
        script = Path(__file__).with_name("grade.py")
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.txt"
            for data, expected in (("unrelated", 2), ("{\"m120_diag\":1", 1), (json.dumps(irq()), 0),
                                   (json.dumps(pipe(ticket=1, key_matches=1)), 1)):
                capture.write_text(data)
                run = subprocess.run([sys.executable, script, capture], capture_output=True, text=True)
                self.assertEqual(run.returncode, expected, run.stderr)
                self.assertEqual(json.loads(run.stdout)["fix_image"], "HOLD")


if __name__ == "__main__":
    unittest.main()
