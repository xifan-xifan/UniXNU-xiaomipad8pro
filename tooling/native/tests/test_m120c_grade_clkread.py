import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


GRADER = Path(__file__).resolve().parents[1] / "m120c-grade-clkread.py"


def sample(serial, x, corner=320, mdp=575_000_000, mode=144):
    rows = [
        f"snapshot {serial} at {serial * 200000} us:",
        f"mode: live {mode} Hz (both links)",
        f"clkread_mdp status=ok hz={mdp}",
        "clkread_pll0 status=ok hz=1725000000",
        "clkread_cesta status=ok enabled=1",
        "clkread_rpmh voter=apps-drv2 aggregate=unavailable",
    ]
    for i, (name, ab, ib) in enumerate((("MM0", x, 4257), ("SH0", 5, 6), ("ACV", 0, 1), ("MC0", 7, 8))):
        raw = 0x20000000 | (ab << 14) | ib
        rows.append(f"clkread_bcm {name} status=ok addr={0x50000 + i * 4:08x} raw={raw:08x} x={ab} y={ib}"
                    " unit=1000000 width=32 vcd=1 rc=0 cmd_status=00010100")
    for i, name in enumerate(("mmcx.lvl", "mxc.lvl")):
        rows.append(f"clkread_arc {name} status=ok addr={0x30000 + i * 4:08x} raw=00000008 hlvl=8 vlvl={corner}"
                    " rc=0 cmd_status=00010100")
    return "\n".join(rows) + "\n"


class ClkreadGradeTest(unittest.TestCase):
    def grade(self, command, snapshots, expected_code):
        with tempfile.TemporaryDirectory() as temp:
            paths = []
            for i, text in enumerate(snapshots):
                path = Path(temp) / str(i)
                path.write_text(text)
                paths.append(str(path))
            result = subprocess.run([sys.executable, "-I", str(GRADER), *command, *paths], capture_output=True, text=True)
        self.assertEqual(result.returncode, expected_code, result.stdout + result.stderr)
        return {line.split()[0]: line.split()[-1] for line in result.stdout.splitlines()}

    def test_arm_and_compare(self):
        control = [sample(1, 2128, mode=120), sample(2, 2128, mode=120)]
        treatment = [sample(1, 2554), sample(2, 2554)]
        self.assertEqual(self.grade(["arm", "120"], control, 0)["CLKREAD_MDP"], "PASS")
        self.assertEqual(self.grade(["arm", "144"], treatment, 0)["CLKREAD_MM0"], "PASS")
        result = self.grade(["compare"], control + treatment, 0)
        self.assertEqual(result["CLKREAD_MM0_AB_RAISED"], "PASS")
        self.assertEqual(result["CLKREAD_NO_MMCX_DROP"], "PASS")
        self.assertEqual(result["CLKREAD_AGGREGATE"], "INFO")

    def test_no_raise_corner_drop_and_opp_mismatch(self):
        samples = [sample(1, 2554, mode=120), sample(2, 2554, mode=120), sample(1, 2554, 256), sample(2, 2554, mdp=532_000_000)]
        result = self.grade(["compare"], samples, 1)
        for tag in ("CLKREAD_MM0_AB_RAISED", "CLKREAD_MDP_120_144", "CLKREAD_NO_MMCX_DROP", "CLKREAD_NO_MXC_DROP"):
            self.assertEqual(result[tag], "FAIL")

    def test_stale_unavailable_and_malformed_responses(self):
        before = sample(1, 2554)
        bad = before.replace("cmd_status=00010100", "cmd_status=ffffffff").replace("clkread_mdp status=ok", "clkread_mdp status=unreadable")
        result = self.grade(["arm", "144"], [before, bad], 1)
        for tag in ("CLKREAD_FRESH", "CLKREAD_MDP", "CLKREAD_MM0", "CLKREAD_MMCX"):
            self.assertEqual(result[tag], "FAIL")
        result = self.grade(["arm", "120"], [before, "PianoDpu: clocks: mdp rcg cmd 00000000 cfg 00100105\n"], 1)
        self.assertEqual(result["CLKREAD_MDP"], "FAIL")

    def test_rejects_wrong_mode(self):
        result = self.grade(["arm", "120"], [sample(1, 2128), sample(2, 2128)], 1)
        self.assertEqual(result["CLKREAD_MODE"], "FAIL")

    def test_opp_tolerance_boundaries(self):
        start = sample(1, 2554)
        for hz, expected in ((574_999_000, 0), (575_001_000, 0), (574_998_999, 1), (575_001_001, 1)):
            with self.subTest(hz=hz):
                result = self.grade(["arm", "144"], [start, sample(2, 2554, mdp=hz)], expected)
                self.assertEqual(result["CLKREAD_MDP"], "FAIL" if expected else "PASS")

    def test_bcm_response_limits(self):
        start = sample(1, 2554)
        after = sample(2, 2554)
        for bad in (after.replace("raw=227e90a1", "raw=ffffffff"),
                    after.replace("x=2554", "x=16384"),
                    after.replace("unit=1000000", "unit=0"),
                    after.replace("width=32", "width=0")):
            self.assertNotEqual(bad, after)
            result = self.grade(["arm", "144"], [start, bad], 1)
            self.assertEqual(result["CLKREAD_MM0"], "FAIL")


if __name__ == "__main__":
    unittest.main()
