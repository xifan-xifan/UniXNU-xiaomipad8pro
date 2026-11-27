#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Regression tests for the k27 policy test wrappers."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
PATCHES = HERE.parents[1] / 'patches/k27'


class PolicyChecks(unittest.TestCase):
    def test_patch_comparison_detects_crlf_in_host_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(['git', 'init', '-q', tmp], check=True)
            relative = 'pexpert/arm/piano_bootargs_policy.h'
            patch, = PATCHES.glob('0066-*.patch')
            content = (HERE / 'piano-bootargs' / Path(relative).name).read_bytes()
            header = root / 'header.h'
            for expected, data in ((0, content), (1, content.replace(b'\n', b'\r\n'))):
                header.write_bytes(data)
                result = subprocess.run([sys.executable, str(HERE / 'check-policy-header.py'), relative, str(header), str(patch)],
                                        env=dict(os.environ, TMPDIR=tmp), capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                if expected:
                    self.assertIn('does not add', result.stderr)

    def test_pm_contract_failure_even_with_an_object(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            k27 = root / 'tooling/k27'
            k27.mkdir(parents=True)
            for name in ('test-piano-pm.sh', 'check-policy-header.py'):
                shutil.copyfile(HERE / name, k27 / name)
            shutil.copytree(HERE / 'piano-pm', k27 / 'piano-pm')
            patches = root / 'patches/k27'
            patches.mkdir(parents=True)
            for prefix in ('0061', '0064'):
                patch, = PATCHES.glob(f'{prefix}-*.patch')
                shutil.copyfile(patch, patches / patch.name)
            compiler = root / 'stub-cc'
            compiler.write_text('#!/bin/sh\nwhile [ "$1" != -o ]; do shift; done\n'
                                'printf "#!/bin/sh\\nexit 0\\n" > "$2"\nchmod +x "$2"\n')
            compiler.chmod(0o755)
            (k27 / 'build-native-kext.py').write_text(
                'import sys\nfrom pathlib import Path\n'
                'out = Path(sys.argv[-1])\nout.mkdir(parents=True, exist_ok=True)\n'
                '(out / "contract_check.o").touch()\nraise SystemExit("stub compile failed after writing an object")\n')
            (root / 'python3').symlink_to(sys.executable)
            env = dict(os.environ, CC=str(compiler), K27_KERNEL='', PATH=os.pathsep.join((tmp, os.environ['PATH'])))
            result = subprocess.run(['bash', str(k27 / 'test-piano-pm.sh'), '--kext'], env=env,
                                    capture_output=True, text=True, timeout=15)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn('stub compile failed', result.stderr)


if __name__ == '__main__':
    unittest.main()
