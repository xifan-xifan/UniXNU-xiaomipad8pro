#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Known positives and negatives of scripts/ci/privacy-scan.py's pattern classes (the samples are assembled at run time, so this
file itself stays clean for the scan)."""
import importlib.util
import sys
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


if __name__ == '__main__':
    sys.exit(unittest.main())
