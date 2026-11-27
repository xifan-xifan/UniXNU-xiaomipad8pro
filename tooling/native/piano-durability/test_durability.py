# SPDX-License-Identifier: BSD-2-Clause-Patent
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from types import SimpleNamespace
import re
import unittest
from unittest.mock import patch

from run_durability import (BLOCK, COUNTERS, OPS, PHASES, READS, RECORDS, ROWS,
                            Fail, Void, classify_records, measure, prepared,
                            recovered, reset_seconds, scratch_window, verdict)

RUN = "a" * 32
BOOT = "00000000-0000-4000-8000-000000000001"
NEXT = "00000000-0000-4000-8000-000000000002"
PLAN = {"linux_rootfs": [1_000_000, 4_000_000], "root_blocks": 1_000_000,
        "approved": [3_000_000, 1_800_000], "seed_pages": 1_000_000, "reserve_mib": 1024,
        "protected": [[1_000_000, 1_000_000], [6_000_000, 10_000_000]]}
WINDOW = scratch_window(PLAN)


def event(source, name, **values):
    return " ".join([source, name] + [f"{key}={value}" for key, value in values.items()])


class Fake:
    def __init__(self, action, phase="combined", boot=BOOT, **identity):
        self.phase = phase
        self.counters = dict.fromkeys(COUNTERS, 0)
        wb, queue = PHASES[phase]
        self.identity = dict(mode="concurrent", depth=8, write_back=wb, queue=queue, readers=2,
                             writers=2, scratch=1, ram=1, qual=1, persist=2, state=2,
                             lba=WINDOW[0], bound=WINDOW[0], pages=PLAN["seed_pages"], generation=1,
                             flags=2, seed="a1b2c3d4", store_seed="a1b2c3d4", errors=0,
                             reset_after=900, reset_fastboot=1)
        if phase == "volatile":
            self.identity.update(mode="volatile", depth=1, scratch=0, persist=0, state=0,
                                 lba=0, bound=0, pages=0, generation=0, flags=0)
        self.identity.update(identity)
        self.lines = [event("DUR", "BEGIN", action=action, phase=phase, run=RUN, boot=boot)]
        self.snap("identity", "before")

    def snap(self, row, edge, op=0, **changes):
        for key, value in changes.items():
            self.counters[key] += value
        self.lines.append(event("DUR", "SNAP", row=row, edge=edge, op=op,
                                **self.identity, **self.counters))

    def done(self):
        return self.lines + ["DUR END", "HOST EXIT rc=0"]


def measurement(phase, *, flush=True, positive=True, overlap=READS, plain_deferred=False,
                silent_row=None, cache_at="write"):
    f = Fake("measure", phase)
    direct = phase != "volatile"
    wb, queue = PHASES[phase]
    f.snap("fill", "before")
    first_touch = int(direct and positive)
    f.snap("fill", "after", fua=2 * first_touch, bitmap_publications=first_touch,
           cached=int(wb and cache_at == "fill"))
    for row in ROWS:
        # A plain write can defer all its I/O until a later row's sync.
        quiet = ((plain_deferred or cache_at == "after") and row == "plain") or row == silent_row
        write = int(direct and not quiet)
        cached = int(wb and cache_at in ("write", "after")) * write
        for i in range(OPS):
            f.snap(row, "before", i)
            f.snap(row, "write", i, fua=write - cached, cached=cached if cache_at == "write" else 0)
            sync = int(wb and flush and row in ("full", "barrier"))
            f.snap(row, "after", i, store_sync=sync, flushes=sync, cached=cached if cache_at == "after" else 0)
        f.lines.append(event("DUR", "ROW", row=row, ops=OPS))
    f.snap("mixed", "before")
    f.snap("mixed", "after", store_rd=READS if direct else 0, fua=int(direct), cached=int(wb and cache_at == "mixed"),
           accepted=100 * queue, finished=100 * queue, callbacks=100 * queue)
    f.lines.append(event("DUR", "MIX", reads=READS, writes=64, overlap=overlap,
                         p99_ns=112500, duration_ns=500000000))
    return f.done()


def preparation(**identity):
    f = Fake("prepare", **identity)
    f.snap("crash", "before")
    f.lines.extend(event("DUR", "ACK", seq=i) for i in range(1, RECORDS + 1))
    f.snap("crash", "acked", fua=1, cached=RECORDS, store_sync=RECORDS, flushes=RECORDS)
    f.snap("crash", "ready")
    return f.lines + [f"DUR READY last={RECORDS} tail={RECORDS + 1}",
                      "HOST EXIT rc=255", "HOST RESET seconds=900 wait_ms=10000"]


def recovery(record_overrides=None, boot=NEXT, **identity):
    f = Fake("verify", boot=boot, **identity)
    patches = record_overrides or {}
    for seq in range(1, RECORDS + 2):
        size, match = patches.get(seq, (BLOCK, 1))
        f.lines.append(event("DUR", "RECORD", seq=seq, bytes=size, match=match))
    f.snap("recover", "after", store_rd=RECORDS)
    return f.done()


class MeasurementTests(unittest.TestCase):
    def test_daily_store_counter_format(self):
        lines = [re.sub(r"(store_(?:rd|sync)=\d+)(?=\s|$)", r"\1/123us", line)
                 for line in measurement("writeback")]
        _, _, _, counts = measure(lines, "writeback", RUN, WINDOW)
        self.assertEqual(counts["full"][1]["store_sync"], OPS)

    def test_all_phases(self):
        for phase in PHASES:
            with self.subTest(phase=phase):
                _, _, p99, counts = measure(measurement(phase), phase, RUN,
                                            None if phase == "volatile" else WINDOW)
                self.assertEqual(p99, 112500)
                self.assertEqual(counts["full"][1]["store_sync"], OPS if PHASES[phase][0] else 0)

    def test_plain_row_deferred_by_apfs_passes(self):
        for phase in ("control", "writeback", "queue", "combined"):
            with self.subTest(phase=phase):
                measure(measurement(phase, plain_deferred=True), phase, RUN, WINDOW)

    def test_synced_row_without_provider_writes_is_void(self):
        for row in ("fsync", "full", "barrier"):
            with self.subTest(row=row), self.assertRaisesRegex(Void, "synced write row has no provider write evidence"):
                measure(measurement("control", plain_deferred=True, silent_row=row), "control", RUN, WINDOW)

    def test_cached_writes_deferred_until_sync_pass(self):
        for phase in ("writeback", "combined"):
            with self.subTest(phase=phase):
                _, _, _, counts = measure(measurement(phase, cache_at="after"), phase, RUN, WINDOW)
                self.assertTrue(all(writes["cached"] == 0 for writes, _ in counts.values()))
                self.assertEqual(counts["full"][1]["cached"], OPS)

    def test_cached_writes_in_prefilled_mixed_row_pass(self):
        measure(measurement("writeback", cache_at="mixed"), "writeback", RUN, WINDOW)

    def test_cached_writes_only_in_fill_leave_measurement_void(self):
        with self.assertRaisesRegex(Void, "overwrite workload has no cached WRITE evidence"):
            measure(measurement("writeback", cache_at="fill"), "writeback", RUN, WINDOW)

    def test_missing_known_positive_is_void(self):
        with self.assertRaisesRegex(Void, "first-touch"):
            measure(measurement("combined", positive=False), "combined", RUN, WINDOW)

    def test_no_writer_overlap_is_void(self):
        with self.assertRaisesRegex(Void, "did not overlap"):
            measure(measurement("combined", overlap=0), "combined", RUN, WINDOW)

    def test_missing_full_sync_on_live_writeback_fails(self):
        with self.assertRaisesRegex(Fail, "full has no Data cache sync"):
            measure(measurement("writeback", flush=False, cache_at="after"), "writeback", RUN, WINDOW)

    def test_truncated_capture_is_void(self):
        with self.assertRaises(Void):
            measure(measurement("control")[:-2], "control", RUN, WINDOW)

    def test_counter_reset_is_void(self):
        lines = measurement("queue")
        lines[1] = lines[1].replace("store_rd=0", "store_rd=1")
        with self.assertRaisesRegex(Void, "counter regressed"):
            measure(lines, "queue", RUN, WINDOW)


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.prep = prepared(preparation(), "combined", RUN, WINDOW)

    def recover(self, *args, **kwargs):
        return recovered(recovery(*args, **kwargs), "combined", RUN, self.prep, WINDOW)

    def test_intact(self):
        self.assertEqual(self.recover(), "intact")

    def test_missing_acknowledged_record_fails(self):
        with self.assertRaisesRegex(Fail, "record 7 missing"):
            self.recover({7: (0, 0)})

    def test_torn_acknowledged_record_fails(self):
        with self.assertRaisesRegex(Fail, f"record {RECORDS} torn"):
            self.recover({RECORDS: (1024, 0)})

    def test_corrupt_acknowledged_record_fails(self):
        with self.assertRaisesRegex(Fail, "record 1 corrupt"):
            self.recover({1: (BLOCK, 0)})

    def test_unacknowledged_loss_or_torn_tail_passes(self):
        self.assertEqual(self.recover({RECORDS + 1: (0, 0)}), "missing")
        self.assertEqual(self.recover({RECORDS + 1: (512, 0)}), "torn")
        self.assertEqual(classify_records({1: (BLOCK, 1)}, 1, 2), "missing")

    def test_reset_and_ack_gates(self):
        lines = preparation()
        for broken in (lines[:-1], lines[:3] + lines[4:], lines[:-1] + ["HOST RESET seconds=900 wait_ms=45001"]):
            with self.subTest(length=len(broken)), self.assertRaises(Void):
                prepared(broken, "combined", RUN, WINDOW)

    def test_only_direct_reset_after_ready_qualifies(self):
        direct = b"piano-reset-after: 900 s reached, PSCI SYSTEM_RESET\n"
        clean = b"restart reason: bootloader (ABL fastboot after this reset)\n"
        self.assertEqual(reset_seconds(b"", direct), 900)
        self.assertIsNone(reset_seconds(direct, b""))
        self.assertIsNone(reset_seconds(clean, direct))
        self.assertIsNone(reset_seconds(b"", clean))

    def test_same_boot_is_void(self):
        with self.assertRaisesRegex(Void, "pre-reset"):
            self.recover(boot=BOOT)

    def test_reinitialized_store_fails(self):
        with self.assertRaisesRegex(Fail, "reinitialized"):
            self.recover(state=3)

    def test_different_seed_is_void(self):
        with self.assertRaisesRegex(Void, "another scratch store"):
            self.recover(seed="11112222", store_seed="11112222")

    def test_truncated_recovery_is_void_even_if_all_acks_reported(self):
        with self.assertRaises(Void):
            recovered(recovery()[:-4], "combined", RUN, self.prep, WINDOW)


class SafetyTests(unittest.TestCase):
    def test_protected_window_overlap_is_rejected(self):
        with self.assertRaisesRegex(Void, "overlaps"):
            scratch_window(dict(PLAN, protected=[list(WINDOW)]))

    def test_unapproved_window_is_rejected(self):
        with self.assertRaisesRegex(Void, "approved"):
            scratch_window(dict(PLAN, approved=[WINDOW[0] + 1, WINDOW[1]]))

    def test_owner_store_capture_is_rejected(self):
        with self.assertRaisesRegex(Void, "wrong scratch identity"):
            prepared(preparation(scratch=0), "combined", RUN, WINDOW)


class SummaryTests(unittest.TestCase):
    def test_phase_results_and_void_check(self):
        def capture(_args, phase, action):
            if action == "prepare":
                return preparation(phase=phase)
            if action == "verify":
                if phase == "combined":
                    return recovery(phase=phase)[:-1]
                if phase == "writeback":
                    return recovery({7: (0, 0)}, phase=phase)
                return recovery(phase=phase)
            result = measurement(phase)
            if phase in ("queue", "combined"):
                result[-3] = result[-3].replace("p99_ns=112500", "p99_ns=100000")
            return result

        stdout = StringIO()
        with patch("run_durability.read_capture", capture), redirect_stdout(stdout):
            rc = verdict(SimpleNamespace(out=Path("unused-test-out")), RUN, WINDOW)
        self.assertEqual(rc, 1)
        for marker in ("CONTROL PASS", "WRITEBACK FAIL", "QUEUE PASS", "COMBINED VOID", "VOID CHECK FAIL"):
            self.assertIn(marker, stdout.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
