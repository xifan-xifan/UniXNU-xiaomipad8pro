#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage the public GPU kits and run the Adreno830 host suite in the self-test's scratch directory."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from repo_manifest import load_manifest

PACKAGE = ROOT / 'package/PianoA830'
KITS = ('ArmSmmu4K', 'AdrenoPM4', 'AdrenoFirmware')


def main():
    scratch = Path(sys.argv[1]).resolve()
    unixnu = scratch / 'UniXNU'
    kits = unixnu / 'kits'
    lock = ROOT / 'unixnu.lock'
    pins = load_manifest(lock)['pin']
    fetch = subprocess.run([sys.executable, ROOT / 'scripts/fetch-pins.py', '--only', ','.join(KITS)],
                           env=dict(os.environ, UNIXNU_RESEARCH=str(kits), GIT_TERMINAL_PROMPT='0'))
    if fetch.returncode:
        sys.exit('Adreno830: failed to fetch the pinned UniXNU kits')
    for name in KITS:
        head = subprocess.check_output(['git', '-C', kits / name, 'rev-parse', 'HEAD'], text=True).strip()
        if head != pins[name]['commit']:
            sys.exit(f'{name}: expected {pins[name]["commit"]}, got {head}')

    # The paged builder is part of PianoA830 and extends the pinned SMMU kit.
    for name in ('ArmSmmu4KPaged.c', 'ArmSmmu4KPaged.h'):
        shutil.copyfile(PACKAGE / 'kits/ArmSmmu4K' / name, kits / 'ArmSmmu4K' / name)

    # Use synthetic firmware in the individual tests as well as in the suite runner.
    for name in ('A830_FW_DIR', 'A830_GMU_FW', 'ADRENO830_ROOT'):
        os.environ.pop(name, None)
    os.environ['A830_ZAP_MBN'] = ''
    os.environ['TMPDIR'] = str(scratch)
    # Timing tests are left to local runs: test_a830fence asserts a >= 40 ms sliced wait and test_p1wire a parallel wiring
    # speedup; a loaded 3-core hosted runner cannot guarantee either (first CI run 45790630786: both failed, every functional test
    # passed). test_a830regs needs register inputs that are not published.
    skip = ['test_a830regs', 'test_a830fence', 'test_p1wire']
    os.execv(sys.executable, [sys.executable, str(PACKAGE / 'adreno830/tests/run-host-tests.py'),
                             '--unixnu', str(unixnu), '--out', str(scratch / 'host-tests')] +
             [a for name in skip for a in ('--skip', name)])


if __name__ == '__main__':
    main()
