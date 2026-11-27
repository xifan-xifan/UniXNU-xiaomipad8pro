#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise pinned checkout recovery against an entirely local repository."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


FETCH = Path(__file__).resolve().parents[1] / 'fetch-pins.py'


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


class FetchPins(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.source = self.root / 'source'
        self.research = self.root / 'research'
        self.checkout = self.research / 'fixture'
        self.lock = self.root / 'fixture.lock'
        self.source.mkdir()
        git(self.source, 'init', '-q')
        self.commit = self.make_commit(self.source)
        self.entry = f'[pin.fixture]\ngit = {json.dumps(self.source.as_uri())}\ncommit = "{self.commit}"\n'
        self.lock.write_text(self.entry)

    def make_commit(self, path, message='Pin fixture'):
        git(path, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.test',
            'commit', '--allow-empty', '-qm', message)
        return git(path, 'rev-parse', 'HEAD')

    def fetch(self, *args):
        return subprocess.run(
            [sys.executable, str(FETCH), '--lock', str(self.lock), *args],
            env=dict(os.environ, UNIXNU_RESEARCH=str(self.research)),
            capture_output=True, text=True, timeout=10,
        )

    def test_retries_checkout_interrupted_before_origin_was_added(self):
        self.checkout.mkdir(parents=True)
        git(self.checkout, 'init', '-q')
        result = self.fetch('--only', 'fixture')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(git(self.checkout, 'rev-parse', 'HEAD'), self.commit)

    def test_refuses_invalid_lock_before_creating_checkouts(self):
        for entry, problem in (
            ('pin = []\n', 'pin'),
            (self.entry + '[pin.broken]\ngit = "https://example.test/repo"\n', 'commit'),
            (self.entry.replace('[pin.fixture]', '[pin."../outside"]'), 'name'),
            (self.entry + 'submodules = {"../outside" = "' + self.commit + '"}\n', 'submodule'),
            (self.entry.replace(self.commit, 'HEAD'), 'commit'),
            (self.entry.replace(json.dumps(self.source.as_uri()), 'false'), 'git'),
            (self.entry + 'branch = []\n', 'branch'),
            (self.entry + 'submodules = []\n', 'submodules'),
            (self.entry + 'commit = "duplicate"\n', 'invalid lockfile'),
        ):
            with self.subTest(problem=problem):
                self.lock.write_text(entry)
                result = self.fetch()
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn(problem, result.stderr)
                self.assertNotIn('Traceback', result.stderr)
                self.assertFalse(self.research.exists())

    def test_outdated_distinguishes_missing_branch_from_failed_remote(self):
        self.lock.write_text(self.entry + 'branch = "absent"\n')
        missing = self.fetch('--outdated')
        self.assertEqual(missing.returncode, 1, missing.stdout + missing.stderr)
        self.assertIn('absent not found', missing.stdout)
        self.lock.write_text(self.entry.replace(self.source.as_uri(), (self.root / 'absent').as_uri()))
        unreachable = self.fetch('--outdated')
        self.assertEqual(unreachable.returncode, 1, unreachable.stdout + unreachable.stderr)
        self.assertIn('could not query', unreachable.stdout)

    def test_fetch_and_verify_submodule_named_differently_from_its_path(self):
        sub = self.source / 'sub-origin'
        sub.mkdir()
        git(sub, 'init', '-q')
        git(sub, 'config', 'uploadpack.allowFilter', 'true')
        git(self.source, 'config', 'uploadpack.allowFilter', 'true')
        sub_commit = self.make_commit(sub)
        path = 'deps/path with space'
        (self.source / '.gitmodules').write_text(
            f'[submodule "different-name"]\n    path = {path}\n    url = ./sub-origin\n'
        )
        git(self.source, 'add', '.gitmodules')
        git(self.source, 'update-index', '--add', '--cacheinfo', f'160000,{sub_commit},{path}')
        parent_commit = self.make_commit(self.source, 'Pin submodule')
        self.lock.write_text(self.entry.replace(self.commit, parent_commit) +
                             f'submodules = {{ {json.dumps(path)} = "{sub_commit}" }}\n')
        for mode in ('--verify', '--dry-run'):
            result = self.fetch(mode)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(self.research.exists())
        self.assertIn(f'would fetch fixture/{path}', result.stdout)
        result = self.fetch()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(git(self.checkout / path, 'rev-parse', 'HEAD'), sub_commit)


if __name__ == '__main__':
    unittest.main()
