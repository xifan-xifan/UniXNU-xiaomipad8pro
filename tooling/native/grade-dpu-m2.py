#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Grade offline DPU M2 RAM captures. See docs/drivers/dpu-m2-analysis.md."""

import argparse
from dataclasses import dataclass
from fractions import Fraction
import json
from pathlib import Path
import re


MIN_US = 30_000_000
MIN_FLIPS = 300
MIN_WC = 100
UNDERRUN_MASK = (1 << 26) | (1 << 28)
BOOT_ID = r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}"
CLEAN_ROWS = (
    ("clean: ", "shown maps evictions passes init skip failed alias-failed without"),
    ("clean: fallback ", "forced handoff unknown-mode mode attrs backing init-clean"),
    ("clean: ", "pixel-us check-us"),
    ("clean: m2 ", "policy writes latched full to-boot underruns pending serving"),
)
GOP_FLIPS = re.compile(
    r"NativeGopDisplay: flip mode: (\d+) swaps flipped \(no copy, no refresh copies; "
    r"\d+ '&BGA' plane 0\), (\d+) copied \("
)
CONSOLE_FAILURES = (
    ("DPU underrun", r"PianoDpu: (?:flip:|watch:) underrun [0-9a-f]+"),
    ("DPU stopped serving", r"PianoDpu: (?:revert:|B1b off for the boot:)"),
    ("GOP damage audit mismatch",
     r"NativeGopDisplay: damage: [^\n]*(?:left out a change|[1-9][0-9]* differed|damage off for this boot)"),
    ("GOP fence timeout", r"NativeGopDisplay: (?:the GPU had not finished |[1-9][0-9]* jobs left undrawn |"
     r"flip mode: the boot framebuffer \(an older frame\) is back)"),
)


@dataclass
class Snapshot:
    boot: str
    pid: int
    serial: int
    time_us: int
    hz: int
    frames: tuple
    underruns: int
    counters: dict


def require(condition, message):
    if not condition:
        raise ValueError(message)


def row(data, pattern, name):
    matches = re.findall("^" + pattern + "$", data, re.M)
    require(len(matches) == 1, f"expected one complete {name} row")
    return matches[0]


def snapshot(data):
    boot = row(data, f"boot ({BOOT_ID})", "boot").lower()
    pid = int(row(data, r"windowserver-pid ([1-9][0-9]*)", "WindowServer PID"))
    serial, time_us, loads = map(int, row(
        data, r"snapshot (\d+) at (\d+) us: (\d+) loads, 0 stores; gate open \(.*", "open snapshot"
    ))
    require(serial > 0 and time_us > 0 and loads > 0, "snapshot lacks a live read")
    hz = int(row(data, r"mode: live (60|90|120|144) Hz \(.*", "live mode"))
    frames = []
    for link in (1, 2):
        en, frame = row(data, rf"intf{link}: en ([0-9a-f]{{8}}) .* frame (\d+) line .*", f"INTF{link}")
        require(int(en, 16) & 1 and int(frame) <= 0xffffffff, f"INTF{link} is unavailable")
        frames.append(int(frame))
    status = row(data, r"ctl\d+: .* top intr en [0-9a-f]{8} status ([0-9a-f]{8}); .*", "TOP interrupt")
    counters = {}
    for prefix, fields in CLEAN_ROWS:
        names = fields.split()
        pattern = " ".join(f"{name} " + (r"([0-9a-f]{8})" if name == "pending" else r"(\d+)") for name in names)
        values = row(data, re.escape(prefix) + pattern, prefix + names[0])
        counters.update((name, int(value, 16 if name == "pending" else 10)) for name, value in zip(names, values))
    require(counters["serving"] in (0, 1), "invalid DPU serving state")
    return Snapshot(boot, pid, serial, time_us, hz, tuple(frames),
                    (int(status, 16) | counters.pop("pending")) & UNDERRUN_MASK, counters)


def read_capture(directory, spec, key):
    value = spec.get(key)
    require(isinstance(value, str) and value.strip(), f"missing {key} filename")
    path = directory / value
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ValueError(f"{key} file unreadable ({type(error).__name__})") from error


def count(spec, name):
    value = spec.get(name)
    require(type(value) is int and value >= 0, f"{name} must be a nonnegative integer")
    return value


@dataclass
class Arm:
    image: str
    workload: str
    before: Snapshot
    after: Snapshot
    elapsed: int
    delta: dict
    gop_before: tuple
    gop_after: tuple

    @property
    def gop_delta(self):
        return tuple(b - a for a, b in zip(self.gop_before, self.gop_after))


class Grade:
    def __init__(self):
        self.failures = []
        self.invalid = []

    def check(self, condition, message):
        if not condition:
            self.failures.append(message)

    def evidence(self, condition, message):
        if not condition:
            self.invalid.append(message)

    def arm(self, label, policy, spec, directory):
        try:
            return self.read_arm(label, policy, spec, directory)
        except ValueError as error:
            self.invalid.append(f"{label} {error}")
            return None

    def read_arm(self, label, policy, spec, directory):
        require(isinstance(spec, dict), "missing capture object")
        crashes = count(spec, "windowserver_crashes")
        self.check(crashes == 0, f"{label} WindowServer crash reports={crashes}")
        mismatches = count(spec, "optical_mismatches")
        self.check(mismatches == 0, f"{label} optical mismatches={mismatches}")
        checks = count(spec, "optical_checks")
        self.evidence(checks > 0 and mismatches <= checks, f"{label} missing or inconsistent optical evidence")

        console = read_capture(directory, spec, "console")
        for reason, pattern in CONSOLE_FAILURES:
            self.check(not re.search(pattern, console), f"{label} console reported {reason}")
        self.evidence(spec.get("console_complete") is True, f"{label} console completeness unconfirmed")
        before = snapshot(read_capture(directory, spec, "before"))
        after = snapshot(read_capture(directory, spec, "after"))
        start, end = before.counters, after.counters
        elapsed = after.time_us - before.time_us
        require(before.boot == after.boot, "boot changed within the arm")
        self.check(before.pid == after.pid, f"{label} WindowServer PID changed")
        require(after.serial > before.serial and elapsed > 0, "snapshot pair is stale or reversed")
        delta = {key: end[key] - start[key] for key in start if key not in ("policy", "serving")}
        require(min(delta.values()) >= 0, "DPU counters decreased")
        self.check(start["serving"] == 0 or end["serving"] == 1, f"{label} DPU stopped serving")
        self.evidence(start["serving"] == 1, f"{label} DPU was unready at the start")
        self.check(delta["underruns"] == 0 and not (before.underruns or after.underruns),
                   f"{label} DPU underruns={delta['underruns']}, pending endpoints={before.underruns:#x}/{after.underruns:#x}")
        require(start["policy"] == end["policy"] == policy and delta["writes"] == 0,
                f"expected stable strict policy {policy} for the entire arm")
        self.evidence(elapsed >= MIN_US, f"{label} capture shorter than {MIN_US // 1_000_000} s")
        self.evidence(before.hz == after.hz and all(
            0 < (a - b) % (1 << 32) < (1 << 31) for b, a in zip(before.frames, after.frames)
        ), f"{label} mode changed or a link did not advance")
        self.evidence(delta["latched"] >= MIN_FLIPS, f"{label} fewer than {MIN_FLIPS} latched flips")
        for key in ("failed", "alias-failed", "without", "backing", "init-clean"):
            self.check(delta[key] == 0, f"{label} {key}={delta[key]}")

        shown = delta["shown"]
        self.check(delta["latched"] <= shown and shown == delta["skip"] + delta["init"] + delta["full"],
                   f"{label} shown accounting disagrees with the flip and clean actions")
        extra_passes = delta["passes"] - delta["full"] - delta["init"]
        if policy == 0:
            self.check(delta["forced"] == delta["full"] == shown and extra_passes == 0,
                       f"{label} expected one forced full clean per shown flip")
        else:
            modes = sum(delta[key] for key in ("unknown-mode", "mode", "attrs"))
            self.check(delta["forced"] == 0 and delta["full"] == modes + delta["handoff"],
                       f"{label} strict full cleans disagree with fallback reasons")
            self.check(0 <= extra_passes <= modes, f"{label} clean passes disagree with shown actions")
        self.check(delta["passes"] == 0 or delta["pixel-us"] > 0,
                   f"{label} positive clean pass count without measurable pixel-clean time")

        reports = [tuple(map(int, match)) for match in GOP_FLIPS.findall(console)]
        require(len(reports) >= 2, "expected at least two complete GOP flip summaries")
        for first, second in zip(reports, reports[1:]):
            require(all(b >= a for a, b in zip(first, second)), "GOP counters decreased")
        gop_flips, gop_copies = (b - a for a, b in zip(reports[0], reports[-1]))
        require(shown <= gop_flips and gop_flips * 4 <= shown * 5,
                "GOP reports must bracket the snapshots with at most 25% extra flips")

        image, workload = spec.get("image_sha256"), spec.get("workload")
        require(isinstance(image, str) and re.fullmatch(r"[0-9a-fA-F]{64}", image), "missing image SHA-256")
        require(isinstance(workload, str) and workload.strip(), "missing workload ID")
        print(f"{label} shown={shown} latched={delta['latched']} init={delta['init']} skip={delta['skip']} "
              f"full={delta['full']} passes={delta['passes']} clean_us={delta['pixel-us']} check_us={delta['check-us']} "
              f"gop_flips={gop_flips} gop_copies={gop_copies} to_boot={delta['to-boot']} optical_checks={checks}")
        print(f"{label} clean reasons " + " ".join(f"{key}={delta[key]}" for key in CLEAN_ROWS[1][1].split()))
        return Arm(image.lower(), workload, before, after, elapsed, delta, reports[0], reports[-1])

    def compare(self, control, treatment):
        a, b = control.delta, treatment.delta
        require(control.image == treatment.image and control.workload == treatment.workload,
                "A/B image or workload IDs differ")
        require(control.before.hz == control.after.hz == treatment.before.hz == treatment.after.hz,
                "A/B modes differ")
        if control.before.boot == treatment.before.boot:
            require(control.after.serial < treatment.before.serial and control.after.time_us < treatment.before.time_us
                    and all(control.after.counters[key] <= treatment.before.counters[key] for key in a),
                    "same-boot arms are out of order, overlap or have reset counters")
            self.check(control.after.pid == treatment.before.pid, "WindowServer PID changed between same-boot arms")
            require(treatment.before.counters["writes"] > control.after.counters["writes"],
                    "missing same-boot switch to treatment")
            require(all(a <= b for a, b in zip(control.gop_after, treatment.gop_before)),
                    "same-boot GOP summaries overlap or have reset counters")
        for name, x, y in (("duration", control.elapsed, treatment.elapsed), ("shown flips", a["shown"], b["shown"])):
            require(max(x, y) * 4 <= min(x, y) * 5, f"A/B {name} differ by more than 25%")
        # There must be enough observed frames before rate comparisons can decide the grade.
        if min(a["latched"], b["latched"], a["shown"], b["shown"]) < MIN_FLIPS or min(control.elapsed, treatment.elapsed) < MIN_US:
            return
        a_flips, a_copies = control.gop_delta
        b_flips, b_copies = treatment.gop_delta
        gop_a = Fraction(a_copies, a_flips + a_copies)
        gop_b = Fraction(b_copies, b_flips + b_copies)
        self.check(b_copies <= a_copies and gop_b <= gop_a and b["to-boot"] <= a["to-boot"],
                   "B copy fallback count, share or DPU returns to the boot framebuffer increased")

        eligible = b["skip"] + b["init"]
        self.evidence(eligible >= MIN_WC, f"B fewer than {MIN_WC} verified-WC shown flips ({eligible})")
        if eligible == 0 or a["pixel-us"] == 0:
            return
        skip_ratio = Fraction(b["skip"], eligible)
        a_mean = Fraction(a["pixel-us"], a["shown"])
        b_mean = Fraction(b["pixel-us"], b["shown"])
        reduction = 1 - b_mean / a_mean
        saved = a_mean * b["shown"] - b["pixel-us"]
        print(f"B verified_wc={eligible} skip_ratio={float(skip_ratio):.2%} clean_reduction={float(reduction):.2%} "
              f"clean_us_per_shown={float(a_mean):.2f}/{float(b_mean):.2f} clean_us_saved_at_B_volume={float(saved):.0f}")
        if eligible >= MIN_WC:
            self.check(skip_ratio >= Fraction(9, 10), "B verified-WC skip ratio below 90%")
            self.check(reduction >= Fraction(1, 2), "B pixel-clean time per shown flip fell by less than 50%")

    def verdict(self):
        for message in self.failures:
            print(f"FAIL {message}")
        for message in self.invalid:
            print(f"VOID {message}")
        if self.failures:
            return "FAIL", 1
        if self.invalid:
            return "VOID", 2
        return "PASS", 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="JSON manifest with control and treatment captures")
    args = parser.parse_args()
    grade = Grade()
    try:
        manifest = json.loads(args.capture.read_text(encoding="utf-8"))
        require(isinstance(manifest, dict), "expected a capture manifest object")
        a = grade.arm("A", 0, manifest.get("control"), args.capture.parent)
        b = grade.arm("B", 1, manifest.get("treatment"), args.capture.parent)
        if a and b:
            grade.compare(a, b)
    except (OSError, UnicodeError) as error:
        grade.invalid.append(f"manifest unreadable ({type(error).__name__})")
    except ValueError as error:
        grade.invalid.append(str(error))
    verdict, status = grade.verdict()
    print(f"M2 {verdict}")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
