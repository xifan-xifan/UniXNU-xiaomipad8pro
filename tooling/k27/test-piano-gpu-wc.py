#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the reserved GPU WC policy from the actual XNU patch."""

import ctypes
import re
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / 'patches/k27/0077-k27-gpu-tier-normal-write-combining.patch'
HEADER = 'pmap_piano_gpu_wc.h'
WINDOW = 0x8C0000000
SLAB = 2 << 20
RUNS = (
    (0x880000000, 758 << 20, 1, 1),
    (0x8B0000000, 84 << 20, 1, 1),
    (0x8B5800000, 28 << 20, 1, 1),
)
MODEM = (0x89E00000, 274 << 20, 0, 3)
TAIL = (0x9EE00000, 70 << 20, 0, 2)


def records(*runs):
    return b''.join(struct.pack('<QQII', *run) for run in runs).ljust(384, b'\0')


def test(lib, driver):
    match = lib.match
    match.argtypes = (ctypes.c_uint, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64, ctypes.c_uint64)
    match.restype = ctypes.c_int
    driver.match.argtypes = match.argtypes
    driver.match.restype = match.restype
    for parse in (lib.boot_mode, driver.boot_mode):
        parse.argtypes = (ctypes.c_char_p,)
        parse.restype = ctypes.c_uint
        for args, expected in (
            (None, 0), (b'piano-extra-ram=0 piano-video-extra-ram=0 piano-gpu-tier=0', 0),
            (b'piano-extra-ram=1 piano-video-extra-ram=1', 0), (b'piano-gpu-tier=0x1', 0), (b'piano-gpu-tier=01', 0),
            (b'piano-gpu-tier=1 piano-gpu-tier=', 0),
            (b'piano-gpu-tier=2\tpiano-gpu-tier=0', 0),
            (b'piano-gpu-tier=0\tpiano-gpu-tier=1', 1),
            (b'piano-gpu-tier=2 xpiano-gpu-tier=0', 2),
        ):
            assert parse(args) == expected, args
    payload = records(*RUNS, MODEM, TAIL)

    for mode in (0, 1, 2, 3):
        for base, size, _, _ in RUNS:
            for pa in (base, base + size - 1):
                assert match(mode, payload, len(payload), WINDOW, pa) == (mode in (1, 2))
                assert driver.match(mode, payload, len(payload), WINDOW, pa) == (mode in (1, 2))
            for pa in (base - 1, base + size):
                assert not match(mode, payload, len(payload), WINDOW, pa)
        for addr in (MODEM[0], TAIL[0], WINDOW, 0x3D00000):
            assert not match(mode, payload, len(payload), WINDOW, addr)

    base = RUNS[0][0]
    subset = records((base + SLAB, SLAB, 1, 1))
    assert match(1, subset, len(subset), WINDOW, base + SLAB)
    assert not match(1, subset, len(subset), WINDOW, base)
    for check in (match, driver.match):
        assert not check(1, payload, len(payload), base + RUNS[0][1], base)  # later runs overlap XNU
        assert check(1, subset, len(subset), base + 2 * SLAB, base + SLAB)
        assert not check(1, subset, len(subset), base + 2 * SLAB - 1, base + SLAB)

    for malformed in (
        None, b'', payload[:136], payload[:-1], payload + b'\0', records(),
        payload[:24], payload[:48],
        records((base, SLAB, 1, 2)), records((base, SLAB, 0, 1)),
        records(RUNS[0], RUNS[0]), records(RUNS[0], (base + SLAB, SLAB, 1, 1)),
        records(RUNS[0], (base + SLAB, SLAB, 0, 3)),
        records((base + SLAB, SLAB, 0, 3), RUNS[0]),
        records(RUNS[0], (base + RUNS[0][1], SLAB, 1, 1)),  # crosses into the first hole
        records(RUNS[0], (base + 4096, SLAB, 1, 1)), records(RUNS[0], (base, 0, 1, 1)),
        records(RUNS[0], (base + SLAB, 0, 0, 3)),
        records(RUNS[0], (0xffffffffffc00000, SLAB * 2, 1, 1)),
        records(RUNS[0], (0xffffffffffc00000, SLAB * 2, 0, 3)),
        records(RUNS[0], (0, 0, 0, 0), RUNS[1]),
        records(*((base + i * SLAB, SLAB, 1, 1) for i in range(9))),
    ):
        assert not match(1, malformed, len(malformed or b''), WINDOW, base), malformed
        assert not driver.match(1, malformed, len(malformed or b''), WINDOW, base), malformed


def main():
    text = PATCH.read_text()
    pmap, header = text.split(f'+++ b/osfmk/arm/pmap/{HEADER}\n')
    assert 'case VM_WIMG_WCOMB:\n-\t\tif (is_dram_addr(pa)) {\n' in pmap
    assert 'piano_gpu_wc_contains(piano_gpu_wc_runs, piano_gpu_wc_count, pa)' in pmap
    assert 'piano_gpu_wc_mode(PE_boot_args())' in pmap
    header = ''.join(line[1:] for line in header.splitlines(keepends=True) if line.startswith('+'))
    bounds = re.findall(r'{(0x\w+)ULL, (0x\w+)ULL}', header)
    shared = (ROOT / 'package/include/PianoExtraRam.h').read_text()
    assert bounds == re.findall(r'{(0x\w+)ull, (0x\w+)ull, Region1, UseGpu,', shared)
    with tempfile.TemporaryDirectory(prefix='piano-gpu-wc-') as scratch:
        scratch = Path(scratch)
        (scratch / HEADER).write_text(header)
        source = scratch / 'policy.c'
        source.write_text(f'''#include "{HEADER}"
unsigned boot_mode(const char *args) {{ return piano_gpu_wc_mode(args); }}
int match(unsigned mode, const uint8_t *property, size_t length, uint64_t window, uint64_t pa)
{{
    struct piano_gpu_wc_run runs[PIANO_GPU_WC_RUNS];
    unsigned count = piano_gpu_wc_load(mode, property, length, window, runs);
    return piano_gpu_wc_contains(runs, count, pa);
}}
''')
        library = scratch / 'policy.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                        str(source), '-o', str(library)], check=True)
        driver_source = scratch / 'driver.cpp'
        driver_source.write_text('''#include "A830GpuTier.h"
extern "C" unsigned boot_mode(const char *args) { return a830tier::bootArgMode(args); }
extern "C" int match(unsigned mode, const uint8_t *property, size_t length, uint64_t window, uint64_t pa)
{
    if (!a830tier::eligible(mode, a830tier::kSlab)) return 0;
    a830tier::Run runs[a830tier::kMaxRuns];
    unsigned count;
    if (a830tier::loadRuns(property, length, window, 0, runs, count) != a830tier::Parse::ok) return 0;
    for (unsigned i = 0; i < count; ++i)
        if (pa >= runs[i].base && pa - runs[i].base < runs[i].size) return 1;
    return 0;
}
''')
        driver = scratch / 'driver.so'
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                        '-I', str(ROOT / 'package/PianoA830/adreno830'),
                        str(driver_source), '-o', str(driver)], check=True)
        test(ctypes.CDLL(str(library)), ctypes.CDLL(str(driver)))
    print('PASS: XNU GPU WC policy, switches and published bounds, holes, window and malformed records')


if __name__ == '__main__':
    main()
