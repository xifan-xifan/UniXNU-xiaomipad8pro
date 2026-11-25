#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify the observation artifact set and interpret the synthetic control."""
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

def unpack(archive, output):
    """Verify the remote SHA-256 of each file before writing any members."""
    with tarfile.open(archive, "r:gz") as tar:
        entries = {}
        for member in tar.getmembers():
            name = member.name.removeprefix("./")
            if member.isdir() and member.name == ".":
                continue
            if not member.isfile() or name in entries or not (ARTIFACT.fullmatch(name) or name == "SHA256SUMS"):
                raise ValueError(f"unexpected archive member {name!r}")
            if not 0 < member.size <= 16 * 1024 * 1024:
                raise ValueError(f"invalid artifact size for {name}")
            entries[name] = tar.extractfile(member).read()
    manifest = entries.pop("SHA256SUMS").decode("ascii")
    expected = {}
    for line in manifest.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (?:\./)?(.+)", line)
        if not match or match[2] in expected:
            raise ValueError("malformed or duplicate SHA256SUMS entry")
        expected[match[2]] = match[1]
    if not entries or entries.keys() != expected.keys():
        raise ValueError("SHA256SUMS does not enumerate exactly the fetched files")
    for name, data in entries.items():
        if hashlib.sha256(data).hexdigest() != expected[name]:
            raise ValueError(f"SHA-256 mismatch for {name}")
    output.mkdir()
    for name, data in entries.items():
        (output / name).write_bytes(data)
    (output / "SHA256SUMS").write_text(manifest)
    print(f"VERIFIED_ARTIFACTS {len(entries)} remote SHA-256 matches")


def inspect(output, boot):
    def read(name):
        path = output / name
        return json.loads(path.read_bytes()) if path.is_file() else {}

    log = (output / "diag.log").read_text(errors="replace")
    if not re.search(rf"^DIAG_START boot={re.escape(boot)} uid=[1-9][0-9]+$", log, re.M):
        print("VOID client boot UUID does not match the verified SSH boot")
        return
    expected_complete = "DIAG_COMPLETE COLLECTED" in log
    if not expected_complete:
        print("PARTIAL client completion missing or unsuccessful; review preserved evidence")
    for sensor in ("pattern", "front", "rear"):
        capture = read(f"{sensor}-capture.json")
        if capture.get("frames") != 32 or not capture.get("activeIndex1"):
            print(f"INCONCLUSIVE {sensor} index 1 or frame 32 unverified")
            continue
        if not capture.get("repeatedSurfaceIDs"):
            print(f"INCONCLUSIVE {sensor} no observed IOSurface ID reuse before frame 32")
        first = read(f"{sensor}-1-frame.json")
        later = read(f"{sensor}-32-frame.json")
        try:
            increasing = first["presentationTime"]["value"] * later["presentationTime"]["scale"] < (
                later["presentationTime"]["value"] * first["presentationTime"]["scale"])
        except KeyError:
            increasing = False
        surface_ids = capture.get("surfaceIDs", [])
        if not increasing or len(surface_ids) != 32 or surface_ids[0] == surface_ids[-1]:
            print(f"INCONCLUSIVE {sensor} retained first/frame-32 time or surface identity is invalid")
            continue
        if sensor == "pattern" and first.get("pattern", {}).get("passed") and later.get("pattern", {}).get("passed"):
            if first["pattern"]["rgb"]["counter"] == later["pattern"]["rgb"]["counter"]:
                print("INCONCLUSIVE pattern counter did not advance between retained frames")
                continue
        for frame in (1, 32):
            key = f"{sensor}-{frame}"
            meta = read(f"{key}-frame.json")
            replay = read(f"{key}-replay.json")
            files = {name: output / (key + name) for name in ("-y.bin", "-uv.bin", "-cpu.png",
                                                            "-A.bgra", "-A.png", "-B.bgra", "-B.png")}
            if not meta or meta.get("error") or meta.get("surface", {}).get("id") != surface_ids[frame - 1]:
                print(f"INCONCLUSIVE {key} no valid immutable 420v snapshot")
                continue
            planes = meta["planes"]
            if any(not files[name].is_file() for name in ("-y.bin", "-uv.bin", "-cpu.png")) or any(
                files[name].stat().st_size != plane["rowBytes"] * plane["height"]
                for name, plane in zip(("-y.bin", "-uv.bin"), planes, strict=True)
            ):
                print(f"VOID {key} incomplete CPU capture files")
                continue
            if sensor == "pattern" and not meta["pattern"]["passed"]:
                print(f"OBSERVED {key} CPU 420v failed expected bars/counter and/or plane check; "
                      "examine BGRA-to-420v or earlier handoff before assigning a GPU cause")
                continue
            if sensor == "pattern":
                print(f"KNOWN_POSITIVE {key} CPU 420v RGB/planes/counter")
            if not replay.get("comparable") or any(not path.is_file() for path in files.values()):
                print(f"INCONCLUSIVE {key} matched import, immutable seeds or GPU fence/readback control missing")
                continue
            diff = replay["A_vs_B"]
            print(f"DIFF {key} A/B exact={diff['exactRGB']}/{diff['pixels']} "
                  f"over20={diff['over20RGB']} over40={diff['over40RGB']} mean={diff['meanAbsoluteRGB']:.3f}")
            if sensor != "pattern":
                print(f"REVIEW {key} compare CPU PNG with A/B to assess scene and apply synthetic verdict")
                continue
            a_ok = replay["A_pattern"]["passed"]
            b_ok = replay["B_pattern"]["passed"]
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

if __name__ == "__main__":
    fetched, dest, uuid = sys.argv[1:]
    unpack(Path(fetched), Path(dest))
    inspect(Path(dest), uuid)
