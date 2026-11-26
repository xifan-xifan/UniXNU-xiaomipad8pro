#!/usr/bin/env python3
"""Can the maintenance image fetch from this host? Serves one file on the USB-link address and runs the tablet's wget.

The maintenance session serves its files (tablet.sh, the root image) over HTTP from this host's USB-link address, and a host
firewall that blocks inbound on that interface stops the session at its first fetch with a message that looks like a dead link.
This asks the question directly: HOST is this host's address on the cable, the tablet is PIANO_MAINT_HOST.

  probe-usb-net.py HOST [PORT]

Prints what the tablet received and, when nothing arrived, says so.
"""
import http.server, os, socketserver, subprocess, sys, threading
from pathlib import Path

host = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
tablet = os.environ.get('PIANO_MAINT_HOST')
if not tablet:
    sys.exit('PIANO_MAINT_HOST is not set')
body = b'probe-ok\n'
seen = []


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        seen.append(self.client_address[0])
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


try:
    srv = socketserver.TCPServer((host, port), Handler)
except OSError as e:
    sys.exit(f'cannot serve on {host}:{port}: {e}')
threading.Thread(target=srv.serve_forever, daemon=True).start()
shell = Path(__file__).resolve().parents[2] / 'tooling/native/maint-shell.py'
r = subprocess.run([sys.executable, str(shell), f'wget -q -T 10 -O - http://{host}:{port}/probe.txt; echo rc=$?', '30'],
                   capture_output=True, text=True, timeout=60)
srv.shutdown()
out = (r.stdout or '').replace('\r', '')
print('tablet saw:', ' '.join(out.split())[:400])
print('server served to:', seen or 'nobody')
if 'probe-ok' in out and seen:
    print(f'PASS the tablet can fetch from {host}:{port}')
    sys.exit(0)
print(f'FAIL the tablet could not fetch from {host}:{port} (a host firewall blocking inbound on the USB link is the usual cause)')
sys.exit(1)