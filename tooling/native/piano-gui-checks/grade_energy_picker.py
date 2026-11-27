"""Grade the Energy Mode picker capture without running any commands."""
from dataclasses import dataclass
import re

from grade import parse_fields

MODE_NAMES = ("Automatic", "Low Power", "High Power")
SOURCES = {"AC Power": "ac", "Battery Power": "battery"}
WINDOW_US = 15_000_000
EXIT_CODES = {"PASS": 0, "FAIL": 1, "VOID": 2}


class Void(ValueError):
    """The capture cannot establish what happened."""


class Fail(ValueError):
    """Usable evidence contradicts the expected result."""


def one(text, pattern):
    value, = re.findall(pattern, text, re.MULTILINE)
    return value


def power_preferences(text):
    sections = re.split(r"^([^\s][^\n]*):[ \t]*$", text, flags=re.MULTILINE)
    values = {}
    for heading, body in zip(sections[1::2], sections[2::2]):
        source = SOURCES.get(heading)
        if source:
            if source in values:
                raise ValueError("duplicate power source")
            values[source] = powermode(body)
    if values.keys() != set(SOURCES.values()):
        raise ValueError("missing per-source power preferences")
    return values


def powermode(text):
    value = one(text, r"^[ \t]*powermode(?:[ \t]+([^\n]*))?$").strip()
    if value not in ("0", "1", "2"):
        raise ValueError("invalid powermode")
    return int(value)


def integer(value):
    if type(value) is not int:
        raise ValueError("expected integer evidence")
    return value


def reply(exchange, operation, *, allow_error=False):
    try:
        receipt = exchange["receipt"]
        token = exchange["token"]
        if not re.fullmatch(r"[a-f0-9]{32}", token) or receipt["token"] != token or receipt["op"] != operation:
            raise ValueError
        rc = integer(exchange["rc"])
    except (KeyError, TypeError, ValueError):
        raise Void(f"{operation} receipt missing, malformed or stale") from None
    if not allow_error and (rc or "error" in receipt):
        error = receipt.get("error", "action unconfirmed")
        if not isinstance(error, str) or not re.fullmatch(r"[a-z0-9-]+", error):
            error = "action unconfirmed"
        raise Void(f"{operation} {error}")
    return receipt


@dataclass(frozen=True)
class State:
    started: int
    end: int
    gpu_at: int
    source: str
    preferences: dict
    mode: int
    notify: tuple
    policy: tuple
    profile_rule: int
    thermal: int
    gpu: tuple


def parse_state(exchange):
    r = reply(exchange, "energy-state")
    try:
        start, end = integer(r["started_us"]), integer(r["t_us"])
        source = SOURCES[one(r["source"], r"^Now drawing from '(AC Power|Battery Power)'$")]
        if r["source_after"] != r["source"]:
            raise Void("power source changed during readback")
        preferences = power_preferences(r["custom"])
        mode = powermode(r["pmset"])
        signals = tuple(int(one(r[f"{signal}pm"], rf"^com\.apple\.system\.{name}powermode[ \t]+([01])[ \t]*$"))
                        for signal, name in (("l", "low"), ("h", "high")))
        policy = tuple(int(one(r[key], r"^([01])[ \t]*$")) for key in ("policy_lpm", "policy_hpm"))
        thermal = int(one(r["cpu"], r"^perf=[0-9]+ profile=([0-3]) "))
        cpu_at = 1000 * int(one(r["cpu"], r"\buptime_ms=([0-9]+)\b"))
        gpu = parse_fields(r["gpu"], r"(?:^|\s)(at_us|cap_mhz|ext|bind)=([^\s]+)")
        gpu_at, cap, external = (int(gpu[key]) for key in ("at_us", "cap_mhz", "ext"))
        written = int(one(r["policy"], r"GPU ceiling [0-9]+ MHz \(written ([0-9]+)[,)]"))
        rule = int(r["profile_rule"])
        if rule not in (1, 2) or not (0 < cap <= 1100 and 0 <= external <= 14):
            raise ValueError
    except Void:
        raise
    except (KeyError, TypeError, ValueError):
        raise Void("incomplete Energy Mode readback") from None
    if not (0 < start <= end and start - 999 <= cpu_at <= end and 0 < gpu_at <= end <= gpu_at + 5_000_000):
        raise Void("stale Energy Mode driver readback")
    return State(start, end, gpu_at, source, preferences, mode, signals, policy, rule, thermal,
                 (written, cap, external, gpu["bind"]))


def order(original):
    cycle = (1, 0, 2)
    offset = cycle.index(original) + 1
    return cycle[offset:] + cycle[:offset]


def differences(state, preferences, profile_rule):
    mode = preferences[state.source]
    flags = (int(mode == 1), int(mode == 2))
    profile = {0: 2 if profile_rule == 2 else 0, 1: 1, 2: 3}[mode]
    written, cap, external, binding = state.gpu
    gpu_ok = (written == 342 and 0 < external <= 3 and cap <= 342 if mode == 1 else
              written == 0 and binding != "lpm" and external != 3)
    checks = {
        "pmset powermode and per-source preferences": state.mode == mode and state.preferences == preferences,
        "powerd notifications": state.notify == flags,
        "PianoPolicy LPM/HPM": state.policy == flags,
        "PianoThermal profile": state.thermal == profile and state.profile_rule == profile_rule,
        "PianoA830 ceiling": gpu_ok,
    }
    return [name for name, ok in checks.items() if not ok]


def initial_state(capture):
    state = parse_state(capture.get("initial"))
    missed = differences(state, state.preferences, state.profile_rule)
    if missed:
        raise Fail("initial mode disagrees with " + ", ".join(missed))
    return state


def opened_picker(exchange, initial):
    r = reply(exchange, "energy-open")
    if type(r.get("mode")) is not int or type(r.get("t_us")) is not int or r["t_us"] <= initial.end:
        raise Void("Battery picker mode or open timestamp missing or stale")
    if r.get("source") != initial.source or r["mode"] != initial.mode:
        raise Fail("Battery picker disagrees with the initial Energy Mode")
    return r["t_us"]


def selection(exchange, initial, before, target, after):
    r = reply(exchange, "energy-select")
    try:
        pressed, completed = integer(r["pressed_us"]), integer(r["t_us"])
        status, mode = integer(r["status"]), integer(r["mode"])
        if (not after < pressed <= completed or r["source"] != initial.source or
                integer(r["before"]) != before or integer(r["target"]) != target):
            raise ValueError
    except (KeyError, TypeError, ValueError):
        raise Void("Energy Mode selection receipt malformed or stale") from None
    if status:
        raise Fail(f"Battery picker refused {MODE_NAMES[target]}")
    if mode == -1:
        raise Void("Battery picker readback missing after selection")
    if mode != target:
        raise Fail(f"Battery picker did not select {MODE_NAMES[target]}")
    return pressed, completed


def readback(exchange, initial, preferences, after, gpu_after, *, transition=False):
    state = parse_state(exchange)
    if state.started <= after or state.gpu_at <= gpu_after:
        raise Void("readback predates the Energy Mode action")
    if transition and state.end > gpu_after + WINDOW_US:
        raise Void("Energy Mode readback outside the 15-second propagation window")
    if state.source != initial.source:
        raise Void("power source changed during the picker check")
    return state, differences(state, preferences, initial.profile_rule)


def require_readback(exchange, initial, preferences, after, gpu_after, label, *, transition=False):
    state, missed = readback(exchange, initial, preferences, after, gpu_after, transition=transition)
    if missed:
        if transition and state.end < gpu_after + WINDOW_US - 3_000_000:
            raise Void(f"{label} propagation unconfirmed near the end of the wait")
        raise Fail(f"{label} disagrees with " + ", ".join(missed))
    return state


def finished_at(exchange, after):
    finish = reply(exchange, "energy-finish", allow_error=True)
    if finish.get("restored") is False:
        raise Fail("original AC and battery Energy Modes were not restored")
    if finish.get("closed") is False:
        raise Fail("System Settings did not quit cleanly")
    ended = finish.get("t_us")
    if (exchange["rc"] or finish.get("restored") is not True or finish.get("closed") is not True or
            type(ended) is not int or ended <= after):
        raise Void("System Settings quit and preference restoration unconfirmed")
    return ended


def grade_capture(capture):
    """Return (PASS, FAIL or VOID, explanation) for a single capture."""
    if not isinstance(capture, dict) or capture.get("version") != 1:
        return "VOID", "missing or unsupported Energy Mode capture"
    try:
        initial = initial_state(capture)
    except (Fail, Void) as error:
        return ("FAIL" if isinstance(error, Fail) else "VOID"), str(error)

    outcome = ("PASS", f"all three Energy Modes propagated on {initial.source} and both preferences were restored")
    after = initial.end
    previous = initial
    try:
        if reply(capture.get("arm"), "energy-begin").get("armed") is not True:
            raise Void("original Energy Mode preferences were not journaled")
        after = opened_picker(capture.get("open"), initial)
        steps = capture.get("steps", [])
        targets = order(initial.mode)
        if not isinstance(steps, list) or len(steps) > len(targets):
            raise Void("invalid Energy Mode action sequence")
        for target, step in zip(targets, steps):
            if not isinstance(step, dict):
                raise Void("invalid Energy Mode step")
            pressed, after = selection(step.get("selection"), initial, previous.mode, target, after)
            preferences = dict(initial.preferences, **{initial.source: target})
            previous = require_readback(step.get("state"), initial, preferences, after, pressed,
                                        MODE_NAMES[target], transition=True)
            after = previous.end
        if len(steps) != len(targets) or capture.get("interrupted") is True:
            raise Void("Energy Mode picker sequence interrupted or incomplete")
    except (Fail, Void) as error:
        outcome = ("FAIL" if isinstance(error, Fail) else "VOID"), str(error)

    if "arm" in capture:
        try:
            finished = finished_at(capture.get("finish"), after)
            require_readback(capture.get("final"), initial, initial.preferences, finished, finished, "restored Energy Mode")
        except Fail as error:
            return "FAIL", str(error)
        except Void as error:
            return ("FAIL" if outcome[0] == "FAIL" else "VOID"), f"{outcome[1]}; {error}" if outcome[0] != "PASS" else str(error)
    return outcome
