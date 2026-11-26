#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# firmware/patches/darwinboot/region0-allow.json (what DarwinBoot publishes for region0) and package/include/PianoExtraRam.h (what the
# kexts accept) must agree: every allowed run with its use tag and stage, every never-use span. Also checks the JSON itself: 2 MiB
# alignment, sizes, no overlap between allowed runs and never spans, everything inside region0.
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
J = ROOT / "firmware/patches/darwinboot/region0-allow.json"
H = ROOT / "package/include/PianoExtraRam.h"
PATCH_DIR = J.parent
SLAB = 2 << 20
USE = {"UseAny": 0, "UseGpu": 1, "UseDsp": 2, "UseVideo": 3, "UseCamera": 4, "UseDisplay": 5, "UseNone": None}


def table(src, name):
    m = re.search(r"kSpan\s+%s\[\]\s*=\s*\{(.*?)\n\};" % name, src.replace("constexpr Span", "kSpan"), re.S)
    if not m:
        sys.exit("region0_allow_check: no table %s in %s" % (name, H))
    rows = re.findall(r'\{(0x[0-9a-f]+)ull,\s*(0x[0-9a-f]+)ull,\s*(Region[01]),\s*(Use\w+),\s*(\d+),\s*"([^"]+)"\}', m.group(1))
    return [(int(a, 16), int(b, 16), bank, USE[use], int(stage), n) for a, b, bank, use, stage, n in rows]


def main():
    d = json.loads(J.read_text())
    src = H.read_text()
    allowed = [r for r in table(src, "kAllowed") if r[2] == "Region0"]
    never = [r for r in table(src, "kNever") if r[2] == "Region0"]
    errors = []
    ja = [(int(r["base"], 16), int(r["base"], 16) + int(r["size"], 16), "Region0", r["use"], r["stage"], r["name"]) for r in d["allow"]]
    jn = [(int(r["base"], 16), int(r["end"], 16), r["name"]) for r in d["never"]]
    patch = next(PATCH_DIR.glob("0002-*.patch")).read_text()
    fw0 = re.search(r"DarwinExtraRamRegion0\[\] = \{(.*?)\n\+\};", patch, re.S).group(1)
    fw1 = re.search(r"DarwinExtraRamRegion1\[\]\[2\] = \{(.*?)\n\+\};", patch, re.S).group(1)
    firmware_allow = re.findall(r'\{\{(0x[0-9a-f]+)ULL, (0x[0-9a-f]+)ULL, (\d+), (\d+)\}, (\d+), "([^"]+)"\}', fw0)
    json_allow = [(r["base"], r["size"], str(r["bank"]), str(r["use"]), str(r["stage"]), r["name"]) for r in d["allow"]]
    if firmware_allow != json_allow:
        errors.append("allow: JSON and DarwinBoot region0 table disagree")
    firmware_region1 = [(int(a, 16), int(b, 16)) for a, b in re.findall(r"\{(0x[0-9a-f]+)ULL, (0x[0-9a-f]+)ULL\}", fw1)]
    if firmware_region1 != [(a, b) for a, b, bank, *_ in table(src, "kAllowed") if bank == "Region1"]:
        errors.append("allow: DarwinBoot and PianoExtraRam.h region1 bounds disagree")
    if ja != allowed:
        errors.append("allow: JSON %s != header %s" % (ja, allowed))
    if [(a, b, n) for a, b, _, _, _, n in never] != jn:
        errors.append("never: JSON %s != header %s" % (jn, [(a, b, n) for a, b, _, _, _, n in never]))
    if d.get("slab_bytes") != SLAB:
        errors.append("slab_bytes is not 2 MiB")
    for r in d["allow"]:
        base, size = int(r["base"], 16), int(r["size"], 16)
        if base % SLAB or size % SLAB or not size:
            errors.append("%s: not whole 2 MiB slabs" % r["name"])
        if r["mib"] != size >> 20:
            errors.append("%s: mib %d != size %d MiB" % (r["name"], r["mib"], size >> 20))
        if base < 0x80000000 or base + size > 0x100000000 or r["bank"] != 0:
            errors.append("%s: outside region0" % r["name"])
        for a, b, n in jn:
            if base < b and a < base + size:
                errors.append("%s overlaps never-use %s" % (r["name"], n))
        if not r.get("reason"):
            errors.append("%s: no reason" % r["name"])
    s = sorted(ja)
    for x, y in zip(s, s[1:]):
        if x[1] > y[0]:
            errors.append("allowed runs %s and %s overlap" % (x[5], y[5]))
    for e in errors:
        print("region0_allow_check: " + e, file=sys.stderr)
    if errors:
        return 1
    print("region0_allow_check: ok (%d allowed runs, %d MiB; %d never spans)" % (len(ja), sum(b - a for a, b, *_ in ja) >> 20, len(jn)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
