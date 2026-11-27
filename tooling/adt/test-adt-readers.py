#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the offline ADT readers with malformed binary inputs."""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent


def node(props, children=()):
    return (struct.pack('<II', len(props), len(children)) +
            b''.join(name.encode().ljust(32, b'\0') + struct.pack('<I', len(data)) + data +
                     bytes(-len(data) % 4) for name, data in props) + b''.join(children))


class AdtReaders(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.adt = Path(tmp.name) / 'test.adt'
        self.kc = Path(tmp.name) / 'test.kc'
        self.tree = node([], [node([
            ('name', b'chosen\0'), ('dram-base', struct.pack('<Q', 0x8c0000000)),
            ('dram-size', struct.pack('<Q', 0x340000000)), ('syscfg-v2-data', b'\xff' * 24),
            ('mac-address-wifi0', bytes.fromhex('025049414e01')),
        ])])
        self.adt.write_bytes(self.tree)

    def run_tool(self, tool, *args):
        return subprocess.run([sys.executable, str(TOOLS / tool), *map(str, args)],
                              capture_output=True, text=True)

    def test_syscfg_rejects_truncated_padding_and_trailing_bytes(self):
        self.assertEqual(self.run_tool('check-syscfg.py', self.adt).returncode, 0)
        for broken, reason in [(self.tree[:-1], 'property value or padding'),
                               (self.tree[:7], 'node header'),
                               (self.tree + b'\0', 'trailing bytes')]:
            with self.subTest(reason=reason):
                self.adt.write_bytes(broken)
                result = self.run_tool('check-syscfg.py', self.adt)
                self.assertEqual(result.returncode, 1)
                self.assertIn(reason, result.stdout + result.stderr)
                self.assertNotIn('Traceback', result.stderr)

    def test_window_rejects_segment_shorter_than_its_header(self):
        header = struct.pack('<8I', 0xfeedfacf, 0x0100000c, 0, 12, 1, 72, 0, 0)
        segment = struct.pack('<II16sQQQQiiii', 0x19, 72, b'__TEXT', 0x100000000,
                              0x4000, 0, 0, 5, 5, 0, 0)
        self.kc.write_bytes(header + segment)
        args = ('--adt', self.adt, '--kc', self.kc)
        good = self.run_tool('check-ram-window.py', *args)
        self.assertEqual(good.returncode, 0, good.stderr)
        self.kc.write_bytes(header + struct.pack('<II', 0x19, 8) + segment[8:])
        bad = self.run_tool('check-ram-window.py', *args)
        self.assertEqual(bad.returncode, 1)
        self.assertIn('short LC_SEGMENT_64', bad.stdout + bad.stderr)


if __name__ == '__main__':
    unittest.main()
