"""Capture transitions during an already authorized piano RAM trial."""
import argparse
import os
from datetime import datetime, timezone
from pathlib import Path
import subprocess
import time


def probe(executable, *arguments):
    try:
        result = subprocess.run([executable, *arguments], capture_output=True,
                                text=True, timeout=2)
        if result.returncode:
            return f"error:{result.returncode}"
        return result.stdout.strip()
    except (OSError, subprocess.TimeoutExpired) as error:
        return type(error).__name__


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools", type=Path, required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--seconds", type=int, default=90)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds < 1:
        parser.error("--seconds must be positive")
    suffix = ".exe" if os.name == "nt" else ""
    fastboot = args.tools / ("fastboot" + suffix)
    adb = args.tools / ("adb" + suffix)
    end = time.monotonic() + args.seconds
    previous = None
    with args.log.open("x", encoding="utf-8") as log:
        while time.monotonic() < end:
            timestamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
            devices = probe(fastboot, "devices")
            bootloaders = [line.split()[0] for line in devices.splitlines()
                           if line and line.split()[0] in (args.serial, "piano-uefi")]
            android = probe(adb, "devices")
            adb_states = [line.split()[1] for line in android.splitlines()
                          if len(line.split()) >= 2 and line.split()[0] == args.serial]
            errors = [value for value in (devices, android)
                      if value.startswith("error:") or value in ("FileNotFoundError", "TimeoutExpired")]
            state = (bootloaders, adb_states, errors)
            if state != previous:
                print(timestamp, "fastboot", bootloaders, "adb", adb_states,
                      "probe errors", errors, file=log, flush=True)
                previous = state
            time.sleep(0.1)
        print(datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
              "observation ended", file=log)


if __name__ == "__main__":
    main()
