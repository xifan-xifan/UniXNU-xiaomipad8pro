#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host-only regressions for the audio CLI and WAV boundaries."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

NATIVE = Path(__file__).resolve().parents[1]


def chunk(name, payload):
    return struct.pack("<4sI", name, len(payload)) + payload + bytes(len(payload) % 2)


def wav(*chunks):
    return chunk(b"RIFF", b"WAVE" + b"".join(chunks))


def pcm_fmt(rate, channels=1):
    return struct.pack("<HHIIHH", 1, channels, rate, rate * channels * 2, channels * 2, 16)


def pcm_extensible_fmt(rate, channels=2):
    base = pcm_fmt(rate, channels)
    return (b"\xfe\xff" + base[2:] + struct.pack("<HHI", 22, 16, 3)
            + bytes.fromhex("01000000 0000 1000 8000 00aa00389b71"))


class PythonBoundaries(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.folder = Path(scratch.name)

    def run_cli(self, script, *args, status, marker=None):
        result = subprocess.run([sys.executable, NATIVE / script, *args], capture_output=True, text=True)
        self.assertEqual(result.returncode, status, (result.stdout, result.stderr))
        self.assertNotIn("Traceback", result.stderr)
        if marker:
            self.assertIn(marker, result.stdout)

    def test_truncated_and_partial_pcm(self):
        path = self.folder / "input.wav"
        fmt = chunk(b"fmt ", pcm_fmt(16000, channels=2))
        pcm = chunk(b"data", bytes(16000 * 4))
        valid = wav(fmt, pcm)
        for contents in (b"", valid[:-2], wav(fmt, chunk(b"data", bytes(16000 * 4 - 2))),
                         wav(fmt, pcm, b"JUNK")):
            with self.subTest(size=len(contents)):
                path.write_bytes(contents)
                self.run_cli("audio-listen.py", "detect", path, status=2)
                self.run_cli("piano-vpio-probe/piano-vpio-measure.py", "block",
                             self.folder, self.folder, "input", status=2)
        path.write_bytes(wav(chunk(b"fmt ", pcm_fmt(16000)), pcm)[:-2])
        self.run_cli("piano-mic-check/analyze.py", path, "--mode", "tone", status=2, marker="grade=error")

    def test_float_pcm_nan_is_not_sanitized_by_conversion(self):
        path = self.folder / "input.wav"
        fmt = chunk(b"fmt ", struct.pack("<HHIIHH", 3, 1, 16000, 64000, 4, 32))
        data = chunk(b"data", struct.pack("<f", float("nan")) + bytes(63996))
        path.write_bytes(wav(fmt, data))
        self.run_cli("piano-mic-check/analyze.py", path, "--mode", "tone", status=2, marker="grade=error")

    def test_mic_diagnostic_rejects_nonfinite_metrics(self):
        measure = self.folder / "measure.py"
        measure.write_text("def read(path): return [0] * 960000, 48000, 1\n"
                           "def metrics(*args): return {'tone_dbfs': float('nan')}\n")
        report = self.folder / "report.json"
        report.write_text(json.dumps({"failures": [], "file_onset": 1}))
        timeline = self.folder / "timeline.jsonl"
        events = [dict(event="play", phase=phase, before=at)
                  for at, phase in enumerate(("unity", "lower", "mute", "recover"), 1)]
        events.extend([dict(event="silence-start", at=10), dict(event="silence-end", at=16)])
        timeline.write_text("\n".join(map(json.dumps, events)))
        self.run_cli("check-mic-diag.py", "windows", measure, self.folder / "input.wav",
                     timeline, report, status=2, marker="FAIL HARNESS")

    def test_nonfinite_vpio_measurement_report(self):
        (self.folder / "vpio-measure.json").write_text('{"echo": 1e999, "void": []}')
        self.run_cli("piano-vpio-probe/piano-vpio-measure.py", "verdict", self.folder, status=2)

    def test_last_complete_detection_window(self):
        path = self.folder / "input.wav"
        path.write_bytes(wav(chunk(b"fmt ", pcm_fmt(16000)), chunk(b"data", bytes(1600))))
        self.run_cli("audio-listen.py", "detect", path, status=0, marker="(1 windows of 50 ms)")

    def test_nonfinite_and_bad_cli_values(self):
        missing = self.folder / "missing.wav"
        self.run_cli("piano-mic-check/make-tone.py", status=2)
        self.run_cli("audio-listen.py", "detect", missing, status=2)
        self.run_cli("audio-listen.py", "record", "--seconds", "nan", "--out", missing, status=2)
        self.assertFalse(missing.exists())
        self.run_cli("piano-mic-route.py", "fixture", missing, "--carrier", "9" * 350, status=2)
        self.assertFalse(missing.exists())
        self.run_cli("piano-vpio-probe/piano-vpio-measure.py", "stimuli", self.folder / "assets",
                     "--rate", "0", status=2)
        self.assertFalse((self.folder / "assets").exists())


class NativeBoundaries(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = tempfile.TemporaryDirectory()
        cls.addClassCleanup(scratch.cleanup)
        cls.folder = Path(scratch.name)
        cls.env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
        sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
        cls.tools = {}
        for name, source, frameworks in (
            ("hal", "piano-hal-rec.c", ("AudioToolbox", "CoreFoundation")),
            ("diag", "piano-mic-diag.c", ("IOKit", "CoreFoundation")),
            ("probe", "piano-audio-probe.c", ("CoreAudio", "AudioToolbox", "CoreFoundation")),
            ("vpio", "tests/audio_vpio_wav_host.m", ("AudioToolbox", "CoreAudio", "AVFoundation", "Foundation")),
            ("check", "piano-mic-check/piano-mic-check.m",
             ("AppKit", "AVFoundation", "AudioToolbox", "CoreMedia", "CoreAudio")),
        ):
            output = cls.folder / name
            command = ["xcrun", "--sdk", "macosx", "clang", "-isysroot", sdk, "-mmacosx-version-min=14.0",
                       "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined,float-divide-by-zero", "-fno-sanitize-recover=all"]
            if source.endswith(".m"):
                command.append("-fobjc-arc")
            for framework in frameworks:
                command += ["-framework", framework]
            subprocess.run([*command, NATIVE / source, "-o", output], check=True)
            cls.tools[name] = output

    def run_tool(self, name, *args, status, marker=None):
        result = subprocess.run([self.tools[name], *args], capture_output=True, text=True, env=self.env, timeout=15)
        self.assertEqual(result.returncode, status, (result.stdout, result.stderr))
        self.assertNotIn("Sanitizer", result.stderr)
        self.assertNotIn("runtime error:", result.stderr)
        if marker:
            self.assertIn(marker, result.stdout)

    def test_native_cli_parsing_and_diag_ring(self):
        path = self.folder / "unused.wav"
        self.run_tool("hal", "1", path, "nan", status=2)
        self.run_tool("hal", "1junk", path, status=2)
        self.assertFalse(path.exists())
        self.run_tool("probe", "test", "output", "4294967295", status=2)
        self.run_tool("vpio", "raw", "1", "none", path, "--far-delay", "nan", status=2)
        self.run_tool("vpio", "raw", "1", "none", path, "--agc", "2", status=2)
        self.run_tool("diag", "--selftest", status=0, marker="SELFTEST_PASS")

    def test_native_wave_boundaries(self):
        path = self.folder / "input.wav"
        fmt = chunk(b"fmt ", pcm_extensible_fmt(16000))
        pcm = chunk(b"data", struct.pack("<hh", 16384, 0))
        valid = wav(chunk(b"JUNK", b"A"), fmt, pcm)
        path.write_bytes(valid)
        self.run_tool("vpio", "--load-wav", path, status=0, marker="16000 Hz 1 frames first 0.2500")
        for invalid in (
            valid[:-1],
            wav(fmt, chunk(b"data", b"")),
            wav(fmt, chunk(b"data", b"XX")),
            wav(chunk(b"fmt ", pcm_extensible_fmt(16000)[:26]), pcm),
            wav(chunk(b"fmt ", pcm_extensible_fmt(16000)[:-1] + b"X"), pcm),
            wav(b"JUNK" + struct.pack("<I", 0xffffffff), fmt, pcm),
            wav(fmt, pcm, b"JUNK"),
        ):
            with self.subTest(invalid=invalid[:16]):
                path.write_bytes(invalid)
                self.run_tool("vpio", "--load-wav", path, status=1)
        fmt = chunk(b"fmt ", pcm_fmt(16000))
        path.write_bytes(wav(fmt, chunk(b"data", bytes(32000)))[:-2])
        self.run_tool("check", "--analyze", path, status=1, marker="result=error")
        path.unlink()
        self.run_tool("check", "--analyze", path, status=1, marker="result=error")


if __name__ == "__main__":
    unittest.main()
