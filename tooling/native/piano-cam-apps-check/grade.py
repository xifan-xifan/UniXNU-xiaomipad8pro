#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Recognize positive camera-app evidence without macOS dependencies."""
import datetime as dt
import json
import math
import re
import sys
import zlib
from pathlib import Path

from frame_image import read_frame

APPS = {"facetime": "FaceTime", "quicktime": "QuickTime"}
START = re.compile(r"camera stream start sensor=([012]) index=[0-9]+ ")
OUTPUT = re.compile(r"camera output( stop)? sensor=([012]) (?:index=[0-9]+ )?sent=([0-9]+) sources=([0-9]+) ")
LUMA = re.compile(r"(?:^| )nonblack=([0-9]+)(?: |$)")
STOP = re.compile(r"camera stream stop sensor=([012]) result=(0x[0-9a-f]+|0) state=([0-9]+) clean=([01]) guard=([01]) smmu=(0x[0-9a-f]+|0) ")
FAULT = re.compile(r"camera (?:stream start|RAW wait|RAW lease release) failed sensor=([012]) ")
HASH = re.compile(r"[0-9a-f]{64}")
LOG_TIMESTAMP = re.compile(r"^(\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d)(?:\.(\d+))?([+-]\d\d):?(\d\d)$")
MIN_MEAN_LUMA = 24
NONBLACK_LUMA = 32
MIN_NONBLACK_FRACTION = 0.10
MIN_LUMA_VARIANCE = 64


def natural(value):
    return type(value) is int and value >= 0


def seconds(value):
    return type(value) in (float, int) and 0 <= value <= 86400 and math.isfinite(value)


def reduce_log(rows, since, until):
    """Keep only scalar PianoCamera stream events inside this app's time window."""
    events = []
    for row in rows:
        if not isinstance(row, dict) or row.get("subsystem") != "com.unixnu.PianoCamera" or row.get("category") != "extension":
            continue
        date = row.get("timestamp")
        match = LOG_TIMESTAMP.fullmatch(date) if isinstance(date, str) else None
        if not match:
            continue
        try:
            fraction = (match[2] or "").ljust(6, "0")[:6]
            stamp = dt.datetime.fromisoformat(f"{match[1]}.{fraction}{match[3]}:{match[4]}").timestamp()
        except (ValueError, OverflowError):
            continue
        if not since <= stamp <= until:
            continue
        pid, message = row.get("processID"), row.get("eventMessage")
        if not natural(pid) or not pid or not isinstance(message, str):
            continue
        if match := START.match(message):
            fields = {"kind": "start", "sensor": int(match[1])}
        elif match := OUTPUT.match(message):
            fields = {"kind": "end_count" if match[1] else "count", "sensor": int(match[2]),
                      "sent": int(match[3]), "sources": int(match[4])}
            if luma := LUMA.search(message, match.end() - 1):
                fields["nonblack"] = int(luma[1])
        elif match := STOP.match(message):
            fields = {"kind": "stop", "sensor": int(match[1]),
                      "clean": (int(match[2], 16), match[3], match[4], match[5], int(match[6], 16)) == (0, "3", "1", "0", 0)}
        elif match := FAULT.match(message):
            fields = {"kind": "fault", "sensor": int(match[1])}
        else:
            continue
        events.append((stamp, {"pid": pid, **fields}))
    return [event for _, event in sorted(events, key=lambda pair: pair[0])]


def stream(events, *, front=False, stopped=True, require_luma=True):
    """Require one isolated hardware stream and increasing live source and delivery counters."""
    if not isinstance(events, list) or not all(isinstance(event, dict) for event in events):
        return "VOID", "camera log unavailable"
    if not events:
        return "VOID", "no PianoCamera stream evidence"
    starts = [event for event in events if event.get("kind") == "start"]
    if not starts and len({(event.get("pid"), event.get("sensor")) for event in events
                           if natural(event.get("pid")) and natural(event.get("sensor"))}) == 1:
        if all(event.get("kind") == "fault" for event in events):
            return "FAIL", "PianoCamera reported a stream fault"
    first = events[0]
    pid, sensor = first.get("pid"), first.get("sensor")
    if (len(starts) != 1 or first != starts[0] or not natural(pid) or pid == 0 or
            not natural(sensor) or sensor not in (0, 1, 2) or
            any(event.get("pid") != pid or event.get("sensor") != sensor for event in events)):
        return "VOID", "ambiguous camera stream"
    if sensor == 0 or (front and sensor != 2):
        return "FAIL", "wrong PianoCamera sensor"
    if any(event.get("kind") == "fault" for event in events):
        return "FAIL", "PianoCamera reported a stream fault"
    counts = events[1:]
    if stopped:
        if [event.get("kind") for event in counts[-2:]] != ["end_count", "stop"]:
            return "VOID", "camera stop evidence missing"
        if counts[-1].get("clean") is False:
            return "FAIL", "PianoCamera stream stopped uncleanly"
        if counts[-1].get("clean") is not True:
            return "VOID", "camera stop evidence invalid"
        counts = counts[:-1]
    live = counts[:-1] if stopped else counts
    if any(event.get("kind") != "count" for event in live):
        return "VOID", "ambiguous camera counter sequence"
    previous = None
    for event in counts:
        sent, sources = event.get("sent"), event.get("sources")
        if not natural(sent) or not natural(sources):
            return "VOID", "camera counters invalid"
        if previous and (sent < previous["sent"] or sources < previous["sources"]):
            return "VOID", "camera counters reset"
        previous = event
    if len(live) < 2:
        return "VOID", "too few live counter samples"
    if live[-1]["sent"] <= live[0]["sent"] or live[-1]["sources"] <= live[0]["sources"]:
        return "FAIL", "live camera source or delivery count stalled"
    if front and require_luma:
        lit = [event.get("nonblack") for event in live]
        if not all(natural(count) and count <= event["sent"] for count, event in zip(lit, live)):
            return "VOID", "front stream lacks non-black telemetry"
        if any(before > after for before, after in zip(lit, lit[1:])):
            return "VOID", "front stream luminance counter reset"
        if lit[-1] - lit[0] < 2:
            return "FAIL", "front stream did not deliver multiple non-black frames"
    return "PASS", "live source and delivery counters advanced"


def rect(value):
    if (not isinstance(value, list) or len(value) != 4 or
            not all(type(point) in (int, float) and -1_000_000 <= point <= 1_000_000 for point in value) or
            min(value[2:]) <= 0):
        raise ValueError("invalid window or screen bounds")
    return value


def preview_crop(geometry, width, height):
    x, y, w, h = rect(geometry.get("window"))
    sx, sy, sw, sh = rect(geometry.get("screen"))
    x, y = x - sx, y - sy
    if x < 0 or y < 0 or x + w > sw or y + h > sh:
        raise ValueError("window extends beyond the captured screen")
    # FaceTime's video is on the right. Keep comfortably inside the image area,
    # clear of the left sidebar, window chrome and bottom call controls.
    left = math.ceil((x + 0.55 * w) * width / sw)
    right = math.floor((x + 0.90 * w) * width / sw)
    top = math.ceil((y + 0.25 * h) * height / sh)
    bottom = math.floor((y + 0.75 * h) * height / sh)
    if right - left < 16 or bottom - top < 16:
        raise ValueError("window crop too small")
    return left, top, right, bottom


def frame_stats(data, geometry):
    """Return only aggregate pixel statistics, or None for unusable capture evidence."""
    if not isinstance(geometry, dict):
        return None
    try:
        width, height, layout, rows = read_frame(data)
        left, top, right, bottom = preview_crop(geometry, width, height)
        channels = len(layout)
        red, blue = (2, 0) if layout.startswith("B") else (0, 2)
        gray, alpha = layout.startswith("L"), layout.endswith("A")
        total = squared = nonblack = 0
        for y, row in enumerate(rows):
            if top <= y < bottom:
                for x in range(left * channels, right * channels, channels):
                    luma = row[x] if gray else (54 * row[x + red] + 183 * row[x + 1] + 19 * row[x + blue] + 128) >> 8
                    if alpha:
                        luma = (luma * row[x + channels - 1] + 127) // 255
                    total += luma
                    squared += luma * luma
                    nonblack += luma >= NONBLACK_LUMA
    except (ValueError, zlib.error):
        return None
    count = (right - left) * (bottom - top)
    return {"mean_luma": total / count, "nonblack_fraction": nonblack / count,
            "luma_variance": (squared * count - total * total) / (count * count)}


def frame_grades(capture, events):
    frames = capture.get("frames") if isinstance(capture, dict) else None
    if not isinstance(frames, list) or len(frames) != 2:
        return "VOID", "two FaceTime framebuffer captures with window bounds are required"
    for frame in frames:
        if not isinstance(frame, dict) or not all(
            type(frame.get(key)) in (int, float) and 0 <= frame[key] <= maximum
            for key, maximum in (("mean_luma", 255), ("nonblack_fraction", 1), ("luma_variance", 255 ** 2 / 4))
        ):
            return "VOID", "FaceTime framebuffer capture or crop unavailable"
    before, after = capture.get("before"), capture.get("after")
    live = events[1:-2]
    if before not in live or after not in live:
        return "VOID", "live front-camera counters did not bracket the captures"
    if after["sent"] <= before["sent"] or after["sources"] <= before["sources"]:
        return "FAIL", "front-camera counters did not advance across the captures"
    for frame in frames:
        if frame["mean_luma"] < MIN_MEAN_LUMA or frame["nonblack_fraction"] < MIN_NONBLACK_FRACTION:
            return "FAIL", "FaceTime self-view crop was black or too dark"
        if frame["luma_variance"] < MIN_LUMA_VARIANCE:
            return "FAIL", "FaceTime self-view crop lacked spatial luminance variation"
    return "PASS", "two non-black FaceTime self-view crops with advancing front-camera counters"


def movie(stats):
    if not isinstance(stats, dict) or type(stats.get("valid")) is not bool:
        return "VOID", "movie inspection unavailable"
    if not stats["valid"]:
        return "FAIL", "saved file is missing or undecodable"
    size, duration = stats.get("bytes"), stats.get("duration")
    span = stats.get("video_span")
    count, lit, hashes = stats.get("decoded"), stats.get("nonblack"), stats.get("frame_hashes")
    if not (natural(size) and seconds(duration) and seconds(span) and natural(count) and natural(lit) and lit <= count and
            isinstance(hashes, list) and len(hashes) <= min(lit, 3) and
            all(isinstance(digest, str) and HASH.fullmatch(digest) for digest in hashes) and
            isinstance(stats.get("container"), str) and isinstance(stats.get("codec"), str)):
        return "VOID", "movie inspection incomplete"
    if not (size > 0 and stats["container"] == "quicktime" and stats["codec"] == "avc1" and
            4 <= duration <= 7 and span >= duration - 0.75 and count >= 12 and lit >= 3):
        return "FAIL", "saved movie failed size, format, duration or decoded frame checks"
    if len(hashes) != 3:
        return "VOID", "decoded frame hashes missing"
    return "PASS", "saved H.264 QuickTime movie decoded non-black frames"


def playback(stats):
    if not isinstance(stats, dict) or type(stats.get("playing")) is not bool:
        return "VOID", "QuickTime playback evidence unavailable"
    before, after, elapsed = (stats.get(key) for key in ("first", "last", "elapsed"))
    if not all(seconds(value) for value in (before, after, elapsed)):
        return "VOID", "QuickTime playback timing invalid"
    if not (stats["playing"] and 2.5 <= elapsed <= 5 and 1.5 < after - before <= elapsed + 0.75):
        return "FAIL", "QuickTime playback clock did not advance as expected"
    return "PASS", "QuickTime playback clock advanced"


def grade_phase(key, phase):
    if not isinstance(phase, dict) or phase.get("cleanup") is not True:
        return "VOID", "app or temporary movie cleanup unconfirmed"
    crashes = phase.get("crashes")
    if natural(crashes) and crashes:
        return "FAIL", "new camera stack crash report"
    outcome = phase.get("outcome")
    if outcome == "failed":
        return "FAIL", "app did not complete the requested operation"
    if outcome != "observed":
        return "VOID", "app observation incomplete"
    if not natural(crashes):
        return "VOID", "crash report count unavailable"
    if phase.get("window") is not True:
        return ("FAIL", "app window missing") if phase.get("window") is False else ("VOID", "app window evidence missing")
    captured = key == "facetime" and "gop_shot" in phase
    live = stream(phase.get("events"), front=key == "facetime", require_luma=not captured)
    if key == "facetime":
        if captured and live[0] == "PASS":
            return frame_grades(phase["gop_shot"], phase["events"])
        return live
    preview = stream(phase.get("preview"), stopped=False)
    checks = (live, preview, movie(phase.get("movie")), playback(phase.get("playback")))
    for kind in ("FAIL", "VOID"):
        for verdict, reason in checks:
            if verdict == kind:
                return verdict, reason
    if phase["events"][:len(phase["preview"])] != phase["preview"]:
        return "VOID", "recording preview and final stream evidence disagree"
    return "PASS", "live preview, saved movie and QuickTime playback verified"


def grade(evidence):
    if not isinstance(evidence, dict) or type(evidence.get("schema")) is not int or evidence["schema"] != 1:
        return {app: ("VOID", "unsupported evidence schema") for app in APPS}
    version = evidence.get("macos")
    if not isinstance(version, str) or not re.fullmatch(r"27\.3(?:\.[0-9]+)?", version):
        return {app: ("VOID", "macOS 27.3 is required") for app in APPS}
    if evidence.get("preflight") != "ready":
        return {app: ("VOID", "host preflight did not pass") for app in APPS}
    return {app: grade_phase(app, evidence.get(app)) for app in APPS}


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print("usage: grade.py evidence.json", file=sys.stderr)
        return 2
    try:
        evidence = json.loads(Path(argv[0]).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        evidence = None
    result = grade(evidence)
    for app, (verdict, reason) in result.items():
        print(f"{verdict} Cameras: {APPS[app]} — {reason}")
    verdicts = {verdict for verdict, _ in result.values()}
    return 2 if "VOID" in verdicts else int("FAIL" in verdicts)


if __name__ == "__main__":
    raise SystemExit(main())
