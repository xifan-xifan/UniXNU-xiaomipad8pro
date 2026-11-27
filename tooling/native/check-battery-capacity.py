#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""Read the percentage property from a saved AppleSmartBattery registry dump."""
import argparse
from pathlib import Path
import re


def current_capacity(text):
    # AppleRawCurrentCapacity is in mAh, despite sharing the suffix.
    values = re.findall(r'^[ \t]*"CurrentCapacity"[ \t]*=([^\n]*)$', text, re.M)
    value = values[0].strip(' \t') if len(values) == 1 else ''
    if not re.fullmatch(r'[0-9]+', value):
        raise ValueError('expected one CurrentCapacity percentage in 0..100')
    digits = value.lstrip('0') or '0'
    if len(digits) > 3 or int(digits) > 100:
        raise ValueError('expected one CurrentCapacity percentage in 0..100')
    return int(digits)


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
