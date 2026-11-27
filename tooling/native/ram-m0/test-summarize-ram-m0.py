#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the summary CLI against snapshot fixtures without macOS or SSH."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SUMMARY = Path(__file__).with_name('summarize-ram-m0.py')
COOL = """phase=idle
hw.memsize=13956546560
hw.pagesize=16384
kern.memorystatus_vm_pressure_level=1
vm.swapusage=total = 1024.00M  used = 0.00M  free = 1024.00M  (encrypted)
Pages free: 1024.
Pages speculative: 64.
Pages occupied by compressor: 128.
"""
TIER = 'machdep.piano_gpu_tier='


def tier(mode, state, free=0, used=0, allocs=0, refusals=0, imports=0, maps=0):
    qualification = 'runs=3 pass-mask=0x7' if state == 'ready' else 'runs=0 pass-mask=0'
    return (f'{TIER}mode={mode} state={state} {qualification} '
            f'free-mib={free} used-mib={used} refusals={refusals} allocs={allocs} '
            f'tier-import-denied={imports} tier-cpu-map-denied={maps}')


class SummarizeTest(unittest.TestCase):
    def summarize(self, *snapshots, rc=0):
        with tempfile.TemporaryDirectory() as temp:
            paths = []
            for i, snapshot in enumerate(snapshots):
                path = Path(temp) / f'{i}.txt'
                path.write_text(snapshot, encoding='utf-8')
                paths.append(path)
            result = subprocess.run([sys.executable, '-I', SUMMARY, *paths], capture_output=True, text=True)
        self.assertEqual(result.returncode, rc, result.stdout + result.stderr)
        self.assertEqual(result.stderr, '')
        return result.stdout

    def test_vm_stat_and_memory_pressure_are_separate_samples(self):
        summary = self.summarize(
            '== size\nhw.pagesize=4096\n'
            'vm.swapusage=total = 1024.00M  used = 0.00M  free = 1024.00M\n'
            'kern.memorystatus_vm_pressure_level=1\n'
            '== vm_stat\nMach Virtual Memory Statistics: (page size of 4096 bytes)\n'
            'Pages free: 1024.\nPages speculative: 512.\n"Pages wired down": 512.\n'
            'Pages occupied by compressor: 1024.\nSwapouts: 12.\n'
            '== memory_pressure\nPages free: 32768\nPages speculative: 32768\n'
            'Pages wired down: 65536\nSwapouts: 99\nSystem-wide memory free percentage: 35%\n'
            'M0_DONE\n')
        self.assertIn('free_mib=6 wired_mib=2 compressed_mib=4 swap_used_mib=0 swapouts=12 free_pct=35', summary)
        self.assertIn('no RAM pressure', summary)

    def test_page_size_source_and_memory_window(self):
        for page_size in ('hw.pagesize=broken\nvm.pagesize=16384',
                          'Mach Virtual Memory Statistics: (page size of 16384 bytes)\nhw.pagesize=4096'):
            with self.subTest(page_size=page_size):
                summary = self.summarize(
                    f'phase=idle\nhw.memsize=13956546560\n{page_size}\n'
                    'Pages free: 64.\nPages speculative: 64.\n'
                    'vm.swapusage=used = 300.00M\nM0_DONE\n')
                self.assertIn('memsize_mib=13310 free_mib=2', summary)
                self.assertIn('swap_used_mib=300', summary)
                self.assertIn('RAM pressure seen (idle)', summary)

    def test_missing_or_malformed_pressure_is_unknown(self):
        for bad_size, free, speculative in (('16384', '128', ''), ('junk', '128', '129'),
                                            ('0', '128', '129'), ('16384', '１２８', '129')):
            with self.subTest(page_size=bad_size, free=free, speculative=speculative):
                summary = self.summarize(
                    f'hw.pagesize={bad_size}\nhw.memsize=²\nPages free: {free}.\n'
                    f'Pages speculative: {speculative}\nSwapouts: 4.\n'
                    'vm.swapusage=total = 0.00M  used = 1..2M  free = 0.00M\n'
                    'kern.memorystatus_vm_pressure_level=unknown\n'
                    'System-wide memory free percentage: 101%\nM0_DONE\n')
                self.assertIn('memsize_mib=- free_mib=-', summary)
                self.assertIn('swap_used_mib=- swapouts=4 free_pct=- pressure=-', summary)
                self.assertIn('insufficient data', summary)

    def test_partial_and_duplicate_records(self):
        summary = self.summarize(
            '== vm_stat\nMach Virtual Memory Statistics: (page size of 16384 bytes)\n'
            'Pages free:\n256.\nPages speculative: 64.\n'
            'Pages occupied by compressor: 65536.\nPages occupied by compressor: 0.\n'
            '== memory_pressure\nSystem-wide memory free percentage:\n88%\n'
            '== size\nvm.swapusage=used = 5M used = 1M\nkern.memorystatus_vm_pressure_level=99\nM0_DONE\n')
        self.assertIn('free_mib=- wired_mib=- compressed_mib=- swap_used_mib=- swapouts=- free_pct=- pressure=-', summary)
        self.assertIn('insufficient data', summary)

    def test_pressure_signal_does_not_require_other_probes(self):
        for signal in ('vm.swapusage=used = 0.01M (encrypted)',
                       'kern.memorystatus_vm_pressure_level=4',
                       'hw.pagesize=16384\nPages occupied by compressor: 65537.'):
            with self.subTest(signal=signal):
                summary = self.summarize(f'phase=apps\n{signal}\nM0_DONE\n')
                self.assertIn('RAM pressure seen (apps)', summary)

    def test_completion_requires_one_final_marker(self):
        for end in ('', 'M0_DONE\ntrailing payload\n', 'M0_DONE\nM0_DONE\n'):
            with self.subTest(end=end):
                summary = self.summarize(COOL + end, rc=1)
                self.assertIn('done=False', summary)
                self.assertIn('insufficient data', summary)
        self.assertIn('done=True', self.summarize(COOL + 'M0_DONE\r\n\r\n'))
        self.assertIn('insufficient data', self.summarize(rc=1))

    def test_tier_modes_and_residency(self):
        def snapshot(line):
            return COOL + f'== piano\n{line}\nM0_DONE\n'

        self.assertIn('tier_mode=0 tier_state=off tier_status=off tier_free_mib=0 tier_used_mib=0',
                      self.summarize(snapshot(tier(0, 'off'))))
        residency = (0, 2, 6, 14, 30, 62, 126, 0)
        rows = self.summarize(*(snapshot(tier(1, 'ready', 870 - used, used, min(i, 6)))
                                for i, used in enumerate(residency)))
        self.assertEqual(len(rows.splitlines()), len(residency) + 1)
        for used, line in zip(residency, rows.splitlines()):
            self.assertIn(f'tier_mode=1 tier_state=ready tier_status=ready tier_free_mib={870 - used} tier_used_mib={used}', line)
        self.assertIn('tier_refusals=0 tier_allocs=6 tier_import_denied=0 tier_cpu_map_denied=0', rows)
        summary = self.summarize(snapshot(tier(2, 'ready', 0, 870, 7, 3, 2, 1)))
        self.assertIn('tier_mode=2 tier_state=ready tier_status=ready tier_free_mib=0 tier_used_mib=870', summary)
        self.assertIn('tier_refusals=3 tier_allocs=7 tier_import_denied=2 tier_cpu_map_denied=1', summary)

    def test_tier_unavailable_or_invalid(self):
        def snapshot(line):
            return self.summarize(COOL + f'{line}\nM0_DONE\n')

        for state in ('failed', 'closed', 'bad length', 'region1 record refused', 'write-combine-disabled',
                      'cached-coherent', 'output-width', 'absent', 'empty'):
            with self.subTest(state=state):
                self.assertIn(f'tier_state={state.replace(" ", "-")} tier_status=failed', snapshot(tier(1, state)))
        self.assertIn('tier_state=pending tier_status=pending', snapshot(tier(2, 'pending')))
        self.assertIn('tier_mode=- tier_state=- tier_status=missing', snapshot('MISSING sysctl machdep.piano_gpu_tier'))
        self.assertIn('tier_status=missing', snapshot(''))
        for line in (TIER, tier(1, 'ready', 870).rsplit(' ', 1)[0], tier(0, 'ready', 870),
                     tier(1, 'off'), tier(3, 'ready', 870), tier(1, 'ready'), tier(2, 'failed', 870),
                     tier(0, 'off', imports=1), tier(2, 'ready', 870, refusals=-1),
                     tier(1, 'ready', 870, allocs=2**64), tier(1, 'ready', '870MiB'),
                     tier(1, 'ready', 870) + ' used-mib=2', tier(1, 'ready', 870, maps='0 tail'),
                     tier(1, 'ready', 870).replace('runs=3', 'runs=3 dangling'),
                     tier(1, 'ready', 870).replace('tier-import-denied=0', 'tier-import-denied='),
                     tier(0, 'off') + '\n' + tier(1, 'ready', 870)):
            with self.subTest(line=line):
                self.assertIn('tier_mode=- tier_state=- tier_status=invalid tier_free_mib=-', snapshot(line))


if __name__ == '__main__':
    unittest.main()
