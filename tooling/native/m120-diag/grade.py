#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Summarize retained m120 console, notification and diagnostic records from one boot."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re


PIPE_TOTALS = """captured polls ready used pending_used fallback cancelled failed lead_us
                 overlapped overlap_us""".split()
FI_TOTALS = """programs complete_hooks dropped_hooks cleanup_calls type3_attempts type3_accepted
               type3_errors frame_info_valid frame_info_missing frame_info_packets""".split()
FI_LAST = """type3_last_mask type3_last_slots type3_last_result frame_info_first_time_raw
             frame_info_last_time_raw frame_info_last_delta_raw""".split()
AUDIT_TOTALS = """not_armed no_sample clock_back before_flush after_end same_frame other_frame
                  flush_pending not_new flip_failed no_coverage not_direct provider_unavailable
                  serial_mismatch swap_mismatch no_handoff dropped""".split()
QUEUE_FIELDS = "ticks current created pending head layers ready drops main_surface".split()
PIPE_FIELDS = "swap worker preparation take ticket failure key_matches".split()
PIPE_CLOCKS = ("main_fence_signal_ticks", "complete_ticks")
FLUSH_FIELDS = """generation worker selected_irq armed enabled flushed latched disabled disarmed
                  end mask latch_ctl frame1 line1 frame2 line2""".split()
IRQ_FIELDS = """generation armed_flush ticks status ctl before1 after1 line1 before2 after2 line2""".split()
FENCE_FIELDS = """capture_target program_target submitted retired level cap last_pending_ticks
                  first_retired_ticks query_begin_ticks query_end_ticks""".split()
FEEDBACK_FIELDS = ("mask", "send_result", "presentation_ticks", "disposition")

UINT64_MAX = (1 << 64) - 1
UINT32_MAX = (1 << 32) - 1
PACING = re.compile(r"NativeGopDisplay: pacing120 at-us (\d+) window-us (\d+):")
EVENTS = re.compile(r"NativeGopDisplay: pacing120 (program|complete-hook|software-vbl): events (\d+) ")
STAGES = re.compile(r"NativeGopDisplay: pacing120 (stage-[\w-]+): n (\d+) sum (\d+) "
                    r"min (\d+) mean (\d+) max (\d+) us;")
PIPE_LOG = re.compile(r"NativeGopDisplay: pipe120: (captured=.*?) \(cumulative\)")
OBJECT = re.compile(r'\{\s*"(m120_diag|version)"\s*:')


def uint(value, limit=UINT64_MAX):
    if type(value) is not int or not 0 <= value <= limit:
        raise ValueError("expected an unsigned integer in range")
    return value


def numeric_fields(record, required, optional=(), wide=()):
    if not isinstance(record, dict) or not set(required) <= record.keys() <= set(required) | set(optional):
        raise ValueError("unexpected or missing record fields")
    for field, value in record.items():
        uint(value, UINT64_MAX if field in wide else UINT32_MAX)


def object_pairs(pairs):
    result = dict(pairs)
    if len(result) != len(pairs):
        raise ValueError("duplicate JSON field")
    return result


def read_raw(row):
    row = dict(row)
    if uint(row.pop("m120_diag")) != 1:
        raise ValueError("unsupported m120 diagnostic version")
    kind = row.pop("kind")
    if kind == "pipe":
        for phase in ("prepare", "program"):
            sample = row.pop(phase)
            numeric_fields(sample, QUEUE_FIELDS, wide=("ticks",))
            if sample["main_surface"] > 1:
                raise ValueError("invalid main-surface presence")
            row[phase] = sample
        fence = row.get("fence", {})
        numeric_fields(fence, (), FENCE_FIELDS, wide=FENCE_FIELDS[6:])
        feedback = row.get("feedback", {})
        numeric_fields(feedback, ("disposition",) if "feedback" in row else (),
                       FEEDBACK_FIELDS[:3], wide=("presentation_ticks",))
        if feedback:
            uint(feedback["disposition"], 3)
        numeric_fields({key: value for key, value in row.items() if key not in
                        {"prepare", "program", "fence", "feedback"}},
                       PIPE_FIELDS, PIPE_CLOCKS, ("worker", *PIPE_CLOCKS))
        for key, limit in (("preparation", 2), ("take", 2), ("ticket", 3), ("failure", 3), ("key_matches", 1)):
            uint(row[key], limit)
        if not row["swap"]:
            raise ValueError("pipe row lacks a relative swap ID")
    elif kind == "flush":
        numeric_fields(row, FLUSH_FIELDS, ("qualifier_reasons",), FLUSH_FIELDS[:10])
        if not row["generation"] or not row["worker"]:
            raise ValueError("flush row lacks generation or worker identity")
    elif kind == "irq":
        numeric_fields(row, IRQ_FIELDS, wide=IRQ_FIELDS[:3])
        if not row["generation"] or not row["ticks"]:
            raise ValueError("IRQ row lacks generation or observation clock")
    else:
        raise ValueError("unknown m120 diagnostic kind")
    return kind, row


def rejection(sample):
    if sample["head"]:
        for name, failed in (("layers", sample["layers"] != 1), ("not_ready", not sample["ready"] & 1),
                             ("drops", sample["drops"] & ~8), ("no_surface", not sample["main_surface"])):
            if failed:
                yield name


def fallback(row, prepare_rejected):
    if not row["take"]:
        return "unobserved"
    usable = bool(row["key_matches"]) and row["ticket"] in (1, 2)
    if (row["take"] == 1) != usable:
        return "inconsistent"
    if row["take"] == 1:
        return "used"
    if row["preparation"] != 2:
        return ("no_preparation", "power_off")[row["preparation"]]
    if not row["prepare"]["head"]:
        return "empty_head"
    if row["prepare"]["head"] != row["swap"]:
        return "head_replaced"
    if prepare_rejected:
        return "ineligible_head"
    if not row["ticket"]:
        return "no_ticket"
    if not row["key_matches"]:
        return "key_mismatch"
    if row["ticket"] != 3:
        return "inconsistent"
    return ("failed_unknown", "capture_failed", "poll_failed", "deadline")[row["failure"]]


def counter_advance(pre, before, after):
    if before != after:
        return "unstable"
    return {0: "same", 1: "next"}.get((after - pre) & UINT32_MAX, "other")


def physical(flush, irq):
    flags = []
    if not flush["armed"] or not flush["mask"] or not flush["flushed"] or flush["end"] < flush["flushed"]:
        flags.append("incomplete_bounds")
    if flush["latched"] and flush["latch_ctl"] & flush["mask"]:
        flags.append("latch_ctl_pending")
    if irq is None:
        return ("missing", "missing"), flags + ["no_irq"]
    for name, observed in (
        ("arm_mismatch", irq["armed_flush"] != flush["generation"]),
        ("no_intf1_vsync", not irq["status"] & (1 << 27)),
        ("before_flush", irq["ticks"] <= flush["flushed"]),
        ("after_end", flush["end"] and irq["ticks"] > flush["end"]),
        ("irq_ctl_pending", irq["ctl"] & flush["mask"]),
        ("after_disable", flush["disabled"] and irq["ticks"] > flush["disabled"]),
        ("after_disarm", flush["disarmed"] and irq["ticks"] > flush["disarmed"]),
    ):
        if observed:
            flags.append(name)
    return tuple(counter_advance(flush[f"frame{i}"], irq[f"before{i}"], irq[f"after{i}"])
                 for i in (1, 2)), flags


def read_notifications(row):
    if uint(row["version"]) != 1 or uint(row["abi"]) != 2 or uint(row["hz"]) != 120:
        raise ValueError("unsupported notification snapshot")
    result = {key: uint(row[key]) for key in ["at_us", *FI_TOTALS]}
    result.update({key: uint(row[key]) for key in FI_LAST if key in row})
    result["audit"] = None
    if uint(row.get("frame_info_audit_enabled", 0), 1):
        audit = row["frame_info_audit"]
        if uint(audit["version"]) != 1:
            raise ValueError("unsupported frame-info audit")
        result["audit"] = {key: uint(audit[key]) for key in ("events", "valid", "missing")}
        result["audit"]["reasons"] = {key: uint(audit["reasons"][key]) for key in AUDIT_TOTALS}
    return result


def deltas(previous, current, keys):
    if previous is None or any(current[key] < previous[key] for key in keys):
        return None
    return {key: current[key] - previous[key] for key in keys}


def fi_delta(previous, current):
    if previous is None or current["at_us"] <= previous["at_us"]:
        return None
    delta = deltas(previous, current, FI_TOTALS)
    if delta is None:
        return None
    delta["window_us"] = current["at_us"] - previous["at_us"]
    before, after = previous["audit"], current["audit"]
    delta["audit"] = None
    if before is not None and after is not None:
        counts = deltas(before, after, ("events", "valid", "missing"))
        reasons = deltas(before["reasons"], after["reasons"], AUDIT_TOTALS)
        if counts is not None and reasons is not None:
            delta["audit"] = dict(counts, reasons=reasons)
    return delta


def summarize_raw(records, details):
    pipe = Counter()
    prepare, program = Counter(), Counter()
    intf1, intf2, observations = Counter(), Counter(), Counter()
    irqs_by_arm = Counter(irq["armed_flush"] for irq in records["irq"].values())
    report = {}
    if details:
        report["records"] = {"pipe": [], "flush": [], "irq": list(records["irq"].values())}
    for row in records["pipe"].values():
        rejected = list(rejection(row["prepare"])) if row["preparation"] == 2 else []
        programmed = list(rejection(row["program"]))
        bucket = fallback(row, rejected)
        pipe[bucket] += 1
        prepare.update(rejected)
        program.update(programmed)
        if details:
            report["records"]["pipe"].append(dict(row, bucket=bucket, prepare_rejections=rejected,
                                                   program_rejections=programmed))
    # Job zero denotes a missing handoff identity and must never join two rows.
    by_worker = {}
    for row in records["pipe"].values():
        if row["worker"]:
            by_worker.setdefault(row["worker"], []).append(row["swap"])
    for row in records["flush"].values():
        irq = records["irq"].get(row["selected_irq"])
        (first, second), flags = physical(row, irq)
        intf1[first] += 1
        intf2[second] += 1
        observations.update(flags)
        swaps = by_worker.get(row["worker"], [])
        if details:
            report["records"]["flush"].append(dict(row, swap=swaps[0] if len(swaps) == 1 else None,
                                                  intf1=first, intf2=second, observations=flags,
                                                  irq_rows_armed_for_flush=irqs_by_arm[row["generation"]]))
    report["pipe"] = None if not pipe else {
        "take_buckets": pipe, "prepare_rejections": prepare, "program_rejections": program,
        "fence_rows": sum(bool(row.get("fence")) for row in records["pipe"].values()),
        "feedback": Counter(("unobserved", "suppressed", "send_accepted", "send_error")
                            [row.get("feedback", {}).get("disposition", 0)]
                            for row in records["pipe"].values()),
    }
    report["physical"] = None if not records["flush"] and not records["irq"] else {
        "flush_rows": len(records["flush"]), "irq_rows": len(records["irq"]),
        "intf1": intf1, "intf2": intf2, "observations": observations,
    }
    return report


def analyze(lines, details=False):
    records = {kind: {} for kind in ("pipe", "flush", "irq")}
    windows, pipe_reports, notifications, malformed = [], [], [], []
    previous_pipe = previous_fi = None
    window = None
    decoder = json.JSONDecoder(object_pairs_hook=object_pairs)
    for number, line in enumerate(lines, 1):
        try:
            obj = OBJECT.search(line)
            if obj and (obj[1] == "m120_diag" or "native_gop_notifications" in line or
                        '"frame_info_enabled"' in line):
                row, _ = decoder.raw_decode(line[obj.start():])
                if "m120_diag" in row:
                    kind, sample = read_raw(row)
                    key = sample["swap" if kind == "pipe" else "generation"]
                    if key in records[kind]:
                        raise ValueError("duplicate diagnostic identity")
                    records[kind][key] = sample
                elif "frame_info_enabled" in row:
                    current = read_notifications(row)
                    notifications.append(dict(current, delta=fi_delta(previous_fi, current)))
                    previous_fi = current
            if match := PACING.search(line):
                at_us, window_us = map(int, match.groups())
                window = {"at_us": at_us, "window_us": window_us, "events": {}, "stages": {}}
                windows.append(window)
            if window is not None and (match := EVENTS.search(line)):
                window["events"][match[1]] = int(match[2])
            if window is not None and (match := STAGES.search(line)):
                window["stages"][match[1]] = dict(zip(("n", "sum_us", "min_us", "mean_us", "max_us"),
                                                    map(int, match.groups()[1:])))
            if "NativeGopDisplay: pipe120: captured=" in line:
                match = PIPE_LOG.search(line)
                if match is None:
                    raise ValueError("incomplete pipe rollup")
                fields = match[1].split()
                current = {key: uint(int(value)) for key, value in (part.split("=") for part in fields)}
                if len(fields) != len(PIPE_TOTALS) or current.keys() != set(PIPE_TOTALS):
                    raise ValueError("unexpected pipe rollup fields")
                delta = deltas(previous_pipe, current, PIPE_TOTALS)
                pipe_reports.append({"total": current, "delta": delta})
                previous_pipe = current
        except (KeyError, TypeError, ValueError):
            # A console may contain unrelated or personal data. Report positions only.
            malformed.append(number)
    return {
        "fix_image": "HOLD",
        "legacy": {"pacing_windows": windows, "pipe_reports": pipe_reports, "notifications": notifications},
        "raw": summarize_raw(records, details),
        "external_evidence": {
            "ca_page_and_submission_lifecycle": "unavailable",
            "surface_specific_gpu_timing": "unavailable",
            "iomfb_receive_to_drawable_feedback": "unavailable",
        },
        "malformed_lines": malformed,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path, help="retained files in capture order for a single boot")
    parser.add_argument("--details", action="store_true", help="include numeric raw records and identity joins")
    args = parser.parse_args()

    def lines():
        for capture in args.captures:
            with capture.open(errors="replace") as stream:
                yield from stream

    try:
        report = analyze(lines(), args.details)
    except OSError:
        parser.error("unable to read a capture")
    print(json.dumps(report, sort_keys=True, indent=2))
    pipe = report["raw"]["pipe"]
    if report["malformed_lines"] or (pipe and "inconsistent" in pipe["take_buckets"]):
        return 1
    if any(report["legacy"].values()) or report["raw"]["pipe"] or report["raw"]["physical"]:
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
