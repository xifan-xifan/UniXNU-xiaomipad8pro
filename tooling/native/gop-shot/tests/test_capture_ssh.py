#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the capture wrapper and real chunk verifier through a local transport."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

CAPTURE = Path(__file__).resolve().parents[1] / 'capture-ssh.sh'


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


PNG = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(b'\0\x12\x34\x56')) + chunk(b'IEND', b''))


class Capture(unittest.TestCase):
    def test_local_transport_uses_selected_python_and_only_publishes_png(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            python = root / 'python3'
            python.symlink_to(sys.executable)
            transport = root / 'transport'
            transport.write_text('#!/bin/bash\n[[ "$1" == --raw ]] || exit 2\nexec /bin/bash -c "$2"\n')
            tool = root / "tool's path"
            tool.write_text('#!/bin/bash\n[[ "$1" == --10bit && -f "$2" ]]\n')
            for path in (transport, tool):
                path.chmod(0o700)

            remote = root / "remote's PNG"
            output = root / 'local PNG'
            env = dict(os.environ, GOP_SHOT_TSUDO=str(transport), PATH=str(root) + ':' + os.environ['PATH'])
            command = ['/bin/bash', str(CAPTURE), '--10bit', str(tool), str(remote)]
            result = subprocess.run([*command, ''], env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 2, result.stderr)
            damaged = PNG[:41] + bytes([PNG[41] ^ 1]) + PNG[42:]  # IDAT data changed without updating its CRC.
            for contents, expected in ((b'', 1), (PNG[:-12], 1), (damaged, 1), (PNG, 0)):
                with self.subTest(length=len(contents)):
                    remote.write_bytes(contents)
                    result = subprocess.run([*command, str(output)], env=env, text=True, capture_output=True)
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                    self.assertNotIn('Traceback', result.stderr)
                    self.assertEqual(output.exists(), expected == 0)
                    self.assertEqual(list(root.glob('.gop-shot.*')), [])
            self.assertEqual(output.read_bytes(), PNG)
            result = subprocess.run([*command, str(output)], env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(output.read_bytes(), PNG)


if __name__ == '__main__':
    unittest.main()
