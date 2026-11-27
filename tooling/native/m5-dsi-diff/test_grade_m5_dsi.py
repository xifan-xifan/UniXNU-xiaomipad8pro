# SPDX-License-Identifier: BSD-2-Clause-Patent

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from grade_m5_dsi import compare, parse_snapshot


BOOT = "00000000-0000-4000-8000-0000000000ab"
GRADER = Path(__file__).with_name("grade_m5_dsi.py")


def capture(serial=1, time_us=500_000, frames=(100, 101), line=10, vtotal=2310):
    lines = [
        f"boot {BOOT}",
        f"snapshot {serial} at {time_us} us: 238 loads, 0 stores; gate open (gcc disp ahb 80000001, gdsc 80000000, "
        "ahb 00000001, mdp 00000001, byte 00000001/00000001, pclk 00000001/00000001); "
        "panel id io 00000000 (CSOT, rate group A)",
        "ctl0: top f0000000 intf-active 00000006 master 00000001 lms 03 flush 00000000 intf-flush 00000000 "
        "dspp-flush 00000000/00000000; top intr en 00000000 status 00000000; mdss intr 00000000; path ok",
        "scanout ctl0: dsc-active 00000003 dsc-flush 00000000 pipe-active 00000003 fetch-active 00000003",
    ]
    for intf, frame in enumerate(frames, 1):
        lines.append(
            f"intf{intf}: en 00000001 status 00000001 config 00000000 hsync 07200008 period 003db170 vwidth 00000e40 "
            "dv 000054b8-003c1117 dhctl 06530014 skew 00000000 prog-fetch 00000000 intr 00000000; "
            f"counters en 00000000 frame {frame} line {line} (htotal 1824 vtotal {vtotal}); config2 00000000 data-hctl 00000000"
        )
    for link in (0, 1):
        # The clock mux, PHY status and local PLL values are from the supplied bright control.
        clk1, phy_status, pll_ctrl, pll_status, locked = [(0x30, 0x1f, 1, 0x51, 1), (0x34, 0x19, 0, 0x22, 0)][link]
        lines.extend([
            f"dsi{link}: ctrl 000001f3 status0 00000000 total 08a9071f active-h 06530014 active-v 0857000c; "
            "timing shadow +1e4 00000000 +1e8 00000000 +1ec 00000000; cmd-dma 00000000 trig 00001004 "
            "(dma trigger software, block-in-frame 1) intr 00000000 tpg 00000000 +390 00000000; "
            "clk-ctrl 00200b3f lanes 00001f00/00000000; video-compression 03200b81",
            f"dsi{link} m5: vid-cfg0 00000000 vid-cfg1 00000000 hsync 00000008 vsync-hpos 00000000 "
            "vsync-vpos 00000002 lp-timer ffffffff hs-timer 00000000 clkout 00000f20 eot 00000001 lane-swap 00000000 "
            "t-clk-pre-extend 00000000 cmd-compression 00000000/00000000 clk-status 008067c3 err-mask 00000000 "
            "ack-err 00000000 fifo 00001010 phy-err 00000000 timeout 00000000",
            f"dsi{link} m5: cmd-cfg0 00000000 cmd-cfg1 00000000 dma-length 00000000 reset 00000000 misr-ctrl 00000000",
            f"dsi{link} phy: clk-cfg0 00000051 clk-cfg1 {clk1:08x} rbuf-ctrl 00000001 phy-status {phy_status:08x} "
            "lane-status0 0000001f lane-status1 0000003f",
            f"dsi{link} phy: ctrl0 0000007f ctrl1 00000000 ctrl2 00000000 ctrl3 00000004 ctrl5 00000007 "
            f"lane-cfg0 00000021 lane-cfg1 00000084 lane-ctrl0 0000001f lane-ctrl1 00000000 pll-ctrl {pll_ctrl:08x}",
            f"dsi{link} phy timing 0..13: 00000000 00000027 0000000a 0000000a 0000001b 00000019 0000000a "
            "0000000b 0000000a 00000002 00000004 00000000 00000020 0000000f",
            f"dsi{link} pll: status {pll_status:08x} local-lock {locked}",
        ])
    for dspp in (0, 1):
        lines.append(f"dspp{dspp}: pcc en 00000000 c 00000000 00000000 00000000 | 00000000 00000000 00000000 | "
                     "00000000 00000000 00000000 (+00000000 00000000 00000000); gc en 00000000")
    lines += ["mode: live 120 Hz (both links)",
              "vfp switch (DpuRefresh.h): the path takes it (120 Hz family, totals agree, no programmable fetch)"]
    return "\n".join(lines) + "\n"


def edit_row(text, prefix, old, new):
    lines = text.splitlines(keepends=True)
    matches = [i for i, line in enumerate(lines) if line.startswith(prefix) and old in line]
    if len(matches) != 1:
        raise AssertionError(f"fixture edit requires one matching {prefix} row")
    i = matches[0]
    lines[i] = lines[i].replace(old, new, 1)
    return "".join(lines)


def blanking_words(text):
    for prefix, old, new in (
        ("ctl0:", "lms 03 flush 00000000", "lms 03 flush 80000000"),
        ("ctl0:", "top intr en 00000000 status 00000000", "top intr en 00000001 status 00000001"),
        ("dsi0:", "lanes 00001f00", "lanes 00001f1f"),
        ("dsi1:", "lanes 00001f00", "lanes 00001f1f"),
        ("dsi0 m5:", "clk-status 008067c3", "clk-status 00006343"),
        ("dsi1 m5:", "clk-status 008067c3", "clk-status 00006343"),
        ("dsi0 m5:", "fifo 00001010", "fifo 11111010"),
        ("dsi1 m5:", "fifo 00001010", "fifo 11111010"),
    ):
        text = edit_row(text, prefix, old, new)
    return text


def run_grader(baseline, candidate, *options):
    with tempfile.TemporaryDirectory() as temporary:
        before = Path(temporary) / "bright.txt"
        after = Path(temporary) / "wake.txt"
        before.write_text(baseline, encoding="utf-8")
        if candidate is not None:
            after.write_text(candidate, encoding="utf-8")
        cmd = [sys.executable, "-I", str(GRADER), str(before), str(after), *options]
        return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", check=False)


class GradeM5DsiTests(unittest.TestCase):
    bright = capture()
    wake = capture(2, 700_000, (124, 125), line=30)
    blanking = blanking_words(capture(2, 700_000, (124, 125), line=2231))

    def grade(self, candidate):
        return compare(parse_snapshot(self.bright), parse_snapshot(candidate))

    def test_match_with_bonded_slave_unlocked(self):
        for wake in (
            self.wake,
            self.wake.replace("snapshot 2", "machdep.piano_dpu_snapshot: snapshot 2"),
            f"kern.bootsessionuuid: {BOOT.upper()}\r\n" + "\r\n".join(
                "console PianoDpu: snap: " + row for row in self.wake.splitlines()[1:]
            ) + "\r\n",
        ):
            with self.subTest(wake=wake.splitlines()[0]):
                report = self.grade(wake)
                self.assertEqual(report["verdict"], "PASS")
                self.assertEqual(report["frame_advance"], {"intf1": 24, "intf2": 24})
                self.assertEqual(report["baseline"], {
                    "loads": 238, "stores": 0, "serial": 1, "time_us": 500_000, "phase": "active",
                    "timing": {intf: {"line": 10, "vtotal": 2310, "phase": "active"} for intf in ("intf1", "intf2")},
                })
                self.assertEqual(report["changes"], [])
                self.assertEqual(report["flags"], [])
                self.assertEqual(report["static_changes"], [])
                self.assertEqual(report["transient_changes"], [])
                self.assertNotIn("counter_reset", report)

    def test_lp_timer_all_ones_is_a_valid_register_value(self):
        for link in ("dsi0", "dsi1"):
            self.assertEqual(parse_snapshot(self.bright).registers[link, "lp-timer"], 0xffffffff)
        report = self.grade(edit_row(self.wake, "dsi1 m5:", "lp-timer ffffffff", "lp-timer fffffffe"))
        self.assertEqual(report["verdict"], "DRIFT")
        self.assertEqual(report["static_changes"], [
            {"domain": "dsi-timing", "block": "dsi1", "register": "lp-timer",
             "baseline": "ffffffff", "candidate": "fffffffe"},
        ])

    def test_active_blanking_changes_remain_strict_drift(self):
        for baseline, candidate in (
            (self.bright, self.blanking),
            (blanking_words(capture(line=2231)), self.wake),
        ):
            with self.subTest(baseline=baseline.splitlines()[4]):
                report = compare(parse_snapshot(baseline), parse_snapshot(candidate))
                self.assertEqual(report["verdict"], "DRIFT")
                self.assertEqual(report["static_changes"], [])
                self.assertEqual(report["changes"], report["transient_changes"])
                self.assertCountEqual(
                    [(row["block"], row["register"]) for row in report["transient_changes"]],
                    [("ctl", name) for name in ("flush", "top-intr-en", "top-intr-status")]
                    + [(link, name) for link in ("dsi0", "dsi1") for name in ("lane-status", "clk-status", "fifo")],
                )
                self.assertEqual(report["flags"], [])
                blank = next(report[role] for role in ("baseline", "candidate") if report[role]["phase"] == "blanking")
                self.assertEqual(blank["timing"], {
                    intf: {"line": 2231, "vtotal": 2310, "phase": "blanking"} for intf in ("intf1", "intf2")
                })

    def test_phase_boundary_and_unclassifiable_positions(self):
        for line, vtotal, expected in (
            (0, 2310, "active"), (2135, 2310, "active"), (2136, 2310, "blanking"),
            (2309, 2310, "blanking"), (2310, 2310, "unknown"), (0, 0, "unknown"),
            (0xffffffff, 2310, "unknown"), (30, 2**32, "unknown"),
        ):
            with self.subTest(line=line, vtotal=vtotal):
                report = self.grade(capture(2, 700_000, (124, 125), line=line, vtotal=vtotal))
                self.assertEqual(report["verdict"], "PASS")
                self.assertEqual(report["candidate"]["timing"]["intf1"],
                                 {"line": line, "vtotal": vtotal, "phase": expected})

    def test_mixed_link_phases_only_explain_the_blanking_link(self):
        report = self.grade(edit_row(self.blanking, "intf2:", "line 2231", "line 2135"))
        self.assertEqual(report["candidate"]["phase"], "mixed")
        self.assertEqual([row["block"] for row in report["transient_changes"]], ["dsi0"] * 3)
        self.assertCountEqual([row["block"] for row in report["static_changes"]], ["ctl"] * 3 + ["dsi1"] * 3)
        self.assertEqual(report["flags"], [
            {"kind": "phy-drift", "link": "dsi1", "registers": ["lane-status"]},
            {"kind": "host-error", "link": "dsi1", "registers": ["fifo"]},
        ])

    def test_ctl_changes_across_a_route_change_stay_visible(self):
        report = self.grade(self.blanking.replace("ctl0:", "ctl1:"))
        self.assertCountEqual([(row["block"], row["register"]) for row in report["static_changes"]],
                              [("ctl", name) for name in ("index", "flush", "top-intr-en", "top-intr-status")])
        self.assertEqual(len(report["transient_changes"]), 6)

    def test_static_drift_and_unexpected_live_values_stay_visible_across_phases(self):
        wake = self.blanking
        for prefix, old, new in (
            ("dsi0:", "lanes 00001f1f", "lanes 00001f1e"),
            ("dsi0:", "video-compression 03200b81", "video-compression 00000000"),
            ("dsi1 m5:", "clk-status 00006343", "clk-status 00016343"),
            ("dsi1 phy timing", "00000020 0000000f", "00000020 000000ff"),
            ("dsi0 pll:", "00000051 local-lock 1", "00000050 local-lock 0"),
        ):
            wake = edit_row(wake, prefix, old, new)
        report = self.grade(wake)
        self.assertEqual(report["verdict"], "DRIFT")
        self.assertCountEqual([(row["block"], row["register"]) for row in report["static_changes"]], [
            ("dsi0", "lane-status"), ("dsi0", "video-compression"), ("dsi1", "clk-status"),
            ("dsi1", "phy-timing-13"), ("dsi0", "pll-status"), ("dsi0", "local-lock"),
        ])
        self.assertEqual(len(report["transient_changes"]), 7)
        self.assertEqual({flag["kind"] for flag in report["flags"]}, {"pll-loss", "dsc-mismatch", "phy-drift"})

    def test_each_domain_and_per_link_flags(self):
        edits = (
            ("intf1:", "config2 00000000", "config2 00000001", "intf", "config2"),
            ("scanout ctl0:", "pipe-active 00000003", "pipe-active 00000001", "intf", "pipe-active"),
            ("dsi0:", "ctrl 000001f3", "ctrl 000001f1", "dsi-ctrl", "ctrl"),
            ("dsi0:", "+1ec 00000000", "+1ec 00000002", "dsi-timing", "shadow-1ec"),
            ("dsi0:", "video-compression 03200b81", "video-compression 00000000", "dsc-compression", "video-compression"),
            ("dsi1 m5:", "cmd-compression 00000000/00000000", "cmd-compression 00000000/00000003",
             "dsc-compression", "cmd-compression2"),
            ("dsi0 phy:", "phy-status 0000001f", "phy-status 0000001e", "phy-clk-lane", "phy-status"),
            ("dsi1 phy:", "lane-status1 0000003f", "lane-status1 0000001e", "phy-clk-lane", "lane-status1"),
            ("dsi1 m5:", "clk-status 008067c3", "clk-status 00000001", "phy-clk-lane", "clk-status"),
            ("dsi1 phy timing", "00000020 0000000f", "00000020 000000ff", "phy-timing", "phy-timing-13"),
            ("dsi0 pll:", "00000051", "00000050", "pll-lock", "pll-status"),
            ("dsi0 pll:", "local-lock 1", "local-lock 0", "pll-lock", "local-lock"),
            ("dsi0 phy:", "pll-ctrl 00000001", "pll-ctrl 00000000", "pll-lock", "pll-ctrl"),
            ("dsi1 m5:", "fifo 00001010", "fifo 00000010", "errors-fifo", "fifo"),
        )
        wake = self.wake
        for prefix, old, new, _, _ in edits:
            wake = edit_row(wake, prefix, old, new)
        report = self.grade(wake)
        self.assertEqual(report["verdict"], "DRIFT")
        self.assertEqual(report["changes"], report["static_changes"])
        self.assertEqual(report["transient_changes"], [])
        self.assertCountEqual([(row["domain"], row["register"]) for row in report["changes"]],
                              [(domain, reg) for _, _, _, domain, reg in edits])
        self.assertIn({"domain": "pll-lock", "block": "dsi0", "register": "local-lock", "baseline": 1, "candidate": 0},
                      report["changes"])
        self.assertIn({"domain": "phy-timing", "block": "dsi1", "register": "phy-timing-13",
                       "baseline": "0000000f", "candidate": "000000ff"}, report["changes"])
        self.assertEqual({(flag["kind"], flag["link"]): set(flag["registers"]) for flag in report["flags"]}, {
            ("pll-loss", "dsi0"): {"local-lock", "pll-ctrl"},
            ("dsc-mismatch", "dsi0"): {"video-compression"},
            ("dsc-mismatch", "dsi1"): {"cmd-compression2"},
            ("phy-drift", "dsi0"): {"phy-status"},
            ("phy-drift", "dsi1"): {"lane-status1", "phy-timing-13"},
            ("host-error", "dsi1"): {"fifo"},
        })

    def test_host_error_registers_are_grouped_by_link(self):
        wake = self.wake
        for name in ("ack-err", "phy-err", "timeout", "err-mask"):
            wake = edit_row(wake, "dsi1 m5:", f"{name} 00000000", f"{name} 00000001")
        wake = edit_row(wake, "dsi0:", "intr 00000000", "intr 00000001")
        report = self.grade(wake)
        self.assertEqual(report["verdict"], "DRIFT")
        self.assertEqual(report["flags"], [
            {"kind": "host-error", "link": "dsi1", "registers": ["ack-err", "phy-err", "timeout"]},
        ])

    def test_cleared_tpg_is_drift_without_tpg_active(self):
        bright = edit_row(self.bright, "dsi0:", "tpg 00000000", "tpg 00030006")
        report = compare(parse_snapshot(bright), parse_snapshot(self.wake))
        self.assertEqual(report["verdict"], "DRIFT")
        self.assertEqual(report["flags"], [])

    def test_pll_loss_either_indication_and_slave_status_drift(self):
        for prefix, old, new, expected in (
            ("dsi0 pll:", "00000051 local-lock 1", "00000050 local-lock 0", ["local-lock"]),
            ("dsi0 phy:", "pll-ctrl 00000001", "pll-ctrl 00000000", ["pll-ctrl"]),
            ("dsi1 pll:", "00000022 local-lock 0", "00000000 local-lock 0", []),
        ):
            with self.subTest(prefix=prefix):
                report = self.grade(edit_row(self.wake, prefix, old, new))
                self.assertEqual(report["verdict"], "DRIFT")
                self.assertEqual(report["flags"], [{"kind": "pll-loss", "link": "dsi0", "registers": expected}]
                                 if expected else [])

    def test_u32_wrap_but_stale_reversed_and_disabled_are_invalid(self):
        report = compare(parse_snapshot(capture(frames=(0xffffffff, 0xfffffffc))),
                         parse_snapshot(capture(2, 700_000, (0, 5))))
        self.assertEqual(report["frame_advance"], {"intf1": 1, "intf2": 9})
        for wake, reason in (
            (self.bright, "cached, stale or reversed"),
            (capture(2, 499_999, (120, 121)), "cached, stale or reversed"),
            (self.wake.replace(BOOT, BOOT[:-1] + "c"), "boot UUID changed"),
            (capture(2, 700_000, (100, 125)), "intf1 frame counter did not advance"),
            (capture(2, 700_000, (124, 100)), "intf2 frame counter did not advance"),
            (edit_row(self.wake, "intf2:", "en 00000001", "en 00000000"), "intf2 timing engine is disabled"),
            (edit_row(self.wake, "intf1:", "status 00000001", "status 00000000"), "intf1 timing engine is disabled"),
        ):
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                self.grade(wake)

    def test_cli_counter_reset_requires_opt_in(self):
        bright = capture(frames=(242673, 242674))
        wake = capture(2, 700_000, (8806, 8807))
        strict = run_grader(bright, wake)
        self.assertEqual(strict.returncode, 2, strict.stdout + strict.stderr)
        self.assertEqual(json.loads(strict.stdout), {
            "verdict": "VOID", "reason": "intf1 frame counter did not advance", "changes": [], "flags": [],
        })
        opted_in = run_grader(bright, wake, "--allow-counter-reset")
        self.assertEqual(opted_in.returncode, 0, opted_in.stdout + opted_in.stderr)
        report = json.loads(opted_in.stdout)
        self.assertEqual(report["verdict"], "PASS")
        self.assertIs(report["counter_reset"], True)
        self.assertEqual(report["frame_advance"], {"intf1": 8806, "intf2": 8807})
        self.assertIn("counter reset", opted_in.stderr)

        wake2 = capture(3, 4_791_667, (9297, 9298))
        followup = compare(parse_snapshot(wake), parse_snapshot(wake2), allow_counter_reset=True)
        self.assertEqual(followup["verdict"], "PASS")
        self.assertEqual(followup["frame_advance"], {"intf1": 491, "intf2": 491})
        self.assertNotIn("counter_reset", followup)

    def test_counter_reset_preserves_other_interface_advance_and_wrap(self):
        candidate = parse_snapshot(capture(2, 700_000, (7, 5)))
        mixed = compare(parse_snapshot(capture(frames=(242673, 0xfffffffc))), candidate, allow_counter_reset=True)
        self.assertEqual(mixed["verdict"], "PASS")
        self.assertIs(mixed["counter_reset"], True)
        self.assertEqual(mixed["frame_advance"], {"intf1": 7, "intf2": 9})
        wrapped = compare(parse_snapshot(capture(frames=(0xffffffff, 0xfffffffc))), candidate, allow_counter_reset=True)
        self.assertEqual(wrapped["verdict"], "PASS")
        self.assertEqual(wrapped["frame_advance"], {"intf1": 8, "intf2": 9})
        self.assertNotIn("counter_reset", wrapped)

    def test_cli_counter_reset_rejects_invalid_pairs(self):
        bright = capture(2, 500_000, (242673, 242674))
        wake = capture(3, 700_000, (8806, 8807))
        for candidate, reason in (
            (capture(3, 700_000, (0, 8807)), "intf1 candidate frame counter must be positive"),
            (capture(3, 700_000, (8806, 0)), "intf2 candidate frame counter must be positive"),
            (capture(3, 700_000, (8806, 242674)), "intf2 frame counter did not advance"),
            (capture(3, 700_000, (8806, 242674 + 2**31)), "intf2 frame counter did not advance"),
            (edit_row(wake, "intf1:", "en 00000001", "en 00000002"), "candidate intf1 timing engine is disabled"),
            (edit_row(wake, "intf2:", "status 00000001", "status 00000002"), "candidate intf2 timing engine is disabled"),
            (wake.replace("snapshot 3", "snapshot 2"), "cached, stale or reversed"),
            (wake.replace("700000 us", "500000 us"), "cached, stale or reversed"),
            (wake.replace(BOOT, BOOT[:-1] + "c"), "boot UUID changed"),
            (wake.replace("0 stores", "1 stores"), "requires 0 stores"),
            (wake.replace("gate open", "gate closed"), "gate is closed"),
        ):
            with self.subTest(reason=reason):
                result = run_grader(bright, candidate, "--allow-counter-reset")
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                report = json.loads(result.stdout)
                self.assertEqual(report["verdict"], "VOID")
                self.assertIn(reason, report["reason"])
                self.assertNotIn("counter_reset", report)

        disabled = edit_row(bright, "intf2:", "en 00000001", "en 00000000")
        result = run_grader(disabled, wake, "--allow-counter-reset")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["reason"], "baseline intf2 timing engine is disabled")

    def test_cli_wake_errors_after_counter_reset(self):
        bright = capture(frames=(242673, 242674))
        wake = capture(2, 700_000, (8806, 8807))
        for link, status, fifo in (("dsi0", "00002008", "88881090"), ("dsi1", "00000008", "99991090")):
            bright = edit_row(bright, link + ":", "status0 00000000", f"status0 {status}")
            bright = edit_row(bright, link + " m5:", "reset 00000000", "reset 00001c00")
            wake = edit_row(wake, link + ":", "status0 00000000", "status0 8000a00f")
            wake = edit_row(wake, link + ":", "tpg 00000000", "tpg 00030006")
            wake = edit_row(wake, link + " m5:", "fifo 00001010", f"fifo {fifo}")
            wake = edit_row(wake, link + " m5:", "clk-status 008067c3", "clk-status 008077c3")

        result = run_grader(bright, wake, "--allow-counter-reset")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["verdict"], "DRIFT")
        self.assertIs(report["counter_reset"], True)
        self.assertCountEqual(report["flags"], [
            {"kind": kind, "link": link, "registers": registers}
            for link in ("dsi0", "dsi1")
            for kind, registers in (("host-error", ["fifo", "status0"]), ("reset-drift", ["reset"]),
                                    ("tpg-active", ["tpg"]))
        ])
        self.assertCountEqual([(row["block"], row["register"]) for row in report["static_changes"]], [
            (link, name) for link in ("dsi0", "dsi1") for name in ("status0", "fifo", "reset", "tpg", "clk-status")
        ])
        self.assertEqual(report["transient_changes"], [])

    def test_invalid_capture(self):
        for text, reason in (
            ("gop-shot: EBUSY\n", "EBUSY"),
            (self.wake + "sysctl: Device or resource busy\n", "EBUSY"),
            ("", "empty"),
            (self.wake.split("vfp switch")[0], "expected 1 vfp switch"),
            (self.wake[:-10], "ends mid-row"),
            (self.wake.split("\n", 1)[1], "expected 1 boot"),
            (self.wake.replace(BOOT, "missing"), "boot.*malformed"),
            (self.wake.replace("0 stores", "1 stores"), "requires 0 stores"),
            (self.wake.replace("238 loads", "0 loads"), "positive load count"),
            (self.wake.replace("gate open", "gate closed"), "gate is closed"),
            (self.wake + "snapshot: 1 page maps failed so far (those words read as ffffffff)\n", "page-map"),
            (self.wake.replace("dsc-active 00000003", "dsc-active ffffffff"), "unreadable"),
            (self.wake.replace("dsi1 m5: cmd-cfg0", "unrelated: cmd-cfg0"), "expected 2 dsi1 m5"),
            (self.wake.replace("dsi1: ctrl", "dsi1: not read (its byte or pixel clock is off)\nctrl"), "dsi1:.*malformed"),
            (self.wake.replace("dsi0 m5: vid-cfg0", "dsi0 m5: host and PHY probe skipped\nvid-cfg0"), "m5:.*malformed"),
            (edit_row(self.wake, "dsi0 phy timing", " 0000000f", ""), "timing.*truncated"),
            (edit_row(self.wake, "dsi0 phy timing", " 0000000f", " 0000000f 00000000"), "timing.*malformed"),
            (self.wake.replace("status 00000051 local-lock 1", "status unread"), "dsi0 pll:.*unread"),
            (self.wake.replace("status 00000022 local-lock 0", "status 00000022 local-lock 1"), "PLL status and local-lock disagree"),
            (self.wake + "dsi0 pll: status 00000051 local-lock 1\n", "expected 1 dsi0 pll"),
        ):
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                parse_snapshot(text)

    def test_cli_json_and_one_line_summary_for_all_exit_codes(self):
        for text, code, verdict in (
            (self.wake, 0, "PASS"),
            (self.wake.replace("video-compression 03200b81", "video-compression 00000000"), 1, "DRIFT"),
            (self.blanking, 1, "DRIFT"),
            ("EBUSY\n", 2, "VOID"),
            (None, 2, "VOID"),
        ):
            with self.subTest(code=code, text_present=text is not None):
                result = run_grader(self.bright, text)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                self.assertEqual(json.loads(result.stdout)["verdict"], verdict)
                self.assertEqual(len(result.stderr.splitlines()), 1)
                self.assertTrue(result.stderr.startswith(verdict))
                if text == self.blanking:
                    self.assertIn("0 unexplained, 9 scan-phase", result.stderr)


if __name__ == "__main__":
    unittest.main()
