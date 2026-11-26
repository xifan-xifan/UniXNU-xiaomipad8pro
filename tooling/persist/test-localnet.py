#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Host test for the Local Network preflight (localnet.py) and its use in maint-session.py (LT-D6, 2026-11-25).
Mocks: the route/arp/nc commands and the socket; maint-session.py runs in-process with a nonexistent step, so a preflight that did
not stop could reach nothing but the "unknown step" stop. LOCALNET_LIVE=1 adds the real probe of this process (prints its state)."""
import contextlib
import errno
import importlib.util
import io
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import localnet  # noqa: E402

spec = importlib.util.spec_from_file_location('ms', here / 'maint-session.py')
ms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ms)

ROUTE = '   route to: default\ndestination: default\n       mask: default\n    gateway: 192.0.2.2\n  interface: en1\n'


def runner(gateway=True, arp=False, nc=0):
    def run(cmd, timeout=5):
        if cmd[0] == '/sbin/route':
            return subprocess.CompletedProcess(cmd, 0 if gateway else 1, ROUTE if gateway else 'route: not in table\n', '')
        if cmd[0] == '/usr/sbin/arp':
            out = f'? ({cmd[-1]}) at 2:50:49:41:4e:1 on en7 ifscope [ethernet]\n' if arp else f'{cmd[-1]} ({cmd[-1]}) -- no entry\n'
            return subprocess.CompletedProcess(cmd, 0, out, '')
        if cmd[0] == '/usr/bin/nc':
            return subprocess.CompletedProcess(cmd, nc, '', '')
        raise AssertionError(cmd)
    return run


class Sock:
    """socket.socket stand-in: sendto raises ERR (an errno) or records the datagram."""
    sent = []

    def __init__(self, err):
        self.err = err

    def __call__(self, *a):
        return self

    def sendto(self, data, addr):
        if self.err:
            raise OSError(self.err, os.strerror(self.err))
        Sock.sent.append(addr)

    def close(self):
        pass


class Preflight(unittest.TestCase):
    def test_denied_by_privacy(self):
        state, detail = localnet.preflight(None, env={}, run=runner(), sock=Sock(errno.EHOSTUNREACH))
        self.assertEqual(state, 'denied')
        self.assertIn('open -a Terminal', detail)
        self.assertIn('errno 65', detail)

    def test_allowed(self):
        Sock.sent = []
        state, _ = localnet.preflight(None, env={'PIANO_MAINT_HOST': '192.0.2.3'}, run=runner(), sock=Sock(0))
        self.assertEqual(state, 'allowed')
        self.assertEqual(Sock.sent, [('192.0.2.2', 9)])   # the gateway first; one datagram to the discard port

    def test_maint_host_reachable_by_nc(self):
        state, _ = localnet.preflight(None, env={'PIANO_MAINT_HOST': '192.0.2.3'}, run=runner(gateway=False, nc=0),
                                      sock=Sock(errno.EHOSTUNREACH))
        self.assertEqual(state, 'denied')

    def test_unknown_without_target_or_proof(self):
        self.assertEqual(localnet.preflight(None, env={}, run=runner(gateway=False), sock=Sock(errno.EHOSTUNREACH))[0], 'unknown')
        self.assertEqual(localnet.preflight(None, env={'PIANO_MAINT_HOST': '192.0.2.3'}, run=runner(gateway=False, nc=1),
                                            sock=Sock(errno.EHOSTUNREACH))[0], 'unknown')
        self.assertEqual(localnet.preflight(None, env={}, run=runner(), sock=Sock(errno.ENETUNREACH))[0], 'unknown')

    def test_connect_error_classification(self):
        e65 = OSError(errno.EHOSTUNREACH, 'No route to host')
        self.assertIn('open -a Terminal', localnet.classify_connect_error('192.0.2.3', 2323, e65, run=runner(nc=0)))
        self.assertIsNone(localnet.classify_connect_error('192.0.2.3', 2323, e65, run=runner(nc=1)))
        self.assertIsNone(localnet.classify_connect_error('192.0.2.3', 2323, ConnectionRefusedError(61, 'refused'), run=runner()))

    @unittest.skipUnless(os.environ.get('LOCALNET_LIVE') == '1', 'LOCALNET_LIVE=1: the real probe of this process')
    def test_live(self):
        state, detail = localnet.preflight()
        print(f'\nlive: {state}: {detail}')
        self.assertIn(state, ('allowed', 'denied', 'unknown'))


class MaintSession(unittest.TestCase):
    def main(self, verdict, *args, env=None):
        with tempfile.TemporaryDirectory() as d, \
             mock.patch.object(ms.localnet, 'preflight', return_value=verdict) as pf, \
             mock.patch.dict(os.environ, env or {}), \
             mock.patch.object(sys, 'argv', ['maint-session.py', '--session', d, *args, 'no-such-step']), \
             contextlib.redirect_stdout(io.StringIO()) as out:
            try:
                ms.main()
                rc = 0
            except SystemExit as e:
                rc = e.code
            return rc, out.getvalue(), pf.called

    def test_real_run_refuses_before_any_step(self):
        rc, out, _ = self.main(('denied', 'errno 65 ... ' + localnet.DENIED))
        self.assertEqual(rc, 3)
        self.assertIn('STOPPED: errno 65', out)
        self.assertIn('open -a Terminal', out)
        self.assertNotIn('unknown step', out)      # stopped before the steps

    def test_dry_run_reports_and_goes_on(self):
        rc, out, _ = self.main(('denied', localnet.DENIED), '--dry-run')
        self.assertIn('a real run from this shell would stop here', out)
        self.assertIn('unknown step no-such-step', out)

    def test_allowed_goes_on(self):
        rc, out, _ = self.main(('allowed', 'a UDP datagram to the default gateway left this process'))
        self.assertIn('Local Network preflight: allowed', out)
        self.assertIn('unknown step no-such-step', out)

    def test_switch_off(self):
        rc, out, called = self.main(('denied', 'x'), env={'PIANO_LOCALNET_PREFLIGHT': 'off'})
        self.assertFalse(called)
        self.assertIn('preflight skipped', out)

    def test_wait_shell_stops_at_once_on_privacy_errno(self):
        fake = mock.Mock(execute=True, env={'PIANO_MAINT_HOST': '192.0.2.3'})
        e65 = OSError(errno.EHOSTUNREACH, 'No route to host')
        with mock.patch.object(ms.socket, 'create_connection', side_effect=e65), \
             mock.patch.object(ms.localnet, 'nc_reaches', return_value=True), mock.patch.object(ms.time, 'sleep'):
            t0 = time.time()
            with self.assertRaises(ms.Stop) as c:
                ms.Session.wait_shell(fake, 180)
            self.assertLess(time.time() - t0, 5)
        self.assertIn('open -a Terminal', str(c.exception))

    def test_wait_shell_link_down_keeps_waiting(self):
        fake = mock.Mock(execute=True, env={'PIANO_MAINT_HOST': '192.0.2.3'})
        clock = iter(range(0, 1000, 5))
        with mock.patch.object(ms.socket, 'create_connection', side_effect=OSError(errno.EHOSTUNREACH, 'x')), \
             mock.patch.object(ms.localnet, 'nc_reaches', return_value=False), mock.patch.object(ms.time, 'sleep'), \
             mock.patch.object(ms.time, 'time', side_effect=lambda: next(clock)):
            with self.assertRaises(ms.Stop) as c:
                ms.Session.wait_shell(fake, 60)
        self.assertIn('no maintenance shell', str(c.exception))


if __name__ == '__main__':
    unittest.main()
