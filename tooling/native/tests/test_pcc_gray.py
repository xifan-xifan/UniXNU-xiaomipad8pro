#!/usr/bin/env python3
"""Regression for the y12s grader's dark-blue-channel false failure."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "pcc-gray.py"
SPEC = importlib.util.spec_from_file_location("pcc_gray", SCRIPT)
grader = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(grader)

# Exact ascending-register rows from y12s-qual1's supplied snapshots.
CAPTURES = {
    "base": "074a0 3ff8a 3ff14 | 009af 06c83 3fca7 | 001b2 00039 05ba7",
    "off": "07498 3ff8a 3ff14 | 009b6 06c75 3fca6 | 001b3 00039 05b96",
    "half": "062fd 3fecd 3fdf0 | 01916 04dbc 3f8c3 | 003ee 00077 02a53",
    "warm": "0534e 3fe15 3fdb0 | 027d4 030df 3f8c2 | 004de 00067 0112a",
}


def snapshot(words, enable="00000001"):
    return "\n".join(
        f"dspp{d}: pcc en {enable} c {words} (+00000000 00000000 00000000); gc en 00000000"
        for d in range(2))


class PccGrayTests(unittest.TestCase):
    def run_cli(self, text, expect):
        with tempfile.TemporaryDirectory(prefix="pcc-gray-") as directory:
            path = Path(directory) / "dpu.txt"
            path.write_text(text)
            return subprocess.run([sys.executable, str(SCRIPT), str(path), "--expect", expect],
                                  text=True, capture_output=True)

    def test_captured_off_is_a_failure(self):
        for name in ("base", "off"):
            with self.subTest(name=name):
                result = self.run_cli(snapshot(CAPTURES[name]), "off")
                self.assertEqual(result.returncode, 1)
                self.assertIn("FAIL off", result.stdout)

    def test_full_strength_and_half_pass(self):
        for name in ("half", "warm"):
            with self.subTest(name=name):
                result = self.run_cli(snapshot(CAPTURES[name]), "warm")
                self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("B 0.059", result.stdout)

    def test_input_groups_and_signed_cross_terms(self):
        rows = grader.responses(snapshot(CAPTURES["warm"]))
        self.assertEqual(len(rows), 2)
        for _, (r, g, b), enabled in rows:
            self.assertTrue(enabled)
            self.assertAlmostEqual(r, 1, delta=.001)
            self.assertAlmostEqual(g, .370, delta=.001)
            self.assertAlmostEqual(b, .059, delta=.001)

    def test_word_limits_and_reserved_bits(self):
        rows = grader.responses(snapshot("fffc8000 20000 00000 | 00000 1ffff 3ffff | 00000 00000 00000"))
        for _, gray, enabled in rows:
            self.assertTrue(enabled)
            self.assertEqual(gray, [1, -1 / 32768, -1 / 32768])
        identity = snapshot("08000 00000 00000 | 00000 08000 00000 | 00000 00000 08000")
        self.assertEqual(self.run_cli(identity, "off").returncode, 0)

    def test_channel_order_and_red_floor(self):
        for gray in ([.8, .5, .05], [1, .2, .3], [.9, 1, .1], [1, 0, -.1]):
            # Negative B satisfies the requested ordering; this isn't an
            # optical clipping classifier or an invented minimum-B rule.
            self.assertEqual(grader.passes(gray, "warm"), gray == [1, 0, -.1])
        self.assertTrue(grader.passes([.9, .5, .01], "warm"))

    def test_off_two_percent(self):
        self.assertTrue(grader.passes([1, .98, 1.02], "off"))
        self.assertTrue(grader.passes([1, .981, .981], "off"))
        self.assertFalse(grader.passes([1, .979, 1], "off"))
        self.assertFalse(grader.passes([.5, .5, .5], "off"))

    def test_bypass_off_and_warm(self):
        text = snapshot(CAPTURES["warm"], "00000000")
        self.assertEqual(self.run_cli(text, "off").returncode, 0)
        self.assertEqual(self.run_cli(text, "warm").returncode, 1)

    def test_both_halves_must_pass(self):
        text = snapshot(CAPTURES["warm"]).splitlines()[0] + "\n"
        text += snapshot("08000 00000 00000 | 00000 04000 00000 | 00000 00000 06000").splitlines()[1]
        self.assertEqual(self.run_cli(text, "warm").returncode, 1)

    def test_incomplete_or_malformed_is_invalid(self):
        for text in ("", snapshot(CAPTURES["warm"]).splitlines()[0], snapshot("08000 00000"),
                     snapshot("nan 0 0 | 0 8000 0 | 0 0 8000")):
            with self.subTest(text=text):
                self.assertEqual(self.run_cli(text, "off").returncode, 2)


if __name__ == "__main__":
    unittest.main()
