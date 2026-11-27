#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""E1 (piano-video-enc=1): check the Annex B stream the tablet's encode self-test exported.

The stream comes from the IORegistry (`ioreg -r -c PianoVideo -l -w0 > enc-ioreg.txt` on the tablet, then --ioreg) or a raw
.h264 file (--stream). ffmpeg decodes it; every frame is compared with the self-test pattern (VideoEncode.h patternY/U/V, the
same integer formulas below) and the result is one line: PASS/FAIL, frames, size, profile, PSNR-Y min/avg.

  enc_stream_check.py --ioreg enc-ioreg.txt [--size 640x480] [--frames 10] [--min-psnr 30]
  enc_stream_check.py --stream e1.h264 ...
  enc_stream_check.py --self-test --pattern-tool out/native-kexts/PianoVideo/host-tests/enc_host
"""
import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile


def pattern_frame(w, h, f):
    """One packed NV12 frame (w x h) of the self-test pattern."""
    y = bytearray(w * h)
    for j in range(h):
        row = j * w
        for i in range(w):
            y[row + i] = 16 + (i // 3 + j // 2 + 6 * f) % 220
    uv = bytearray(w * (h // 2))
    for j in range(h // 2):
        row = j * w
        for i in range(w // 2):
            uv[row + 2 * i] = 96 + (j // 8 + f) % 64
            uv[row + 2 * i + 1] = 96 + (i // 10 + 2 * f) % 64
    return bytes(y + uv)


def stream_from_ioreg(text):
    m = re.search(r'"piano-video-enc-stream"\s*=\s*<([0-9a-fA-F]*)>', text)
    if not m:
        return None
    return bytes.fromhex(m.group(1))


def result_from_ioreg(text):
    m = re.search(r'"piano-video-enc-result"\s*=\s*"([^"]*)"', text)
    return m.group(1) if m else None


def nal_types(data):
    types, i, n = [], 0, len(data)
    while True:
        k = data.find(b"\x00\x00\x01", i)
        if k < 0 or k + 3 >= n:
            break
        types.append(data[k + 3] & 31)
        i = k + 3
    return types


def psnr(a, b):
    if len(a) != len(b) or not a:
        return 0.0
    se = sum((x - y) * (x - y) for x, y in zip(a, b))
    if se == 0:
        return 99.0
    return 10 * math.log10(255 * 255 * len(a) / se)


def ffmpeg_tools():
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    return ffmpeg, ffprobe


def check(data, w, h, frames, min_psnr, label="stream"):
    ffmpeg, ffprobe = ffmpeg_tools()
    if not ffmpeg or not ffprobe:
        return False, f"FAIL {label}: ffmpeg/ffprobe not found"
    if not data:
        return False, f"FAIL {label}: empty stream"
    types = nal_types(data)
    with tempfile.TemporaryDirectory() as tmp:
        src, raw = os.path.join(tmp, "e1.h264"), os.path.join(tmp, "e1.nv12")
        open(src, "wb").write(data)
        probe = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
                                "stream=width,height,profile,level,pix_fmt", "-of", "default=nw=1", src],
                               capture_output=True, text=True)
        info = dict(line.split("=", 1) for line in probe.stdout.split() if "=" in line)
        dec = subprocess.run([ffmpeg, "-v", "error", "-f", "h264", "-i", src, "-f", "rawvideo", "-pix_fmt", "nv12", raw],
                             capture_output=True, text=True)
        out = open(raw, "rb").read() if os.path.exists(raw) else b""
    size = w * h * 3 // 2
    got = len(out) // size
    scores = []
    for f in range(min(got, frames)):
        ref = pattern_frame(w, h, f)
        scores.append(psnr(out[f * size:f * size + w * h], ref[:w * h]))
    ok = (dec.returncode == 0 and got == frames and info.get("width") == str(w) and info.get("height") == str(h)
          and 7 in types and 8 in types and 5 in types and scores and min(scores) >= min_psnr)
    line = (f"{'PASS' if ok else 'FAIL'} {label}: bytes {len(data)} NAL sps {types.count(7)} pps {types.count(8)} "
            f"idr {types.count(5)} slice {types.count(1)} decoded {got}/{frames} size {info.get('width')}x{info.get('height')} "
            f"profile {info.get('profile')} level {info.get('level')} PSNR-Y min {min(scores) if scores else 0:.1f} "
            f"avg {sum(scores) / len(scores) if scores else 0:.1f} (threshold {min_psnr})")
    if dec.returncode:
        line += f" ffmpeg-error {dec.stderr.strip()[:200]}"
    return ok, line


def self_test(tool):
    ffmpeg, _ = ffmpeg_tools()
    w, h, n = 320, 240, 6
    with tempfile.TemporaryDirectory() as tmp:
        # 1. The C++ pattern (the kernel's) and this one are byte-identical.
        if tool:
            pat = os.path.join(tmp, "p.nv12")
            if subprocess.run([tool, "--pattern", pat, str(w), str(h), str(n)]).returncode:
                print("enc_stream_check self-test: the pattern tool failed")
                return 1
            ref = b"".join(pattern_frame(w, h, f) for f in range(n))
            if open(pat, "rb").read() != ref:
                print("enc_stream_check self-test: FAIL the C++ and Python patterns differ")
                return 1
        if not ffmpeg:
            print("enc_stream_check self-test: pattern identical; ffmpeg absent, decode checks skipped")
            return 0
        # 2. A real H.264 of the pattern passes; a truncated one and a wrong size fail.
        raw = os.path.join(tmp, "in.nv12")
        open(raw, "wb").write(b"".join(pattern_frame(w, h, f) for f in range(n)))
        enc = os.path.join(tmp, "ref.h264")
        r = subprocess.run([ffmpeg, "-v", "error", "-f", "rawvideo", "-pix_fmt", "nv12", "-s", f"{w}x{h}", "-r", "30", "-i", raw,
                            "-c:v", "libx264", "-profile:v", "high", "-bf", "0", "-b:v", "2M", "-f", "h264", enc],
                           capture_output=True, text=True)
        if r.returncode:
            print("enc_stream_check self-test: ffmpeg could not make a reference (" + r.stderr.strip()[:120] + "); skipped")
            return 0
        data = open(enc, "rb").read()
        ok, line = check(data, w, h, n, 30, "reference")
        print("  " + line)
        bad1, line1 = check(data[: len(data) // 3], w, h, n, 30, "truncated")
        print("  " + line1)
        bad2, line2 = check(data, 640, 480, n, 30, "wrong-size")
        print("  " + line2)
        ioreg = '  | |   "piano-video-enc-result" = "PASS"\n  | |   "piano-video-enc-stream" = <' + data.hex() + '>\n'
        parsed = stream_from_ioreg(ioreg) == data and result_from_ioreg(ioreg) == "PASS"
        if ok and not bad1 and not bad2 and parsed:
            print("enc_stream_check self-test: PASS")
            return 0
        print("enc_stream_check self-test: FAIL")
        return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ioreg")
    ap.add_argument("--stream")
    ap.add_argument("--size", default="640x480")
    ap.add_argument("--frames", type=int, default=10)
    ap.add_argument("--min-psnr", type=float, default=30.0)
    ap.add_argument("--save", help="also write the stream as a raw .h264")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--pattern-tool")
    a = ap.parse_args()
    if a.self_test:
        return self_test(a.pattern_tool)
    w, h = (int(v) for v in a.size.split("x"))
    if a.ioreg:
        text = open(a.ioreg, errors="replace").read()
        print("kernel result: " + str(result_from_ioreg(text)))
        data = stream_from_ioreg(text)
        if data is None:
            print("FAIL ioreg: no piano-video-enc-stream property (known-positive: the result line above)")
            return 1
    elif a.stream:
        data = open(a.stream, "rb").read()
    else:
        ap.error("--ioreg or --stream")
    if a.save:
        open(a.save, "wb").write(data)
    ok, line = check(data, w, h, a.frames, a.min_psnr)
    print(line)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
