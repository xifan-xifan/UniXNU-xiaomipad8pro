#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Test the k27 tree fetch and patch sequence with a local Git fixture."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class FetchTreeTests(unittest.TestCase):
    def test_failed_tree_check_can_be_retried(self):
        with tempfile.TemporaryDirectory(prefix='k27-tree-') as tmp:
            root = Path(tmp)
            upstream = root / 'local upstream'
            upstream.mkdir()

            def git(*args, cwd=upstream):
                return subprocess.run(['git', '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.test', *args],
                                      cwd=cwd, capture_output=True, text=True, check=True, timeout=15).stdout

            git('init', '-q')
            source = upstream / 'source'
            source.write_text('base\n')
            git('add', 'source')
            git('commit', '-qm', 'base')
            base = git('rev-parse', 'HEAD').strip()
            source.write_text('patched\n')
            git('commit', '-qam', 'apply fixture patch')
            tree = git('rev-parse', 'HEAD^{tree}').strip()

            repo = root / 'repo'
            helper = repo / 'tooling/k27/fetch-k27-tree.sh'
            helper.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / 'tooling/k27/fetch-k27-tree.sh', helper)
            (repo / 'scripts').mkdir()
            for name in ('fetch-pins.py', 'repo_manifest.py'):
                shutil.copyfile(ROOT / 'scripts' / name, repo / 'scripts' / name)
            patches = repo / 'patches/k27-tree'
            patches.mkdir(parents=True)
            (patches / '0001-fixture.patch').write_text(git('format-patch', '-1', '--stdout'))
            lock = root / 'fixture.lock'

            def pin(result):
                lock.write_text(f'[pin."k27"]\ngit = {json.dumps(str(upstream))}\ncommit = "{base}"\n'
                                f'patches = "patches/k27-tree"\ntree = "{result}"\n')

            dest = root / 'destination parent/k27 tree'
            tools = root / 'bin'
            tools.mkdir()
            (tools / 'python3').symlink_to(sys.executable)
            env = dict(os.environ, PATH=os.pathsep.join((str(tools), os.environ['PATH'])))

            def fetch():
                return subprocess.run(['bash', str(helper), str(dest), str(lock)], env=env,
                                      capture_output=True, text=True, timeout=30)

            pin('0' * 40)
            result = fetch()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('k27 tree mismatch', result.stderr)
            self.assertFalse(dest.exists())
            self.assertEqual(list(dest.parent.iterdir()), [])

            pin(tree)
            result = fetch()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(git('rev-parse', 'HEAD^{tree}', cwd=dest).strip(), tree)
            self.assertEqual((dest / 'source').read_text(), 'patched\n')
            self.assertIn('is the pinned one', fetch().stdout)
            (dest / 'source').write_text('local change\n')
            self.assertNotEqual(fetch().returncode, 0)
            self.assertEqual((dest / 'source').read_text(), 'local change\n')


if __name__ == '__main__':
    unittest.main()
