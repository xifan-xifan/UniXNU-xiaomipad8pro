#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Grade a read-only Bluetooth preflight. Exit 0 READY, 1 NOT-READY, or 2 VOID."""

import argparse
import re
from pathlib import Path

SECTIONS = (
    "sw_vers", "boot", "bootargs", "sysctl", "dev", "pianobt", "hci", "service",
    "adt", "runtime", "helpers", "mount", "launchd", "probe", "profiler",
    "lemask-log", "trace", "end",
)
SETTLE = re.compile(r"BT_PREFLIGHT_SETTLE status=(ready|timeout|skipped) elapsed_seconds=(\d+) wait_seconds=(\d+)")
MASK = re.compile(r"bt-lemask1: LE event mask [0-9a-f]{16} -> ([0-9a-f]{16})\b", re.I)
RUNTIME_FILES = (
    "bin/bluetoothd", "bin/BlueTool", "lib/QcaBTType0.dylib", "bin/PianoBluetooth.runtime",
)
HELPERS = (
    "/usr/local/libexec/piano-bluetooth-start",
    "/usr/local/libexec/piano-bt-runtime-mount",
    "/usr/local/libexec/piano-bt-probe",
    "/Library/LaunchDaemons/com.unixnu.piano-bluetooth-path1.plist",
)
JOBS = ("com.unixnu.piano-bluetooth-path1", "com.apple.BlueTool", "com.apple.bluetoothd")


def parse_capture(text):
    sections = {}
    section = None
    settle = None
    for line in text.splitlines():
        if line.startswith("@@ "):
            section = line[3:]
            if section in sections:
                raise ValueError("capture has a duplicate section")
            sections[section] = []
        elif section is not None:
            sections[section].append(line)
        elif line.startswith("BT_PREFLIGHT_SETTLE"):
            match = SETTLE.fullmatch(line)
            if not match or settle is not None:
                raise ValueError("capture has invalid settle metadata")
            settle = match.groups()
    if tuple(sections) != SECTIONS:
        missing = [name for name in SECTIONS if name not in sections]
        detail = ", ".join(missing) if missing else "unexpected section order or name"
        raise ValueError(f"capture has incomplete sections ({detail})")
    if [line for line in sections["end"] if line.strip()] != ["BT_PREFLIGHT_DONE"]:
        raise ValueError("capture lacks a clean BT_PREFLIGHT_DONE terminator")
    return {name: "\n".join(lines) for name, lines in sections.items()}, settle


def has_mask_line(text):
    return any(int(match[1], 16) & 0x840 == 0x840 for match in MASK.finditer(text))


def mask_source(log, boot_console):
    if has_mask_line(log):
        return "unified log"
    if boot_console is not None:
        with boot_console.open(errors="replace") as console:
            if any(has_mask_line(line) for line in console):
                return "boot console"
    return None


def matches(pattern, section):
    return re.search(pattern, section, re.MULTILINE) is not None


def grade(sections, settle, source):
    failures = 0

    def check(tag, passed, requirement):
        nonlocal failures
        print(f"{'PASS' if passed else 'FAIL'} BT-{tag} {requirement}")
        if not passed:
            failures += 1

    if settle:
        status, elapsed, budget = settle
        print(f"INFO BT-settle status={status} elapsed_seconds={elapsed} wait_seconds={budget}")
        if status == "timeout":
            check("settle", False, "startup readiness within the wait budget")

    check("build", matches(r"^BuildVersion:[ \t]+26C5035r[ \t]*$", sections["sw_vers"]), "macOS build 26C5035r")
    check("bootsession", matches(
        r"^kern\.bootsessionuuid: [0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$", sections["boot"],
    ), "boot session UUID readable")
    args = {}
    for token in sections["bootargs"].split():
        key, sep, value = token.partition("=")
        if sep:
            args.setdefault(key, set()).add(value)
    check("arg-bt", args.get("piano-bt") == {"5"}, "effective piano-bt=5")
    check("arg-usbnet", args.get("piano-usb-net") == {"1"}, "effective piano-usb-net=1")
    check("arg-btstop", args.get("piano-bt-stop", {"0"}) == {"0"}, "piano-bt-stop absent or zero")
    check("arg-baud", args.get("piano-bt-baud", {"3200000"}) == {"3200000"}, "transport speed 3200000 bit/s")
    check("arg-ibs", args.get("piano-bt-ibs", {"0"}) == {"0"}, "IBS disabled")

    values = dict(line.split(":", 1) for line in sections["sysctl"].splitlines() if line.startswith("machdep.") and ":" in line)
    check("boot0", values.get("machdep.piano_bt_boot", "").strip() == "0", "machdep.piano_bt_boot=0")
    check("firmware", bool(values.get("machdep.piano_bt_firmware", "").strip()), "nonempty firmware build label")
    check("lemask-sysctl", values.get("machdep.piano_bt_lemask", "").strip() == "1", "machdep.piano_bt_lemask=1")
    check("lerdy-sysctl", values.get("machdep.piano_bt_lerdy", "").strip() == "0", "machdep.piano_bt_lerdy=0")
    check("dev", matches(r"^c\S+[ \t]+.+[ \t]/dev/cu\.BLTH$", sections["dev"]), "character device /dev/cu.BLTH")
    check("host-ready", matches(r'^[ \t]*"piano-bt-host-ready" = Yes$', sections["pianobt"]), "PianoBluetooth host ready")
    check("hci", matches(r"^\+-o IOBluetoothHCIController[ \t]+<class IOBluetoothHCIController\b", sections["hci"]),
          "IOBluetoothHCIController published")
    check("service", matches(r"^\+-o bluetooth[ \t]+<class ", sections["service"]), "bluetooth service published")
    check("adt", matches(r'^[ \t]*"transport-encoding" = <01000000>$', sections["adt"])
          and matches(r'^[ \t]*"transport-speed" = <00[dD]43000>$', sections["adt"]),
          "ADT transport encoding 1 and speed 3200000")

    check("mask-counter", matches(r'^[ \t]*"piano-bt-h4-le-masks-patched" = [0-9]*[1-9][0-9]*$', sections["pianobt"]),
          "piano-bt-h4-le-masks-patched > 0")
    check("mask-line", source is not None, f"LE event mask bits 6 and 11 set ({source or 'no matching line'})")

    runtime = sections["runtime"].splitlines()
    check("runtime", all(f"{name}: OK" in runtime for name in RUNTIME_FILES)
          and "staged and mounted runtime match" in runtime and "FAILED" not in sections["runtime"],
          "four runtime hashes pass and staged files match mounted files")
    for path in HELPERS:
        check("helper", matches(r"^\S+[ \t]+.+[ \t]" + re.escape(path) + r"$", sections["helpers"]), f"exists {path}")
    for target in ("/usr/sbin", "/usr/lib"):
        mounts = re.findall(rf"^.+ on {target} \(([^)\n]+)\)$", sections["mount"], re.MULTILINE)
        check("mount", bool(mounts) and all({"bindfs", "read-only"} <= set(options.split(", ")) for options in mounts),
              f"read-only bindfs view at {target}")
    launchd = re.split(r"^system/(\S+) = \{$", sections["launchd"], flags=re.MULTILINE)
    jobs = dict(zip(launchd[1::2], launchd[2::2]))
    for job in JOBS:
        check("launchd", job in jobs, f"launchd has {job}")
    bluetoothd = jobs.get("com.apple.bluetoothd", "")
    check("bluetoothd", matches(r"^\tstate = running$", bluetoothd) and matches(r"^\tpid = [1-9][0-9]*$", bluetoothd),
          "bluetoothd running with a PID")

    check("power", matches(r"^piano-bt-path1 probe: default_object=1 power=1$", sections["probe"]),
          "probe default_object=1 power=1")
    check("profiler", matches(r"^[ \t]*State: On[ \t]*$", sections["profiler"]), "System Profiler Bluetooth State: On")

    traces = re.findall(r"^\S.*\.pklg$", sections["trace"], re.MULTILINE)
    armed = matches(r"^[ \t]*HCIFileTraces[ \t]*=[ \t]*1;?[ \t]*$", sections["trace"])
    print(f"INFO BT-trace HCI trace preference armed={int(armed)}, pklg files={len(traces)}")
    print(f"BT_PREFLIGHT NOT-READY ({failures} gate(s) failed)" if failures else "BT_PREFLIGHT READY")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("preflight", type=Path, help="structured preflight.txt")
    parser.add_argument("boot_console", type=Path, nargs="?", help="console log from the same boot for LE mask fallback")
    args = parser.parse_args()
    try:
        sections, settle = parse_capture(args.preflight.read_text(errors="replace"))
        source = mask_source(sections["lemask-log"], args.boot_console)
    except ValueError as exc:
        print(f"VOID BT {exc}")
    except OSError:
        print("VOID BT could not read the preflight or requested boot console")
    else:
        return grade(sections, settle, source)
    print("BT_PREFLIGHT VOID")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
