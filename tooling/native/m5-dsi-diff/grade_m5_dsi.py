#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare two saved PianoDpu M5 register snapshots offline."""

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
from string import Formatter
import sys


BOOT_ID = r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}"
BUSY = re.compile(r"\bEBUSY\b|\bresource busy\b", re.IGNORECASE)
U32 = 1 << 32
ROW_TYPES = {
    "": (r"[0-9a-fA-F]{8}", 16),
    "byte": (r"[0-9a-fA-F]{2}", 16),
    "uint": (r"[0-9]{1,20}", 10),
    "ctl": (r"-1|[0-5]", 10),
    "bit": (r"[01]", 10),
    "boot": (BOOT_ID, None),
    "text": (r".+?", None),
}


def words(names):
    return " ".join(f"{name} {{{name}}}" for name in names.split())


# Keep these rows in the order of snapshot::format in DpuSnapshot.h.
DSI_HOST = (
    words("ctrl status0 total active-h active-v")
    + "; timing shadow +1e4 {shadow-1e4} +1e8 {shadow-1e8} +1ec {shadow-1ec}; "
    + words("cmd-dma trig") + " (dma trigger {:text}, block-in-frame {:bit}) "
    + words("intr tpg") + " +390 {dma-ctrl1}; clk-ctrl {clk-ctrl} lanes {lane-status}/{lane-ctrl}; "
    + words("video-compression")
)
DSI_M5 = (
    words("vid-cfg0 vid-cfg1 hsync vsync-hpos vsync-vpos lp-timer hs-timer clkout eot lane-swap t-clk-pre-extend")
    + " cmd-compression {cmd-compression}/{cmd-compression2} "
    + words("clk-status err-mask ack-err fifo phy-err timeout")
)
PHY_STATUS = words("clk-cfg0 clk-cfg1 rbuf-ctrl phy-status lane-status0 lane-status1")
PHY_CTRL = words("ctrl0 ctrl1 ctrl2 ctrl3 ctrl5 lane-cfg0 lane-cfg1 lane-ctrl0 lane-ctrl1 pll-ctrl")
PHY_TIMING = tuple(f"phy-timing-{i}" for i in range(14))
DSI_DOMAINS = {
    "dsi-ctrl": "ctrl cmd-dma trig tpg dma-ctrl1 vid-cfg0 vid-cfg1 eot cmd-cfg0 cmd-cfg1 dma-length reset misr-ctrl",
    "dsi-timing": "total active-h active-v shadow-1e4 shadow-1e8 shadow-1ec hsync vsync-hpos vsync-vpos "
                  "lp-timer hs-timer clkout t-clk-pre-extend",
    "dsc-compression": "video-compression cmd-compression cmd-compression2",
    "phy-clk-lane": "clk-ctrl lane-status lane-ctrl lane-swap clk-status clk-cfg0 clk-cfg1 rbuf-ctrl phy-status "
                    "lane-status0 lane-status1 ctrl0 ctrl1 ctrl2 ctrl3 ctrl5 lane-cfg0 lane-cfg1 lane-ctrl0 lane-ctrl1",
    "phy-timing": " ".join(PHY_TIMING),
    "pll-lock": "pll-ctrl pll-status local-lock",
    "errors-fifo": "status0 intr err-mask ack-err fifo phy-err timeout",
}
DSI_REGISTER_DOMAIN = {name: domain for domain, names in DSI_DOMAINS.items() for name in names.split()}
HOST_ERROR_WORDS = {"status0", "fifo", "ack-err", "phy-err", "timeout"}
PHY_STATUS_WORDS = {"phy-status", "lane-status", "lane-status0", "lane-status1"}
LINKS = ("dsi0", "dsi1")
INTERFACES = ("intf1", "intf2")
# v131y12u-ram2 bright controls on the 2136-line SM8750 Piano panel.
ACTIVE_LINES = 2136
PHASE_CTL_WORDS = {"flush", "top-intr-en", "top-intr-status"}
PHASE_DSI_WORDS = {
    "lane-status": (0x00001f00, 0x00001f1f),
    "clk-status": (0x008067c3, 0x00006343),
    "fifo": (0x00001010, 0x11111010),
}


def match_row(line, template, label):
    pattern, captures = [], []
    for literal, name, kind, _ in Formatter().parse(template):
        pattern.append(re.escape(literal))
        if name is None:
            continue
        value_pattern, base = ROW_TYPES[kind]
        if name:
            pattern.append(f"({value_pattern})")
            captures.append((name, base))
        else:
            pattern.append(f"(?:{value_pattern})")
    match = re.fullmatch("".join(pattern), line)
    if not match:
        raise ValueError(f"{label} row malformed, unread or truncated")
    return {name: int(value, base) if base else value
            for (name, base), value in zip(captures, match.groups())}


def read_rows(lines, prefix, *templates, selector=None):
    if selector:
        rows = [line for line in lines if re.match(selector, line)]
    else:
        rows = [line for line in lines if line.startswith(prefix)]
    label = prefix.rstrip()
    if len(rows) != len(templates):
        raise ValueError(f"expected {len(templates)} {label} row(s), found {len(rows)}")
    return [match_row(line, template, label) for line, template in zip(rows, templates)]


def snapshot_line(line):
    line = line.strip().removeprefix("machdep.piano_dpu_snapshot: ")
    line = line.split("PianoDpu: snap: ", 1)[-1]
    if line.startswith("kern.bootsessionuuid: "):
        return "boot " + line.removeprefix("kern.bootsessionuuid: ")
    return line


@dataclass
class ScanPosition:
    line: int
    vtotal: int

    @property
    def phase(self):
        if not (ACTIVE_LINES < self.vtotal < U32 and 0 <= self.line < self.vtotal):
            return "unknown"
        return "active" if self.line < ACTIVE_LINES else "blanking"

    def metadata(self):
        return {"line": self.line, "vtotal": self.vtotal, "phase": self.phase}


@dataclass
class Snapshot:
    boot: str
    serial: int
    time_us: int
    loads: int
    frames: dict[str, int]
    registers: dict[tuple[str, str], int]
    timing: dict[str, ScanPosition]

    @property
    def phase(self):
        a, b = (self.timing[block].phase for block in INTERFACES)
        if "unknown" in (a, b):
            return "unknown"
        return a if a == b else "mixed"

    def metadata(self):
        return {"serial": self.serial, "time_us": self.time_us, "loads": self.loads, "stores": 0,
                "phase": self.phase, "timing": {block: position.metadata() for block, position in self.timing.items()}}


# DSI_HOST_LP_TIMER is programmed to 0xffffffff (max/disabled LP RX timeout) by firmware on live SM8750 panels.
ALLOWED_ALL_ONES_REGISTERS = {"lp-timer"}


def parse_snapshot(text):
    if BUSY.search(text):
        raise ValueError("capture reported EBUSY")
    if not text.endswith(("\n", "\r")):
        raise ValueError("snapshot is empty or ends mid-row")
    lines = [snapshot_line(line) for line in text.splitlines()]
    boot, = read_rows(lines, "boot", "boot {id:boot}", selector=r"boot(?: |$)")
    boot_id = boot["id"].lower()
    if not boot_id.replace("-", "").strip("0"):
        raise ValueError("boot UUID is nil")
    header, = read_rows(
        lines, "snapshot ",
        "snapshot {serial:uint} at {time-us:uint} us: {loads:uint} loads, {stores:uint} stores; gate {gate:text} "
        "(gcc disp ahb {gcc-disp-ahb}, gdsc {gdsc}, ahb {ahb}, mdp {mdp}, byte {byte0}/{byte1}, pclk {pclk0}/{pclk1}); "
        "panel id io {panel-id-io} ({:text}, rate group {:text})",
    )
    if header["stores"] or not 0 < header["loads"] < U32:
        raise ValueError("snapshot requires 0 stores and a positive load count")
    if header["gate"] != "open":
        raise ValueError("display clock gate is closed")
    if not (0 < header["serial"] < 1 << 64 and 0 < header["time-us"] < 1 << 64):
        raise ValueError("snapshot requires a live serial and timestamp")
    if any(re.match(r"snapshot: [1-9][0-9]* page maps failed", line) for line in lines):
        raise ValueError("snapshot reported MMIO page-map failures")
    read_rows(lines, "vfp switch", "vfp switch (DpuRefresh.h): {:text}")

    ctl, = read_rows(
        lines, "ctl",
        "ctl{index:ctl}: top {top} intf-active {intf-active} master {master} lms {lms:byte} "
        "flush {flush} intf-flush {intf-flush} dspp-flush {dspp0-flush}/{dspp1-flush}; "
        "top intr en {top-intr-en} status {top-intr-status}; mdss intr {mdss-intr}; path {:text}",
        selector=r"ctl-?[0-9]+:",
    )
    scanout = []
    if ctl["index"] >= 0:
        scanout.append(f"scanout ctl{ctl['index']}: " + words("dsc-active dsc-flush pipe-active fetch-active"))
    for row in read_rows(lines, "scanout ctl", *scanout):
        ctl.update(row)

    registers = {("ctl", name): value for name, value in ctl.items()}
    for name in "gcc-disp-ahb gdsc ahb mdp byte0 byte1 pclk0 pclk1 panel-id-io".split():
        registers["gate", name] = header[name]
    frames, timing = {}, {}
    for block in INTERFACES:
        intf, = read_rows(
            lines, block + ":",
            block + ": " + words("en status config hsync period vwidth") + " dv {dv-start}-{dv-end} "
            + words("dhctl skew prog-fetch intr") + "; counters en {counters-en} frame {frame:uint} line {line:uint} "
            "(htotal {:uint} vtotal {vtotal:uint}); " + words("config2 data-hctl"),
        )
        frames[block] = intf.pop("frame")
        if frames[block] >= U32:
            raise ValueError(f"{block} frame is outside the 32-bit counter range")
        timing[block] = ScanPosition(intf.pop("line"), intf.pop("vtotal"))
        registers.update(((block, name), value) for name, value in intf.items())

    for link in LINKS:
        sections = (
            (link + ":", DSI_HOST),
            (link + " m5:", DSI_M5, words("cmd-cfg0 cmd-cfg1 dma-length reset misr-ctrl")),
            (link + " phy:", PHY_STATUS, PHY_CTRL),
            (link + " phy timing 0..13:", " ".join("{" + name + "}" for name in PHY_TIMING)),
            (link + " pll:", "status {pll-status} local-lock {local-lock:bit}"),
        )
        for prefix, *templates in sections:
            for row in read_rows(lines, prefix, *(f"{prefix} {template}" for template in templates)):
                registers.update(((link, name), value) for name, value in row.items())
        if registers[link, "pll-status"] & 1 != registers[link, "local-lock"]:
            raise ValueError(f"{link} PLL status and local-lock disagree")

    for (block, name), value in registers.items():
        if value == U32 - 1 and name not in ALLOWED_ALL_ONES_REGISTERS:
            raise ValueError(f"{block} {name} is unreadable (ffffffff)")
    return Snapshot(boot_id, header["serial"], header["time-us"], header["loads"], frames, registers, timing)


def register_domain(block, name):
    if block in LINKS:
        return DSI_REGISTER_DOMAIN[name]
    if block == "gate" and name.startswith(("byte", "pclk")):
        return "phy-clk-lane"
    if block == "ctl" and name.startswith("dsc-"):
        return "dsc-compression"
    if name in {"intr", "top-intr-en", "top-intr-status", "mdss-intr"}:
        return "errors-fifo"
    return "intf"


def register_value(block, name, value):
    if value is None or name in {"index", "local-lock"}:
        return value
    return f"{value:02x}" if (block, name) == ("ctl", "lms") else f"{value:08x}"


def is_phase_change(baseline, candidate, block, name, old, new):
    if block == "ctl" and name in PHASE_CTL_WORDS:
        ctl = baseline.registers["ctl", "index"]
        return (ctl >= 0 and ctl == candidate.registers["ctl", "index"]
                and {baseline.phase, candidate.phase} == {"active", "blanking"})
    if block in LINKS and name in PHASE_DSI_WORDS:
        intf = INTERFACES[LINKS.index(block)]
        active, blanking = PHASE_DSI_WORDS[name]
        return ((baseline.timing[intf].phase, old), (candidate.timing[intf].phase, new)) in (
            (("active", active), ("blanking", blanking)),
            (("blanking", blanking), ("active", active)),
        )
    return False


def compare(baseline, candidate, allow_counter_reset=False):
    """Compare parsed captures, using the post-reset count for a restarted counter's advance."""
    if baseline.boot != candidate.boot:
        raise ValueError("boot UUID changed")
    if candidate.serial <= baseline.serial or candidate.time_us <= baseline.time_us:
        raise ValueError("snapshot pair is cached, stale or reversed")

    advances = {}
    counter_reset = False
    for block, frame in baseline.frames.items():
        for role, snap in (("baseline", baseline), ("candidate", candidate)):
            if not (snap.registers[block, "en"] & snap.registers[block, "status"] & 1):
                raise ValueError(f"{role} {block} timing engine is disabled")
        current = candidate.frames[block]
        if allow_counter_reset and current <= 0:
            raise ValueError(f"{block} candidate frame counter must be positive")
        advance = (current - frame) % U32
        if not 0 < advance < U32 // 2:
            if not (allow_counter_reset and current < frame):
                raise ValueError(f"{block} frame counter did not advance")
            advance = current
            counter_reset = True
        advances[block] = advance

    before, after = baseline.registers, candidate.registers
    changes, static_changes, transient_changes = [], [], []
    flags = []
    for block, name in sorted(before.keys() | after.keys()):
        old, new = before.get((block, name)), after.get((block, name))
        if old != new:
            row = {"domain": register_domain(block, name), "block": block, "register": name,
                   "baseline": register_value(block, name, old), "candidate": register_value(block, name, new)}
            changes.append(row)
            if is_phase_change(baseline, candidate, block, name, old, new):
                transient_changes.append(row)
            else:
                static_changes.append(row)

    # PLL_CTRL bit 0 starts the PLL. DSI1 uses the master's clock on the bright bonded path.
    pll_lost = [name for name in ("local-lock", "pll-ctrl") if before["dsi0", name] & 1 and not after["dsi0", name] & 1]
    if pll_lost:
        flags.append({"kind": "pll-loss", "link": "dsi0", "registers": pll_lost})
    for link in LINKS:
        dsc = [row["register"] for row in changes if row["block"] == link and row["domain"] == "dsc-compression"]
        static = [row["register"] for row in static_changes if row["block"] == link]
        phy = [name for name in static if name in PHY_TIMING or name in PHY_STATUS_WORDS]
        host_errors = [name for name in static if name in HOST_ERROR_WORDS]
        if dsc:
            flags.append({"kind": "dsc-mismatch", "link": link, "registers": dsc})
        if phy:
            flags.append({"kind": "phy-drift", "link": link, "registers": phy})
        if host_errors:
            flags.append({"kind": "host-error", "link": link, "registers": host_errors})
        if "reset" in static:
            flags.append({"kind": "reset-drift", "link": link, "registers": ["reset"]})
        if "tpg" in static and after[link, "tpg"] != 0:
            flags.append({"kind": "tpg-active", "link": link, "registers": ["tpg"]})
    report = {"verdict": "DRIFT" if changes else "PASS", "baseline": baseline.metadata(), "candidate": candidate.metadata(),
              "frame_advance": advances, "changes": changes, "flags": flags,
              "static_changes": static_changes, "transient_changes": transient_changes}
    if counter_reset:
        report["counter_reset"] = True
    return report


def read_snapshot(path, role):
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ValueError(f"{role} file unreadable ({type(error).__name__})") from error
    try:
        return parse_snapshot(text)
    except ValueError as error:
        raise ValueError(f"{role} capture {error}") from error


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="Each file holds one complete, newline-terminated snapshot and its boot UUID as a 'boot UUID' or "
               "'kern.bootsessionuuid: UUID' row. Supply a newer sample from the same boot with both INTF timing engines "
               "enabled. By default, both frame counters must advance. JSON goes to stdout and a one-line verdict to stderr. "
               "Exit codes are 0 PASS, 1 DRIFT, and 2 VOID. All register changes count toward DRIFT.",
    )
    parser.add_argument("baseline", type=Path, help="saved bright reference snapshot")
    parser.add_argument("candidate", type=Path, help="saved dark or post-wake snapshot")
    parser.add_argument("--allow-counter-reset", action="store_true",
                        help="accept a frame counter reset across sleep/wake if both candidate counters are positive")
    args = parser.parse_args(argv)
    try:
        report = compare(read_snapshot(args.baseline, "baseline"), read_snapshot(args.candidate, "candidate"),
                         allow_counter_reset=args.allow_counter_reset)
    except ValueError as error:
        report = {"verdict": "VOID", "reason": str(error), "changes": [], "flags": []}

    print(json.dumps(report, indent=2))
    verdict = report["verdict"]
    if verdict == "VOID":
        summary = report["reason"]
    elif verdict == "DRIFT":
        summary = (f"{len(report['changes'])} changed registers "
                   f"({len(report['static_changes'])} unexplained, {len(report['transient_changes'])} scan-phase)")
        if report["flags"]:
            summary += " (" + ", ".join(f"{flag['link']} {flag['kind']}" for flag in report["flags"]) + ")"
    elif report.get("counter_reset"):
        summary = "registers match and both INTF timing engines are scanning after a counter reset"
    else:
        summary = "registers match and both INTF frame counters advanced"
    print(f"{verdict} — {summary}", file=sys.stderr)
    return {"PASS": 0, "DRIFT": 1, "VOID": 2}[verdict]


if __name__ == "__main__":
    sys.exit(main())
