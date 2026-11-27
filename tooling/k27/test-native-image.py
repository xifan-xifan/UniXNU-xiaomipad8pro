#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the native image script's descriptor handoff with a stub kernel tree."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


class ImageBuildTests(unittest.TestCase):
    def test_descriptor_paths_and_order(self):
        with tempfile.TemporaryDirectory(prefix='native-image-') as temp:
            repo = Path(temp, 'worktree # & with spaces')
            k27 = repo / 'tooling/k27'
            kexts = k27 / 'kext'
            kexts.mkdir(parents=True)
            for name in ('build-native-image.sh', 'k27-tree.sh'):
                shutil.copyfile(HERE / name, k27 / name)
            names = ('PianoPower', 'PianoKeyboard')
            for name, builder in zip(names, ('build-native-piano-power.sh', 'build-native-piano-keyboard.sh')):
                (kexts / f'{name}.json').write_text('{"objects": "@OUT@/native-kexts/objects.json"}\n')
                (k27 / builder).write_text('#!/bin/bash\nexit 0\n')
                (k27 / builder).chmod(0o755)
            kc = repo / 'tooling/kc/build-kc-k27.sh'
            kc.parent.mkdir()
            shutil.copyfile(HERE.parent / 'kc' / kc.name, kc)
            kc.chmod(0o755)
            shutil.copyfile(HERE / 'check-native-kexts.py', k27 / 'check-native-kexts.py')
            (k27 / 'check-kext-sysctls.py').touch()
            (k27 / 'normalize-bluetooth-codeless.py').write_text('import sys\nfrom pathlib import Path\nPath(sys.argv[2]).touch()\n')
            (k27 / 'kc-space.py').touch()
            (repo / 'k27').mkdir()
            (repo / 'k27/kc27-layout.py').touch()
            (repo / 'k27/kc-build27-piano.py').write_text(
                'import json, sys\nfrom pathlib import Path\n'
                'Path(sys.argv[3]).touch()\n'
                'Path("descriptors").write_text(json.dumps(sys.argv[sys.argv.index("--native") + 1:]))\n'
                'print("wrote KC")\n')
            (repo / 'configs').mkdir()
            (repo / 'configs/kc-keep.txt').touch()
            kernel = repo / 'linked kernel'
            kernel.touch()
            config = repo / 'list with spaces.txt'
            config.write_text('PianoPower # first\n\nPianoKeyboard\n')
            extra = repo / 'extra descriptor'
            extra.mkdir()
            (extra / 'PianoPower.json').touch()
            (extra / 'Other.json').write_text('{}\n')

            tools = Path(temp, 'bin')
            tools.mkdir()
            (tools / 'python3').symlink_to(sys.executable)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(('PIANO_', 'K27_', 'KC_'))}
            env['PATH'] = os.pathsep.join((str(tools), env['PATH']))
            env.update(PIANO_K27_TREE=str(repo), PIANO_KEXT_LIST=str(config), PIANO_KC_ONLY='1', K27_KERNEL=str(kernel),
                       LLVM_NM='/usr/bin/true',
                       PIANO_EXTRA_KEXTS='extra descriptor/PianoPower.json')
            cmd = ['bash', str(k27 / 'build-native-image.sh')]
            for value, expected_extra in ((env['PIANO_EXTRA_KEXTS'], []),
                                          ('extra descriptor/Other.json', [str(extra / 'Other.json')])):
                with self.subTest(value=value):
                    result = subprocess.run(cmd, cwd=temp, env=dict(env, PIANO_EXTRA_KEXTS=value), capture_output=True,
                                            text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    output = repo / 'out/k27/kext'
                    expected = [str(output / f'{name}.json') for name in names] + expected_extra
                    got = json.loads((repo / 'descriptors').read_text())
                    self.assertEqual(got, expected)
                    self.assertEqual(json.loads((output / 'PianoPower.json').read_text())['objects'],
                                     str(repo / 'out/native-kexts/objects.json'))


if __name__ == '__main__':
    unittest.main()
