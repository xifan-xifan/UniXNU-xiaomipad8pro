#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# summarize-ram-m0.py SNAPSHOT.txt...: one line per ram-m0-tab.sh snapshot (MiB), then the M0 verdict hints of ANALYSIS.md:
# window (hw.memsize) vs 13310 MiB, free+speculative, compressor use, swap use, pressure level, GPU paged-VM mode.
import re, sys
WINDOW_MIB = 13310

def parse(path):
    s = {}
    txt = open(path, errors='replace').read()
    for m in re.finditer(r'^([a-z_.]+)=(.*)$', txt, re.M):
        s[m.group(1)] = m.group(2).strip()
    pg = int(s.get('hw.pagesize') or s.get('vm.pagesize') or 16384)
    vs = {}
    for m in re.finditer(r'^"?(Pages [a-z \-]+|Swapins|Swapouts|Pageouts|Compressions|Decompressions)"?:\s+(\d+)\.?$', txt, re.M):
        vs[m.group(1)] = int(m.group(2))
    mp = re.search(r'System-wide memory free percentage:\s*(\d+)%', txt)
    swap = re.search(r'used = ([0-9.]+)M', s.get('vm.swapusage', ''))
    mib = lambda n: n * pg / 1048576
    r = {
        'file': path, 'phase': s.get('phase', '?'), 'missing': len(re.findall(r'^MISSING ', txt, re.M)),
        'memsize_mib': int(s['hw.memsize']) / 1048576 if s.get('hw.memsize', '').isdigit() else None,
        'free_mib': mib(vs.get('Pages free', 0) + vs.get('Pages speculative', 0)) if vs else None,
        'wired_mib': mib(vs['Pages wired down']) if 'Pages wired down' in vs else None,
        'compressed_mib': mib(vs['Pages occupied by compressor']) if 'Pages occupied by compressor' in vs else None,
        'swapouts': vs.get('Swapouts'),
        'swap_used_mib': float(swap.group(1)) if swap else None,
        'free_pct': int(mp.group(1)) if mp else None,
        'pressure': s.get('kern.memorystatus_vm_pressure_level'),
        'gpu_pt': s.get('machdep.piano_gpu_pt'),
        'done': bool(re.search(r'^M0_DONE$', txt, re.M)),
    }
    return r

def fmt(v):
    return '-' if v is None else (f'{v:.0f}' if isinstance(v, float) else str(v))

def main(paths):
    rows = [parse(p) for p in paths]
    keys = ['phase', 'memsize_mib', 'free_mib', 'wired_mib', 'compressed_mib', 'swap_used_mib', 'swapouts', 'free_pct', 'pressure', 'gpu_pt', 'missing', 'done']
    for r in rows:
        print(' '.join(f'{k}={fmt(r[k])}' for k in keys))
    for r in rows:
        if r['memsize_mib'] is not None and abs(r['memsize_mib'] - WINDOW_MIB) > 64:
            print(f"NOTE {r['phase']}: hw.memsize {r['memsize_mib']:.0f} MiB is not the 13310 MiB window")
    hot = [r for r in rows if (r['swap_used_mib'] or 0) > 0 or (r['compressed_mib'] or 0) > 1024 or (r['pressure'] not in (None, '0', '1'))]
    print('M0 HINT: RAM pressure seen (' + ', '.join(r['phase'] for r in hot) + '): R2 worth a look' if hot else 'M0 HINT: no RAM pressure in these snapshots: R1 (GPU tier) first, R2 not needed')
    return 0 if rows and all(r['done'] for r in rows) else 1

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
