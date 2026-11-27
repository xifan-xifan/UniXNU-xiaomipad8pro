#!/usr/bin/env python3
"""Host test of pianousb-shell.c (the root shell on PianoDwc3's user pipe, docs/drivers/usb-device-debug.md section 6), on the Mac.

The device is a pseudo-terminal standing in for /dev/pianousb, with what made the tablet's helper deaf (desk-dev, 2026-11-12 17:27):
a shim (DYLD_INSERT_LIBRARIES, interposed poll()) answers poll() on that device with POLLNVAL at once, which is what XNU's poll()
does for a character device whose cdevsw is not kqueue-capable (spec_kqfilter, filt_specattach: EINVAL, poll_nocancel: POLLNVAL).
Checked: the shim does that (a probe program), the helper still relays a command and its output both ways, and it does not spin
while idle (its CPU time over a quiet second and a half).

    python3 tooling/native/pianousb-shell/test_pianousb_shell.py
"""
import os
import re
import resource
import select
import shlex
import signal
import subprocess
import sys
import tempfile
import termios
import time
import tty
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SANITIZERS = ('-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all')

SHIM = r'''
#include <poll.h>
#include <stdlib.h>
#include <sys/stat.h>
// poll() as XNU answers it for /dev/pianousb before PianoDwc3 marked its cdevsw kqueue-capable: the device's registration fails,
// it is reported POLLNVAL at once and nothing is waited for.
static int noKqueue(int fd)
{
  const char *path = getenv("PIANOUSB_TEST_NO_KQUEUE");
  struct stat a, b;
  return path && fd >= 0 && fstat(fd, &a) == 0 && stat(path, &b) == 0 && S_ISCHR(a.st_mode) && a.st_rdev == b.st_rdev;
}
static int shimPoll(struct pollfd *fds, nfds_t n, int timeout)
{
  int bad = 0;
  for (nfds_t i = 0; i < n; ++i) bad += noKqueue(fds[i].fd);
  if (!bad) return poll(fds, n, timeout);
  for (nfds_t i = 0; i < n; ++i) fds[i].revents = noKqueue(fds[i].fd) ? POLLNVAL : 0;
  return bad;
}
__attribute__((used, section("__DATA,__interpose"))) static struct { const void *replacement, *original; } interpose[] = {
  {(const void *)shimPoll, (const void *)poll},
};
'''

PROBE = r'''
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
int main(int argc, char **argv)
{
  const int fd = argc > 1 ? open(argv[1], O_RDWR | O_NOCTTY) : -1;
  struct pollfd p = {.fd = fd, .events = POLLIN};
  const int n = poll(&p, 1, 2000);
  printf("%d %x\n", n, p.revents);
  return 0;
}
'''

GATE_PROBE = r'''
#define main shellMain
#define open gateOpen
#define read probeRead
#define write probeWrite
#define sysctlbyname gateSysctl
#include "pianousb-shell.c"
#undef main

// Stop as soon as the real launcher's gate passes. There is no device access.
int gateOpen(const char *path, int flags, ...)
{
  (void)path;
  (void)flags;
  exit(42);
}

int gateSysctl(const char *name, void *answer, size_t *size, void *newValue, size_t newSize)
{
  (void)newValue;
  (void)newSize;
  const int merged = strcmp(name, "kern.piano_bootargs") == 0;
  const char *value = getenv(merged ? "PIANO_TEST_MERGED" : "PIANO_TEST_LEGACY");
  if (!value || (merged && !strcmp(value, "EPERM"))) {
    errno = value ? EPERM : ENOENT;
    return -1;
  }
  const size_t required = strlen(value) + 1;
  if (*size < required) {
    errno = ENOMEM;
    return -1;
  }
  memcpy(answer, value, required);
  *size = required;
  return 0;
}

static int readError, writeError, writes;
static ssize_t readSize = 5;
static char conveyed[5];
static size_t conveyedSize;

ssize_t probeRead(int fd, void *buf, size_t n)
{
  if (fd != 3 || n < sizeof conveyed) abort();
  if (readSize > 0) memcpy(buf, "hello", sizeof conveyed);
  errno = readError;
  return readSize;
}

ssize_t probeWrite(int fd, const void *buf, size_t n)
{
  if (fd != 4) abort();
  if (writeError == EIO) { errno = EIO; return -1; }
  if (writeError == EPIPE) return 0;
  switch (++writes) {
    case 1: if (n == 5) n = 2; break;
    case 2: errno = EINTR; return -1;
    case 3: errno = EAGAIN; return -1;
  }
  if (n > sizeof conveyed - conveyedSize) abort();
  memcpy(conveyed + conveyedSize, buf, n);
  conveyedSize += n;
  return (ssize_t)n;
}

static int relayProbe(void)
{
  if (copy(3, 4) != -1 || writes != 4 || conveyedSize != 5 || memcmp(conveyed, "hello", 5)) return 1;
  writeError = EIO;
  if (copy(3, 4) != 4) return 2;
  writeError = EPIPE;   // the shim returns a zero-byte write
  if (copy(3, 4) != 4) return 3;
  readSize = 0;
  if (copy(3, 4) != 3) return 4;
  readSize = -1;
  readError = EINTR;
  if (copy(3, 4) != -1) return 5;
  readError = EAGAIN;
  if (copy(3, 4) != -1) return 6;
  readError = EIO;
  if (copy(3, 4) != 3) return 7;
  return 0;
}

int main(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1], "--relay-probe")) return relayProbe();
  if (argc == 3) {
    size_t length;
    const char *value = pianoBootArgValue(argv[1], argv[2], &length);
    if (!value) return 1;
    printf("%.*s", (int)length, value);
    return 0;
  }
  return shellMain(argc, argv);
}
'''


def cc(out, *args):
    subprocess.run(['xcrun', 'clang', '-arch', 'arm64', '-mmacosx-version-min=14.0', '-O2', '-Wall', '-Wextra', '-Werror', *args,
                    '-o', str(out)], check=True)


def child_cpu_seconds():
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


class PianoUsbShellTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='pianousb-shell-test-')
        d = Path(cls.tmp.name)
        cls.directory = d
        cls.helper, cls.shim, cls.probe = d / 'pianousb-shell', d / 'shim.dylib', d / 'probe'
        cls.gate = d / 'gate'
        cc(cls.helper, *SANITIZERS, str(HERE / 'pianousb-shell.c'))
        (d / 'shim.c').write_text(SHIM)
        cc(cls.shim, '-dynamiclib', str(d / 'shim.c'))
        (d / 'probe.c').write_text(PROBE)
        cc(cls.probe, str(d / 'probe.c'))
        (d / 'gate.c').write_text(GATE_PROBE)
        cc(cls.gate, *SANITIZERS, '-I', str(HERE), str(d / 'gate.c'))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        # The Mac's end of the cable is the master; the device the helper opens is the slave, raw both ways like the USB pipe (no
        # echo, no newline translation), kept open here so that its settings stay (a pty's first open resets them).
        self.master, self.slave = os.openpty()
        self.device = os.ttyname(self.slave)
        tty.setraw(self.slave)
        attrs = termios.tcgetattr(self.slave)
        attrs[3] &= ~termios.ECHO
        termios.tcsetattr(self.slave, termios.TCSANOW, attrs)
        self.env = dict(os.environ, DYLD_INSERT_LIBRARIES=str(self.shim), PIANOUSB_TEST_NO_KQUEUE=self.device)
        self.proc = None
        self.addCleanup(os.close, self.slave)
        self.addCleanup(lambda: os.close(self.master))

    def tearDown(self):
        if self.proc:
            if self.proc.poll() is None:
                self.proc.terminate()
            try:
                _, stderr = self.proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                _, stderr = self.proc.communicate()
                self.fail(f'helper did not stop: {stderr!r}')
            self.assertNotIn(b'Sanitizer', stderr)
            self.assertNotIn(b'runtime error', stderr)

    def start(self, device, close_stdin=False):
        args = [str(self.helper), str(device), '/bin/sh']
        if close_stdin:
            args = ['/bin/sh', '-c', 'exec 0<&-; exec "$@"', 'sh', *args]
        self.proc = subprocess.Popen(args, env=self.env, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def read_until(self, needle, seconds, master=None):
        if master is None:
            master = self.master
        got, end = b'', time.monotonic() + seconds
        while needle not in got and time.monotonic() < end:
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                data = os.read(master, 4096)
                if not data:
                    break
                got += data
        return got

    def test_shim_answers_pollnval(self):
        out = subprocess.run([str(self.probe), self.device], env=self.env, capture_output=True, text=True, timeout=10).stdout.split()
        self.assertEqual(out, ['1', '%x' % select.POLLNVAL], 'the shim must answer poll() on the device as XNU did for /dev/pianousb')

    def test_short_io_and_endpoint_errors(self):
        result = subprocess.run([str(self.gate), '--relay-probe'], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_launch_gate_reads_full_boot_arguments(self):
        switch = 'piano-usb-shell=1'
        legacy = 'x' * 1015
        for merged, clipped, expected in (
                (legacy + ' ' + switch, legacy, 42),
                ('x' * (4095 - len(switch) - 1) + ' ' + switch, legacy, 42),
                ('piano-usb-shell=0', switch, 0),
                ('piano-usb-shell=0 ' + switch, '', 42),
                (switch + ' piano-usb-shell=0', switch, 0),
                ('quiet\t' + switch, '', 42),
                ('piano-usb-shell-extra=1 other=' + switch, '', 0),
                ('piano-usb-shell=\t1', '', 0),
                ('  \tpiano-usb-shell\t ', '', 42),
                ('piano-usb-shell=0xffffffff', '', 42),
                ('piano-usb-shell=0x100000000', '', 0),
                ('piano-usb-shell=-1', '', 0),
                ('piano-usb-shell=\r1', '', 0),
                (switch + ' piano-usb-shell=1garbage', '', 0),
                ('piano-usb-shell=' + '9' * 4079, switch, 0),
                ('x' * 4096 + ' ' + switch, switch, 0),
                ('EPERM', switch, 0),
                (None, switch, 42),
                (None, legacy, 0)):
            with self.subTest(merged=merged is not None, length=len(merged or ''), expected=expected):
                env = dict(os.environ, PIANO_TEST_LEGACY=clipped)
                env.pop('PIANO_TEST_MERGED', None)
                if merged is not None:
                    env['PIANO_TEST_MERGED'] = merged
                result = subprocess.run([str(self.gate)], env=env, capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, expected, result.stderr)

    def test_shared_parser_matches_complete_keys(self):
        for line, key, expected in (
                ('piano-rotated=3 other=piano-rotate=2', 'piano-rotate', None),
                ('piano-rotate=2 piano-rotated=3\tpiano-rotate=0', 'piano-rotate', '0'),
                ('piano-backlight-auto=1 piano-backlight-auto-requested=1 piano-backlight-auto=0', 'piano-backlight-auto', '0'),
                ('piano-backlight-native', 'piano-backlight-native', '1')):
            with self.subTest(line=line, key=key):
                result = subprocess.run([str(self.gate), line, key], capture_output=True, text=True, timeout=5)
                self.assertEqual((result.returncode, result.stdout), (1, '') if expected is None else (0, expected))

    def test_relays_both_ways_without_spinning(self):
        self.start(self.device)
        self.assertIn(b'a new shell on the tablet', self.read_until(b'(exit starts another)', 10))
        os.write(self.master, b"printf 'piano%sok\\n' -relay-\n")   # the output, piano-relay-ok, is not the command's text
        self.assertIn(b'piano-relay-ok', self.read_until(b'piano-relay-ok', 10), 'no output of the command came back')
        time.sleep(1.5)
        os.write(self.master, b'exit\n')   # a new shell starts: the banner again
        self.assertIn(b'a new shell on the tablet', self.read_until(b'(exit starts another)', 10))
        self.assertIsNone(self.proc.poll(), 'the helper exited')
        before = child_cpu_seconds()
        self.proc.terminate()
        self.assertEqual(self.proc.wait(timeout=5), 0)
        self.assertLess(child_cpu_seconds() - before, 0.5, 'the helper spins while idle')

    def test_device_can_reuse_closed_stdin(self):
        self.start(self.device, close_stdin=True)
        self.assertIn(b'a new shell', self.read_until(b'(exit starts another)', 5))
        os.write(self.master, b"printf 'piano%sok\\n' -stdin-\n")
        self.assertIn(b'piano-stdin-ok', self.read_until(b'piano-stdin-ok', 5))

    def test_disconnect_reopens_with_a_fresh_shell(self):
        with tempfile.TemporaryDirectory(dir=self.directory) as directory:
            endpoint = Path(directory) / 'cable'
            endpoint.symlink_to(self.device)
            self.start(endpoint)
            self.assertIn(b'a new shell', self.read_until(b'(exit starts another)', 5))
            os.write(self.master, b"trap '' HUP; printf 'child:%s:end\\n' \"$$\"\n")
            match = re.search(rb'child:(\d+):end', self.read_until(b':end\r\n', 5))
            self.assertIsNotNone(match)
            old_pid = int(match[1])

            next_master, next_slave = os.openpty()
            self.addCleanup(os.close, next_master)
            self.addCleanup(os.close, next_slave)
            tty.setraw(next_slave)
            next_endpoint = endpoint.with_name('next')
            next_endpoint.symlink_to(os.ttyname(next_slave))
            next_endpoint.replace(endpoint)

            # Closing the cable while a shell is active makes the old device readable at EOF.
            old_master = self.master
            self.master = os.dup(next_master)
            os.close(old_master)
            self.assertIn(b'a new shell', self.read_until(b'(exit starts another)', 8))
            with self.assertRaises(ProcessLookupError):
                os.kill(old_pid, 0)
            fifo = Path(directory) / 'blocked'
            os.mkfifo(fifo)
            command = f"""trap '' HUP; printf 'child:%s:end\\n' "$$"; read value < {shlex.quote(str(fifo))}\n"""
            os.write(self.master, command.encode())
            match = re.search(rb'child:(\d+):end', self.read_until(b':end\r\n', 5))
            self.assertIsNotNone(match)
            new_pid = int(match[1])
            self.assertNotEqual(old_pid, new_pid)
            self.proc.send_signal(signal.SIGTERM)
            self.assertEqual(self.proc.wait(timeout=5), 0)
            with self.assertRaises(ProcessLookupError):
                os.kill(new_pid, 0)


if __name__ == '__main__':
    if sys.platform != 'darwin':
        print('test_pianousb_shell: macOS only (XNU poll semantics, xcrun clang)')
        sys.exit(0)
    unittest.main()
