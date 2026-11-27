#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Collect and qualify piano-durability transcripts. The outer harness owns USB and booting."""

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import threading
import time

PHASES = {"volatile": (0, 0), "control": (0, 0), "writeback": (1, 0),
          "queue": (0, 1), "combined": (1, 1)}
ROWS = ("plain", "fsync", "full", "barrier")
SYNCED_ROWS = ("fsync", "full", "barrier")
OPS, READS, RECORDS, BLOCK = 32, 4096, 32, 4096
COUNTERS = ("fua", "cached", "store_sync", "flushes", "barriers", "forced",
            "store_rd", "bitmap_publications", "accepted", "finished", "callbacks", "full")
IDENTITY = ("lba", "bound", "pages", "generation", "seed", "store_seed", "flags")
UUID = r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}"
RESET = re.compile(rb"piano-reset-after: (\d+) s reached, PSCI SYSTEM_RESET")
CLEAN = re.compile(rb"restart reason: bootloader|piano-psci-halt: PE_halt_restart|shutdown notifications")
FAULT = re.compile(rb"Data write-back cache sync failed|halt/restart timed out|"
                   rb"unconfirmed Data write-back epoch|UFS.*(?:timeout|quarantin)|panic\(",
                   re.I)


class Void(ValueError):
    """Missing or contradictory evidence."""


class Fail(ValueError):
    """Valid evidence of a qualification failure."""


def require(condition, reason, kind=Void):
    if not condition:
        raise kind(reason)


def count(row, key):
    # The daily sysctl reports these two counters as OPS/USus. Older captures use bare counts.
    pattern = r"(\d{1,20})(?:/\d{1,20}us)?" if key in ("store_rd", "store_sync") else r"(\d{1,20})"
    match = re.fullmatch(pattern, row.get(key, ""))
    require(match, f"missing or invalid {key}")
    return int(match[1])


def fields(tokens):
    result = {}
    for token in tokens:
        key, sep, value = token.partition("=")
        require(sep and key and value and key not in result, "invalid protocol field")
        result[key] = value
    return result


class Transcript:
    def __init__(self, lines):
        self.lines = iter(lines)

    def take(self, source, event):
        line = next(self.lines, "").split()
        if line[:2] == ["DUR", "ERROR"]:
            row = fields(line[2:])
            kind = Fail if row.get("status") == "FAIL" else Void
            raise kind(f"tablet {row.get('reason', 'error')} (errno {row.get('errno', '?')})")
        require(line[:2] == [source, event], f"missing {source} {event}")
        return fields(line[2:])

    def end(self, cut=False):
        if not cut:
            require(not self.take("DUR", "END"), "malformed END")
        status = self.take("HOST", "EXIT")
        require(count(status, "rc") == (255 if cut else 0), "unexpected SSH exit")
        if not cut:
            require(next(self.lines, None) is None, "trailing transcript data")


def snapshot(stream, phase, row, edge, op, first=None):
    sample = stream.take("DUR", "SNAP")
    require((sample.get("row"), sample.get("edge"), count(sample, "op")) == (row, edge, op),
            f"wrong snapshot for {row} {edge} {op}")
    require((count(sample, "write_back"), count(sample, "queue")) == PHASES[phase],
            "wrong live switch values")
    require(count(sample, "ram") == count(sample, "qual") == 1, "wrong RAM profile")
    state, lba, bound, pages, gen, flags = (
        count(sample, k) for k in ("state", "lba", "bound", "pages", "generation", "flags"))
    if phase == "volatile":
        require(sample.get("mode") == "volatile" and count(sample, "persist") == 0
                and state == lba == bound == pages == gen == flags == 0, "wrong volatile identity")
    else:
        require(sample.get("mode") == "concurrent" and 2 <= count(sample, "depth") <= 8
                and count(sample, "scratch") == 1 and count(sample, "persist") == 2
                and state in (2, 3) and lba == bound > 0 and pages > 0 and gen > 0 and flags == 2
                and re.fullmatch(r"[0-9a-f]{8}", sample.get("seed", ""))
                and sample.get("seed") == sample.get("store_seed"), "wrong scratch identity")
        if PHASES[phase][1]:
            require(count(sample, "readers") > 0 and count(sample, "writers") > 0, "queue has no workers")
    require(count(sample, "errors") == count(sample, "full") == 0, "store I/O failure or queue refusal", Fail)
    if first:
        require(same_store(sample, first) and sample["state"] == first["state"], "store identity changed")
    for key in COUNTERS:
        count(sample, key)
    return sample


def same_store(a, b):
    return all(a[k] == b[k] for k in IDENTITY)


def begin(stream, phase, run, action, window=None):
    header = stream.take("DUR", "BEGIN")
    require((header.get("action"), header.get("phase"), header.get("run")) == (action, phase, run)
            and re.fullmatch(UUID, header.get("boot", "")), "wrong run or boot UUID")
    sample = snapshot(stream, phase, "identity", "before", 0)
    if window:
        pages = count(sample, "pages")
        require(window == (count(sample, "lba"), pages + (pages + 32767) // 32768 + 1),
                "window does not match independent scratch plan")
    return header["boot"].lower(), sample


def delta(start, end):
    changes = Counter({key: count(end, key) - count(start, key) for key in COUNTERS})
    require(all(value >= 0 for value in changes.values()), "counter regressed")
    return changes


def measure(lines, phase, run, window=None):
    stream = Transcript(lines)
    boot, first = begin(stream, phase, run, "measure", window)
    previous = first

    def take(row, edge, op):
        nonlocal previous
        sample = snapshot(stream, phase, row, edge, op, first)
        delta(previous, sample)
        previous = sample
        return sample

    fill = delta(take("fill", "before", 0), take("fill", "after", 0))
    evidence = {}
    for row in ROWS:
        writes, syncs = Counter(), Counter()
        for op in range(OPS):
            pre = take(row, "before", op)
            wrote = take(row, "write", op)
            after = take(row, "after", op)
            writes += delta(pre, wrote)
            syncs += delta(wrote, after)
        done = stream.take("DUR", "ROW")
        require(done.get("row") == row and count(done, "ops") == OPS, "row missing readback")
        evidence[row] = writes, syncs

    mixed = delta(take("mixed", "before", 0), take("mixed", "after", 0))
    sample = stream.take("DUR", "MIX")
    p99 = count(sample, "p99_ns")
    require(count(sample, "reads") == READS and count(sample, "writes") > 0
            and READS // 2 <= count(sample, "overlap") <= READS and 0 < p99 <= count(sample, "duration_ns"),
            "mixed workload did not overlap")
    stream.end()

    total = delta(first, previous)
    wb, queue = PHASES[phase]
    if phase == "volatile":
        require(not any(total.values()), "unexpected persistent Data traffic", Fail)
    else:
        require(fill["fua"] and fill["bitmap_publications"], "first-touch FUA/bitmap known-positive absent")
        require(mixed["store_rd"] and mixed["fua"] + mixed["cached"], "mixed UFS I/O known-positive absent")
        # A plain pwrite row has no sync: APFS may hold its data until a later fence (m5dur-m4cc-cut3 on the tablet: plain fua +0,
        # fsync +41, full +450, barrier +420), so the plain row reports whatever APFS submits (docs/tools/durability.md) and only
        # the synced rows must show provider writes in their own window.
        require(all((a + b)["cached"] + (a + b)["fua"] for row, (a, b) in evidence.items() if row in SYNCED_ROWS),
                "synced write row has no provider write evidence")
        if wb:
            for row in ("full", "barrier"):
                syncs = evidence[row][1]
                require(syncs["store_sync"] and syncs["flushes"], f"{row} has no Data cache sync", Fail)
            # APFS can defer provider writes until sync, even with F_NOCACHE on the file.
            cached = mixed["cached"] + sum(writes["cached"] + syncs["cached"]
                                           for writes, syncs in evidence.values())
            require(cached > 0, "overwrite workload has no cached WRITE evidence")
        else:
            require(not total["cached"] and not total["flushes"], "write-through sent write-back I/O", Fail)
        if queue:
            require(all(mixed[k] for k in ("accepted", "finished", "callbacks")), "queued I/O known-positive absent")
        else:
            require(not total["accepted"], "queue-off accepted queued work", Fail)
    return boot, first, p99, evidence


def prepared(lines, phase, run, window=None):
    stream = Transcript(lines)
    boot, first = begin(stream, phase, run, "prepare", window)
    baseline = snapshot(stream, phase, "crash", "before", 0, first)
    delta(first, baseline)
    for seq in range(1, RECORDS + 1):
        require(count(stream.take("DUR", "ACK"), "seq") == seq, "ACK sequence gap")
    acked_stats = snapshot(stream, phase, "crash", "acked", 0, first)
    ready_stats = snapshot(stream, phase, "crash", "ready", 0, first)
    result = stream.take("DUR", "READY")
    last, tail = count(result, "last"), count(result, "tail")
    require((last, tail) == (RECORDS, RECORDS + 1), "missing prepared tail")
    stream.end(cut=True)
    reset = stream.take("HOST", "RESET")
    require(count(reset, "seconds") == count(first, "reset_after")
            and count(first, "reset_fastboot") == 1 and 0 < count(reset, "wait_ms") <= 45000
            and next(stream.lines, None) is None, "hard reset evidence absent or invalid")
    changes = delta(baseline, acked_stats)
    delta(acked_stats, ready_stats)
    require(changes["cached"] + changes["fua"] > 0, "crash writes have no provider traffic")
    if PHASES[phase][0]:
        require(changes["cached"] > 0, "crash log did not exercise cached overwrites")
        require(changes["store_sync"] >= last and changes["flushes"] >= last,
                "ACK stream has fewer cache syncs than full-sync acknowledgements", Fail)
    return boot, first, last, tail


def classify_records(records, last, tail):
    """Every ACK must have an exact record. Loss or tearing of the unacknowledged tail is allowed."""
    for seq in range(1, tail + 1):
        row = records.get(seq)
        if row is None:
            if seq <= last:
                raise Fail(f"acknowledged record {seq} missing")
            return "missing"
        size, match = row
        if size != BLOCK or not match:
            state = "missing" if size == 0 else "torn" if size < BLOCK else "corrupt"
            if seq <= last:
                raise Fail(f"acknowledged record {seq} {state}")
            return state
    return "intact"


def recovered(lines, phase, run, prep, window=None):
    stream = Transcript(lines)
    boot, first = begin(stream, phase, run, "verify", window)
    old_boot, old_store, last, tail = prep
    require(boot != old_boot, "recovery ran on the pre-reset boot")
    require(first["lba"] == old_store["lba"] and first["pages"] == old_store["pages"]
            and first["seed"] == old_store["seed"], "recovery boot targeted another scratch store")
    require(first["state"] == "2" and same_store(first, old_store),
            "scratch store was reinitialized or changed generation", Fail)

    records = {}
    # The tablet emits an observation even for EOF. A truncated SSH transcript is a VOID.
    for _ in range(tail):
        row = stream.take("DUR", "RECORD")
        seq, size, match = (count(row, key) for key in ("seq", "bytes", "match"))
        require(1 <= seq <= tail and seq not in records and 0 <= size <= BLOCK and match in (0, 1)
                and (not match or size == BLOCK), "invalid recovery record")
        records[seq] = size, match
    delta(first, snapshot(stream, phase, "recover", "after", 0, first))
    stream.end()
    return classify_records(records, last, tail)


def scratch_window(plan):
    """Derive the only allowed window from an independently reviewed GPT/extent plan."""
    def extent(name, value):
        require(isinstance(value, list) and len(value) == 2 and
                all(type(x) is int for x in value), f"invalid {name} extent")
        start, size = value
        require(0 < start < start + size <= 0xFFFFFFFF, f"invalid {name} extent")
        return start, start + size

    require(isinstance(plan, dict), "invalid scratch plan")
    part_first, part_end = extent("linux_rootfs", plan.get("linux_rootfs"))
    approved_first, approved_end = extent("approved", plan.get("approved"))
    root, pages, reserve = (plan.get(k) for k in ("root_blocks", "seed_pages", "reserve_mib"))
    require(all(type(x) is int and x > 0 for x in (root, pages, reserve)) and 288 <= reserve <= 16384,
            "invalid root, seed or log reserve")
    protected = plan.get("protected")
    require(isinstance(protected, list) and protected, "missing protected extents (including owner Data)")
    size = 1 + (pages + 32767) // 32768 + pages
    end = part_end - reserve * 256
    start = end - size
    require(part_first + root + 1024 <= approved_first <= start < end <= approved_end <= part_end,
            "scratch window does not fit the approved free extent")
    for value in protected:
        lo, hi = extent("protected", value)
        require(end <= lo or hi <= start, "scratch window overlaps a protected extent")
    return start, size


def reset_seconds(before_ready, after_ready):
    resets = RESET.findall(after_ready)
    capture = before_ready + after_ready
    if len(resets) == 1 and not RESET.search(before_ready) and not CLEAN.search(capture) and not FAULT.search(capture):
        return int(resets[0])
    return None


def run_capture(args, run, window, expected_boot="-"):
    if not re.fullmatch(r"/[A-Za-z0-9_./-]+", args.remote):
        raise Void("remote binary path must be absolute and contain no spaces")
    argv = [args.remote, args.action, args.phase, run, *map(str, window or (0, 0)), expected_boot]
    path = args.out / f"{args.phase}-{args.action}.log"
    console = None
    if args.action == "prepare":
        require(args.console is not None, "prepare requires the live USB console capture")
        console = args.console.open("rb")
        console.seek(0, os.SEEK_END)
        require(console.tell() > 0, "USB console capture is empty")
    with path.open("x", encoding="ascii") as capture:
        with subprocess.Popen([args.transport, shlex.join(argv)], stdout=subprocess.PIPE) as child:
            timer = threading.Timer(args.timeout, lambda: child.kill() if child.poll() is None else None)
            timer.start()
            ready_at = None
            before_ready = b""
            try:
                for raw in child.stdout:
                    if not raw.startswith(b"DUR ") or not raw.isascii() or not raw.endswith(b"\n"):
                        continue
                    line = raw.decode("ascii")
                    capture.write(line)
                    capture.flush()
                    if raw.startswith((b"DUR ACK ", b"DUR READY ")):
                        os.fsync(capture.fileno())
                        print(line.rstrip(), flush=True)
                    if raw.startswith(b"DUR READY ") and console:
                        ready_at = time.monotonic()
                        before_ready = console.read()
                rc = child.wait()
            finally:
                timer.cancel()
        capture.write(f"HOST EXIT rc={rc}\n")
        if console:
            elapsed = int((time.monotonic() - ready_at) * 1000) if ready_at else 0
            events = console.read()
            # The USB capture process and SSH may observe disconnect in either order.
            if ready_at and not RESET.search(events):
                time.sleep(1)
                events += console.read()
            console.close()
            seconds = reset_seconds(before_ready, events)
            if ready_at and seconds is not None:
                capture.write(f"HOST RESET seconds={seconds} wait_ms={elapsed}\n")
        capture.flush()
        os.fsync(capture.fileno())
    return path.read_text(encoding="ascii").splitlines()


def read_capture(args, phase, action):
    return (args.out / f"{phase}-{action}.log").read_text(encoding="ascii").splitlines()


def verdict(args, run, window):
    def summarize_phase(phase, control):
        checks = []
        p99 = None
        try:
            boot, store, p99, evidence = measure(read_capture(args, phase, "measure"), phase, run, window)
            for row, (writes, syncs) in evidence.items():
                counts = " ".join(f"{key}={writes[key]}/{syncs[key]}"
                                  for key in ("fua", "cached", "store_sync", "flushes", "barriers", "forced"))
                print(f"  {phase} {row} write/sync {counts}")
            print(f"  {phase} mixed reader p99={p99 / 1000:.1f} us")
            checks.append(("PASS", "measurement"))
            if PHASES[phase][1]:
                if control is None or not same_store(store, control[1]):
                    checks.append(("VOID", "no measured p99 for this scratch store in control"))
                else:
                    status = "PASS" if p99 < control[0] else "FAIL"
                    checks.append((status, f"mixed p99 {p99 / control[0]:.2f}x control"))
        except (Fail, Void, OSError) as exc:
            checks.append(("FAIL" if isinstance(exc, Fail) else "VOID", str(exc)))
            boot = store = None
        if phase != "volatile":
            try:
                prep = prepared(read_capture(args, phase, "prepare"), phase, run, window)
                if boot is not None:
                    require(prep[0] == boot and same_store(prep[1], store), "prepare differs from measurement boot")
                tail = recovered(read_capture(args, phase, "verify"), phase, run, prep, window)
                checks.append(("PASS", f"{RECORDS} ACKs recovered; unacknowledged tail {tail}"))
            except (Fail, Void, OSError) as exc:
                checks.append(("FAIL" if isinstance(exc, Fail) else "VOID", str(exc)))
        status = "FAIL" if any(s == "FAIL" for s, _ in checks) else "VOID" if any(s == "VOID" for s, _ in checks) else "PASS"
        details = ", ".join(f"{s} {reason}" for s, reason in checks)
        print(f"{phase.upper()} {status} ({details})")
        return (p99, store) if p99 is not None else None, {s for s, _ in checks}

    control = None
    all_statuses = set()
    for phase in PHASES:
        if phase == "volatile":
            if (args.out / "volatile-measure.log").is_file():
                _, statuses = summarize_phase(phase, None)
                all_statuses |= statuses
            continue
        measured, statuses = summarize_phase(phase, control)
        all_statuses |= statuses
        if phase == "control":
            control = measured
    print(f"VOID CHECK {'FAIL' if 'VOID' in all_statuses else 'PASS'}")
    print(f"M5 {'FAIL' if 'FAIL' in all_statuses else 'VOID' if 'VOID' in all_statuses else 'PASS'}")
    return 1 if "FAIL" in all_statuses else 2 if "VOID" in all_statuses else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, help="per-campaign evidence directory")
    parser.add_argument("--plan", type=Path, help="independently verified scratch plan JSON")
    parser.add_argument("--transport", default="/tmp/tssh", help="SSH wrapper taking a single remote command")
    parser.add_argument("--remote", default="/usr/local/bin/piano-durability")
    parser.add_argument("--console", type=Path, help="live USB console capture for prepare")
    parser.add_argument("--timeout", type=int, default=1100, help="maximum SSH capture seconds")
    parser.add_argument("--reset-after", type=int, default=300, help="uptime for the args command's direct reset")
    parser.add_argument("action", choices=("init", "args", "measure", "prepare", "verify", "summary"))
    parser.add_argument("phase", choices=PHASES, nargs="?")
    args = parser.parse_args()

    try:
        if args.action == "init":
            require(args.out is not None, "init requires --out")
            args.out.mkdir(mode=0o700)
            (args.out / "run-id").write_text(os.urandom(16).hex() + "\n", encoding="ascii")
            return 0
        plan = json.loads(args.plan.read_text()) if args.plan else None
        window = scratch_window(plan) if args.plan else None
        if args.action == "args":
            require(window and args.phase and args.phase != "volatile", "args requires a scratch plan and persistent phase")
            require(1 <= args.reset_after <= 3600, "invalid reset uptime")
            wb, queue = PHASES[args.phase]
            print(f"data-persist=2 native-ufs-root=1 native-ufs-root-part=linux_rootfs "
                  f"data-store-part=linux_rootfs data-store-lba={window[0]} log-reserve={plan['reserve_mib']} log=0 "
                  f"piano-durability=1 data-store-concurrent=1 data-store-write-back={wb} data-store-queue={queue} "
                  f"piano-reset-after={args.reset_after} piano-reset-to=bootloader")
            return 0
        require(args.out is not None, "--out is required")
        run = (args.out / "run-id").read_text(encoding="ascii").strip()
        require(re.fullmatch(r"[0-9a-f]{32}", run), "invalid campaign run ID")
        if args.action == "summary":
            require(window, "summary requires the independently verified --plan")
            return verdict(args, run, window)
        require(args.phase and args.timeout > 0, "phase and positive timeout required")
        if args.phase == "volatile":
            require(args.action == "measure", "volatile supports measure only")
            window = None
        else:
            require(window, "persistent phase requires an independently verified --plan")
        if args.action == "measure":
            result = measure(run_capture(args, run, window), args.phase, run, window)
            print(f"{args.phase} measure PASS p99={result[2] / 1000:.1f} us")
        elif args.action == "prepare":
            boot, store, *_ = measure(read_capture(args, args.phase, "measure"), args.phase, run, window)
            result = prepared(run_capture(args, run, window, boot), args.phase, run, window)
            require(result[0] == boot and same_store(result[1], store), "prepare differs from measurement boot")
            print(f"{args.phase} prepare PASS")
        else:
            prep = prepared(read_capture(args, args.phase, "prepare"), args.phase, run, window)
            tail = recovered(run_capture(args, run, window, prep[0]), args.phase, run, prep, window)
            print(f"{args.phase} recovery PASS, unacknowledged tail {tail}")
        return 0
    except Fail as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        return 1
    except (Void, OSError, ValueError) as exc:
        print(f"VOID {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
