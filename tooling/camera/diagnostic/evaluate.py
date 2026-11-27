#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify the observation artifact set and interpret the synthetic control."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import sys
import tarfile

FRAME = r"(?:pattern|front|rear)-(?:1|32)"
ARTIFACT = re.compile(
    rf"(?:diag\.log|(?:pattern|front|rear)-capture\.json|"
    rf"{FRAME}-(?:y\.bin|uv\.bin|cpu\.png|frame\.json|replay\.json|[AB]\.(?:png|bgra)|[AB]-control\.bgra))"
)
FRAME_WIDTH, FRAME_HEIGHT = 640, 480
BGRA_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 4

def unpack(archive, output):
    """Verify the remote SHA-256 of each file before writing any members."""
    with gzip.open(archive, "rb") as stream, tarfile.open(fileobj=stream, mode="r|") as tar:
        entries = {}
        for member in tar:
            name = member.name.removeprefix("./")
            if member.isdir() and member.name == ".":
                continue
            if not member.isfile() or name in entries or not (ARTIFACT.fullmatch(name) or name == "SHA256SUMS"):
                raise ValueError(f"unexpected archive member {name!r}")
            if not 0 < member.size <= 16 * 1024 * 1024:
                raise ValueError(f"invalid artifact size for {name}")
            entries[name] = tar.extractfile(member).read()
        # tar stops at the zero blocks. Read the gzip trailer to verify the
        # checksum and detect a cut download even after the last member.
        while stream.read(64 * 1024):
            pass
    if "SHA256SUMS" not in entries:
        raise ValueError("missing SHA256SUMS")
    manifest = entries.pop("SHA256SUMS").decode("ascii")
    expected = {}
    for line in manifest.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (?:\./)?(.+)", line)
        if not match or match[2] in expected:
            raise ValueError("malformed or duplicate SHA256SUMS entry")
        expected[match[2]] = match[1]
    if "diag.log" not in entries or entries.keys() != expected.keys():
        raise ValueError("SHA256SUMS does not enumerate exactly the fetched files")
    for name, data in entries.items():
        if hashlib.sha256(data).hexdigest() != expected[name]:
            raise ValueError(f"SHA-256 mismatch for {name}")
    output.mkdir()
    for name, data in entries.items():
        (output / name).write_bytes(data)
    (output / "SHA256SUMS").write_text(manifest)
    print(f"VERIFIED_ARTIFACTS {len(entries)} remote SHA-256 matches")


def mapping(value):
    return value if isinstance(value, dict) else {}


def presentation_time(meta):
    time = mapping(meta.get("presentationTime"))
    value, scale = time.get("value"), time.get("scale")
    if type(value) is int and type(scale) is int and 0 < value < 2**63 and 0 < scale < 2**31:
        return value, scale
    return None


def cpu_files_complete(files, planes):
    if not isinstance(planes, list) or len(planes) != 2 or not files["-cpu.png"].is_file():
        return False
    for name, plane, height in zip(("-y.bin", "-uv.bin"), planes, (FRAME_HEIGHT, FRAME_HEIGHT // 2)):
        plane = mapping(plane)
        row_bytes = plane.get("rowBytes")
        path = files[name]
        if (type(row_bytes) is not int or row_bytes < FRAME_WIDTH
                or type(plane.get("height")) is not int or plane["height"] != height
                or not path.is_file() or path.stat().st_size != row_bytes * height):
            return False
    return True


def inspect(output, boot):
    def read(name):
        path = output / name
        if not path.is_file():
            return {}
        try:
            data = json.loads(path.read_bytes())
        except (ValueError, RecursionError):
            data = None
        if not isinstance(data, dict):
            print(f"VOID {name} invalid metadata JSON")
        return mapping(data)

    log = (output / "diag.log").read_text(errors="replace")
    if not re.search(rf"^DIAG_START boot={re.escape(boot)} uid=[1-9][0-9]+$", log, re.M):
        print("VOID client boot UUID does not match the verified SSH boot")
        return
    expected_complete = "DIAG_COMPLETE COLLECTED" in log.splitlines()
    if not expected_complete:
        print("PARTIAL client completion missing or unsuccessful; review preserved evidence")
    for sensor in ("pattern", "front", "rear"):
        capture = read(f"{sensor}-capture.json")
        frames = capture.get("frames")
        if type(frames) is not int or frames != 32 or capture.get("activeIndex1") is not True:
            print(f"INCONCLUSIVE {sensor} index 1 or frame 32 unverified")
            continue
        first = read(f"{sensor}-1-frame.json")
        later = read(f"{sensor}-32-frame.json")
        t1, t32 = presentation_time(first), presentation_time(later)
        increasing = t1 and t32 and t1[0] * t32[1] < t32[0] * t1[1]
        surface_ids = capture.get("surfaceIDs", [])
        if (not increasing or not isinstance(surface_ids, list) or len(surface_ids) != 32
                or not all(type(sid) is int and sid > 0 for sid in surface_ids) or surface_ids[0] == surface_ids[-1]):
            print(f"INCONCLUSIVE {sensor} retained first/frame-32 time or surface identity is invalid")
            continue
        if len(set(surface_ids)) == len(surface_ids):
            print(f"INCONCLUSIVE {sensor} no observed IOSurface ID reuse through frame 32")
        if sensor == "pattern" and all(mapping(meta.get("pattern")).get("passed") is True for meta in (first, later)):
            counters = [mapping(meta["pattern"].get("rgb")).get("counter") for meta in (first, later)]
            if not all(type(counter) is int and 0 <= counter < 2**24 for counter in counters):
                print("INCONCLUSIVE pattern counter metadata missing or invalid")
                continue
            if counters[0] == counters[1]:
                print("INCONCLUSIVE pattern counter did not advance between retained frames")
                continue
        for frame, meta in ((1, first), (32, later)):
            key = f"{sensor}-{frame}"
            replay = read(f"{key}-replay.json")
            files = {name: output / (key + name) for name in ("-y.bin", "-uv.bin", "-cpu.png",
                                                            "-A.bgra", "-A.png", "-B.bgra", "-B.png")}
            surface_id = mapping(meta.get("surface")).get("id")
            if not meta or meta.get("error") or type(surface_id) is not int or surface_id != surface_ids[frame - 1]:
                print(f"INCONCLUSIVE {key} no valid immutable 420v snapshot")
                continue
            if not cpu_files_complete(files, meta.get("planes")):
                print(f"VOID {key} incomplete CPU capture files")
                continue
            pattern_passed = mapping(meta.get("pattern")).get("passed")
            if sensor == "pattern" and type(pattern_passed) is not bool:
                print(f"VOID {key} missing CPU pattern verdict")
                continue
            if sensor == "pattern" and not pattern_passed:
                print(f"OBSERVED {key} CPU 420v failed expected bars/counter and/or plane check; "
                      "examine BGRA-to-420v or earlier handoff before assigning a GPU cause")
                continue
            if sensor == "pattern":
                print(f"KNOWN_POSITIVE {key} CPU 420v RGB/planes/counter")
            if replay.get("comparable") is not True or any(not path.is_file() for path in files.values()):
                print(f"INCONCLUSIVE {key} matched import, immutable seeds or GPU fence/readback control missing")
                continue
            diff = mapping(replay.get("A_vs_B"))
            pixels, exact, over20, over40 = (diff.get(name) for name in ("pixels", "exactRGB", "over20RGB", "over40RGB"))
            mean = diff.get("meanAbsoluteRGB")
            if (any(files[name].stat().st_size != BGRA_BYTES for name in ("-A.bgra", "-B.bgra"))
                    or any(type(count) is not int for count in (pixels, exact, over20, over40))
                    or pixels != FRAME_WIDTH * FRAME_HEIGHT or not 0 <= over40 <= over20 <= pixels - exact <= pixels
                    or type(mean) not in (int, float) or not 0 <= mean <= 255):
                print(f"VOID {key} incomplete GPU capture files or diff")
                continue
            a_ok = mapping(replay.get("A_pattern")).get("passed")
            b_ok = mapping(replay.get("B_pattern")).get("passed")
            if sensor == "pattern" and (type(a_ok) is not bool or type(b_ok) is not bool):
                print(f"VOID {key} missing GPU pattern verdict")
                continue
            print(f"DIFF {key} A/B exact={diff['exactRGB']}/{diff['pixels']} "
                  f"over20={diff['over20RGB']} over40={diff['over40RGB']} mean={diff['meanAbsoluteRGB']:.3f}")
            if sensor != "pattern":
                print(f"REVIEW {key} compare CPU PNG with A/B to assess scene and apply synthetic verdict")
                continue
            if a_ok:
                print(f"OBSERVED {key} A renders expected pattern; corruption not reproduced, "
                      "coherence candidate unconfirmed")
                if not b_ok:
                    print(f"INCONCLUSIVE {key} copied B failed while A passed; inspect replay")
            elif b_ok:
                print(f"OBSERVED {key} only imported A corrupt; isolated to IOSurface import/cache visibility")
            else:
                print(f"OBSERVED {key} CPU correct and both A/B corrupt; inspect the common 27.3 CI program/operation "
                      "with the saved library, pipeline and dispatch metadata")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("boot")
    args = parser.parse_args()
    try:
        unpack(args.archive, args.output)
        inspect(args.output, args.boot)
    except (OSError, EOFError, tarfile.TarError, ValueError) as error:
        sys.exit(f"evaluate.py: {error}")


if __name__ == "__main__":
    main()
