# SPDX-License-Identifier: BSD-2-Clause-Patent
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


GRADER = Path(__file__).resolve().parents[1] / "grade-dpu-m2.py"
BOOT_A = "00000000-0000-4000-8000-000000000001"
BOOT_B = "00000000-0000-4000-8000-000000000002"
A = dict(shown=1000, maps=3, passes=1000, forced=1000, latched=990, full=1000,
         pixel_us=2_000_000, to_boot=1)
B = dict(shown=1000, maps=3, passes=80, init=3, skip=920, full=77, handoff=10, mode=60,
         attrs=7, latched=990, pixel_us=160_000, check_us=100_000, to_boot=1)
DAMAGE_MISMATCH = ("NativeGopDisplay: damage: a frame's dirty region 1,2 3x4 left out a change: "
                   "damage copies off for this boot\n")


def capture(boot, serial, at, policy, writes, counts, pid=101, hz=120, frame=None, serving=1, status=0, pending=0):
    def fields(*keys):
        return " ".join(f"{key.replace('_', '-')} {counts.get(key, 0)}" for key in keys)

    frame = at * hz // 1_000_000 if frame is None else frame
    return "\n".join([
        f"boot {boot}",
        f"windowserver-pid {pid}",
        f"snapshot {serial} at {at} us: 122 loads, 0 stores; gate open (gcc disp ahb 80000001)",
        f"ctl0: top f0000000 intf-active 00000006; top intr en 00000000 status {status:08x}; path ok",
        f"intf1: en 00000001 status 00000001; counters en 00000000 frame {frame} line 100",
        f"intf2: en 00000001 status 00000001; counters en 00000000 frame {frame} line 100",
        f"mode: live {hz} Hz (both links)",
        "clean: " + fields("shown", "maps", "evictions", "passes", "init", "skip", "failed", "alias_failed", "without"),
        "clean: fallback " + fields("forced", "handoff", "unknown_mode", "mode", "attrs", "backing", "init_clean"),
        "clean: " + fields("pixel_us", "check_us"),
        f"clean: m2 policy {policy} writes {writes} " + fields("latched", "full", "to_boot", "underruns")
        + f" pending {pending:08x} serving {serving}",
    ]) + "\n"


def gop(flips, copies):
    return (f"NativeGopDisplay: flip mode: {flips} swaps flipped (no copy, no refresh copies; {flips} '&BGA' plane 0), "
            f"{copies} copied (PianoDpu refused them, is not serving, or a cursor was on the panel: 0); "
            "surface maps: 0 prepared read-only, 0 failed\n")


class GradeDpuM2Test(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.directory = Path(temp.name)
        self.manifest = {}

    def arm(self, label, *, paired=False, delta=None, duration=60_000_000, extra_gop_flips=4, copies=1,
            console="", before_values=None, after_values=None, **review):
        control = label == "control"
        policy = int(not control)
        same_boot_b = not control and not paired
        boot = BOOT_B if paired and not control else BOOT_A
        delta = dict(A if control else B, **(delta or {}))
        cumulative = A if same_boot_b else {}
        total = {**cumulative, **{key: cumulative.get(key, 0) + value for key, value in delta.items()}}
        serial = 3 if same_boot_b else 1
        at = 90_000_000 if same_boot_b else 10_000_000
        writes = 2 if same_boot_b else 1
        pre = gop(1050, 51) if same_boot_b else gop(20, 50)
        post = gop((1050 if same_boot_b else 20) + delta["shown"] + extra_gop_flips, (51 if same_boot_b else 50) + copies)
        before = dict(boot=boot, serial=serial, at=at, policy=policy, writes=writes, counts=cumulative)
        before.update(before_values or {})
        self.write(f"{label}-before.txt", capture(**before))
        after = dict(boot=boot, serial=serial + 1, at=at + duration, policy=policy, writes=writes, counts=total)
        after.update(after_values or {})
        self.write(f"{label}-after.txt", capture(**after))
        self.write(f"{label}-console.txt", pre + console + post)
        self.manifest[label] = dict(
            before=f"{label}-before.txt", after=f"{label}-after.txt", console=f"{label}-console.txt",
            console_complete=True, image_sha256="a" * 64, workload="mixed-use-60s",
            windowserver_crashes=0, optical_checks=120, optical_mismatches=0,
        )
        self.manifest[label].update(review)

    def write(self, filename, contents):
        (self.directory / filename).write_text(contents, encoding="utf-8")

    def edit(self, filename, old, new):
        source = self.directory / filename
        text = source.read_text(encoding="utf-8")
        self.assertIn(old, text)
        self.write(filename, text.replace(old, new))

    def grade(self, expected, reason=""):
        manifest = self.directory / "capture.json"
        manifest.write_text(json.dumps(self.manifest), encoding="utf-8")
        result = subprocess.run([sys.executable, "-I", str(GRADER), str(manifest)], capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertIn(reason, result.stdout)
        self.assertEqual(result.stdout.splitlines()[-1], ("M2 PASS", "M2 FAIL", "M2 VOID")[expected])
        return result.stdout

    def test_same_boot_and_paired_with_baseline_copy(self):
        self.arm("control")
        self.arm("treatment")
        output = self.grade(0, "verified_wc=923 skip_ratio=99.67% clean_reduction=92.00%")
        self.assertIn("clean_us_saved_at_B_volume=1840000", output)
        self.arm("treatment", paired=True)
        self.grade(0, "B clean reasons forced=0 handoff=10 unknown-mode=0 mode=60 attrs=7")

    def test_insufficient_workload_stale_and_wrong_policy(self):
        self.arm("control")
        for kwargs, reason in (
            (dict(duration=1_000_000), "shorter than 30 s"),
            (dict(after_values={"serial": 3}), "stale or reversed"),
            (dict(after_values={"hz": 60}), "mode changed"),
            (dict(after_values={"frame": 10800}), "link did not advance"),
            (dict(after_values={"writes": 3}), "stable strict policy 1"),
            (dict(after_values={"policy": 0}), "stable strict policy 1"),
            (dict(after_values={"boot": BOOT_B}), "boot changed"),
            (dict(delta={"latched": 0}), "fewer than 300 latched flips"),
            (dict(extra_gop_flips=251), "GOP reports must bracket"),
            (dict(console_complete=False), "console completeness"),
            (dict(optical_checks=0), "missing or inconsistent optical evidence"),
            (dict(image_sha256="b" * 64), "image or workload IDs differ"),
        ):
            with self.subTest(reason=reason):
                self.arm("treatment", **kwargs)
                self.grade(2, reason)

    def test_malformed_or_missing_rows_and_counter_reset(self):
        self.arm("control")
        for old, new, reason in (
            ("clean: pixel-us", "ignored: pixel-us", "expected one complete clean: pixel-us row"),
            ("clean: pixel-us", "clean: pixel-us 0 check-us 0\nclean: pixel-us", "expected one complete clean: pixel-us row"),
            ("check-us 100000", "check-us -1", "expected one complete clean: pixel-us row"),
            ("maps 6", "maps 2", "DPU counters decreased"),
        ):
            with self.subTest(reason=reason):
                self.arm("treatment")
                self.edit("treatment-after.txt", old, new)
                self.grade(2, reason)
        self.arm("treatment")
        self.write("treatment-console.txt", gop(7, 2) + gop(5, 1))
        self.grade(2, "GOP counters decreased")

    def test_wc_coverage_skip_and_time_thresholds(self):
        self.arm("control")
        self.arm("treatment", delta={"skip": 0, "init": 0, "full": 1000, "mode": 983, "passes": 1000})
        self.grade(2, "fewer than 100 verified-WC shown flips (0)")
        self.arm("treatment", delta={"init": 100, "skip": 823, "passes": 177})
        self.grade(1, "skip ratio below 90%")
        self.arm("treatment", delta={"pixel_us": 1_000_001})
        self.grade(1, "fell by less than 50%")
        self.arm("treatment", delta={"init": 90, "skip": 810, "mode": 83, "full": 100, "passes": 190,
                                     "pixel_us": 1_000_000})
        self.grade(0, "skip_ratio=90.00% clean_reduction=50.00%")

    def test_clean_accounting_accepts_extra_pass_on_reverification(self):
        self.arm("control")
        self.arm("treatment", delta={"passes": 81})
        self.grade(0)
        self.arm("treatment", delta={"passes": 79})
        self.grade(1, "clean passes disagree")
        self.arm("treatment", delta={"full": 76, "skip": 921})
        self.grade(1, "strict full cleans disagree")
        self.arm("treatment", delta={"without": 1})
        self.grade(1, "B without=1")
        self.arm("control", delta={"passes": 999})
        self.arm("treatment")
        self.grade(1, "one forced full clean per shown flip")

    def test_fallback_count_share_and_boot_framebuffer_regressions(self):
        self.arm("control")
        self.arm("treatment", copies=2)
        self.grade(1, "fallback count, share or DPU returns")
        self.arm("treatment", delta={"to_boot": 2})
        self.grade(1, "fallback count, share or DPU returns")
        self.arm("treatment", extra_gop_flips=0)
        self.grade(1, "fallback count, share or DPU returns")

    def test_hard_failures_in_either_arm(self):
        self.arm("control")
        for kwargs, reason in (
            (dict(delta={"underruns": 1}), "DPU underruns=1"),
            (dict(after_values={"status": 1 << 26}), "pending endpoints=0x0/0x4000000"),
            (dict(after_values={"pending": 1 << 28}), "pending endpoints=0x0/0x10000000"),
            (dict(after_values={"serving": 0}), "DPU stopped serving"),
            (dict(after_values={"pid": 102}), "WindowServer PID changed"),
            (dict(windowserver_crashes=1), "WindowServer crash reports=1"),
            (dict(optical_mismatches=1), "optical mismatches=1"),
            (dict(console=DAMAGE_MISMATCH), "GOP damage audit mismatch"),
            (dict(console="NativeGopDisplay: the GPU had not finished a frame after 1000 ms\n"), "GOP fence timeout"),
        ):
            with self.subTest(reason=reason):
                self.arm("treatment", **kwargs)
                self.grade(1, reason)
        self.arm("control", console="PianoDpu: watch: underrun 10000000 while a surface was shown\n")
        self.arm("treatment", windowserver_crashes=1, after="absent.txt")
        output = self.grade(1, "A console reported DPU underrun")
        self.assertIn("B WindowServer crash reports=1", output)

    def test_same_boot_rejects_overlap_even_with_good_arm_deltas(self):
        self.arm("control")
        self.arm("treatment")
        self.edit("treatment-before.txt", "snapshot 3 at 90000000", "snapshot 2 at 90000000")
        self.grade(2, "out of order, overlap or have reset counters")
        self.arm("treatment")
        self.edit("treatment-console.txt", gop(1050, 51), gop(1020, 50))
        self.grade(2, "same-boot GOP summaries overlap")

    def test_link_counter_wrap(self):
        self.arm("control")
        self.arm("treatment", paired=True, before_values={"frame": 0xfffffff0}, after_values={"frame": 7184})
        self.grade(0)


if __name__ == "__main__":
    unittest.main()
