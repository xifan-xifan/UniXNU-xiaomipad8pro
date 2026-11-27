#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang. All rights reserved.
"""Compile the actual firmware C adapter with host allocation/UFS substitutes."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
p = root / 'package/PianoBluetooth'
with tempfile.TemporaryDirectory(prefix='bt-fw-host-') as tmp:
    inc = Path(tmp)
    (inc / 'IOKit').mkdir()
    (inc / 'sys').mkdir()
    (inc / 'sys/systm.h').write_text('#include <string.h>\n#include <errno.h>\n')
    (inc / 'IOKit/IOLib.h').write_text('''#include <stddef.h>
extern void *bt_fw_alloc(size_t);
extern void bt_fw_free(void *);
#define IOMallocType(t) ((t *)bt_fw_alloc(sizeof(t)))
#define IOFreeType(p,t) bt_fw_free(p)
#define IOLog(...) ((void)0)
''')
    binary = inc / 'firmware_host'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                    '-I', str(inc), '-I', str(p), '-I', str(root / 'package/PianoPower'),
                    str(p / 'tests/firmware_host.c'), str(p / 'BtFirmware.c'),
                    str(root / 'package/PianoPower/FatVolume.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
