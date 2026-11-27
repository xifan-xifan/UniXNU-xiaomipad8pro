"""Collect Energy Mode receipts via the installed GUI actions transport."""
import json
import os
import signal
import subprocess
import tempfile
import time
from uuid import uuid4

from grade_energy_picker import (
    EXIT_CODES, WINDOW_US, Fail, Void, finished_at, grade_capture, initial_state, opened_picker, order,
    readback, reply, selection,
)


def write_capture(path, capture):
    out = tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False)
    try:
        with out:
            json.dump(capture, out, indent=2, sort_keys=True)
            out.write("\n")
        os.replace(out.name, path)
    finally:
        if os.path.exists(out.name):
            os.unlink(out.name)


def check(bash, parse_reply, verdict, path):
    capture = {"version": 1, "steps": []}
    write_capture(path, capture)  # Invalidate any earlier run before the first probe.

    def exchange(operation, *arguments):
        token = uuid4().hex
        record = {"token": token, "rc": None, "receipt": None}
        limit = 8 if operation == "energy-state" else 55
        try:
            result = bash(f"SSH_T={limit} gui_checks_remote", operation, token, *arguments, timeout=limit + 3)
            record["rc"] = result.returncode
            info = parse_reply(result.stdout.decode(errors="replace"))
            if info.get("token") == token and info.get("op") == operation:
                record["receipt"] = info
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
        return record

    def observe(initial, preferences, after, gpu_after, *, transition=False):
        deadline = time.monotonic() + WINDOW_US / 1_000_000
        latest_usable = None
        while True:
            record = exchange("energy-state")
            try:
                state, missed = readback(record, initial, preferences, after, gpu_after, transition=transition)
                latest_usable = record
                if not missed:
                    return record, state
            except Void:
                pass
            if time.monotonic() >= deadline:
                return latest_usable or record, None
            time.sleep(.4)

    try:
        capture["initial"] = exchange("energy-state")
        initial = initial_state(capture)
        after = initial.end
        capture["arm"] = None  # A lost arm reply can still leave a recovery journal.
        capture["arm"] = exchange("energy-begin", initial.preferences["ac"], initial.preferences["battery"])
        if reply(capture["arm"], "energy-begin").get("armed") is not True:
            raise Void("original preferences were not journaled")
        capture["open"] = exchange("energy-open", initial.source)
        after = opened_picker(capture["open"], initial)
        before = initial.mode
        for target in order(before):
            step = {}
            capture["steps"].append(step)
            step["selection"] = exchange("energy-select", initial.source, before, target)
            gpu_after, after = selection(step["selection"], initial, before, target, after)
            preferences = dict(initial.preferences, **{initial.source: target})
            step["state"], state = observe(initial, preferences, after, gpu_after, transition=True)
            if state is None:
                break
            before, after = target, state.end
    except (Fail, Void):
        pass  # Replay the saved receipts to produce the same result as the offline grader.
    except KeyboardInterrupt:
        capture["interrupted"] = True
    finally:
        # Repeated signals must not interrupt closing Settings and restoring preferences.
        handlers = {sig: signal.signal(sig, signal.SIG_IGN)
                    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
        try:
            if "arm" in capture:
                capture["finish"] = exchange("energy-finish")
                try:
                    finished = finished_at(capture["finish"], after)
                    capture["final"], _ = observe(initial, initial.preferences, finished, finished)
                except (Fail, Void):
                    pass
            write_capture(path, capture)
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)

    status, reason = grade_capture(capture)
    verdict(status, f"Energy Mode picker: {reason} (gui-checks/{path.name})")
    return EXIT_CODES[status]
