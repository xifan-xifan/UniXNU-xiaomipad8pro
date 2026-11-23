#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""firmware/patches/unixnu/0003 carries PianoBootFlag.h: it must be firmware/selector/bootflag.h byte for byte (one codec for the
selector, the FD, the kext and bootflag.sh), and the patch must add the three menu actions."""
import sys
from pathlib import Path

R = Path(__file__).resolve().parents[2]
patch = (R / 'firmware/patches/unixnu/0003-UniXNULib-piano-boot-flag-menu-actions.patch').read_text()
want = (R / 'firmware/selector/bootflag.h').read_text()
sec = patch.split('b/package/UniXNUPkg/Library/UniXNULib/PianoBootFlag.h', 1)[1].split('\n', 1)[1]
body = ''.join(l[1:] + '\n' for l in sec.split('\n') if l.startswith('+'))
fails = 0
for ok, msg in ((body == want, 'PianoBootFlag.h in the patch = firmware/selector/bootflag.h'),
                (all(f'"{a}"' in patch for a in ('android', 'android-default', 'macos-default')), 'the three actions parsed'),
                ('PianoBootFlagAction (PIANO_REQ_ONCE_ANDROID_VALUE)' in patch, 'android = once-Android request'),
                ('PianoBootFlag.c' in patch and 'UniXNULib.inf' in patch, 'sources listed in the inf')):
    print(('PASS ' if ok else 'FAIL ') + msg)
    fails += not ok
print(f'{"FAIL" if fails else "PASS"}: FD patch checks, {fails} failures')
sys.exit(1 if fails else 0)
