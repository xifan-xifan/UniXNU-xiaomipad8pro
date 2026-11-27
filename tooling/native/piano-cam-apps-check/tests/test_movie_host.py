# SPDX-License-Identifier: BSD-2-Clause-Patent
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from action import LocalProbe, inspect_movie
from grade import movie

BINARY = os.environ.get("CAM_APPS_TEST_BIN")
FFMPEG = Path(os.environ.get("CAM_APPS_FFMPEG", "/opt/homebrew/bin/ffmpeg"))


@unittest.skipUnless(BINARY and os.access(FFMPEG, os.X_OK), "set CAM_APPS_TEST_BIN and provide ffmpeg for the native file test")
class MovieHostTests(unittest.TestCase):
    def test_synthetic_movie_decode(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            for name, source in (("good.mov", "testsrc2=size=320x240:rate=15"),
                                 ("black.mov", "color=black:size=320x240:rate=15")):
                subprocess.run([FFMPEG, "-v", "error", "-f", "lavfi", "-i", source, "-t", "5", "-c:v", "libx264",
                                "-threads", "1", "-pix_fmt", "yuv420p", work / name], capture_output=True, check=True)
            subprocess.run([FFMPEG, "-v", "error", "-i", work / "good.mov", "-c", "copy", work / "wrong.mp4"],
                           capture_output=True, check=True)
            (work / "corrupt.mov").write_bytes(b"incomplete")
            probe = LocalProbe(BINARY)
            for name, expected in (("good.mov", "PASS"), ("black.mov", "FAIL"), ("wrong.mp4", "FAIL"), ("corrupt.mov", "FAIL")):
                with self.subTest(name=name):
                    stats = inspect_movie(probe, work / name)
                    result, reason = movie(stats)
                    self.assertEqual(result, expected, reason)
                    if name in ("good.mov", "black.mov"):
                        self.assertEqual(stats["decoded"], 75)
                        self.assertEqual(stats["nonblack"], 75 if name == "good.mov" else 0)
