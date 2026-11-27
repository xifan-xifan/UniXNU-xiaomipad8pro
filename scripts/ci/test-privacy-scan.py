#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Known positives and negatives of scripts/ci/privacy-scan.py's pattern classes (the samples are assembled at run time, so this
file itself stays clean for the scan)."""
import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('privacy_scan', HERE / 'privacy-scan.py')
ps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ps)


def classes(line, path='docs/x.md', diff=True):
    hits = []
    ps.scan_line(path, 1, line, [], hits, diff=diff)
    return sorted({h[2] for h in hits})


J = ''.join


class Positives(unittest.TestCase):
    def test_home(self):
        self.assertEqual(classes('see ' + J(['/Us', 'ers/alice/x'])), ['home path'])
        self.assertEqual(classes(J(['/ho', 'me/bob/.ssh'])), ['home path'])

    def test_private_notes_diff_only(self):
        line = 'evidence in ' + J(['priv', 'ate/host-checks/2026/x.md'])
        self.assertEqual(classes(line), ['maintainer private notes*'])
        self.assertEqual(classes(line, diff=False), [])

    def test_lan(self):
        for ip in (J(['192.', '168.1.20']), J(['10.', '0.0.7']), J(['172.', '20.3.4'])):
            self.assertEqual(classes('ssh ' + ip), ['LAN address'], ip)

    def test_host(self):
        self.assertEqual(classes('user@' + J(['mbp', '.private'])), ['e-mail address', 'local host name'])
        self.assertEqual(classes(J(['nas', '.lan:/share'])), ['local host name'])

    def test_mac(self):
        self.assertEqual(classes(J(['a4:83:e7', ':12:34:56'])), ['MAC address'])

    def test_email(self):
        self.assertEqual(classes('mail ' + J(['someone', '@gmail.com'])), ['e-mail address'])
        self.assertEqual(classes(J(['someone+noreply', '@mail.com'])), ['e-mail address'])
        self.assertEqual(classes(J(['someone', '@example.com.mail.com'])), ['e-mail address'])

    def test_secrets(self):
        self.assertEqual(classes(J(['-----BEGIN ', 'OPENSSH PRIVATE KEY-----'])), ['private key'])
        self.assertEqual(classes('t=' + J(['ghp_', 'A' * 36])), ['access token'])
        self.assertEqual(classes(J(['password', ' = "s3cr3tpw"'])), ['inline secret'])
        self.assertEqual(classes(J(['echo hunter22 ', '| sudo -S true'])), ['password piped to sudo'])


class Negatives(unittest.TestCase):
    def test_clean(self):
        for line in ('/Users/Shared/x', 'for u in /Users/*; do', '/private/var/db/x', '/usr/local/lib',
                     'com.apple.kpi.private', 'xnu-12377.121.6', 'version 10.0.1.2.3',
                     '02:50:49:41:4e:01', 'Co-authored-by: A <1+a@users.noreply.github.com>',
                     'From: Someone <someone@host.example>', 'password = "$SECRET"',
                     'echo [REDACTED] | sudo -S true', 'IOMobileFramebuffer@com.apple.iokit.IOMobileGraphicsFamily'):
            hits = []
            ps.scan_line('docs/x.md', 1, line, ps.load_allow(), hits, diff=True)
            self.assertEqual(hits, [], line)

    def test_marker(self):
        self.assertEqual(classes(J(['192.', '168.1.20']) + '  # privacy-scan: ok'), [])


class GitDiff(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.git('init', '-q')
        self.script = self.root / 'scripts/ci/privacy-scan.py'
        self.script.parent.mkdir(parents=True)
        shutil.copyfile(HERE / 'privacy-scan.py', self.script)
        self.commit('Base fixture')
        self.base = self.git('rev-parse', 'HEAD').strip()

    def git(self, *args):
        return subprocess.check_output(
            ['git', '-C', str(self.root), '-c', 'user.name=Fixture',
             '-c', 'user.email=fixture@example.test', *args], text=True,
        )

    def commit(self, message='Update fixture'):
        self.git('add', '-A')
        self.git('commit', '-qm', message)

    def scan(self, base):
        return subprocess.run(
            [sys.executable, str(self.script), f'--diff={base}'],
            cwd=self.root, capture_output=True, text=True, timeout=10,
        )

    def test_scans_committed_lines_in_quoted_paths_and_diff_disabled_files(self):
        path = self.root / 'café "fixture".txt'
        (self.root / '.gitattributes').write_text('*.txt -diff\n')
        path.write_text('prior fixture\n')
        self.commit()
        base = self.git('rev-parse', 'HEAD').strip()
        path.write_text('prior fixture\n+++ synthetic ' + J(['192.', '168.1.20']) + '\n')
        self.commit()
        path.unlink()
        result = self.scan(base)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f'{path.name}:2: LAN address', result.stdout)
        self.assertIn('1 hits, 0 refused files', result.stdout)

    def test_refuses_committed_outputs_with_quoted_paths(self):
        (self.root / 'fixture\toutput.DMG').write_text('placeholder\n')
        binary = self.root / 'fixture.o'
        binary.write_bytes(b'\0fixture')
        self.commit()
        binary.write_text('changed locally\n')
        result = self.scan(self.base)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('0 hits, 2 refused files', result.stdout)

    def test_rename_only_scans_new_content(self):
        old = self.root / 'old.txt'
        old.write_text('Existing line ' + J(['192.', '168.1.20']) + '\n' + 'unmodified fixture\n' * 10)
        self.commit()
        base = self.git('rev-parse', 'HEAD').strip()
        new = self.root / 'new "fixture".txt'
        old.rename(new)
        with new.open('a') as file:
            file.write('Added line ' + J(['172.', '20.3.4']) + '\n')
        self.commit()
        result = self.scan(base)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f'{new.name}:12: LAN address', result.stdout)
        self.assertIn('1 hits, 0 refused files', result.stdout)

    def test_scans_commit_messages_when_the_file_diff_is_empty(self):
        self.git('commit', '--allow-empty', '-m', 'Fixture ' + J(['192.', '168.1.20']))
        result = self.scan(self.base)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('1 hits, 0 refused files', result.stdout)

    def test_staged_uses_index_bytes_and_refuses_outputs(self):
        path = self.root / 'capture.txt'
        path.write_text('staged fixture ' + J(['192.', '168.1.20']) + '\n')
        (self.root / 'fixture.o').write_bytes(b'\0fixture')
        self.git('add', '-A')
        path.write_text('clean working copy\n')
        (self.root / 'fixture.o').unlink()
        result = subprocess.run([sys.executable, str(self.script), '--staged'], cwd=self.root,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('capture.txt:1: LAN address', result.stdout)
        self.assertIn('1 hits, 1 refused files', result.stdout)

    def test_empty_diff_and_invalid_base(self):
        result = self.scan(self.base)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('0 hits, 0 refused files', result.stdout)
        for base in ('', '--all', 'missing-ref'):
            with self.subTest(base=base):
                result = self.scan(base)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertNotIn('Traceback', result.stderr)

    def test_staged_scan_uses_index_blobs_and_refuses_outputs(self):
        path = self.root / 'café "staged".txt'
        path.write_text('prior fixture\n')
        self.commit()
        path.write_text('prior fixture\nnew line ' + J(['172.', '20.3.4']) + '\n')
        output = self.root / 'staged.DMG'
        output.touch()
        self.git('add', '-A')
        path.write_text('safe working copy\n')
        output.unlink()
        (self.root / 'unstaged.txt').write_text(J(['192.', '168.1.20']) + '\n')
        result = subprocess.run([sys.executable, str(self.script), '--staged'],
                                cwd=self.root, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f'{path.name}:2: LAN address', result.stdout)
        self.assertIn('staged.DMG: file type not allowed', result.stdout)
        self.assertIn('1 hits, 1 refused files', result.stdout)


if __name__ == '__main__':
    sys.exit(unittest.main())
