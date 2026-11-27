#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Print one summary per ram-m0-tab.sh snapshot, then the M0 pressure hint."""
import re
import sys
from pathlib import Path

WINDOW_MIB = 13310
MIB = 1 << 20
TIER_COUNTERS = {
    'free-mib': 'tier_free_mib',
    'used-mib': 'tier_used_mib',
    'refusals': 'tier_refusals',
    'allocs': 'tier_allocs',
    'tier-import-denied': 'tier_import_denied',
    'tier-cpu-map-denied': 'tier_cpu_map_denied',
}
TIER_FAILURES = {
    'failed', 'closed', 'absent', 'bad length', 'region1 record refused', 'empty',
    'write-combine-disabled', 'cached-coherent', 'output-width',
}
KEYS = (
    'phase', 'memsize_mib', 'free_mib', 'wired_mib', 'compressed_mib',
    'swap_used_mib', 'swapouts', 'free_pct', 'pressure', 'gpu_pt', 'missing', 'done',
    'tier_mode', 'tier_state', 'tier_status', *TIER_COUNTERS.values(),
)


def uint(value):
    if re.fullmatch(r'[0-9]{1,20}', value or ''):
        number = int(value)
        if number < 1 << 64:
            return number
    return None


def unique_fields(lines, pattern):
    fields = {}
    for line in lines:
        match = re.fullmatch(pattern, line)
        if match:
            key, value = match.groups()
            key = key.strip('"')
            fields[key] = None if key in fields else value.strip()
    return fields


def page_size(*values):
    for value in values:
        size = uint(value)
        if size is not None and size >= 4096 and not size & (size - 1):
            return size
    return None


def parse_tier(sysctls):
    key = 'machdep.piano_gpu_tier'
    if key not in sysctls:
        return {'tier_status': 'missing'}
    invalid = {'tier_status': 'invalid'}
    parts = re.split(r'(?:^|[ \t]+)([a-z][a-z0-9-]*)=', sysctls[key] or '')
    fields = dict(zip(parts[1::2], parts[2::2]))
    if parts[0] or len(fields) != len(parts) // 2:
        return invalid
    if any(not re.fullmatch(r'[^\s=]+', value) for name, value in fields.items() if name != 'state'):
        return invalid

    mode, state = uint(fields.get('mode')), fields.get('state')
    counters = {label: uint(fields.get(field)) for field, label in TIER_COUNTERS.items()}
    if mode not in (0, 1, 2) or None in counters.values():
        return invalid
    if mode == 0 and state == 'off' and not any(counters.values()):
        status = 'off'
    elif mode in (1, 2) and state == 'ready' and (counters['tier_free_mib'] or counters['tier_used_mib']):
        status = 'ready'
    elif mode in (1, 2) and not any(counters.values()) and (state == 'pending' or state in TIER_FAILURES):
        status = 'pending' if state == 'pending' else 'failed'
    else:
        return invalid
    return {'tier_mode': mode, 'tier_state': state.replace(' ', '-'), 'tier_status': status, **counters}


def parse(path):
    lines = [line for line in Path(path).read_text(encoding='utf-8', errors='replace').split('\n') if line.strip()]
    sections = {'': []}
    section = ''
    for line in lines:
        if line.startswith('== '):
            section = line[3:]
            sections.setdefault(section, [])
        else:
            sections[section].append(line)

    # Older unsectioned fixture/capture files keep their single set of records.
    sysctl_lines = sections[''] + sections.get('meta', []) + sections.get('size', []) + sections.get('piano', [])
    sysctls = unique_fields(sysctl_lines, r'([a-z0-9_.]+)=(.*)')
    vm_stat = sections.get('vm_stat', sections[''])
    sizes = unique_fields(vm_stat, r'(Mach Virtual Memory Statistics): \(page size of (.*?) bytes\)')
    size = page_size(sizes.get('Mach Virtual Memory Statistics'), sysctls.get('hw.pagesize'), sysctls.get('vm.pagesize'))
    pages = unique_fields(vm_stat, r'("[^":]+"|[^":]+):[ \t]*(.*?)\.?[ \t]*')
    free, speculative = uint(pages.get('Pages free')), uint(pages.get('Pages speculative'))
    counts = unique_fields(sections.get('memory_pressure', sections['']),
                           r'(System-wide memory free percentage):[ \t]*(.*?)%[ \t]*')
    free_pct = uint(counts.get('System-wide memory free percentage'))
    swap = re.findall(r'(?<!\S)used[ \t]*=[ \t]*([0-9]{1,20}(?:\.[0-9]{1,20})?)M(?=$|[ \t])',
                      sysctls.get('vm.swapusage') or '')
    memsize = uint(sysctls.get('hw.memsize'))
    pressure = uint(sysctls.get('kern.memorystatus_vm_pressure_level'))

    def page_mib(count):
        return count * size / MIB if count is not None and size is not None else None

    return {
        'phase': sysctls.get('phase') or '?',
        'memsize_mib': memsize / MIB if memsize else None,
        'free_mib': page_mib(free + speculative) if free is not None and speculative is not None else None,
        'wired_mib': page_mib(uint(pages.get('Pages wired down'))),
        'compressed_mib': page_mib(uint(pages.get('Pages occupied by compressor'))),
        'swapouts': uint(pages.get('Swapouts')),
        'swap_used_mib': float(swap[0]) if len(swap) == 1 else None,
        'free_pct': free_pct if free_pct is not None and free_pct <= 100 else None,
        # The sysctl reports the dispatch pressure flags (1 normal, 2 warning, 4 critical).
        'pressure': pressure if pressure in (0, 1, 2, 4) else None,
        'gpu_pt': sysctls.get('machdep.piano_gpu_pt') or None,
        'missing': sum(line.startswith('MISSING ') for line in lines),
        'done': bool(lines and lines[-1] == 'M0_DONE' and lines.count('M0_DONE') == 1),
        **parse_tier(sysctls),
    }


def fmt(value):
    if value is None:
        return '-'
    return f'{value:.0f}' if isinstance(value, float) else str(value)


def main(paths):
    rows = []
    for path in paths:
        try:
            rows.append(parse(path))
        except OSError as error:
            print(f'M0: {error}', file=sys.stderr)
    for row in rows:
        print(' '.join(f'{key}={fmt(row.get(key))}' for key in KEYS))
    for row in rows:
        size = row['memsize_mib']
        if size is not None and abs(size - WINDOW_MIB) > 64:
            print(f"NOTE {row['phase']}: hw.memsize {size:.0f} MiB is not the {WINDOW_MIB} MiB window")
    complete = bool(rows and len(rows) == len(paths) and all(row['done'] for row in rows))
    hot = [row['phase'] for row in rows if row['done'] and (
        (row['swap_used_mib'] or 0) > 0 or (row['compressed_mib'] or 0) > 1024 or row['pressure'] in (2, 4))]
    if hot:
        print('M0 HINT: RAM pressure seen (' + ', '.join(hot) + '): R2 worth a look')
    elif not complete or any(
            row[key] is None for row in rows for key in ('swap_used_mib', 'compressed_mib', 'pressure')):
        print('M0 HINT: insufficient data to assess RAM pressure')
    else:
        print('M0 HINT: no RAM pressure in these snapshots: R1 (GPU tier) first, R2 not needed')
    return 0 if complete else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
