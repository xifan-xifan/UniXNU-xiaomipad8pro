#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the evaluator with synthetic files and build the unsigned client."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
BOOT = "synthetic-boot"
LOG = f"DIAG_START boot={BOOT} uid=501\nDIAG_COMPLETE COLLECTED\n".encode()
WIDTH, HEIGHT = 640, 480


def archive(path, files, manifest=True):
    entries = list(files.items())
    if manifest:
        sums = "".join(f"{hashlib.sha256(data).hexdigest()}  ./{name}\n" for name, data in entries)
        entries.append(("SHA256SUMS", sums.encode()))
    with tarfile.open(path, "w:gz") as tar:
        for name, data in entries:
            member = tarfile.TarInfo("./" + name)
            member.size = len(data)
            tar.addfile(member, io.BytesIO(data))


def observations():
    ids = [1, 2] * 16
    files = {
        "diag.log": LOG,
        "pattern-capture.json": json.dumps({"frames": 32, "activeIndex1": True, "surfaceIDs": ids,
                                            "repeatedSurfaceIDs": 30}).encode(),
    }
    for frame in (1, 32):
        key = f"pattern-{frame}"
        files[f"{key}-frame.json"] = json.dumps({
            "presentationTime": {"value": frame, "scale": 30}, "surface": {"id": ids[frame - 1]},
            "pattern": {"passed": True, "rgb": {"counter": frame}},
            "planes": [{"rowBytes": WIDTH, "height": HEIGHT}, {"rowBytes": WIDTH, "height": HEIGHT // 2}],
        }).encode()
        files[f"{key}-replay.json"] = json.dumps({
            "comparable": True, "A_vs_B": {"pixels": WIDTH * HEIGHT, "exactRGB": 0, "over20RGB": 1,
                                          "over40RGB": 1, "meanAbsoluteRGB": 1},
            "A_pattern": {"passed": False}, "B_pattern": {"passed": True},
        }).encode()
        for name, data in (("y.bin", bytes(WIDTH * HEIGHT)), ("uv.bin", bytes(WIDTH * HEIGHT // 2)),
                           ("cpu.png", b"png"), ("A.png", b"png"), ("B.png", b"png"),
                           ("A.bgra", bytes(WIDTH * HEIGHT * 4)), ("B.bgra", bytes(WIDTH * HEIGHT * 4))):
            files[f"{key}-{name}"] = data
    return files


class DiagnosticHost(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="diag-host-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.archive = self.root / "capture.tar.gz"
        self.output = self.root / "capture"

    def evaluate(self):
        return subprocess.run([sys.executable, HERE / "evaluate.py", self.archive, self.output, BOOT],
                              capture_output=True, text=True, timeout=15)

    def test_valid_and_partial_evidence(self):
        files = observations()
        files["front-capture.json"] = b'{"frames":'
        archive(self.archive, files)
        result = self.evaluate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("VERIFIED_ARTIFACTS", result.stdout)
        self.assertIn("OBSERVED pattern-1 only imported A corrupt", result.stdout)
        self.assertIn("OBSERVED pattern-32 only imported A corrupt", result.stdout)
        self.assertIn("VOID front-capture.json invalid metadata JSON", result.stdout)

    def test_truncated_frames_remain_inconclusive(self):
        files = observations()
        files["pattern-1-A.bgra"] = b"cut"
        meta = json.loads(files["pattern-32-frame.json"])
        meta["planes"].pop()
        files["pattern-32-frame.json"] = json.dumps(meta).encode()
        archive(self.archive, files)
        result = self.evaluate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("VOID pattern-1 incomplete GPU capture files or diff", result.stdout)
        self.assertIn("VOID pattern-32 incomplete CPU capture files", result.stdout)
        self.assertNotIn("OBSERVED pattern", result.stdout)

    def test_invalid_numeric_evidence(self):
        cases = (
            ("pattern-capture.json", {"activeIndex1": "false"}, "INCONCLUSIVE pattern index 1"),
            ("pattern-1-frame.json", {"surface": {"id": True}}, "INCONCLUSIVE pattern-1 no valid immutable"),
            ("pattern-1-frame.json", {"presentationTime": {"value": 1, "scale": 2**31}},
             "INCONCLUSIVE pattern retained first/frame-32 time"),
            ("pattern-1-frame.json", {"planes": [{"rowBytes": WIDTH, "height": float(HEIGHT)},
                                                 {"rowBytes": WIDTH, "height": HEIGHT // 2}]},
             "VOID pattern-1 incomplete CPU capture files"),
        )
        for i, (name, changes, verdict) in enumerate(cases):
            with self.subTest(changes=changes):
                files = observations()
                meta = json.loads(files[name])
                meta.update(changes)
                files[name] = json.dumps(meta).encode()
                self.output = self.root / f"metadata-{i}"
                archive(self.archive, files)
                result = self.evaluate()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(verdict, result.stdout)
                self.assertNotIn("OBSERVED pattern-1 ", result.stdout)

    def test_invalid_diff_counts_and_nonfinite_mean(self):
        for i, changes in enumerate((
            {"over40RGB": 2}, {"exactRGB": WIDTH * HEIGHT}, {"pixels": float(WIDTH * HEIGHT)},
            {"meanAbsoluteRGB": float("nan")}, {"meanAbsoluteRGB": float("inf")},
        )):
            with self.subTest(changes=changes):
                files = observations()
                name = "pattern-1-replay.json"
                replay = json.loads(files[name])
                replay["A_vs_B"].update(changes)
                files[name] = json.dumps(replay).encode()
                self.output = self.root / f"diff-{i}"
                archive(self.archive, files)
                result = self.evaluate()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("VOID pattern-1 incomplete GPU capture files or diff", result.stdout)
                self.assertNotIn("OBSERVED pattern-1 ", result.stdout)

    def test_rejects_bad_archives_before_writing(self):
        for kind in ("missing manifest", "cut gzip trailer", "unexpected name"):
            with self.subTest(kind=kind):
                files = {"diag.log": LOG}
                if kind == "unexpected name":
                    files["../diag.log"] = LOG
                archive(self.archive, files, manifest=kind != "missing manifest")
                if kind == "cut gzip trailer":
                    self.archive.write_bytes(self.archive.read_bytes()[:-3])
                result = self.evaluate()
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertTrue(result.stderr.startswith("evaluate.py: "), result.stderr)
                self.assertFalse(self.output.exists())

    def test_compile_and_frame_helpers(self):
        subprocess.run([sys.executable, HERE / "build-client.py", "--build-only", self.root], check=True, timeout=90)
        self.assertTrue((self.root / "DiagClient.app/Contents/MacOS/DiagClient").is_file())
        binary = self.root / "frame_evidence_host"
        cmd = ["xcrun", "clang++", "-std=c++17", "-g", "-O1", "-fobjc-arc", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all", HERE / "frame_evidence_host.mm",
               HERE / "FrameEvidence.mm", "-o", binary]
        for framework in ("Foundation", "CoreMedia", "CoreVideo", "CoreGraphics", "ImageIO", "IOSurface"):
            cmd.extend(("-framework", framework))
        subprocess.run(cmd, check=True, timeout=90)
        subprocess.run([binary, self.root / "surface.png"], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
