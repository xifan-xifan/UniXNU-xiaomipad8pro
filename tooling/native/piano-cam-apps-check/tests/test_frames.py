# SPDX-License-Identifier: BSD-2-Clause-Patent
import struct
import unittest
import zlib

from test_grade import good_evidence, logs
from grade import frame_stats, grade

WIDTH, HEIGHT = 201, 150
# Independent framebuffer scales of 2 horizontally and 1.5 vertically.
GEOMETRY = {"screen": [-160, 70, 100.5, 100], "window": [-148, 80, 70, 80]}


def scene(interior="texture", shift=0):
    def pixel(x, y):
        if 101 <= x < 150 and 45 <= y < 105:
            if interior == "black":
                return (0, 0, 0)
            if interior == "gray":
                return (112, 112, 112)
            return (60 + ((x // 6 + y // 8 + shift) % 3) * 64, 147 if y < 72 else 55, 91)
        # Populate the desktop and app chrome so including them would mask a black or flat preview.
        return (235, 245, 225) if (x // 7 + y // 8) % 2 else (8, 25, 40)

    return [[pixel(x, y) for x in range(WIDTH)] for y in range(HEIGHT)]


def make_png(pixels, alpha=None):
    def chunk(name, data):
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))

    channels = 3 if alpha is None else 4
    scanlines = bytearray()
    above = bytes(WIDTH * channels)
    for y, line in enumerate(pixels):
        row = bytes(value for rgb in line for value in (rgb if alpha is None else (*rgb, alpha)))
        method = y % 5
        scanlines.append(method)
        for x, value in enumerate(row):
            left, up = (row[x - channels] if x >= channels else 0), above[x]
            corner = above[x - channels] if x >= channels else 0
            paeth = min((left, up, corner), key=lambda v: abs(left + up - corner - v))
            prediction = (0, left, up, (left + up) // 2, paeth)[method]
            scanlines.append((value - prediction) % 256)
        above = row
    header = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2 if alpha is None else 6, 0, 0, 0)
    compressed = zlib.compress(scanlines)
    middle = len(compressed) // 2
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", compressed[:middle]) + chunk(b"IDAT", compressed[middle:]) + chunk(b"IEND", b""))


def make_bmp(pixels, *, top_down=False, depth=24):
    channels = depth // 8
    padding = bytes(-(WIDTH * channels) % 4)
    rows = pixels if top_down else reversed(pixels)
    raster = b"".join(b"".join(bytes((b, g, r) if depth == 24 else (b, g, r, 0)) for r, g, b in row) +
                      padding for row in rows)
    file_header = struct.pack("<2sIHHI", b"BM", 54 + len(raster), 0, 0, 54)
    bitmap_header = struct.pack("<IiiHHIIiiII", 40, WIDTH, -HEIGHT if top_down else HEIGHT, 1, depth,
                                0, len(raster), 0, 0, 0, 0)
    return file_header + bitmap_header + raster


def capture_events():
    events = logs(luma=False)
    events.insert(3, dict(events[2], sent=90, sources=57))
    return events


def graded_capture(first, second):
    evidence = good_evidence()
    events = capture_events()
    evidence["facetime"].update(events=events, gop_shot={"frames": [first, second],
                                                        "before": events[2], "after": events[3]})
    return evidence


class FrameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.textured = [make_png(scene()), make_bmp(scene(shift=1))]

    def test_textured_crops_pass_with_installed_style_counters(self):
        stats = [frame_stats(frame, GEOMETRY) for frame in self.textured]
        self.assertEqual(grade(graded_capture(*stats))["facetime"][0], "PASS")
        self.assertEqual(grade(graded_capture(*stats))["quicktime"][0], "PASS")

    def test_png_and_bmp_decode_to_same_crop_statistics(self):
        pixels = scene()
        expected = frame_stats(self.textured[0], GEOMETRY)
        luma = [(54 * r + 183 * g + 19 * b + 128) >> 8 for row in pixels[45:105] for r, g, b in row[101:150]]
        mean = sum(luma) / len(luma)
        self.assertAlmostEqual(expected["mean_luma"], mean)
        self.assertAlmostEqual(expected["nonblack_fraction"], sum(value >= 32 for value in luma) / len(luma))
        self.assertAlmostEqual(expected["luma_variance"], sum((value - mean) ** 2 for value in luma) / len(luma))
        for image in (make_bmp(pixels), make_bmp(pixels, top_down=True, depth=32), make_png(pixels, alpha=255)):
            with self.subTest(magic=image[:8]):
                self.assertEqual(frame_stats(image, GEOMETRY), expected)

    def test_black_flat_and_transparent_crops_fail_with_bright_surroundings(self):
        textured = frame_stats(self.textured[0], GEOMETRY)
        for frame, mean in ((make_png(scene("black")), 0), (make_bmp(scene("gray")), 112),
                            (make_png(scene(), alpha=0), 0)):
            with self.subTest(mean=mean):
                stats = frame_stats(frame, GEOMETRY)
                self.assertEqual(stats["mean_luma"], mean)
                self.assertEqual(stats["luma_variance"], 0)
                self.assertEqual(grade(graded_capture(textured, stats))["facetime"][0], "FAIL")

    def test_missing_truncated_and_corrupt_frames_void(self):
        corrupt = bytearray(self.textured[0])
        corrupt[42] ^= 1
        textured = frame_stats(self.textured[0], GEOMETRY)
        for frame in (None, b"", b"not an image", self.textured[0][:-4], self.textured[1][:-4], bytes(corrupt)):
            with self.subTest(image_type=type(frame), size=len(frame or b"")):
                stats = frame_stats(frame, GEOMETRY)
                self.assertIsNone(stats)
                self.assertEqual(grade(graded_capture(textured, stats))["facetime"][0], "VOID")

    def test_invalid_geometry_void(self):
        textured = frame_stats(self.textured[0], GEOMETRY)
        for geometry in (None, {}, {"window": GEOMETRY["window"]}, dict(GEOMETRY, window=None),
                         dict(GEOMETRY, window=[-161, 80, 70, 80]), dict(GEOMETRY, window=[-148, 80, 90, 80]),
                         dict(GEOMETRY, window=[-148, 69, 70, 80]), dict(GEOMETRY, window=[-148, 80, 70, 91]),
                         dict(GEOMETRY, window=[-148, 80, 0, 80]), dict(GEOMETRY, window=[-148, 80, 1, 1]),
                         dict(GEOMETRY, window=[float("nan"), 80, 70, 80])):
            with self.subTest(geometry=geometry):
                stats = frame_stats(self.textured[0], geometry)
                self.assertIsNone(stats)
                self.assertEqual(grade(graded_capture(textured, stats))["facetime"][0], "VOID")

    def test_explicit_positive_luma_bounds(self):
        bounds = {"mean_luma": 24, "nonblack_fraction": 0.10, "luma_variance": 64}
        self.assertEqual(grade(graded_capture(bounds, bounds))["facetime"][0], "PASS")
        for key, value in bounds.items():
            with self.subTest(metric=key):
                below = {**bounds, key: value - 0.001}
                self.assertEqual(grade(graded_capture(bounds, below))["facetime"][0], "FAIL")

    def test_texture_cannot_replace_advancing_front_stream(self):
        stats = frame_stats(self.textured[0], GEOMETRY)
        evidence = graded_capture(stats, stats)
        phase = evidence["facetime"]
        phase["events"][3]["sources"] = phase["events"][2]["sources"]
        self.assertEqual(grade(evidence)["facetime"][0], "FAIL")
        phase["gop_shot"].pop("after")
        self.assertEqual(grade(evidence)["facetime"][0], "VOID")
        phase["events"] = logs(1, luma=False)
        self.assertEqual(grade(evidence)["facetime"][0], "FAIL")
