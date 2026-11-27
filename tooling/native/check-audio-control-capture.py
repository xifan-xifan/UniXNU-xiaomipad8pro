#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""Strict host-side checks of bounded IOAudio2 test captures; no tablet access."""
import argparse
import math
from pathlib import Path
import re
import sys


class InvalidCapture(ValueError):
    pass


def records(text, prefix):
    found = []
    for line in text.splitlines():
        if not line.startswith(prefix):
            continue
        pairs = re.findall(r"(?:^|\s)(\w+)=(\S*)", line[len(prefix):])
        record = dict(pairs)
        if len(record) != len(pairs):
            raise InvalidCapture(f"duplicate field in {prefix.strip()} record")
        found.append(record)
    return found


def one(text, prefix):
    found = records(text, prefix)
    if len(found) != 1:
        raise InvalidCapture(f"expected one {prefix.strip()} record, got {len(found)}")
    return found[0]


def success(text, tag):
    codes = re.findall(rf"^{re.escape(tag)}=(.*)$", text, re.M)
    if any(c in ("124", "137", "142") for c in codes) or re.search(
            r"Alarm clock|timed? out|TIMEOUT", text, re.I):
        raise InvalidCapture(f"TIMEOUT ({tag}); no numeric readback")
    if codes != ["0"]:
        raise InvalidCapture(f"incomplete/failed capture: {tag}={codes}")


def getter(text, wanted=None):
    success(text, "getter_rc")
    lines = text.strip().splitlines()
    # A numeric substring in stderr or a second answer can never be a value.
    if len(lines) != 2 or not re.fullmatch(r"(?:0|[1-9][0-9]?|100)", lines[0]) or lines[1] != "getter_rc=0":
        raise InvalidCapture("expected one whole-line integer 0..100 and getter_rc=0")
    value = int(lines[0])
    if wanted is not None and value != wanted:
        raise InvalidCapture(f"readback {value}, wanted {wanted}")
    return value


def number(record, key):
    s = record.get(key, "")
    if not re.fullmatch(r"[-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?", s):
        raise InvalidCapture(f"invalid {key}={s}")
    n = float(s)
    if not math.isfinite(n):
        raise InvalidCapture(f"nonfinite {key}={s}")
    return n


def require(record, **fields):
    for key, expected in fields.items():
        if record.get(key) != str(expected):
            raise InvalidCapture(f"{key}={record.get(key)}, expected {expected}")


def probe(text, wanted=None):
    success(text, "probe_rc")
    dev = one(text, "default_output ")
    require(dev, status=0)
    if not re.fullmatch(r"[1-9][0-9]*", dev.get("device", "")):
        raise InvalidCapture("no default output")
    controls = one(text, "control_list_read ")
    require(controls, status=0)
    all_controls = records(text, "control ")
    if not all_controls or controls.get("count") != str(len(all_controls)):
        raise InvalidCapture("incomplete control list")
    ids = [c.get("id", "") for c in all_controls]
    if len(set(ids)) != len(ids) or not all(re.fullmatch(r"[1-9][0-9]*", cid) for cid in ids):
        raise InvalidCapture("control IDs must be unique positive integers")
    level = [c for c in all_controls if c.get("class") == "0x766c6d65(vlme)"]
    if len(level) != 1:
        raise InvalidCapture("expected one main volume control")
    level = level[0]
    require(level, scope_status=0, scope="0x6f757470(outp)", element_status=0,
            element=0, class_status=0, base_status=0, scalar_status=0, db_status=0)
    db = number(level, "db")
    if not -36 <= db <= 0:
        raise InvalidCapture(f"dB outside advertised range: {db}")
    main = [c for c in records(text, "device_volume ") if c.get("element") == "0"]
    if len(main) != 1:
        raise InvalidCapture("missing main volm")
    main = main[0]
    vm = one(text, "virtual_main ")
    require(main, status=0, size=4)
    require(vm, status=0, size=4)
    scalars = [number(c, "scalar") for c in (level, main, vm)]
    if not all(0 <= n <= 1 for n in scalars) or max(scalars)-min(scalars) > 0.00001:
        raise InvalidCapture(f"incoherent scalar properties: {scalars}")
    if wanted is not None and any(abs(n-wanted/100) > 0.00001 for n in scalars):
        raise InvalidCapture(f"scalars {scalars}, wanted {wanted}%")
    return f"device={dev['device']} controls=" + ",".join(sorted(ids))


def identity(text):
    ids = probe(text)
    before = one(text, "pid_before ")
    after = one(text, "pid_after ")
    require(before, rc=0)
    require(after, rc=0)
    if not re.fullmatch(r"[1-9][0-9]*", before.get("pid", "")) or before["pid"] != after.get("pid"):
        raise InvalidCapture("coreaudiod PID absent/changing within snapshot")
    return ids + f" pid={before['pid']}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("getter", "probe", "identity", "stable"))
    parser.add_argument("files", type=Path, nargs="+")
    parser.add_argument("--wanted", type=int)
    args = parser.parse_args()
    try:
        texts = [p.read_text() for p in args.files]
        if args.kind == "stable":
            identities = [identity(t) for t in texts]
            if len(identities) < 2 or len(set(identities)) != 1:
                raise InvalidCapture(f"unstable identities: {identities}")
            print("stable " + identities[0])
        elif len(texts) != 1:
            raise InvalidCapture("expected one capture file")
        elif args.kind == "getter":
            print(getter(texts[0], args.wanted))
        elif args.kind == "probe":
            print("finite coherent level/volm/vmvc; " + probe(texts[0], args.wanted))
        else:
            print(identity(texts[0]))
    except (ValueError, OSError, KeyError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
