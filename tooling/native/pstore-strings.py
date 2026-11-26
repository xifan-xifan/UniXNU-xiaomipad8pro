#!/usr/bin/env python3
"""Pull readable lines out of a pstore dump when the majority decode is degraded.

`xnu-log.py decode` votes over the copies k27 keeps (11 for DBGR/DBGB) and repairs the DRAM bit decay; after a hot run the vote can
fall to its weak threshold and the text arrives broken in places while the important lines are still there. This reads the raw dump
and prints runs of printable bytes, which is enough to grep a stage name, a panic line or one of our own IOLog lines.

    tooling/native/pstore-strings.py DUMP.bin OUT.txt [MIN-RUN]
"""
import pathlib
import re
import sys

dump, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
minimum = int(sys.argv[3]) if len(sys.argv) > 3 else 10
raw = dump.read_bytes()
runs = [m.group(0) for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minimum, raw)]
out.write_text("\n".join(r.decode("ascii", "replace").strip() for r in runs) + "\n", encoding="utf-8")
print(f"{dump.name}: {len(raw)} bytes -> {len(runs)} runs, {out} ({out.stat().st_size} bytes)")
