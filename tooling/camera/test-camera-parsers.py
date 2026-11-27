#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the thumbnail and sensor-table checks against synthetic inputs."""
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent

for name in ("console-thumb.py", "sensor-tables.py"):
    subprocess.run([sys.executable, HERE / name, "--self-test"], check=True)
