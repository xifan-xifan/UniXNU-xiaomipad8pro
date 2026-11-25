#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Rehearse the real staged install/remove paths with an isolated launchd model.
Never calls the host's launchctl or modifies the host's disabled overrides.
"""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('stage_tests', HERE / 'test-stage.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

LAUNCHCTL = '''#!/usr/bin/env python3
import json, os, pathlib, sys
p = pathlib.Path(os.environ['LAUNCHD'])
s = json.loads((p / 'model.json').read_text())
a = sys.argv[1:]
with (p / 'calls').open('a') as f: f.write(' '.join(a) + '\\n')
if s.get('fail') == a[0]: sys.exit(5)
rc = 0
if a[0] == 'print':
    rc = 0 if (a[1] == 'gui/501' and s['gui']) or a[1] in s['loaded'] else 113
elif a[0] == 'print-disabled':
    if a[1] == 'gui/501' and not s['gui']: sys.exit(113)
    print('disabled services = {')
    for label, v in s['overrides'].items(): print('  "' + label + '" => ' + str(v).lower())
    print('}')
elif a[0] in ('enable', 'disable'):
    receipt = pathlib.Path(os.environ['PIANO_USERLAND_ROOT']) / 'private/var/db/piano-userland/receipt.tsv'
    if not receipt.is_file() or 'piano-passd-policy' not in receipt.read_text():
        print('MISSING_POLICY_RECEIPT_BEFORE_OVERRIDE', file=sys.stderr); sys.exit(77)
    if a[1].startswith('gui/501/') and not s['gui']: sys.exit(113)
    s['overrides'][a[1].rsplit('/', 1)[1]] = a[0] == 'disable'
elif a[0] == 'bootout':
    s['loaded'] = [x for x in s['loaded'] if x != a[1]]
elif a[0] == 'bootstrap':
    label = pathlib.Path(a[2]).stem
    if s['overrides'].get(label): sys.exit(119)
    s['loaded'].append(a[1] + '/' + label)
elif a[0] != 'kickstart': rc = 64
(p / 'model.json').write_text(json.dumps(s))
sys.exit(rc)
'''


class PolicyTests(unittest.TestCase):
    def rig(self, tmp, shell, *, gui=True, prior=None, loaded=True):
        rig = base.Rig(tmp, shell)
        base.write(tmp / 'bin/launchctl', LAUNCHCTL, 0o755)
        base.write(tmp / 'bin/id', '#!/bin/sh\necho "${TEST_UID:-0}"\n', 0o755)
        rig.env['TEST_UID'] = '0'
        state = {'gui': gui, 'overrides': {} if prior is None else {'com.apple.passd': prior},
                 'loaded': ['gui/501/com.apple.passd'] if gui and loaded else []}
        (tmp / 'launchd/model.json').write_text(json.dumps(state))
        rig.files(self.units())
        return rig

    def units(self, on=True):
        return [('piano-passd-policy', on, [
            ('/usr/local/libexec/piano-passd-policy', '0755', (HERE / 'piano-passd-policy.sh').read_bytes()),
            ('/Library/LaunchAgents/com.unixnu.piano-passd-policy.plist', '0644',
             (HERE / 'com.unixnu.piano-passd-policy.plist').read_bytes())])]

    def model(self, rig, **changes):
        p = rig.tmp / 'launchd/model.json'
        state = json.loads(p.read_text())
        if changes:
            state.update(changes)
            p.write_text(json.dumps(state))
        return state

    def policy(self, rig, mode, uid='501'):
        return subprocess.run([rig.shell, str(rig.root('/usr/local/libexec/piano-passd-policy')), mode],
                              env={**rig.env, 'TEST_UID': uid}, capture_output=True, text=True, timeout=10)

    def state(self, rig, name):
        return rig.root('/private/var/db/piano-userland/passd-policy') / name

    def test_boot_and_live_repair_restrictive_state_parent(self):
        for shell in base.SHELLS:
            with self.subTest(shell=shell), tempfile.TemporaryDirectory() as d:
                rig = self.rig(Path(d), shell, gui=False)
                state_parent = self.state(rig, 'status').parent.parent
                for mode in ('--boot', '--live'):
                    if mode == '--live':
                        state_parent.chmod(0o700)
                    command = 'umask 077; exec "$@"' if mode == '--boot' else 'exec "$@"'
                    r = subprocess.run([shell, '-c', command, 'fixture', shell,
                                        str(rig.tmp / 'stage/install.sh'), mode],
                                       env=rig.env, capture_output=True, text=True, timeout=15)
                    self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                    self.assertIn(f'{mode} done: ok', r.stdout)
                    self.assertEqual(state_parent.stat().st_mode & 0o777, 0o711)
                    self.assertEqual(self.state(rig, 'status').parent.stat().st_mode & 0o777, 0o700)
                    self.assertIn('PENDING gui/501 absent', self.state(rig, 'status').read_text())

    def test_installer_matrix(self):
        for shell in base.SHELLS:
            for gui in (False, True):
                for prior in (None, False, True):
                    for loaded in (False, True):
                        with self.subTest(shell=shell, gui=gui, prior=prior, loaded=loaded), tempfile.TemporaryDirectory() as d:
                            rig = self.rig(Path(d), shell, gui=gui, prior=prior, loaded=loaded)
                            rc, out = rig.run('--live')
                            self.assertEqual(rc, 0, out)
                            if not gui:
                                self.assertIn('PENDING', self.state(rig, 'status').read_text())
                                self.assertFalse(self.state(rig, 'original').exists())
                                self.model(rig, gui=True)
                                p = self.policy(rig, '--apply')
                                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                            self.assertEqual(self.state(rig, 'original').read_text().strip(),
                                             'default' if prior is None else str(prior).lower())
                            self.assertTrue(self.model(rig)['overrides']['com.apple.passd'])
                            self.assertNotIn('gui/501/com.apple.passd', self.model(rig)['loaded'])
                            self.assertIn('PASS disabled=true loaded=false', self.state(rig, 'status').read_text())
                            rc, out = rig.run('--live')
                            self.assertEqual(rc, 0, out)
                            p = self.policy(rig, '--apply', '502')
                            self.assertIn('SKIP uid=502', p.stdout)
                            rc, out = rig.run('--check')
                            self.assertEqual(rc, 0, out)
                            rig.files([])
                            rc, out = rig.run('--live')
                            self.assertEqual(rc, 0, out)
                            self.assertFalse(rig.root('/usr/local/libexec/piano-passd-policy').exists())
                            self.assertEqual(self.model(rig)['overrides']['com.apple.passd'], prior is True)
                            self.assertEqual('gui/501/com.apple.passd' in self.model(rig)['loaded'], prior is not True)
                            self.assertFalse(self.state(rig, 'original').exists())
                            self.assertEqual(rig.root('/private/var/db/piano-userland/receipt.tsv').read_text(), '')

    def test_logged_out_removal_retains_unit_until_retry(self):
        for shell in base.SHELLS:
            with self.subTest(shell=shell), tempfile.TemporaryDirectory() as d:
                rig = self.rig(Path(d), shell)
                self.assertEqual(rig.run()[0], 0)
                self.model(rig, gui=False, loaded=[])
                rig.files([])
                rc, out = rig.run('--live')
                self.assertEqual(rc, 1, out)
                self.assertTrue(rig.root('/usr/local/libexec/piano-passd-policy').is_file())
                self.assertTrue(rig.root('/Library/LaunchAgents/com.unixnu.piano-passd-policy.plist').is_file())
                self.assertEqual(len(rig.root('/private/var/db/piano-userland/receipt.tsv').read_text().splitlines()), 2)
                self.assertEqual(self.state(rig, 'original').read_text(), 'default\n')
                self.model(rig, gui=True)
                rc, out = rig.run('--live')
                self.assertEqual(rc, 0, out)
                self.assertFalse(rig.root('/usr/local/libexec/piano-passd-policy').exists())

    def test_failures_do_not_claim_success_or_lose_receipt(self):
        for failure in ('print-disabled', 'disable', 'bootout'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as d:
                rig = self.rig(Path(d), '/bin/sh')
                self.model(rig, fail=failure)
                rc, out = rig.run()
                self.assertEqual(rc, 1, out)
                self.assertIn('FAIL', self.state(rig, 'status').read_text())
                if failure != 'bootout':
                    self.assertNotIn('bootout gui/501/com.apple.passd', rig.calls)
        for failure in ('enable', 'bootstrap'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as d:
                rig = self.rig(Path(d), '/bin/sh')
                self.assertEqual(rig.run()[0], 0)
                self.model(rig, fail=failure)
                rig.files([])
                rc, out = rig.run('--live')
                self.assertEqual(rc, 1, out)
                self.assertTrue(self.state(rig, 'original').is_file())
                self.assertTrue(rig.root('/usr/local/libexec/piano-passd-policy').is_file())

    def test_removal_before_first_login_and_off_profile(self):
        for shell in base.SHELLS:
            with self.subTest(shell=shell), tempfile.TemporaryDirectory() as d:
                rig = self.rig(Path(d), shell, gui=False)
                self.assertEqual(rig.run()[0], 0)
                rig.files([])
                self.assertEqual(rig.run('--live')[0], 0)
                self.assertNotIn('com.apple.passd', self.model(rig)['overrides'])
            with self.subTest(shell=shell, off=True), tempfile.TemporaryDirectory() as d:
                rig = self.rig(Path(d), shell)
                self.assertEqual(rig.run()[0], 0)
                rig.files(self.units(on=False))
                rc, out = rig.run('--live')
                self.assertEqual(rc, 0, out)
                self.assertFalse(self.model(rig)['overrides']['com.apple.passd'])

    def test_first_install_receipt_write_failure_does_not_disable(self):
        with tempfile.TemporaryDirectory() as d:
            rig = self.rig(Path(d), '/bin/sh', prior=False)
            rig.root('/private/var/db/piano-userland/receipt.tsv.new').mkdir(parents=True)
            rc, out = rig.run('--live')
            self.assertEqual(rc, 1, out)
            self.assertIn('receipt.tsv.new', out)
            self.assertFalse(self.model(rig)['overrides']['com.apple.passd'])
            self.assertNotIn('disable gui/501/com.apple.passd', rig.calls)


if __name__ == '__main__':
    unittest.main()
