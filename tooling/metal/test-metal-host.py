#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the Metal host checks that require only repository sources and Xcode."""
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
TURNIP = HERE / "turnip-a830"
TESTS = TURNIP / "tests"

AIR_FIXTURE = """\
define void @fixture() {
  ret void
}
!air.visible = !{!0}
!0 = !{ptr @fixture, !1}
!1 = !{!"air.stitching_info", null}
"""


def main():
    subprocess.run([sys.executable, HERE / "check-bundle-paths.py", "--self-test"], check=True)
    for script in (HERE / "build-turnip-a830.sh", *sorted(TURNIP.glob("test-*-host.sh"))):
        subprocess.run(["bash", "-n", script], check=True)
    with tempfile.TemporaryDirectory(prefix="metal-host-") as tmp:
        out = Path(tmp)
        source, bitcode = out / "fixture.ll", out / "fixture.bc"
        source.write_text(AIR_FIXTURE)
        subprocess.run(["xcrun", "clang", "-Wno-override-module", "-x", "ir", "-emit-llvm", "-c",
                        source, "-o", bitcode], check=True)
        binary = out / "air_signature_host"
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-I", TURNIP / "frontend", TESTS / "air_signature_host.cpp", "-o", binary], check=True)
        subprocess.run([binary, bitcode], check=True, timeout=30)
        subprocess.run([sys.executable, TESTS / "frontend_pressure_host.py", "--mock-iosurface", "--out", tmp], check=True)


if __name__ == "__main__":
    main()
