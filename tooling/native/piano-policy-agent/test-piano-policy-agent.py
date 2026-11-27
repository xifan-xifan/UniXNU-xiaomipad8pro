#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Host tests of piano-policy-agent's power state for background services (docs/policy.md section 4.1).

The real script runs under /bin/sh and /bin/dash against stub tools on PATH (launchctl, taskpolicy, notifyutil, pmset, sysctl, date,
stat, sleep, logger, ioreg) and a scratch root (PIANO_USERLAND_ROOT): a launchd model with jobs and pids, a clock (uptime) and the
notify states. Never calls the host's launchctl or taskpolicy. Every test also checks the two rules of the plan: no signal and no
persistent override (no launchctl enable/disable, kickstart, mdutil, kill) is ever sent.
"""
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
AGENT = HERE / 'piano-policy-agent.sh'
SHELLS = [s for s in ('/bin/sh', '/bin/dash') if Path(s).exists()]
BOOT = 1000000
LPM, HPM = 'com.apple.system.lowpowermode', 'com.apple.system.highpowermode'
TICK, SESSION = 'com.unixnu.piano.slim.tick', 'com.unixnu.piano.slim.session'
SRC = 'com.apple.system.powersources.source'
LID = 'com.apple.system.powermanagement.clamshellstate'
FORBIDDEN = ('launchctl enable', 'launchctl disable', 'launchctl kickstart', 'mdutil', 'kill ', 'launchctl remove')

LAUNCHCTL = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
m = pathlib.Path(os.environ['MODEL'])
s = json.loads((m / 'model.json').read_text())
a = sys.argv[1:]
with (m / 'calls').open('a') as f: f.write('launchctl ' + ' '.join(a) + '\n')
rc = 0
if a[0] == 'print':
    t = a[1]
    if t in s['domains']: print(t + ' = {'); print('}')
    elif t in s['jobs']:
        pid = s['jobs'][t]
        print(t + ' = {'); print('\tactive count = 1'); print('\tstate = ' + ('running' if pid else 'not running'))
        if pid: print('\tpid = %d' % pid)
        print('}')
    else: print('Could not find service "%s" in domain' % t.rsplit('/', 1)[-1], file=sys.stderr); rc = 113
elif a[0] == 'bootout':
    fail = s['fail'].get('bootout', {}).get(a[1].rsplit('/', 1)[-1])
    if fail: rc = fail
    elif a[1] in s['jobs']: del s['jobs'][a[1]]
    else: rc = 113
elif a[0] == 'bootstrap':
    label = pathlib.Path(a[2]).stem
    key = a[1] + '/' + label
    seq = s['fail'].get('bootstrap', {}).get(label, [])
    if not pathlib.Path(a[2]).is_file(): rc = 2
    elif seq: rc = seq.pop(0)
    elif key in s['jobs']: rc = 5
    elif a[1] not in s['domains']: rc = 125
    else:
        s['nextpid'] += 1; s['jobs'][key] = s['nextpid'] if label in s.get('runatload', []) else 0
else: rc = 0
(m / 'model.json').write_text(json.dumps(s))
sys.exit(rc)
'''
TASKPOLICY = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
m = pathlib.Path(os.environ['MODEL'])
s = json.loads((m / 'model.json').read_text())
a = sys.argv[1:]
with (m / 'calls').open('a') as f: f.write('taskpolicy ' + ' '.join(a) + '\n')
seq = s['fail'].get('taskpolicy', [])
rc = seq.pop(0) if seq else 0
if rc == 0 and a[:2] in (['-b', '-p'], ['-B', '-p']): s['bg'][a[2]] = a[0] == '-b'
(m / 'model.json').write_text(json.dumps(s))
sys.exit(rc)
'''
NOTIFYUTIL = r'''#!/usr/bin/env python3
import json, os, pathlib, sys, time
m = pathlib.Path(os.environ['MODEL'])
a = sys.argv[1:]
def model(): return json.loads((m / 'model.json').read_text())
def save(s): (m / 'model.json').write_text(json.dumps(s))
def log(t):
    with (m / 'calls').open('a') as f: f.write(t + '\n')
if a[0] == '-g':
    s = model()
    print('%s %s' % (a[1], s['notify'].get(a[1], 0)))
    sys.exit(s['fail'].get('notify', {}).get(a[1], 0))
elif a[0] == '-p': log('notifyutil -p ' + a[1])
elif a[0] == '-w':
    log('notifyutil ' + ' '.join(a))
    state = pathlib.Path(os.environ['PIANO_USERLAND_ROOT']) / 'private/var/db/piano-userland/slim/state'
    def seq():
        try: return int(dict(x.split('=', 1) for x in state.read_text().split()).get('seq', 0))
        except Exception: return -1
    for ev in model()['events']:
        s = model(); s.update(ev.get('set', {})); s['notify'].update(ev.get('notify', {})); save(s)
        for name in ('clock', 'source', 'ioreg'):
            if name in ev: (m / name).write_text(str(ev[name]))
        if 'key' not in ev: continue
        want = seq() + 1
        print(ev['key'], flush=True)
        if not ev.get('counted', True): continue
        deadline = time.monotonic() + 15
        while seq() < want:
            if time.monotonic() >= deadline: sys.exit('no state update for notification %s at seq %d' % (ev['key'], want))
            time.sleep(0.01)
        with (m / 'states').open('a') as f: f.write(state.read_text())
'''
SH_STUBS = {
    'sysctl': '''#!/bin/sh
case "$*" in
  "-n kern.boottime") echo "{ sec = %d, usec = 0 } Mon Jan 12 13:46:40 1970" ;;
  "-n kern.bootsessionuuid") cat "$MODEL/boot" ;;
  -w*) echo "sysctl $*" >> "$MODEL/calls" ;;
  *) exit 1 ;;
esac
''' % BOOT,
    'date': '''#!/bin/sh
if [ "$*" = "+%%s" ]; then echo $(( %d + $(cat "$MODEL/clock") )); else exec /bin/date "$@"; fi
''' % BOOT,
    'stat': '''#!/bin/sh
if [ "$*" = "-f %u /dev/console" ]; then cat "$MODEL/console"; else exec /usr/bin/stat "$@"; fi
''',
    'sleep': '''#!/bin/sh
echo "sleep $*" >> "$MODEL/calls"
''',
    'logger': '''#!/bin/sh
echo "$*" >> "$MODEL/logger"
''',
    'pmset': '''#!/bin/sh
case "$*" in
  "-g ps") echo "Now drawing from '$(cat "$MODEL/source")'"; echo " -InternalBattery-0 (id=1)	80%; charged; 0:00 remaining present: true" ;;
  *) echo "pmset $*" >> "$MODEL/calls" ;;
esac
''',
    'ioreg': '''#!/bin/sh
cat "$MODEL/ioreg"
''',
}
ENTRIES = '''# MODE DOMAIN LABEL PROCESS
bg gui com.apple.knowledge-agent knowledge-agent
unload gui com.apple.tipsd tipsd
unload system com.apple.msrpc.wkssvc wkssvc
bg system com.apple.watchdogd watchdogd
unload gui com.apple.routined routined
'''


class Rig:
    def __init__(self, tmp, shell):
        self.tmp, self.shell = tmp, shell
        self.root, self.bin, self.m = tmp / 'root', tmp / 'bin', tmp / 'model'
        for d in (self.bin, self.m, self.root / 'System/Library/LaunchAgents', self.root / 'System/Library/LaunchDaemons',
                  self.root / 'Library/Logs/DiagnosticReports', self.root / 'Users/u/Library/Logs/DiagnosticReports'):
            d.mkdir(parents=True, exist_ok=True)
        (self.bin / 'python3').symlink_to(sys.executable)
        for name, text in (('launchctl', LAUNCHCTL), ('taskpolicy', TASKPOLICY), ('notifyutil', NOTIFYUTIL), *SH_STUBS.items()):
            (self.bin / name).write_text(text); (self.bin / name).chmod(0o755)
        for label in ('com.apple.knowledge-agent', 'com.apple.tipsd'):
            (self.root / 'System/Library/LaunchAgents' / (label + '.plist')).write_text('<plist/>')
        for label in ('com.apple.msrpc.wkssvc', 'com.apple.watchdogd'):
            (self.root / 'System/Library/LaunchDaemons' / (label + '.plist')).write_text('<plist/>')
        self.model = {'domains': ['system', 'gui/501'], 'nextpid': 900, 'runatload': ['com.apple.knowledge-agent'],
                      'jobs': {'gui/501/com.apple.knowledge-agent': 700, 'gui/501/com.apple.tipsd': 0,
                               'system/com.apple.msrpc.wkssvc': 0, 'system/com.apple.watchdogd': 50},
                      'notify': {LPM: 0, HPM: 0}, 'fail': {}, 'bg': {}, 'events': []}
        self.save()
        for k, v in (('clock', '200'), ('boot', '0A1B2C3D-0001'), ('console', '501'), ('source', 'AC Power'), ('calls', ''),
                     ('ioreg', '')):
            (self.m / k).write_text(v)
        self.env = {'PATH': '%s:/usr/bin:/bin:/usr/sbin:/sbin' % self.bin, 'PIANO_USERLAND_ROOT': str(self.root),
                    'MODEL': str(self.m), 'LC_ALL': 'C', 'HOME': str(tmp), 'TMPDIR': str(tmp)}
        self.slim = self.root / 'private/var/db/piano-userland/slim'

    def save(self): (self.m / 'model.json').write_text(json.dumps(self.model))
    def load(self): self.model = json.loads((self.m / 'model.json').read_text()); return self.model
    def put(self, k, v): (self.m / k).write_text(str(v))
    def notify(self, **kv):
        self.load(); self.model['notify'].update({LPM if k == 'lpm' else HPM: v for k, v in kv.items()}); self.save()

    def run(self, *args, clock=None, expected=0, timeout=60):
        if clock is not None: self.put('clock', clock)
        r = subprocess.run([self.shell, str(AGENT), *args], env=self.env, capture_output=True, text=True, timeout=timeout)
        if r.returncode != expected:
            raise AssertionError('agent %s rc %d (expected %d): %s%s' % (args, r.returncode, expected, r.stdout, r.stderr))
        return r

    def event(self, name, clock=None): return self.run('--event', name, clock=clock)
    def calls(self): return (self.m / 'calls').read_text()
    def clear(self): self.put('calls', '')
    def log(self): return (self.slim / 'log').read_text() if (self.slim / 'log').exists() else ''
    def applied(self): return [l.split() for l in (self.slim / 'applied').read_text().splitlines() if l.strip()] if (self.slim / 'applied').exists() else []
    def state(self): return dict(x.split('=', 1) for x in (self.slim / 'state').read_text().split())
    def tests_on(self): (self.slim / 'test-pause').write_text(ENTRIES)

    def wait_call(self, text, t=5):
        end = time.time() + t
        while time.time() < end:
            if text in self.calls(): return True
            time.sleep(0.05)
        return text in self.calls()

    def into_save(self):   # Low Power Mode on at uptime 200, the tick 30 s later: Save with the test list
        self.run('--start'); self.tests_on()
        self.notify(lpm=1); self.event('lpm', clock=200)
        assert self.state()['state'] == 'pending', self.state()
        self.event('tick', clock=230)
        assert self.state()['state'] == 'save', self.state()


class AgentTests(unittest.TestCase):
    def each(self, fn):
        for shell in SHELLS:
            d = tempfile.mkdtemp()
            try:
                with self.subTest(shell=shell):
                    rig = Rig(Path(d), shell)
                    fn(rig)
                    for bad in FORBIDDEN:
                        self.assertNotIn(bad, rig.calls(), 'a forbidden call (%s) in %s' % (bad, rig.calls()))
            finally:
                for _ in range(50):   # a scheduled tick's sleeping child may still write its post
                    try: shutil.rmtree(d); break
                    except OSError: time.sleep(0.1)

    def test_empty_list_save_and_normal(self):
        def t(rig):
            rig.run('--start')
            self.assertEqual(rig.state()['state'], 'normal')
            self.assertFalse((rig.slim / 'test-pause').exists())
            rig.notify(lpm=1)
            rig.event('lpm', clock=200)
            self.assertIn('sysctl -w machdep.piano_policy_lpm=1', rig.calls())
            s = rig.state(); self.assertEqual((s['state'], s['since'], s['want'], s['lpm']), ('pending', '200', 'save', '1'))
            self.assertTrue(rig.wait_call('notifyutil -p ' + TICK))
            self.assertIn('sleep 30', rig.calls())
            rig.event('tick', clock=215)
            self.assertEqual(rig.state()['state'], 'pending')
            rig.event('tick', clock=230)
            self.assertEqual(rig.state()['state'], 'save')
            self.assertEqual(rig.applied(), [])
            self.assertNotIn('taskpolicy', rig.calls()); self.assertNotIn('bootout', rig.calls())
            rig.notify(lpm=0)
            rig.event('lpm', clock=240)
            self.assertEqual(rig.state()['state'], 'normal')
            self.assertIn('slim: Normal (event lpm', rig.log())
            self.assertTrue((rig.slim.stat().st_mode & 0o777) == 0o700 and (rig.slim.parent.stat().st_mode & 0o777) == 0o711)
        self.each(t)

    def test_test_list_actions_and_class_a_guard(self):
        def t(rig):
            rig.into_save()
            c = rig.calls()
            self.assertIn('taskpolicy -b -p 700', c)
            self.assertIn('launchctl bootout gui/501/com.apple.tipsd', c)
            self.assertIn('launchctl bootout system/com.apple.msrpc.wkssvc', c)
            self.assertNotIn('taskpolicy -b -p 50', c)
            self.assertIn('entry refused (test: bg system com.apple.watchdogd): class A', rig.log())
            self.assertIn('entry refused (test: unload gui com.apple.routined): class A', rig.log())
            self.assertNotIn('com.apple.routined', rig.calls())
            self.assertEqual(sorted(a[3] for a in rig.applied()), ['com.apple.knowledge-agent', 'com.apple.msrpc.wkssvc', 'com.apple.tipsd'])
            self.assertTrue(all(a[0] == '0A1B2C3D-0001' for a in rig.applied()))
            m = rig.load()
            self.assertTrue(m['bg']['700']); self.assertNotIn('gui/501/com.apple.tipsd', m['jobs'])
            rig.clear(); rig.event('tick', clock=240)   # Save again: idempotent
            self.assertNotIn('taskpolicy', rig.calls()); self.assertNotIn('bootout', rig.calls())
            rig.notify(lpm=0); rig.event('lpm', clock=250)
            c = rig.calls()
            self.assertIn('taskpolicy -B -p 700', c)
            self.assertIn('launchctl bootstrap gui/501 %s/System/Library/LaunchAgents/com.apple.tipsd.plist' % rig.root, c)
            self.assertIn('launchctl bootstrap system %s/System/Library/LaunchDaemons/com.apple.msrpc.wkssvc.plist' % rig.root, c)
            m = rig.load()
            self.assertFalse(m['bg']['700']); self.assertIn('gui/501/com.apple.tipsd', m['jobs']); self.assertIn('system/com.apple.msrpc.wkssvc', m['jobs'])
            self.assertEqual(rig.applied(), []); self.assertEqual(rig.state()['state'], 'normal')
            (rig.slim / 'test-pause').unlink()   # the flag gone: no entry at all
            rig.clear(); rig.notify(lpm=1); rig.event('lpm', clock=300); rig.event('tick', clock=330)
            self.assertEqual(rig.state()['state'], 'save'); self.assertEqual(rig.applied(), [])
            self.assertNotIn('taskpolicy', rig.calls()); self.assertNotIn('bootout', rig.calls())
        self.each(t)

    def test_debounce_battery_and_high_power(self):
        def t(rig):
            rig.run('--start'); rig.tests_on()
            rig.put('source', 'Battery Power'); rig.event('source', clock=200)
            self.assertEqual(rig.state()['state'], 'pending')
            rig.put('source', 'AC Power'); rig.event('source', clock=210)
            self.assertEqual(rig.state()['state'], 'normal'); self.assertIn('Save cancelled after 10 s', rig.log())
            rig.put('source', 'Battery Power'); rig.event('source', clock=215)
            rig.event('tick', clock=230)   # the first flap's tick: only 15 s stable
            self.assertEqual(rig.state()['state'], 'pending'); self.assertEqual(rig.applied(), [])
            self.assertNotIn('taskpolicy', rig.calls())
            rig.event('tick', clock=245)
            self.assertEqual(rig.state()['state'], 'save'); self.assertEqual(len(rig.applied()), 3)
            rig.notify(hpm=1); rig.event('hpm', clock=250)   # High Power on battery: Normal at once
            self.assertEqual(rig.state()['state'], 'normal'); self.assertEqual(rig.applied(), [])
            self.assertIn('sysctl -w machdep.piano_policy_hpm=1', rig.calls())
            rig.notify(lpm=1); rig.event('lpm', clock=260)   # Low Power Mode wins over High Power
            self.assertEqual(rig.state()['want'], 'save')
            rig.notify(lpm=0); rig.event('lpm', clock=262)
            rig.put('source', 'UPS Power'); rig.notify(hpm=0); rig.event('source', clock=270)
            self.assertEqual(rig.state()['want'], 'normal')
        self.each(t)

    def test_min_uptime(self):
        def t(rig):
            rig.run('--start', clock=20); rig.tests_on()
            rig.notify(lpm=1); rig.event('lpm', clock=20)
            self.assertTrue(rig.wait_call('sleep 100'))
            rig.event('tick', clock=60)
            self.assertEqual(rig.state()['state'], 'pending')
            rig.event('tick', clock=119)
            self.assertEqual(rig.state()['state'], 'pending'); self.assertNotIn('taskpolicy', rig.calls())
            rig.event('tick', clock=120)
            self.assertEqual(rig.state()['state'], 'save'); self.assertIn('taskpolicy -b -p 700', rig.calls())
        self.each(t)

    def test_rapid_energy_modes(self):
        def t(rig):
            rig.run('--start')
            # One notification can arrive after both flags have changed.
            for lpm, hpm, event, writes in (
                (1, 0, 'hpm', ('lpm=1', 'hpm=0')),
                (0, 1, 'lpm', ('hpm=1', 'lpm=0')),
                (1, 1, 'hpm', ('lpm=1', 'hpm=1')),
                (0, 0, 'hpm', ('hpm=0', 'lpm=0')),
                (2, -1, 'lpm', ('hpm=0', 'lpm=0')),
            ):
                rig.clear()
                rig.notify(lpm=lpm, hpm=hpm)
                rig.event(event)
                self.assertEqual(
                    [call for call in rig.calls().splitlines() if call.startswith('sysctl -w ')],
                    ['sysctl -w machdep.piano_policy_' + write for write in writes],
                )
        self.each(t)

    def test_lid_requires_explicit_lock_only(self):
        def t(rig):
            for state, sleeps, expected in (
                ('Yes', 'Yes', False), ('No', 'No', False), ('Yes', '', False),
                ('Yes', 'Unknown', False), ('Yes', 'No', True), ('No', 'No', False),
            ):
                rig.clear()
                registry = '    "AppleClamshellState" = ' + state + '\n'
                if sleeps:
                    registry += '    "AppleClamshellCausesSleep" = ' + sleeps + '\n'
                rig.put('ioreg', registry)
                rig.event('lid')
                self.assertEqual('pmset displaysleepnow' in rig.calls(), expected)
        self.each(t)

    def test_energy_read_failure(self):
        def t(rig):
            rig.into_save()
            before = rig.state()
            rig.notify(lpm=0, hpm=1)
            rig.load(); rig.model['fail']['notify'] = {LPM: 1}; rig.save()
            rig.clear(); rig.event('hpm', clock=240)
            self.assertNotIn('sysctl -w ', rig.calls())
            self.assertNotIn('taskpolicy -B', rig.calls())
            self.assertEqual(rig.state(), before)
            self.assertFalse((rig.slim / 'lock').exists())

            # The kill switch still restores jobs while the mode read fails.
            (rig.slim / 'off').touch()
            rig.event('tick', clock=241)
            self.assertEqual(rig.state()['state'], 'off')
            self.assertEqual(rig.applied(), [])
            (rig.slim / 'off').unlink()

            rig.load(); rig.model['fail'].clear(); rig.save()
            rig.event('hpm', clock=242)
            self.assertIn('sysctl -w machdep.piano_policy_hpm=1', rig.calls())
            self.assertIn('sysctl -w machdep.piano_policy_lpm=0', rig.calls())
            self.assertEqual(rig.state()['state'], 'normal')
        self.each(t)

    def test_test_list_without_final_newline(self):
        def t(rig):
            rig.run('--start')
            (rig.slim / 'test-pause').write_text(
                'bg system com.apple.watchdogd watchdogd extra\n'
                'bg gui com.apple.knowledge-agent knowledge-agent'
            )
            rig.notify(lpm=1); rig.event('lpm', clock=200); rig.event('tick', clock=230)
            self.assertEqual([a[3] for a in rig.applied()], ['com.apple.knowledge-agent'])
            self.assertIn('taskpolicy -b -p 700', rig.calls())
            self.assertNotIn('taskpolicy -b -p 50', rig.calls())
        self.each(t)

    def test_corrupt_state_numbers(self):
        def t(rig):
            rig.run('--start'); rig.tests_on()
            rig.notify(lpm=1)
            state = rig.slim / 'state'
            state.write_text('state=pending gen=0008 since=0200 seq=0008 due=0230\n')
            rig.event('tick', clock=215)
            s = rig.state()
            self.assertEqual((s['state'], s['gen'], s['since'], s['seq'], s['due']), ('pending', '8', '200', '9', '230'))
            self.assertEqual(rig.applied(), [])
            self.assertFalse((rig.slim / 'lock').exists())

            state.write_text('state=pending gen=8 since=18446744073709551616 seq=9 due=230\n')
            rig.event('tick', clock=250)
            self.assertEqual((rig.state()['state'], rig.state()['since']), ('pending', '250'))
            self.assertEqual(rig.applied(), [])
            rig.event('tick', clock=280)
            self.assertEqual(rig.state()['state'], 'save')
            self.assertEqual(len(rig.applied()), 3)
        self.each(t)

    def test_state_write_failure_releases_lock(self):
        def t(rig):
            rig.run('--start')
            before = rig.state()
            pending_file = rig.slim / 'state.new'
            pending_file.mkdir()
            rig.run('--start', expected=1)
            self.assertEqual(rig.state(), before)
            self.assertFalse((rig.slim / 'lock').exists())
            pending_file.rmdir()
            rig.run('--start')
            self.assertEqual(int(rig.state()['gen']), int(before['gen']) + 1)
        self.each(t)

    def test_unavailable_boot_id_keeps_restore_records(self):
        def t(rig):
            rig.into_save()
            before = rig.applied()
            rig.put('boot', '')
            rig.clear(); rig.run('--restore', expected=1)
            self.assertEqual(rig.applied(), before)
            self.assertNotIn('taskpolicy -B', rig.calls())
            self.assertNotIn('launchctl bootstrap', rig.calls())
            self.assertFalse((rig.slim / 'lock').exists())

            rig.put('boot', before[0][0])
            rig.run('--restore')
            self.assertEqual(rig.applied(), [])
            self.assertIn('taskpolicy -B -p 700', rig.calls())
            self.assertIn('launchctl bootstrap system', rig.calls())
        self.each(t)

    def test_apply_requires_writable_ledger(self):
        def t(rig):
            rig.run('--start'); rig.tests_on()
            rig.notify(lpm=1); rig.event('lpm', clock=200)
            ledger = rig.slim / 'applied'
            ledger.mkdir()
            rig.clear(); rig.run('--event', 'tick', clock=230, expected=1)
            self.assertNotIn('taskpolicy', rig.calls())
            self.assertNotIn('launchctl bootout', rig.calls())
            self.assertEqual(rig.state()['state'], 'pending')
            self.assertFalse((rig.slim / 'lock').exists())
            ledger.rmdir()
            rig.event('tick', clock=231)
            self.assertEqual(rig.state()['state'], 'save')
            self.assertEqual(len(rig.applied()), 3)
        self.each(t)

    def test_restart_restores_first(self):
        def t(rig):
            rig.into_save()
            rig.clear(); rig.run('--start', clock=300)
            c = rig.calls()
            self.assertIn('taskpolicy -B -p 700', c)
            self.assertIn('launchctl bootstrap gui/501', c); self.assertIn('launchctl bootstrap system', c)
            self.assertEqual(rig.applied(), []); self.assertEqual(rig.state()['state'], 'normal')
            self.assertIn('restore first (3 action(s) of this boot undone', rig.log())
            rig.event('tick', clock=301)   # Low Power Mode still on: pending again, Save after the debounce
            self.assertEqual(rig.state()['state'], 'pending')
            rig.event('tick', clock=331)
            self.assertEqual(rig.state()['state'], 'save'); self.assertEqual(len(rig.applied()), 3)
        self.each(t)

    def test_reboot_starts_normal(self):
        def t(rig):
            rig.into_save()
            m = rig.load()   # a new boot: launchd loaded every job again, new pids, the record is the old boot's
            m['jobs'] = {'gui/501/com.apple.knowledge-agent': 701, 'gui/501/com.apple.tipsd': 0, 'system/com.apple.msrpc.wkssvc': 0,
                         'system/com.apple.watchdogd': 51}
            rig.model = m; rig.save(); rig.put('boot', '0A1B2C3D-0002'); rig.clear()
            rig.run('--start', clock=15)
            self.assertNotIn('taskpolicy', rig.calls()); self.assertNotIn('bootstrap', rig.calls())
            self.assertIn('recorded on another boot: dropped', rig.log())
            self.assertEqual(rig.applied(), []); self.assertEqual(rig.state()['state'], 'normal')
        self.each(t)

    def test_broken_after_two_failures(self):
        def t(rig):
            rig.load(); rig.model['fail'] = {'taskpolicy': [1, 1]}; rig.save()
            rig.into_save()
            self.assertIn('FAIL com.apple.knowledge-agent: taskpolicy -b -p 700 rc 1 (failure 1 of 2)', rig.log())
            rig.event('tick', clock=240)
            self.assertIn('BROKEN com.apple.knowledge-agent', rig.log())
            self.assertIn('com.apple.knowledge-agent', (rig.slim / 'broken.tsv').read_text())
            rig.clear(); rig.event('tick', clock=250)
            self.assertNotIn('taskpolicy', rig.calls())
            # a reload that fails twice: broken at once, the record dropped (it loads at the next login)
            rig.load(); rig.model['fail'] = {'bootstrap': {'com.apple.tipsd': [5, 5]}}; rig.save()
            rig.notify(lpm=0); rig.event('lpm', clock=260)
            self.assertIn('BROKEN com.apple.tipsd: launchctl bootstrap gui/501 failed twice', rig.log())
            self.assertIn('sleep 2', rig.calls())
            self.assertEqual(rig.applied(), [])
            rig.clear(); rig.notify(lpm=1); rig.event('lpm', clock=300); rig.event('tick', clock=330)
            c = rig.calls()
            self.assertNotIn('com.apple.tipsd', c); self.assertNotIn('taskpolicy', c)
            self.assertIn('launchctl bootout system/com.apple.msrpc.wkssvc', c)
        self.each(t)

    def test_crash_reports_count(self):
        def t(rig):
            rig.into_save()
            w = rig.slim / 'watch/com.apple.msrpc.wkssvc'
            m0 = w.stat().st_mtime
            def crash(d, name, at):
                f = rig.root / d / name; f.write_text('{}'); os.utime(f, (at, at))
            crash('Library/Logs/DiagnosticReports', 'wkssvc-2026-11-22-120001.ips', m0 + 2)
            crash('Library/Logs/DiagnosticReports', 'wkssvcx-2026-11-22-120001.ips', m0 + 2)   # another process: not counted
            rig.event('tick', clock=240)
            self.assertIn('FAIL com.apple.msrpc.wkssvc: 1 new crash report(s) of wkssvc (failure 1 of 2)', rig.log())
            os.utime(w, (m0 + 3, m0 + 3))   # time moves past the first report
            rig.event('tick', clock=245)
            self.assertNotIn('failure 2 of 2', rig.log())   # the same report is not counted twice
            crash('Users/u/Library/Logs/DiagnosticReports', 'wkssvc-2026-11-22-120002.ips', m0 + 5)
            rig.event('tick', clock=250)
            self.assertIn('BROKEN com.apple.msrpc.wkssvc', rig.log())
            self.assertIn('system/com.apple.msrpc.wkssvc', rig.load()['jobs'])   # restored at once
            self.assertEqual(sorted(a[3] for a in rig.applied()), ['com.apple.knowledge-agent', 'com.apple.tipsd'])
        self.each(t)

    def test_crash_window_ends(self):
        def t(rig):
            rig.into_save()
            mark = (rig.slim / 'watch/com.apple.msrpc.wkssvc').stat().st_mtime
            f = rig.root / 'Library/Logs/DiagnosticReports/wkssvc-2026-11-22-130000.ips'
            f.write_text('{}'); os.utime(f, (mark + 5, mark + 5))
            rig.event('tick', clock=230 + 601)
            self.assertNotIn('crash report', rig.log())
            self.assertFalse((rig.slim / 'watch/com.apple.msrpc.wkssvc').exists())
        self.each(t)

    def test_kill_switch(self):
        def t(rig):
            rig.into_save()
            (rig.slim / 'off').write_text('')
            rig.event('tick', clock=240)
            self.assertEqual(rig.state()['state'], 'off'); self.assertEqual(rig.applied(), [])
            self.assertIn('kill switch', rig.log()); self.assertIn('taskpolicy -B -p 700', rig.calls())
            rig.clear(); rig.event('lpm', clock=280); rig.event('tick', clock=320)
            self.assertEqual(rig.state()['state'], 'off')
            self.assertNotIn('taskpolicy', rig.calls()); self.assertNotIn('bootout', rig.calls())
            (rig.slim / 'off').unlink(); rig.event('tick', clock=330)
            self.assertEqual(rig.state()['state'], 'pending')
            rig.event('tick', clock=360); self.assertEqual(len(rig.applied()), 3)
            rig.clear(); rig.run('--restore', clock=370)
            self.assertIn('taskpolicy -B -p 700', rig.calls()); self.assertEqual(rig.applied(), [])
            self.assertEqual(rig.state()['state'], 'normal')
        self.each(t)

    def test_session_event(self):
        def t(rig):
            rig.put('console', '0')   # the login window: gui entries wait
            rig.into_save()
            self.assertEqual([a[3] for a in rig.applied()], ['com.apple.msrpc.wkssvc'])
            self.assertIn('save com.apple.knowledge-agent: no console session', rig.log())
            rig.put('console', '501'); rig.event('session', clock=240)
            self.assertEqual(sorted(a[3] for a in rig.applied()), ['com.apple.knowledge-agent', 'com.apple.msrpc.wkssvc', 'com.apple.tipsd'])
            # log out and in: launchd loads the agents again in the new session, knowledge-agent with a new pid
            m = rig.load(); m['jobs']['gui/501/com.apple.tipsd'] = 0; m['jobs']['gui/501/com.apple.knowledge-agent'] = 800
            rig.model = m; rig.save(); rig.clear()
            rig.event('session', clock=300)
            c = rig.calls()
            self.assertIn('taskpolicy -b -p 800', c); self.assertIn('launchctl bootout gui/501/com.apple.tipsd', c)
            self.assertIn('loaded again (a new session', rig.log()); self.assertIn('pid 700 gone: record dropped', rig.log())
            self.assertEqual(len(rig.applied()), 3)
        self.each(t)

    def test_daemon_loop(self):
        def t(rig):
            (rig.slim.parent).mkdir(parents=True, exist_ok=True); rig.slim.mkdir(); rig.tests_on()
            rig.load()
            rig.model['events'] = [{'notify': {LPM: 1}, 'clock': 200, 'key': LPM}, {'clock': 231, 'key': TICK},
                                   {'clock': 240, 'key': SESSION}, {'notify': {LPM: 0}, 'clock': 250, 'key': LPM},
                                   {'clock': 260, 'key': LID, 'counted': False},
                                   {'clock': 261, 'key': SRC}]
            rig.save()
            rig.put('ioreg', '    "AppleClamshellState" = Yes\n    "AppleClamshellCausesSleep" = No\n')
            r = subprocess.run([rig.shell, str(AGENT)], env=rig.env, capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            c = rig.calls()
            self.assertIn('notifyutil -w %s -w %s -w com.apple.system.powermanagement.clamshellstate -w %s -w %s -w %s' % (LPM, HPM, SRC, SESSION, TICK), c)
            self.assertIn('taskpolicy -b -p 700', c); self.assertIn('taskpolicy -B -p 700', c)
            self.assertIn('pmset displaysleepnow', c)
            self.assertIn('sysctl -w machdep.piano_policy_lpm=1', c); self.assertIn('sysctl -w machdep.piano_policy_lpm=0', c)
            s = rig.state(); self.assertEqual((s['state'], s['event']), ('normal', 'source'))
            lg = (rig.m / 'logger').read_text()
            self.assertIn('slim: start: Normal', lg); self.assertIn('notifyutil ended: exiting', lg)
        self.each(t)

    def test_daemon_power_churn(self):
        def t(rig):
            rig.slim.mkdir(parents=True)
            (rig.slim / 'test-pause').write_text('bg gui com.apple.knowledge-agent knowledge-agent\n')
            events = rig.model['events']
            names = {SRC: 'source', LPM: 'lpm', HPM: 'hpm', TICK: 'tick'}
            sources = {'ac': 'AC Power', 'battery': 'Battery Power'}
            mode_write = 'sysctl -w machdep.piano_policy_'
            expected_calls = [mode_write + 'hpm=0', mode_write + 'lpm=0']
            expected_states = []

            def add(clock, key, source, lpm, hpm, state, lid=None, since=None):
                if lid is not None:
                    closed, sleeps = lid
                    registry = ('    "AppleClamshellState" = %s\n    "AppleClamshellCausesSleep" = %s\n' % (closed, sleeps))
                    # Lid does not store state. The following power event acknowledges both notifications in FIFO order.
                    events.append({'key': LID, 'counted': False, 'ioreg': registry, 'clock': clock})
                    if lid == ('Yes', 'No'):
                        expected_calls.append('pmset displaysleepnow')
                events.append({'key': key, 'clock': clock, 'source': sources[source], 'notify': {LPM: lpm, HPM: hpm}})
                if key in (LPM, HPM):
                    writes = ('lpm=%d' % lpm, 'hpm=%d' % hpm)
                    expected_calls.extend(mode_write + write for write in (writes if lpm else reversed(writes)))
                expected_states.append(dict(
                    state=state, want='normal' if state == 'normal' else 'save', event=names[key], source=source,
                    lpm=str(lpm), hpm=str(hpm), seq=str(2 + len(expected_states)), up=str(clock), applied=str(int(state == 'save')),
                    since=None if since is None else str(since),
                ))

            for start in range(200, 296, 8):
                add(start,     SRC, 'battery', 0, 0, 'pending', ('No', 'Yes'), since=start)
                add(start + 1, SRC, 'ac',      0, 0, 'normal',  ('Yes', 'No'))
                add(start + 2, HPM, 'ac',     1, 0, 'pending', ('No', 'No'), since=start + 2)
                add(start + 3, TICK, 'ac',    1, 0, 'pending', ('Yes', 'No'), since=start + 2)
                add(start + 4, LPM, 'battery', 0, 1, 'normal', ('Yes', 'Yes'))
                add(start + 5, HPM, 'battery', 1, 1, 'pending', ('No', 'Yes'), since=start + 5)
                add(start + 6, LPM, 'ac',     0, 1, 'normal',  ('Yes', 'No'))
                add(start + 7, HPM, 'ac',     0, 0, 'normal',  ('No', 'No'))
            add(296, SRC, 'battery', 0, 0, 'pending', since=296)
            add(325, TICK, 'battery', 0, 0, 'pending', since=296)
            add(326, TICK, 'battery', 0, 0, 'save', since=296)
            expected_calls.append('taskpolicy -b -p 700')
            add(327, LPM, 'battery', 0, 1, 'normal')
            expected_calls.append('taskpolicy -B -p 700')
            rig.save()

            r = rig.run(timeout=120)
            self.assertEqual(r.stderr, '')
            lines = (rig.m / 'states').read_text().splitlines()
            self.assertEqual(len(lines), len(expected_states))
            for line, expected in zip(lines, expected_states):
                state = dict(x.split('=', 1) for x in line.split())
                with self.subTest(seq=expected['seq'], event=expected['event']):
                    fields = [key for key in expected if expected[key] is not None]
                    self.assertEqual({key: state[key] for key in fields}, {key: expected[key] for key in fields})
            calls = rig.calls().splitlines()
            self.assertEqual([call for call in calls if call.startswith(('sysctl -w ', 'pmset ', 'taskpolicy ',
                                                                         'launchctl bootout ', 'launchctl bootstrap '))],
                             expected_calls)
            self.assertEqual(rig.applied(), [])
            self.assertFalse(rig.load()['bg']['700'])
        self.each(t)

    def test_status_and_usage(self):
        def t(rig):
            rig.into_save()
            r = rig.run('--status', '5')
            self.assertIn('== state\nstate=save', r.stdout); self.assertIn('== applied', r.stdout)
            r = subprocess.run([rig.shell, str(AGENT), '--bogus'], env=rig.env, capture_output=True, text=True, timeout=20)
            self.assertEqual(r.returncode, 2)
        self.each(t)


if __name__ == '__main__':
    unittest.main()
