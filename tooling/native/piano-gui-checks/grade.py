#!/usr/bin/env python3
"""Parse and grade the receipts collected by gui_checks_installed."""
import argparse
import json
import math
from pathlib import Path
import re

READBACK_TOLERANCE = 0.05
MIN_LUMA_STEP = 3.0


def parse_fields(text, pattern):
    fields = {}
    for key, value in re.findall(pattern, text, re.MULTILINE):
        if key in fields:
            raise ValueError(f"duplicate evidence for {key}")
        fields[key] = value
    return fields


def parse_panel(text):
    prefix = r'^\s*"piano-backlight-'
    codes = parse_fields(text, prefix + r'(code|primary-code|secondary-code)" = ([0-9]+)$')
    lit = parse_fields(text, prefix + r'(lit|primary-lit|secondary-lit)" = (Yes|No)$')
    ranges = re.findall(r'"BrightnessMilliNits"=\{([^{}]*)\}', text)
    if len(codes) != 3 or len(lit) != 3 or len(ranges) != 1:
        raise ValueError("incomplete PianoBacklight readback")
    nits = parse_fields(ranges[0], r'"(min|max|value)"=([0-9]+)')
    code = int(codes["code"])
    if len(nits) != 3 or not 0 < code <= 2047:
        raise ValueError("invalid PianoBacklight readback")
    low, high, applied = (int(nits[k]) for k in ("min", "max", "value"))
    if not 0 < low < high or not low <= applied <= high:
        raise ValueError("invalid PianoBacklight nits range")
    coherent = len(set(codes.values())) == 1 and set(lit.values()) == {"Yes"}
    return code, applied, coherent


def parse_luma(text):
    values = re.findall(r"^lavfi\.signalstats\.YAVG=([0-9]+(?:\.[0-9]+)?)$", text, re.MULTILINE)
    if len(values) != 1 or not 0 <= float(values[0]) <= 255:
        raise ValueError("expected one C920 panel luminance sample")
    return float(values[0])


def brightness_at(target, slider, core_level, core_nits, panel):
    """The UI uses a user level, whereas the panel reports physical millinits."""
    code, panel_millinits, coherent = panel
    core_millinits = core_nits * 1000
    return (coherent and code > 0 and math.isfinite(core_millinits) and core_millinits > 0
            and all(0 <= level <= 1 and abs(level - target) <= READBACK_TOLERANCE + 1e-9
                    for level in (slider, core_level))
            and abs(panel_millinits - core_millinits) <= READBACK_TOLERANCE * core_millinits)


def visible_change(low, high, restored, original):
    """Require a real 30-to-70 rise and the expected visible return."""
    low_target, low_y = low
    high_target, high_y = high
    restore_target, restore_y = restored
    _, original_y = original
    if high_target <= low_target or high_y - low_y < MIN_LUMA_STEP:
        return False
    if abs(restore_target - high_target) <= READBACK_TOLERANCE:
        return abs(restore_y - original_y) < MIN_LUMA_STEP
    direction = 1 if restore_target > high_target else -1
    return (restore_y - high_y) * direction >= MIN_LUMA_STEP


def wifi_evidence(receipt):
    """Keep scalar Wi-Fi receipts suitable for passing to the harness redactor."""
    evidence = {key: receipt[key] for key in ("power", "associated", "target", "active", "lease", "menu", "row", "switch")
                if type(receipt.get(key)) is bool}
    for key in ("rowCount", "status"):
        if type(receipt.get(key)) is int:
            evidence[key] = receipt[key]
    timestamp = receipt.get("t")
    if type(timestamp) in (int, float) and math.isfinite(timestamp):
        evidence["t"] = timestamp
    if isinstance(receipt.get("before"), dict):
        evidence["before"] = wifi_evidence(receipt["before"])
    return evidence


def wifi_ready(state):
    return all(state.get(key) is True for key in ("power", "associated", "target", "active", "lease"))


def wifi_off(state):
    return (state.get("power") is False and state.get("associated") is False
            and state.get("active") is False)


def wifi_transition(before, samples, started, deadline=30):
    """A complete associated sample must follow a known disconnected baseline."""
    baseline_t = before.get("t")
    return type(baseline_t) in (int, float) and baseline_t <= started and before.get("target") is False and any(
        started <= ts <= started + deadline and wifi_ready(sample) for ts, sample in samples
    )


def main():
    from grade_energy_picker import EXIT_CODES, grade_capture

    parser = argparse.ArgumentParser(description="Replay a GUI check capture offline.")
    parser.add_argument("check", choices=("energy-picker",))
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    try:
        capture = json.loads(args.capture.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        capture = None
    status, reason = grade_capture(capture)
    print(f"{status} Energy Mode picker: {reason}")
    return EXIT_CODES[status]


if __name__ == "__main__":
    raise SystemExit(main())
