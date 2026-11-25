#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""Read the percentage property from a saved AppleSmartBattery registry dump."""
import argparse
from pathlib import Path
import re


def current_capacity(text):
    # AppleRawCurrentCapacity is in mAh, despite sharing the suffix.
    values = re.findall(r'^ *"CurrentCapacity" *= *([0-9]+) *$', text, re.M)
    if len(values) != 1 or not 0 <= int(values[0]) <= 100:
        raise ValueError('expected one CurrentCapacity percentage in 0..100')
    return int(values[0])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('registry', type=Path)
    args = p.parse_args()
    try:
        print(current_capacity(args.registry.read_text()))
    except (OSError, ValueError) as exc:
        p.exit(1, 'FAIL battery registry: ' + str(exc) + '\n')


if __name__ == '__main__':
    main()
