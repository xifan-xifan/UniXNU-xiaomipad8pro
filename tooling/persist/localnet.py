#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Local Network preflight for the host tools that open python sockets to the tablet (maint-session.py, the install .command files).

macOS Local Network privacy grants the right to reach local addresses per responsible app. A python started under a tmux server or
a tool shell that never got the grant gets EHOSTUNREACH (errno 65) at once for every LAN, link-local and USB-NCM address, while
Apple's /usr/bin/nc (exempt) and the same python inside Terminal.app work. maint-session.py then waited 180 s for "the maintenance
shell" and stopped with a misleading "USB NCM link up?" (11-25: two y12i install rounds, nothing written).

Probe: one UDP datagram to the discard port (9) of an on-link address: the default gateway first (always on-link when there is one),
then PIANO_MAINT_HOST. Sending needs no answer, so the result is immediate: sent = allowed; EHOSTUNREACH on an address the ARP table
knows (or that nc reaches) = denied by the privacy layer. No on-link address to probe = unknown (the tool goes on; the connect-time
check below still catches it).

    localnet.py [--host ADDR] [--port N]     exit 0 allowed or unknown, 3 denied (the message names the fix)

classify_connect_error() is the connect-time check: an EHOSTUNREACH from python to HOST:PORT that /usr/bin/nc reaches is the
privacy denial, not a link that is down.
"""
import argparse
import errno
import os
import re
import socket
import subprocess
import sys

DENIED = ('python sockets to the local network are blocked for this process (macOS Local Network privacy: errno 65 EHOSTUNREACH '
          'while the address is reachable). This shell (tmux, a tool or agent shell) has no Local Network grant: run the session '
          'in Terminal.app instead, e.g. open -a Terminal <the install .command file>. Nothing was sent to the tablet.')


def _run(cmd, timeout=5):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None


def default_gateway(run=_run):
    r = run(['/sbin/route', '-n', 'get', 'default'])
    m = r and re.search(r'^\s*gateway:\s*(\d+\.\d+\.\d+\.\d+)\s*$', r.stdout, re.M)
    return m.group(1) if m else None


def arp_known(ip, run=_run):
    r = run(['/usr/sbin/arp', '-n', ip])
    return bool(r and re.search(r' at [0-9a-f]{1,2}(:[0-9a-f]{1,2}){5} ', r.stdout))


def nc_reaches(host, port, run=_run):
    r = run(['/usr/bin/nc', '-z', '-G', '2', host, str(port)], timeout=6)
    return bool(r and r.returncode == 0)


def udp_probe(ip, sock=socket.socket):
    """None when the datagram left, else the OSError."""
    s = sock(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.sendto(b'\0', (ip, 9))
        return None
    except OSError as e:
        return e
    finally:
        s.close()


def preflight(host=None, port=2323, env=os.environ, run=_run, sock=socket.socket):
    """('allowed'|'denied'|'unknown', detail)."""
    targets = []
    gw = default_gateway(run)
    if gw:
        targets.append(('default gateway', gw))
    host = host or env.get('PIANO_MAINT_HOST')
    if host and re.fullmatch(r'\d+\.\d+\.\d+\.\d+', host) and host != gw:
        targets.append(('PIANO_MAINT_HOST', host))
    unreach = []
    for label, ip in targets:
        err = udp_probe(ip, sock)
        if err is None:
            return 'allowed', f'a UDP datagram to the {label} left this process'
        if err.errno == errno.EHOSTUNREACH:
            if arp_known(ip, run) or (label == 'PIANO_MAINT_HOST' and nc_reaches(ip, port, run)):
                return 'denied', f'the {label} is reachable (ARP/nc) but python got errno 65: {DENIED}'
            unreach.append(f'{label}: errno 65 (not in ARP)')
        else:
            unreach.append(f'{label}: {err}')
    if unreach and gw and any(u.startswith('default gateway: errno 65') for u in unreach):
        # the gateway is on-link by definition: an immediate EHOSTUNREACH to it is the privacy layer even before ARP learnt it
        return 'denied', f'errno 65 to the default gateway: {DENIED}'
    return 'unknown', ('; '.join(unreach) if unreach else 'no on-link address to probe') + ' (going on; connects are checked again)'


def classify_connect_error(host, port, err, run=_run):
    """The DENIED message when python's EHOSTUNREACH to HOST:PORT is the privacy layer (nc reaches the same port), else None."""
    if isinstance(err, OSError) and err.errno == errno.EHOSTUNREACH and nc_reaches(host, port, run):
        return f'{host}:{port} answers /usr/bin/nc but python gets errno 65: {DENIED}'
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host', help='a tablet-side address to probe after the gateway (default PIANO_MAINT_HOST)')
    p.add_argument('--port', type=int, default=2323, help='its TCP port for the nc cross-check (default 2323, the maintenance shell)')
    a = p.parse_args()
    state, detail = preflight(a.host, a.port)
    print(f'localnet preflight: {state}: {detail}', file=sys.stderr if state == 'denied' else sys.stdout)
    sys.exit(3 if state == 'denied' else 0)


if __name__ == '__main__':
    main()
