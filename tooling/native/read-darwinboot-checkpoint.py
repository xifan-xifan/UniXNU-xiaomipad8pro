#!/usr/bin/env python3
"""Decode the DarwinBoot ExitBootServices records from a full piano CVP dump."""

import argparse
import struct
from pathlib import Path

CVP_SIZE = 0x700000
FIRST_RECORD = 0x6F5000
MAGIC = 0x315342454F4E4150
MASK = (1 << 64) - 1
STAGES = {1: "before GetMemoryMap", 2: "before ExitBootServices",
          3: "ExitBootServices returned", 4: "GetMemoryMap failed"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    args = parser.parse_args()
    with args.dump.open("rb") as stream:
        data = stream.read(CVP_SIZE + 1)
    if len(data) != CVP_SIZE:
        parser.error(f"expected a complete {CVP_SIZE}-byte CVP dump")

    records = set()
    for copy in range(11):
        offset = FIRST_RECORD + copy * 0x1000
        magic, packed, status, complement = struct.unpack_from("<4Q", data, offset)
        stage, attempt = packed & 0xFFFFFFFF, packed >> 32
        if magic != MAGIC or packed ^ complement != MASK or stage not in STAGES or attempt >= 3:
            print(f"copy {copy}: no valid record")
            continue
        records.add((stage, attempt, status))
        print(f"copy {copy}: {STAGES[stage]}, attempt {attempt}, EFI_STATUS=0x{status:016x}")

    if not records:
        print("No checkpoint recovered. This does not identify the last executed stage.")
        return 1
    if len(records) != 1:
        print("Checkpoint copies disagree. Inspect the individual records.")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
