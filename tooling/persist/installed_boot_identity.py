"""Verify the boot arguments and bound Data store of an installed ESP."""

import plistlib
import re
import shlex
from xml.parsers.expat import ExpatError


BOOT_PROBE = (
    "/usr/sbin/sysctl -n kern.bootsessionuuid && "
    "/usr/sbin/sysctl -n kern.bootargs && "
    "/usr/sbin/ioreg -a -l -p IODeviceTree -r -n chosen -d 1 && "
    "/usr/sbin/sysctl -n kern.bootsessionuuid"
)
BOOT_UUID = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}", re.I)
STORE = re.compile(
    r"^NativeRamBlock: Data store state (\d+) [^\r\n]*?first LBA (\d+),"
    r"[^\r\n]*?\bseed ([0-9a-f]{8}), store seed ([0-9a-f]{8})(?:,|\s|$)",
    re.M,
)
DRIVER_ARGS = {
    "piano-usb": r"^PianoDwc3: start: piano-usb=(\d+) ",
    "piano-usb-net": r"^PianoDwc3: start: [^\n]*?\bnet=(\d+) ",
    "piano-backlight": r"^PianoBacklight: start: piano-backlight=(\d+) ",
    "piano-backlight-keys": r"^PianoBacklight: start: [^\n]*?, keys (\d+),",
    "piano-backlight-auto": r"^PianoBacklight: start: [^\n]*?\(piano-backlight-auto=(1):",
    "piano-audio": r"^PianoAudio: start: piano-audio=(\d+) ",
    "piano-audio-gain": r"^PianoAudio: live: level \d+: [^\n]*?playing gain (\d+) ",
    "piano-video": r"^PianoVideo: start: piano-video=(\d+) ",
    "piano-video-vt": r"^PianoVideo: vt attach: piano-video-vt=(\d+) ",
    "piano-gpu-user-vm": r"^PianoA830: gpu bo backing: user fallback=(unmapped-vm|mapped-control);",
}


class MissingBootEvidence(ValueError):
    """The capture or transport cannot establish the live identity."""


def effective(line, keys):
    settings = {}
    for word in shlex.split(line):
        key = word.partition("=")[0]
        if key in keys and settings.setdefault(key, word) != word:
            raise ValueError(f"conflicting installed boot arguments for {key}")
    return settings


def need_args(line, needed):
    required = effective(needed, {word.partition("=")[0] for word in needed.split()})
    settings = effective(line, required)
    if settings != required:
        raise ValueError("NEED_ARGS do not match the effective boot arguments")


def argument_profile(cmdline, split):
    if not isinstance(cmdline, str) or not isinstance(split, dict) or not isinstance(split.get("tail", ""), str):
        raise ValueError("invalid installed boot-argument record")
    selector = effective(cmdline, {"piano-boot-args"}).get("piano-boot-args")
    if selector is None:
        tail_property = "piano-boot-args"
    elif selector == "piano-boot-args=0":
        tail_property = None
    elif re.fullmatch(r"piano-boot-args=[A-Za-z0-9._-]{1,15}", selector):
        tail_property = selector.replace("=", "-")
    else:
        raise ValueError("invalid installed ADT boot-args selector")

    tail = split.get("tail", "").strip(" ")
    if split and (tail_property is None or split.get("property") != tail_property or not tail):
        raise ValueError("installed ADT boot-args tail is missing or belongs to a different entry")
    if selector not in (None, "piano-boot-args=0") and not tail:
        raise ValueError("installed ADT boot-args tail is missing or belongs to a different entry")
    args = " ".join(part for part in (cmdline, tail) if part)
    if not re.fullmatch(r"[ -~]{1,4095}", args):
        raise ValueError("invalid installed boot-argument text")
    return {"cmdline": cmdline, "tail": tail, "tail_property": tail_property, "args": args}


def legacy_clip(line):
    # k27 0066's BOOT_LINE_LENGTH buffer keeps only whole arguments.
    prefix = line[:1023]
    if len(line) > 1023 and line[1023] != " " and not prefix.endswith(" "):
        prefix = prefix.rpartition(" ")[0]
    return prefix.rstrip(" ")


def chosen_observation(reported, tail_property):
    try:
        before, cmdline, frame = reported.split("\n", 2)
        xml, after = frame.rstrip("\r\n").rsplit("\n", 1)
        if not BOOT_UUID.fullmatch(before) or before != after:
            raise ValueError
        roots = plistlib.loads(xml.encode())
        chosen, = [node for node in roots if node.get("IORegistryEntryName") == "chosen"]
    except (AttributeError, ValueError, TypeError, plistlib.InvalidFileException, ExpatError) as exc:
        raise MissingBootEvidence("missing or malformed same-boot /chosen read") from exc
    raw = chosen.get(tail_property, b"") if tail_property else b""
    if not isinstance(raw, bytes):
        raise MissingBootEvidence("malformed /chosen boot-args property")
    text, nul, padding = raw.partition(b"\0")
    if raw and (not nul or any(padding) or any(not 0x20 <= byte <= 0x7e for byte in text)):
        raise MissingBootEvidence("malformed /chosen boot-args property")
    tail = text.decode("ascii").strip(" ")
    return cmdline, tail


def check_legacy(profile, reported):
    if not re.fullmatch(r"[ -~]{1,1023}", reported):
        raise MissingBootEvidence("missing or malformed kern.bootargs")
    if reported not in {profile["cmdline"], legacy_clip(profile["cmdline"]), legacy_clip(profile["args"])}:
        raise ValueError("kern.bootargs differs from the ESP cmdline and its k27 clip")


def check_driver_args(data, needed, must_show):
    for word in needed.split():
        key, _, expected = word.partition("=")
        pattern = DRIVER_ARGS.get(key)
        seen = set(re.findall(pattern, data, re.M)) if pattern else set()
        if key == "piano-gpu-user-vm":
            seen = {"1" if value == "unmapped-vm" else "0" for value in seen}
        if not seen and key in must_show:
            raise MissingBootEvidence(f"no same-boot console evidence for {key}")
        if seen and seen != {expected}:
            raise ValueError(f"same-boot driver value differs for {key}")


def live_identity(profile, needed, console, source, reported):
    need_args(profile["args"], needed)
    try:
        data = console.read_bytes().decode(errors="replace")
    except OSError as exc:
        raise MissingBootEvidence("cannot read the disk-boot console") from exc
    must_show = set()
    if source == "chosen":
        cmdline, tail = chosen_observation(reported, profile["tail_property"])
        check_legacy(profile, cmdline)
        if tail != profile["tail"]:
            raise ValueError("live /chosen boot-args tail differs from the ESP")
        proof = "kern.bootargs and exact /chosen tail"
    elif source == "console-replay":
        # The archived launchd line was interleaved. The replay supplies only its intact cmdline
        # segment and requires positive driver evidence for every requested argument beyond it.
        cmdline = reported.rstrip("\r\n")
        check_legacy(profile, cmdline)
        observed = effective(cmdline, {word.partition("=")[0] for word in needed.split()})
        must_show = {word.partition("=")[0] for word in needed.split()
                     if observed.get(word.partition("=")[0]) != word}
        proof = "archived cmdline and requested driver values (replay)"
    else:
        raise MissingBootEvidence("unsupported live boot evidence source")

    check_driver_args(data, needed, must_show)
    stores = set(STORE.findall(data))
    if not stores:
        raise MissingBootEvidence("console lacks a parseable owner Data store line")
    if stores != {("2", str(profile["lba"]), profile["seed"], profile["seed"])}:
        raise ValueError("console does not show the recorded owner Data store resumed")
    print(f"installed identity matched via {proof}; resumed owner Data verified")
