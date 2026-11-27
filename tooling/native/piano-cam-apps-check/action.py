#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Collect local evidence for the harness spec action."""
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from grade import frame_stats, movie, reduce_log, stream

MOVIES = Path("/private/var/tmp/cam-apps")
CRASH_DIRS = (Path.home() / "Library/Logs/DiagnosticReports", Path("/Library/Logs/DiagnosticReports"))
CRASH = re.compile(r"^(?:FaceTime|QuickTime[ _]Player|QuickTimePlayerX|cameracaptured|PianoCameraExtension|"
                   r"com\.unixnu\.PianoCamera\.Extension)[-_].*\.(?:ips|crash)$")
PREDICATE = 'subsystem == "com.unixnu.PianoCamera" AND category == "extension" AND eventMessage BEGINSWITH "camera "'
stop_requested = False


class Invalid(Exception):
    def __init__(self, stage, outcome="void", returncode=None):
        self.stage, self.outcome = stage, outcome
        self.returncode = returncode


def check_interrupted():
    if stop_requested:
        raise Invalid("interrupted")


def command(argv, timeout=20, *, binary=False):
    # Child diagnostics may contain private information. They stay off disk.
    try:
        return subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              timeout=timeout, **({} if binary else {"encoding": "utf-8", "errors": "replace"}),
                              env={**os.environ, "LC_ALL": "C"})
    except (OSError, subprocess.TimeoutExpired):
        raise Invalid("command_unavailable") from None


def version():
    result = command(["/usr/bin/sw_vers", "-productVersion"])
    value = result.stdout.strip()
    return value if result.returncode == 0 and re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,2}", value) else None


def crash_reports():
    reports = set()
    for location in CRASH_DIRS:
        for directory in (location, location / "Retired"):
            try:
                entries = list(directory.iterdir())
            except FileNotFoundError:
                continue
            for entry in entries:
                if CRASH.fullmatch(entry.name) and not entry.is_symlink():
                    try:
                        info = entry.stat()
                    except FileNotFoundError:
                        continue
                    if stat.S_ISREG(info.st_mode):
                        reports.add((entry.name, info.st_dev, info.st_ino, info.st_mtime_ns))
    return reports


def camera_events(since, until):
    start = dt.datetime.fromtimestamp(since - 1).strftime("%Y-%m-%d %H:%M:%S")
    result = command(["/usr/bin/log", "show", "--style", "json", "--info", "--start", start, "--predicate", PREDICATE])
    try:
        rows = json.loads(result.stdout)
    except ValueError:
        rows = None
    if result.returncode != 0 or not isinstance(rows, list):
        raise Invalid("camera_log_unavailable")
    return reduce_log(rows, since, until)


def movie_dir():
    try:
        MOVIES.mkdir(mode=0o700)
    except FileExistsError:
        pass
    info = MOVIES.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise Invalid("private_movie_directory_required")
    return Path(tempfile.mkdtemp(prefix="run-", dir=MOVIES))


def inspect_movie(probe, path):
    info = probe.read("inspect", path)
    if "valid" in info or info.get("codec") != "avc1" or info.get("container") != "quicktime":
        return info
    # AVFoundation can read the container while the VT decoder is unavailable
    # to the harness. Fall back to a local software decoder when available.
    ffmpeg = Path(os.environ.get("CAM_APPS_FFMPEG", "/opt/homebrew/bin/ffmpeg"))
    if not ffmpeg.is_absolute() or not ffmpeg.is_file() or not os.access(ffmpeg, os.X_OK):
        return info
    cap = 420
    result = command([ffmpeg, "-nostdin", "-v", "error", "-xerror", "-i", path, "-map", "0:v:0",
                      "-vf", "scale=32:32:flags=neighbor:out_range=tv", "-fps_mode", "passthrough",
                      "-pix_fmt", "yuv420p", "-c:v", "rawvideo", "-threads", "1",
                      "-frames:v", str(cap), "-f", "rawvideo", "pipe:1"], binary=True)
    yuv420_size = 1536
    count, remainder = divmod(len(result.stdout), yuv420_size)
    if result.returncode or remainder or not 0 < count < cap:
        return {"valid": False}
    nonblack, hashes = 0, []
    for offset in range(0, len(result.stdout), yuv420_size):
        luma = result.stdout[offset:offset + 1024]
        if sum(value >= 32 for value in luma) >= 52:
            nonblack += 1
            if len(hashes) < 3:
                hashes.append(hashlib.sha256(luma).hexdigest())
    return {**info, "valid": True, "decoded": count, "nonblack": nonblack, "frame_hashes": hashes}


class LocalProbe:
    def __init__(self, binary):
        self.binary = binary

    def call(self, op, *args, pending=False, cleanup=False):
        if not cleanup:
            check_interrupted()
        result = command([self.binary, op, *args])
        if result.returncode == 0:
            return result.stdout
        if pending and result.returncode == 3:
            return None
        raise Invalid(op, "failed" if result.returncode == 1 else "void", result.returncode)

    def poll(self, op, *args, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (value := self.call(op, *args, pending=True)) is not None:
                return value
            time.sleep(0.25)
        raise Invalid(op, "failed")

    def read(self, op, *args, poll=False):
        value = self.poll(op, *args) if poll else self.call(op, *args)
        try:
            data = json.loads(value)
        except ValueError:
            data = None
        if not isinstance(data, dict):
            raise Invalid(op)
        return data

    def hold(self, seconds, window):
        deadline = time.monotonic() + seconds
        while (remaining := deadline - time.monotonic()) > 0:
            if self.call(window, pending=True) is None:
                raise Invalid(window, "failed")
            time.sleep(min(0.4, remaining))


class App:
    """Own the app only after a launch or a launch with an uncertain reply."""

    def __init__(self, probe, name):
        self.probe, self.name = probe, name
        self.needs_quit = False
        self.discard_recording = False

    def launch(self):
        check_interrupted()
        self.needs_quit = True
        op = f"launch-{self.name}"
        try:
            self.probe.call(op)
        except Invalid as exc:
            # Exit 2 from the native launch means that it refused before dispatch.
            if exc.stage == "interrupted" or (exc.stage == op and exc.returncode == 2):
                self.needs_quit = False
            raise

    def quit(self):
        if self.needs_quit:
            operation = "quit-qt-discard" if self.discard_recording else f"quit-{self.name}"
            self.probe.call(operation, cleanup=True)
            self.needs_quit = False
            self.discard_recording = False


def face_time_frames(probe, binary):
    if not Path(binary).is_absolute():
        raise Invalid("gop_shot_path")
    frames = []
    geometry = probe.read("geometry-ft")
    for index in range(2):
        if index:
            time.sleep(1.5)
            if probe.read("geometry-ft") != geometry:
                raise Invalid("facetime_capture_geometry")
        result = command(["/usr/bin/sudo", "-n", "--", binary, "--stdout"], timeout=8, binary=True)
        if result.returncode != 0 or not result.stdout:
            raise Invalid("gop_shot_capture")
        if probe.read("geometry-ft") != geometry:
            raise Invalid("facetime_capture_geometry")
        frames.append(result.stdout)
    return frames, geometry


def face_time_capture(probe, binary, before):
    frames, geometry = face_time_frames(probe, binary)
    finished = time.time()
    receipt = {"before": before}
    # Counter records arrive periodically and can reach the unified log after the capture.
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        probe.call("window-ft")
        counts = [event for event in camera_events(finished, time.time()) if event["kind"] == "count"]
        if counts:
            receipt["after"] = counts[-1]
            break
        time.sleep(0.25)
    receipt["frames"] = [frame_stats(frame, geometry) for frame in frames]
    return receipt


def face_time(probe, evidence, since):
    binary = os.environ.get("CAM_APPS_GOP_SHOT")
    if binary:
        evidence["gop_shot"] = None
    probe.poll("window-ft")
    evidence["window"] = True
    deadline = time.monotonic() + 14
    while time.monotonic() < deadline:
        probe.call("window-ft")
        events = camera_events(since, time.time())
        state, _ = stream(events, front=True, stopped=False, require_luma=not binary)
        if state == "PASS" and binary:
            try:
                evidence["gop_shot"] = face_time_capture(probe, binary, events[-1])
            except Invalid:
                pass
        if state in ("PASS", "FAIL"):
            return
        time.sleep(0.75)


def quick_time(probe, app, evidence, since, path):
    probe.poll("qt-new")
    app.discard_recording = True
    probe.poll("window-qt")
    evidence["window"] = True
    deadline = time.monotonic() + 14
    while time.monotonic() < deadline:
        probe.call("window-qt")
        events = camera_events(since, time.time())
        state, _ = stream(events, stopped=False)
        if state == "FAIL":
            raise Invalid("quicktime_preview", "failed")
        if state == "PASS":
            evidence["preview"] = events
            break
        time.sleep(0.75)
    else:
        raise Invalid("quicktime_preview")

    probe.call("qt-start")
    probe.hold(5, "window-qt")
    probe.call("qt-stop")
    probe.poll("qt-finalized")
    probe.call("qt-save", path)
    app.quit()
    evidence["movie"] = inspect_movie(probe, path)
    verdict, _ = movie(evidence["movie"])
    if verdict != "PASS":
        return

    app.launch()
    probe.poll("qt-open", path)
    probe.poll("window-qt")
    probe.poll("qt-play", path)
    first = probe.read("qt-time", path, poll=True)
    ready = time.monotonic() + 2
    while first.get("playing") is False and time.monotonic() < ready:
        time.sleep(0.2)
        first = probe.read("qt-time", path)
    started = time.monotonic()
    probe.hold(3, "window-qt")
    last = probe.read("qt-time", path)
    evidence["playback"] = {"first": first.get("time"), "last": last.get("time"),
                            "playing": first.get("playing") is True and last.get("playing") is True,
                            "elapsed": time.monotonic() - started}


def phase(probe, name):
    evidence = {"outcome": "void", "cleanup": True}
    directory, baseline, since = None, None, None
    app = App(probe, name)
    try:
        probe.call("prerequisites")
        probe.call("idle")
        baseline = crash_reports()
        if name == "qt":
            directory = movie_dir()
        since = time.time()
        app.launch()
        if name == "ft":
            face_time(probe, evidence, since)
        else:
            quick_time(probe, app, evidence, since, directory / "recording.mov")
        check_interrupted()
        evidence["outcome"] = "observed"
    except Invalid as exc:
        evidence.update(outcome=exc.outcome, stage=exc.stage)
    except OSError:
        evidence["stage"] = "local_evidence_unavailable"
    finally:
        try:
            app.quit()
        except Invalid:
            evidence["cleanup"] = False
        if directory is not None:
            try:
                shutil.rmtree(directory)
            except OSError:
                evidence["cleanup"] = False

    if since is not None:
        # ReportCrash and the unified log deliver their final records asynchronously.
        time.sleep(5)
        try:
            evidence["events"] = camera_events(since, time.time())
        except Invalid:
            pass
        try:
            evidence["crashes"] = len(crash_reports() - baseline)
        except OSError:
            pass
    return evidence


def collect(probe):
    evidence = {"schema": 1, "preflight": "void"}
    try:
        evidence["macos"] = version()
        probe.call("unlocked")
        probe.call("prerequisites")
        camera_events(time.time(), time.time())
        crash_reports()
    except Invalid as exc:
        evidence["stage"] = exc.stage
        return evidence
    except OSError:
        evidence["stage"] = "crash_reports_unavailable"
        return evidence
    evidence["preflight"] = "ready"
    evidence["facetime"] = phase(probe, "ft")
    if evidence["facetime"]["cleanup"] and not stop_requested:
        evidence["quicktime"] = phase(probe, "qt")
    return evidence


def interrupted(_signum, _frame):
    # Defer interruption until the next action boundary. Cleanup always runs.
    global stop_requested
    stop_requested = True


def main(argv):
    if len(argv) != 2:
        return 2
    for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    try:
        evidence = collect(LocalProbe(str(Path(argv[0]).resolve())))
        # A harness run must use its fresh result directory. A stale report is never graded.
        with Path(argv[1]).open("x", encoding="utf-8") as output:
            json.dump(evidence, output, indent=2, allow_nan=False)
            output.write("\n")
    except (KeyboardInterrupt, OSError, ValueError):
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
