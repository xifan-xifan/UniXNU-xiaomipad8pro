#!/usr/bin/env python3
"""Grade PianoDpu clock and apps DRV-2 evidence in paired m120c snapshots."""

import argparse
from dataclasses import dataclass
from pathlib import Path
import re


BCMS = ("MM0", "SH0", "ACV", "MC0")
ARCS = ("mmcx.lvl", "mxc.lvl")
OPP_HZ = 575_000_000
OPP_TOLERANCE_HZ = 1_000


@dataclass(frozen=True)
class Bcm:
    addr: int
    unit: int
    width: int
    vcd: int
    x: int
    y: int

    @property
    def identity(self):
        return self.addr, self.unit, self.width, self.vcd


class Snapshot:
    def __init__(self, path):
        self.rows = {}
        self.stamp = None
        self.mode = None
        self.error = ""
        try:
            data = path.read_text(errors="replace")
            stamps = re.findall(r"^snapshot (\d+) at (\d+) us:", data, re.M)
            if len(stamps) != 1:
                raise ValueError("expected one snapshot header")
            self.stamp = tuple(map(int, stamps[0]))
            modes = re.findall(r"^mode: live (\d+) Hz ", data, re.M)
            if len(modes) == 1:
                self.mode = int(modes[0])
            for line in data.splitlines():
                if not line.startswith("clkread_"):
                    continue
                key, *fields = line.split()
                if key in ("clkread_bcm", "clkread_arc"):
                    key = fields.pop(0)
                row = dict(field.split("=", 1) for field in fields)
                if key in self.rows or len(row) != len(fields):
                    raise ValueError(f"duplicate fields or row for {key}")
                self.rows[key] = row
        except (OSError, ValueError, IndexError) as exc:
            self.error = f"{path.name}: {exc}"
            self.rows.clear()
            self.stamp = None
            self.mode = None

    def ok(self, name):
        row = self.rows.get(name, {})
        return row if row.get("status") == "ok" else {}

    def number(self, name, key):
        try:
            return int(self.ok(name)[key])
        except (KeyError, ValueError):
            return None

    def response(self, name, slave):
        row = self.ok(name)
        scope = self.rows.get("clkread_rpmh", {})
        if scope.get("voter") != "apps-drv2":
            return None
        try:
            addr, raw, status = (int(row[key], 16) for key in ("addr", "raw", "cmd_status"))
            complete = 0 <= status < 0xffffffff and status & 0x10100 == 0x10100
            if addr >> 16 == slave and not addr & 3 and 0 <= raw < 0xffffffff and complete and row["rc"] == "0":
                return addr, raw
        except (KeyError, ValueError):
            pass
        return None

    def bcm(self, name):
        response = self.response(name, 5)
        if response:
            addr, raw = response
            try:
                row = self.ok(name)
                unit, width, vcd = (int(row[key]) for key in ("unit", "width", "vcd"))
                x, y = (raw >> 14) & 0x3fff, raw & 0x3fff
                if (x, y) == (int(row["x"]), int(row["y"])):
                    return Bcm(addr, unit, width, vcd, x, y)
            except (KeyError, ValueError):
                pass
        return None

    def arc(self, name):
        response = self.response(name, 3)
        hlvl = self.number(name, "hlvl")
        vlvl = self.number(name, "vlvl")
        if response and response[1] == hlvl and 0 <= hlvl < 16 and vlvl is not None and 0 <= vlvl <= 0xffff:
            return vlvl
        return None


def grade(tag, passed, evidence):
    print(f"{tag} {evidence} {'PASS' if passed else 'FAIL'}")
    return passed


def fresh(before, after):
    return (before.stamp is not None and after.stamp is not None
            and after.stamp[0] > before.stamp[0] and after.stamp[1] - before.stamp[1] >= 100_000)


def mm0_field(kbps, bcm, channels):
    def divide(value, divisor):
        return max(1, value // divisor) if value else 0

    scaled = divide(kbps * bcm.width, 32 * channels) * 1000
    return min(0x3fff, divide(scaled, bcm.unit))


def live_mm0(snap):
    vote = snap.bcm("MM0")
    return vote if vote and vote.unit > 0 and vote.width > 0 else None


def matches_144(vote):
    return vote and (vote.x, vote.y) == (mm0_field(5_109_350, vote, 2), mm0_field(4_257_792, vote, 1))


def arm(hz, before, after):
    samples = (before, after)
    valid = grade("CLKREAD_FRESH", fresh(*samples), before.error or after.error or f"{before.stamp} -> {after.stamp}")
    valid &= grade("CLKREAD_MODE", before.mode == after.mode == hz, f"snapshot mode {before.mode} -> {after.mode} expected={hz}")
    mdp = [snap.number("clkread_mdp", "hz") for snap in samples]
    pll = [snap.number("clkread_pll0", "hz") for snap in samples]
    clock_ok = all(rate is not None and abs(rate - OPP_HZ) <= OPP_TOLERANCE_HZ for rate in mdp)
    valid &= grade("CLKREAD_MDP", clock_ok and all(rate is not None and rate > 0 for rate in pll), f"mdp_hz={mdp} pll0_hz={pll}")
    mm0 = [live_mm0(snap) for snap in samples]
    mm0_ok = all(matches_144(vote) for vote in mm0) if hz == 144 else all(mm0)
    valid &= grade("CLKREAD_MM0", mm0_ok, f"apps-drv2 {hz}Hz {mm0}")
    for name in BCMS[1:]:
        votes = [snap.bcm(name) for snap in samples]
        valid &= grade(f"CLKREAD_{name}", all(votes), f"apps-drv2 {votes}")
    for name in ARCS:
        levels = [snap.arc(name) for snap in samples]
        valid &= grade(f"CLKREAD_{name.split('.')[0].upper()}", None not in levels, f"apps-drv2 vlvl={levels}")
    crm = [snap.rows.get("clkread_cesta", {}) for snap in samples]
    valid &= grade("CLKREAD_CESTA", all(row.get("status") == "ok" and row.get("enabled") in {"0", "1"} for row in crm), f"{crm}")
    print("CLKREAD_AGGREGATE all-voter BCM allocations, ARC corners and CESTA owner unavailable INFO")
    return valid


def compare(control, treatment):
    valid = grade("CLKREAD_PAIR_FRESH", fresh(*control) and fresh(*treatment), "two distinct captures per boot")
    modes = [snap.mode for snap in (*control, *treatment)]
    valid &= grade("CLKREAD_PAIR_MODE", modes == [120, 120, 144, 144], f"snapshot modes={modes}")
    mm0 = [(live_mm0(low), live_mm0(high)) for low, high in zip(control, treatment)]
    ab_raised = all(low and matches_144(high) and low.identity == high.identity and high.x > low.x for low, high in mm0)
    valid &= grade("CLKREAD_MM0_AB_RAISED", ab_raised, f"apps-drv2 120->144 {mm0}")
    mdp = [(low.number("clkread_mdp", "hz"), high.number("clkread_mdp", "hz")) for low, high in zip(control, treatment)]
    clock_ok = all(None not in pair and all(abs(rate - OPP_HZ) <= OPP_TOLERANCE_HZ for rate in pair) for pair in mdp)
    valid &= grade("CLKREAD_MDP_120_144", clock_ok, f"120->144 mdp_hz={mdp}")
    for name in ARCS:
        levels = [(low.arc(name), high.arc(name)) for low, high in zip(control, treatment)]
        addresses_match = all(low.ok(name).get("addr") == high.ok(name).get("addr") for low, high in zip(control, treatment))
        no_drop = addresses_match and all(low is not None and high is not None and high >= low for low, high in levels)
        valid &= grade(f"CLKREAD_NO_{name.split('.')[0].upper()}_DROP", no_drop, f"apps-drv2 120->144 vlvl={levels}")
    print("CLKREAD_AGGREGATE all-voter BCM allocations, ARC corners and CESTA owner unavailable INFO")
    return valid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    one = subparsers.add_parser("arm", help="grade two snapshots from one fixed-rate boot")
    one.add_argument("hz", type=int, choices=(120, 144))
    one.add_argument("snapshots", type=Path, nargs=2, metavar="SNAPSHOT")
    pair = subparsers.add_parser("compare", help="grade matching before/after snapshots from fixed 120 and fixed 144")
    pair.add_argument("control", type=Path, nargs=2, metavar="SNAPSHOT_120")
    pair.add_argument("treatment", type=Path, nargs=2, metavar="SNAPSHOT_144")
    args = parser.parse_args()
    if args.command == "arm":
        passed = arm(args.hz, *map(Snapshot, args.snapshots))
    else:
        passed = compare(tuple(map(Snapshot, args.control)), tuple(map(Snapshot, args.treatment)))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
