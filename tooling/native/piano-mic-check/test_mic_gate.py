# SPDX-License-Identifier: MIT
"""Exercise the installed microphone boot gate with a local sysctl fixture."""

import subprocess
import tempfile
import unittest
from pathlib import Path


SPEC = Path(__file__).resolve().with_name('mic-check.spec')
DRIVER = r'''
. "$1"
export FIXTURE_MODE=$2
R=$3
verdict() { printf '%s\n' "$*"; }
fixture_sysctl() {
    case "$2" in
        kern.bootsessionuuid) echo 00000000-0000-0000-0000-000000000000 ;;
        kern.piano_bootargs)
            case "$FIXTURE_MODE" in
                unavailable) echo "sysctl: read failed" >&2; return 1 ;;
                old) echo "sysctl: unknown oid 'kern.piano_bootargs'" >&2; return 1 ;;
                merged) echo "piano-mic-unattended=0" ;;
            esac ;;
        kern.bootargs) echo "piano-mic-unattended=1" ;;
    esac
}
export -f fixture_sysctl
rssh() {
    [[ $1 == *kern.bootsessionuuid* ]] || return 1
    local command=${1//\/usr\/sbin\/sysctl/fixture_sysctl}
    /bin/bash -c "/bin/sh -c '$command'" || true
}
mic_check_block "$4"
'''


class MicGate(unittest.TestCase):
    def boot_receipt(self, mode):
        with tempfile.TemporaryDirectory() as folder:
            repo = Path(folder) / 'repo'
            seed = repo / 'tooling/native/piano-mic-check/mic-tcc-seed.sh'
            seed.parent.mkdir(parents=True)
            seed.symlink_to(SPEC.with_name(seed.name))
            process = subprocess.run(
                ['/bin/bash', '-c', DRIVER, 'test-mic-gate', str(SPEC), mode, folder, str(repo)],
                capture_output=True, text=True, timeout=10,
            )
            receipt, = Path(folder).glob('mic-check.*/boot.txt')
            return receipt.read_text(), process.stdout

    def test_failed_merged_read_does_not_fall_back_to_enabled_legacy_gate(self):
        receipt, verdict = self.boot_receipt('unavailable')
        self.assertNotIn('MIC_BOOTARGS=', receipt)
        self.assertIn('tablet boot receipt missing', verdict)

    def test_falls_back_when_merged_oid_is_absent(self):
        receipt, _ = self.boot_receipt('old')
        self.assertIn('MIC_BOOTARGS=piano-mic-unattended=1', receipt)

    def test_uses_merged_gate_when_available(self):
        receipt, _ = self.boot_receipt('merged')
        self.assertIn('MIC_BOOTARGS=piano-mic-unattended=0', receipt)


if __name__ == '__main__':
    unittest.main()
