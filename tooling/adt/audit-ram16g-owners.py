#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Offline receipts for the 322 RAM16G slabs. Never authorizes memory release.

Inputs are read-efi-map-audit.py's complete text and retained owner console text.
Only whitelisted facts and public repository references are emitted; private
input paths and raw log lines are not. Static sources are read without writes.
There is no tablet, MMIO, SCM, RM, subprocess or network interface in this tool.
"""
import argparse
import hashlib
import json
import re
import sys
from collections import Counter
from pathlib import Path

PAGE = 4096
SLAB = 2 << 20
RUNTIME = 1 << 63
RANGES = {'CMA': (0xe3800000, 0xf3800000),
          'DXE': (0xbda00000, 0xd4e00000), 'FD': (0xa7000000, 0xa8000000)}
# Partition the documentation's combined tail/workspace rows for exact coverage.
CLASSES = (
    ('CMA/CNSS', 'CMA', 0xe3800000, 0xe5800000),
    ('CMA/QMC', 'CMA', 0xe5800000, 0xe6800000),
    ('CMA/VA-minidump', 'CMA', 0xe6800000, 0xe7800000),
    ('CMA/ADSP-heap', 'CMA', 0xe7800000, 0xe8400000),
    ('CMA/generic', 'CMA', 0xe8400000, 0xea800000),
    ('CMA/VM-comm', 'CMA', 0xea800000, 0xeb000000),
    ('CMA/QSEE-TA', 'CMA', 0xeb000000, 0xee800000),
    ('CMA/QSEE-client', 'CMA', 0xee800000, 0xf2800000),
    ('CMA/SP', 'CMA', 0xf2800000, 0xf3800000),
    ('DXE/lower-tail', 'DXE', 0xbda00000, 0xc0800000),
    ('DXE/secure-CDSP', 'DXE', 0xc0800000, 0xc5000000),
    ('DXE/display', 'DXE', 0xc5000000, 0xd4c00000),
    ('DXE/upper-tail', 'DXE', 0xd4c00000, 0xd4e00000),
    ('FD/image', 'FD', 0xa7000000, 0xa7400000),
    ('FD/reserved', 'FD', 0xa7400000, 0xa7600000),
    ('FD/mixed-runtime', 'FD', 0xa7600000, 0xa7800000),
    ('FD/workspace', 'FD', 0xa7800000, 0xa8000000),
)

# Reviewed tracked inputs, not upstream checkouts or a same-boot owner census.
# A changed/missing file invalidates its scoped proof until reviewed again.
SOURCES = {
    'running-dt': ('docs/hyp-evidence/memory-map-running-dt.txt',
                   'db825201fbea06a9f4beced20a081f98623dbe3aed31c8df6197cc1c45041f08'),
    'native-dts': ('dts/piano.dts',
                   'e0e3b0bc128af152ebf0be1994c973e2cfc0d0b5cb8b360d79c49196f10e5568'),
    'dt-streams': ('docs/hyp-evidence/iommus-running-dt.txt',
                   'd78e4bf143031fe9f3d9d42bd3148f59145ed859ee58a744ba30f66c860c31b0'),
    'firmware-policy': ('firmware/patches/darwinboot/region0-allow.json',
                        '63dff9f5bf7fb802366dd524ff404df60cadd1c29c38438b6ce123fedc224245'),
    'dpu-identity': ('package/PianoDpu/DpuSmmu.h',
                     '6e12ab4007e64f4e9a2ea2b29f3d2de4cd2c94d984ae32583ad36e84de0fb3ff'),
    'publication-contract': ('firmware/patches/darwinboot/0002-DarwinBoot-region1-extra-ram-reserve-and-publish.patch',
                             '8c057187bfa07505354f5268254ba7489047688e74520b136a89a1adfe9046dd'),
}
OPEN_PROOFS = ('R-CPU', 'R-SEC', 'R-VM', 'R-DMA', 'R-DUMP')
EXCLUSION_KINDS = ('dt-reserved', 'dt-hole', 'firmware-policy', 'smmu-reachable',
                   'dma-physical', 'hypervisor-physical')
DENY_KINDS = ('dt-reserved', 'dt-hole', 'firmware-policy', 'dma-physical', 'hypervisor-physical')


def intersects(a, b, c, d):
    return a < b and c < d and a < d and c < b


def static_inventory(root=None):
    """Read only fixed repository inputs; expose safe source IDs and numeric facts.

    Hashes bind this extraction to reviewed sources. Native DTS includes an
    untracked dtsi, so its four explicit reservations are only a partial view.
    Historical /memory, reservations and SID declarations never certify a live
    stage-2 mapping, an owner ACL, or the absence of dynamic borrowers.
    """
    root = Path(__file__).resolve().parents[2] if root is None else Path(root)
    texts, sources, exclusions, memory, streams = {}, {}, [], [], []
    for name, (path, digest) in SOURCES.items():
        try:
            data = (root / path).read_bytes()
            if hashlib.sha256(data).hexdigest() != digest:
                raise ValueError('unreviewed source')
            texts[name] = data.decode('utf-8')
            state = 'verified'
        except (OSError, UnicodeError, ValueError):
            state = 'unknown'
        sources[name] = dict(path=path, sha256=digest, status=state)

    def add(kind, base, end, source, record, owner='unknown', **facts):
        exclusions.append(dict(kind=kind, base=base, end=end, source=source,
                               record=record, owner=owner, **facts))

    for line, text in enumerate(texts.get('running-dt', '').splitlines(), 1):
        match = re.match(r'^(0x[0-9a-f]+) (0x[0-9a-f]+)\s+\S+\s+(RAM|RSV|HOLE)\s', text)
        if match:
            base, end = int(match[1], 16), int(match[2], 16)
            if match[3] == 'RAM':
                memory.append(dict(base=base, end=end, source='running-dt', line=line))
            else:
                add('dt-reserved' if match[3] == 'RSV' else 'dt-hole',
                    base, end, 'running-dt', line)
    if 'native-dts' in texts:
        # Explicit local reserved-memory entries only; never expand an include.
        for index, (base, end) in enumerate(((0xfc800000, 0xff300000), (0x80000000, 0x81200000),
                                            (0x81c60000, 0x81c80000), (0x81d00000, 0x81f00000))):
            add('dt-reserved', base, end, 'native-dts', index)
    if 'firmware-policy' in texts:
        policy = json.loads(texts['firmware-policy'])
        for index, row in enumerate(policy['never']):
            add('firmware-policy', int(row['base'], 16), int(row['end'], 16),
                'firmware-policy', index, 'firmware')
    if 'dpu-identity' in texts:
        add('smmu-reachable', 0x80000000, 0x100000000, 'dpu-identity', 0, 'DPU',
            smmu=0x15000000, sid=0x800, mask=0x2, scope='source-recipe-only')
    for line, text in enumerate(texts.get('dt-streams', '').splitlines(), 1):
        for match in re.finditer(r'iommu@([0-9a-f]+):(0x[0-9a-f]+) (0x[0-9a-f]+)', text):
            streams.append(dict(smmu=int(match[1], 16), sid=int(match[2], 16),
                                mask=int(match[3], 16), source='dt-streams', line=line))
        # Stock overlay declarations do not resolve the target SMMU or mask.
        match = re.search(r'target:(0x[0-9a-f]+)', text)
        if match:
            streams.append(dict(smmu=None, sid=int(match[1], 16), mask=None,
                                source='dt-streams', line=line))
    return dict(sources=sources, exclusions=exclusions, memory=memory, streams=streams,
                scope='tracked historical DT and native source recipes; no live owner census',
                native_dts_scope='explicit reservations only; included dtsi not audited')


def supplemental_exclusions(text):
    """An additive deny/reachability list; no input can assert a release proof.

    Addresses must be PA integers. Unknown owner names are deliberately reduced
    to 'unknown', so private labels and paths cannot enter the report.
    """
    def unique(pairs):
        result = dict(pairs)
        if len(result) != len(pairs):
            raise ValueError('duplicate exclusion fields')
        return result

    try:
        data = json.loads(text, object_pairs_hook=unique)
    except RecursionError:
        raise ValueError('excessively nested exclusions') from None
    if (not isinstance(data, dict) or set(data) != {'schema', 'exclusions'} or
            type(data['schema']) is not int or data['schema'] != 1 or
            not isinstance(data['exclusions'], list) or len(data['exclusions']) > 4096):
        raise ValueError('invalid exclusions')
    result = []
    for index, row in enumerate(data['exclusions']):
        if (not isinstance(row, dict) or not {'kind', 'base', 'end'} <= set(row) or
                set(row) - {'kind', 'base', 'end', 'owner'} or
                row['kind'] not in EXCLUSION_KINDS or
                type(row['base']) is not int or type(row['end']) is not int or
                not 0 <= row['base'] < row['end'] <= 1 << 64 or
                ('owner' in row and not isinstance(row['owner'], str))):
            raise ValueError('invalid physical exclusion')
        owner = row.get('owner', 'unknown')
        result.append(dict(kind=row['kind'], base=row['base'], end=row['end'],
                           owner=owner if owner in ('DPU', 'USB', 'UFS', 'DSP', 'WLAN', 'firmware') else 'unknown',
                           source='supplemental', record=index))
    return result


def covers(base, end, spans):
    cursor = base
    for row in sorted(spans, key=lambda row: row['base']):
        if row['base'] > cursor:
            break
        if row['end'] > cursor:
            cursor = row['end']
        if cursor >= end:
            return True
    return False


def bucket_dispositions(slabs, inventory):
    """Keep scoped static proofs separate from the unresolved live proofs."""
    missing = sorted(name for name, row in inventory['sources'].items() if row['status'] != 'verified')
    result = []
    for name, (base, end) in RANGES.items():
        group = [slabs[b] for b in range(base, end, SLAB)]
        non_runtime_ranges = []
        for row in group:
            if row['efi'] == 'non-runtime':
                if non_runtime_ranges and non_runtime_ranges[-1]['end'] == row['base']:
                    non_runtime_ranges[-1]['end'] = row['end']
                else:
                    non_runtime_ranges.append(dict(base=row['base'], end=row['end']))
        categories = {}
        for kind in EXCLUSION_KINDS:
            indices = [i for i, x in enumerate(inventory['exclusions'])
                       if x['kind'] == kind and intersects(base, end, x['base'], x['end'])]
            dependencies = {'dt-reserved': ('running-dt', 'native-dts'),
                            'dt-hole': ('running-dt',), 'firmware-policy': ('firmware-policy',),
                            'smmu-reachable': ('dpu-identity', 'dt-streams')}.get(kind)
            status = ('overlap' if indices else 'unknown' if dependencies is None or
                      any(s in missing for s in dependencies) else 'scoped-disjoint')
            categories[kind] = dict(status=status, exclusion_indices=indices, overlap_slabs=sum(
                any(i in row['static_matches'] for i in indices) for row in group))
        pages = Counter()
        for row in group:
            for segment in row['segments']:
                pages[str(segment['type'])] += (segment['end'] - segment['base']) // PAGE
        result.append(dict(name=name, base=base, end=end, total_slabs=len(group),
                           non_runtime_slabs=sum(r['efi'] == 'non-runtime' for r in group),
                           non_runtime_mib=2 * sum(r['efi'] == 'non-runtime' for r in group),
                           non_runtime_ranges=non_runtime_ranges,
                           efi_receipts=dict(Counter(r['R-EFI'] for r in group)),
                           disposition='HOLD', released_mib=0, admitted_slabs=0,
                           slab_dispositions=dict(Counter(r['disposition'] for r in group)),
                           static_checks=categories, missing_sources=missing,
                           dt_memory_covered_slabs=sum(covers(r['base'], r['end'], inventory['memory']) for r in group),
                           static_proof='unknown' if missing else 'scoped-only',
                           hypervisor=dict(hlos_ipa='unknown', other_vm_ipas='unknown',
                                           parcels='unknown', secure_acl='unknown'),
                           uefi=dict(type_pages=dict(pages), page_scope='descriptor-intersections',
                                     boot_services_reclamation='conditional-on-EBS-and-last-reference',
                                     ebs_receipt='unknown', cpu_last_reference='unknown',
                                     publication=('unknown' if any(s in missing for s in
                                                  ('firmware-policy', 'publication-contract')) else
                                                  'stage-2-blocked-by-stage-1-cap' if name == 'CMA' else
                                                  'excluded-by-tracked-policy')),
                           open_proofs=list(OPEN_PROOFS)))
    return result


def fields(line, expected):
    tokens = line.split()[1:]
    pairs = [token.split('=', 1) for token in tokens]
    if any(len(pair) != 2 for pair in pairs):
        raise ValueError('invalid map fields')
    result = dict(pairs)
    if len(result) != len(pairs) or set(result) != set(expected.split()):
        raise ValueError('missing, duplicate or unsupported map fields')
    return result


def number(value):
    if not re.fullmatch(r'(0x[0-9a-fA-F]+|[0-9]+)', value):
        raise ValueError('invalid map integer')
    return int(value, 16 if value.startswith('0x') else 10)


def classify(base, descriptors, gop):
    end = base + SLAB
    spans = sorted(((max(base, d['base']), min(end, d['end']), d)
                    for d in descriptors if intersects(base, end, d['base'], d['end'])),
                   key=lambda span: (span[0], span[1], span[2]['index']))
    overlap = any(a[1] > b[0] for a, b in zip(spans, spans[1:]))
    # Firmware counts unique covered pages, so an overlap cannot hide a gap.
    covered, cursor = 0, base
    for a, b, _ in spans:
        covered += max(0, b - max(a, cursor))
        cursor = max(cursor, b)
    gap = covered != SLAB
    rt_type = any(d['type'] in (5, 6) for _, _, d in spans)
    rt_attr = any(d['attr'] & RUNTIME for _, _, d in spans)
    full = not (gap or overlap)
    flags = dict(runtime_type=int(rt_type), runtime_attr=int(rt_attr), gap=int(gap),
                 overlap=int(overlap), conventional=int(full and all(d['type'] == 7 for _, _, d in spans)),
                 wb=int(full and all(d['attr'] & 8 for _, _, d in spans)),
                 gop_overlap=int(bool(gop and intersects(base, end, *gop))))
    flags['efi'] = 'runtime' if rt_type or rt_attr else 'non-runtime' if full else 'unknown'
    return flags, [dict(index=d['index'], base=a, end=b, type=d['type'], attr=d['attr'])
                   for a, b, d in spans]


def parse_map(text):
    headers, descriptors, rows, summaries, owners = [], [], {}, {}, {}
    slab_fields = 'range base end efi runtime_type runtime_attr gap overlap conventional wb gop_overlap'
    for line in text.splitlines():
        if not line.strip():
            continue
        if line.startswith('EFI_MAP '):
            headers.append(fields(line, 'status attempt key map_bytes stride version descriptors slabs'))
        elif line.startswith('EFI_DESC '):
            descriptors.append({k: number(v) for k, v in fields(line, 'index type base pages attr').items()})
        elif line.startswith('SLAB '):
            row = fields(line, slab_fields)
            row = {k: v if k in ('range', 'efi') else number(v) for k, v in row.items()}
            if row['base'] in rows:
                raise ValueError('duplicate slab')
            rows[row['base']] = row
        elif line.startswith('SUMMARY '):
            summary = fields(line, 'range slabs non_runtime runtime unknown gop_overlap')
            name = summary.pop('range')
            if name in summaries:
                raise ValueError('duplicate summary')
            summaries[name] = {k: number(v) for k, v in summary.items()}
        elif line.startswith('OWNER '):
            if line.startswith('OWNER kind=gop '):
                owner = fields(line, 'kind state base end')
                owner['base'], owner['end'] = number(owner['base']), number(owner['end'])
                if not 0 <= owner['base'] <= owner['end'] < 1 << 64:
                    raise ValueError('invalid GOP extent')
                if owner['state'] not in ('observed', 'unknown') or (
                        (owner['state'] == 'observed') != (owner['end'] > owner['base'])):
                    raise ValueError('inconsistent GOP state')
            else:
                owner = fields(line, 'kind state')
            if owner['kind'] in owners:
                raise ValueError('duplicate owner metadata')
            owners[owner['kind']] = owner
        else:
            raise ValueError('unsupported map record')
    if len(headers) != 1:
        raise ValueError('expected one complete map header')
    header = headers[0]
    if header.pop('status') != 'complete':
        raise ValueError('incomplete map')
    header = {k: number(v) for k, v in header.items()}
    if not (header['version'] == 1 and 1 <= header['attempt'] <= 3 and
            header['stride'] >= 40 and header['descriptors'] > 0 and
            header['map_bytes'] == header['stride'] * header['descriptors'] and
            header['slabs'] == 322 and header['descriptors'] == len(descriptors) and
            64 + len(descriptors) * 28 + 322 <= 32768):
        raise ValueError('invalid or truncated map metadata')
    if {d['index'] for d in descriptors} != set(range(len(descriptors))):
        raise ValueError('invalid descriptor indices')
    for d in descriptors:
        d['end'] = d['base'] + d['pages'] * PAGE
        if not (d['pages'] and d['base'] % PAGE == 0 and d['end'] < 1 << 64 and
                d['attr'] < 1 << 64 and d['type'] < 1 << 32):
            raise ValueError('invalid descriptor extent, type or attributes')
    expected = {b: name for name, (start, end) in RANGES.items() for b in range(start, end, SLAB)}
    if set(rows) != set(expected) or set(summaries) != set(RANGES):
        raise ValueError('missing or unexpected slabs/summaries')
    gop_owner = owners.get('gop', {})
    gop = (gop_owner['base'], gop_owner['end']) if gop_owner.get('state') == 'observed' else None
    for base, name in expected.items():
        row = rows[base]
        flags, segments = classify(base, descriptors, gop)
        if (row['range'] != name or row['end'] != base + SLAB or
                any(row[k] != v for k, v in flags.items())):
            raise ValueError('slab flags disagree with descriptors/GOP')
        row['segments'] = segments
        # Non-runtime is an observation of allocation type, never an owner proof.
        bad_type = any(s['type'] not in (1, 2, 3, 4, 7) for s in segments)
        if flags['efi'] == 'runtime' or flags['gop_overlap'] or base == 0xa7600000 or bad_type:
            row['R-EFI'] = 'excluded'
        elif flags['efi'] == 'unknown' or gop is None:
            row['R-EFI'] = 'unknown'
        elif not flags['wb'] or (name == 'CMA' and not flags['conventional']):
            row['R-EFI'] = 'excluded'
        else:
            row['R-EFI'] = 'observed'
    for name in RANGES:
        group = [r for r in rows.values() if r['range'] == name]
        counts = Counter(r['efi'] for r in group)
        actual = dict(slabs=len(group), non_runtime=counts['non-runtime'], runtime=counts['runtime'],
                      unknown=counts['unknown'], gop_overlap=sum(r['gop_overlap'] for r in group))
        if summaries[name] != actual:
            raise ValueError('summary disagrees with descriptors/slabs')
    return header, descriptors, rows


def parse_console(text, source):
    """Whitelist observations. IOVAs stay IOVAs; stages/calls never prove retirement."""
    observations = []

    def add(line, owner, kind, **facts):
        if kind == 'physical-buffer' and not 0 <= facts['base'] < facts['end'] <= 1 << 64:
            kind, facts = 'physical-buffer-unknown', {}
        observations.append(dict(owner=owner, kind=kind, source=source, line=line, **facts))

    for line_number, line in enumerate(text.splitlines(), 1):
        m = re.search(r'NativeUfs: EFI_MAP_DMA sid=0x60 phase=before-takeover '
                      r'mcq=([01]) transfer_iova=(0x[0-9a-f]+) transfer_slots=(\d+) '
                      r'transfer_run=(0x[0-9a-f]+) transfer_doorbell=(0x[0-9a-f]+) '
                      r'task_iova=(0x[0-9a-f]+) task_slots=(\d+) '
                      r'task_run=(0x[0-9a-f]+) task_doorbell=(0x[0-9a-f]+)', line)
        if m:
            values = [int(v, 0) for v in m.groups()]
            mcq, transfer, slots, run, doorbell, task, tasks, task_run, task_doorbell = values
            if 1 <= slots <= 32 and 1 <= tasks <= 8:
                add(line_number, 'UFS', 'inherited-queues', sid=0x60, mcq=mcq,
                    transfer_iova=transfer, transfer_bytes=slots * 32, transfer_run=run,
                    transfer_doorbell=doorbell, task_iova=task, task_bytes=tasks * 80,
                    task_run=task_run, task_doorbell=task_doorbell, physical_translation='unknown')
        if 'NativeUfs: EFI_MAP_DMA sid=0x60 phase=legacy-transfer-list-stopped' in line:
            add(line_number, 'UFS', 'transfer-list-stopped', sid=0x60)
        m = re.search(r'NativeUfs: DMA virt=0x[0-9a-f]+ phys=([0-9a-f]+) contiguous=([0-9a-f]+)', line)
        if m:
            base, size = (int(v, 16) for v in m.groups())
            add(line_number, 'UFS', 'physical-buffer', base=base, end=base + size)
        m = re.search(r'NativeUfs: SID 0x60 found on bank (\d+) '
                      r'\(sctlr ([0-9a-f]+) cbar ([0-9a-f]+) cba2r ([0-9a-f]+)\)', line)
        if m:
            bank, sctlr, cbar, cba2r = m.groups()
            add(line_number, 'UFS', 'inherited-route', sid=0x60, bank=int(bank),
                sctlr=int(sctlr, 16), cbar=int(cbar, 16), cba2r=int(cba2r, 16))
        m = re.search(r'PianoDwc3:.*GEVNTADR ([0-9a-f]{16}).*GEVNTSIZ ([0-9a-f]{8})'
                      r'.*GEVNTCOUNT ([0-9a-f]{8})', line)
        if m:
            address, size, count = (int(v, 16) for v in m.groups())
            add(line_number, 'USB', 'event-buffer', sid=0x40, iova=address,
                bytes=size & 0xffff, size_register=size, pending=count, physical_translation='unknown')
        m = re.search(r'PianoDwc3: SMMU stream 0x40:.*existing entry slot (\d+) '
                      r'\(smr ([0-9a-f]+) s2cr ([0-9a-f]+); bank sctlr ([0-9a-f]+) '
                      r'cbar ([0-9a-f]+) cba2r ([0-9a-f]+)\)', line)
        if m:
            slot, *words = m.groups()
            add(line_number, 'USB', 'stream-route', sid=0x40, slot=int(slot),
                **dict(zip(('smr', 's2cr', 'sctlr', 'cbar', 'cba2r'), (int(w, 16) for w in words))))
        m = re.search(r'PianoDwc3: DMA region (0x[0-9a-f]+) \(\+(0x[0-9a-f]+)\)', line)
        if m:
            base, size = (int(v, 16) for v in m.groups())
            add(line_number, 'USB', 'physical-buffer', base=base, end=base + size)
        m = re.search(r'PianoDpu: SMMU group (\d+) smr ([0-9a-f]+).*s2cr ([0-9a-f]+) '
                      r'.*cbar ([0-9a-f]+).*cba2r ([0-9a-f]+)', line)
        if m:
            group, *words = m.groups()
            smr, s2cr, cbar, cba2r = (int(w, 16) for w in words)
            matches = [sid for sid in (0x800, 0x801, 0x802, 0x803)
                       if smr & (1 << 31) and not ((sid ^ (smr & 0x7fff)) & ~(smr >> 16 & 0x7fff))]
            add(line_number, 'DPU', 'stream-route', group=int(group), smr=smr, s2cr=s2cr,
                cbar=cbar, cba2r=cba2r, matching_sids=matches)
        m = re.search(r'PianoDpu: pipe (\w+) rect (\d+): (\d+)x(\d+) @(\d+),(\d+) '
                      r'.*addr ([0-9a-f]+) stride (\d+)', line)
        if m:
            pipe, rect, width, height, x, y, address, stride = m.groups()
            add(line_number, 'DPU', 'plane', pipe=pipe, rect=int(rect), width=int(width),
                height=int(height), x=int(x), y=int(y), iova=int(address, 16), stride=int(stride),
                full_fetch_extent='unknown')
        m = re.search(r'PianoSensors: arena: (0x[0-9a-f]+) \+ (0x[0-9a-f]+).*mapped', line)
        if m:
            base, size = (int(v, 16) for v in m.groups())
            add(line_number, 'Sensors', 'physical-buffer', base=base, end=base + size)
        m = re.search(r'PianoPower: adsp: smc: PAS mem_setup \((DSP|DTB), '
                      r'(0x[0-9a-f]+) \+ (0x[0-9a-f]+)\): ok \(status 0,', line)
        if m:
            image, base, size = m.groups()
            base, size = int(base, 16), int(size, 16)
            add(line_number, 'ADSP/' + image, 'physical-buffer', base=base, end=base + size)
        m = re.search(r'PianoA830: t g7\.4 SCM c 42000202 3 d ([0-9a-f]+)', line)
        if m:
            add(line_number, 'GPU/ZAP', 'setup-call', base=int(m[1], 16), extent='unknown')
        if re.search(r'PianoA830: (?:t g7 ZAP end ok|row3 ZAP x0 0 x1 0)', line):
            add(line_number, 'GPU/ZAP', 'success-status')
        m = re.search(r'PianoVideo: start: piano-video=(\d+).*carveout=([01]).*'
                      r'carveout (0x[0-9a-f]+)', line)
        if m:
            add(line_number, 'Video', 'configuration', level=int(m[1]), carveout_enabled=int(m[2]),
                carveout_base=int(m[3], 16), arena_physical_extents='unknown')
        m = re.search(r'PianoVideo: V9 allocated arena (\d+).*table-pages (\d+)', line)
        if m:
            add(line_number, 'Video', 'arena-size', bytes=int(m[1]), table_pages=int(m[2]),
                physical_extents='unknown')
        m = re.search(r'PianoWifi: dma: (\d+) MiB at (0x[0-9a-f]+)', line)
        if m:
            size, base = int(m[1]) << 20, int(m[2], 16)
            add(line_number, 'WLAN', 'physical-buffer', base=base, end=base + size)
    return observations


def audit(map_text, owner_text, console_text='', run_label='unspecified', exclusions_text=None,
          source_root=None):
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', run_label):
        raise ValueError('invalid run label')
    header, descriptors, slabs = parse_map(map_text)
    inventory = static_inventory(source_root)
    if exclusions_text is not None:
        inventory['exclusions'].extend(supplemental_exclusions(exclusions_text))
    observations = parse_console(owner_text, 'owners') + parse_console(console_text, 'console')
    for row in slabs.values():
        row['native_overlaps'] = [i for i, o in enumerate(observations)
                                  if o['kind'] == 'physical-buffer' and
                                  intersects(row['base'], row['end'], o['base'], o['end'])]
        row['static_matches'] = [i for i, x in enumerate(inventory['exclusions'])
                                 if intersects(row['base'], row['end'], x['base'], x['end'])]
        denied = row['native_overlaps'] or any(inventory['exclusions'][i]['kind'] in DENY_KINDS
                                              for i in row['static_matches'])
        row['disposition'] = 'excluded' if row['R-EFI'] == 'excluded' or denied else 'unknown'
        row['open_proofs'] = list(OPEN_PROOFS)
    classes = []
    for name, region, start, end in CLASSES:
        group = [slabs[b] for b in range(start, end, SLAB)]
        efi = dict(Counter(r['R-EFI'] for r in group))
        dpu = any(o['owner'] == 'DPU' for o in observations)
        classes.append(dict(name=name, range=region, base=start, end=end, slabs=len(group),
                            receipts={'R-EFI': efi,
                                      'R-DMA': 'observed-owner' if any(r['native_overlaps'] for r in group)
                                      else 'partial' if observations else 'unknown',
                                      'R-DPU': 'partial' if dpu else 'unknown',
                                      'R-CPU': 'unknown', 'R-SEC': 'unknown',
                                      'R-VM': 'unknown', 'R-DUMP': 'unknown'},
                            descriptor_indices=sorted({s['index'] for r in group for s in r['segments']}),
                            native_overlap_slabs=sum(bool(r['native_overlaps']) for r in group),
                            scoped_physical_exclusions=[i for i, o in enumerate(observations)
                                                        if o['kind'] == 'physical-buffer' and
                                                        not intersects(start, end, o['base'], o['end'])]))
    return dict(schema=2, run_label=run_label, provenance='operator must verify same-boot input pairing',
                one_change='HOLD', gate='HOLD', released_mib=0, map=header, descriptors=descriptors,
                summary={region: dict(Counter(r['efi'] for r in slabs.values() if r['range'] == region))
                         for region in RANGES},
                static_inventory=inventory, bucket_dispositions=bucket_dispositions(slabs, inventory),
                classes=classes, observations=observations, slabs=list(slabs.values()))


def matrix(report):
    lines = [f"RAM16G {report['run_label']}: HOLD; released=0 MiB; 322 slabs",
             'ONE change: HOLD',
             'Observed means map fact only. Partial/unknown receipts cannot admit memory.',
             'R-DMA partial means scoped console evidence only; R-SEC/R-VM/R-CPU/R-DUMP remain unknown.',
             '',
             '| Bucket | Non-runtime slabs / MiB | DT reserved overlap slabs | Firmware policy overlap slabs | SMMU reachable slabs | Disposition |',
             '|---|---:|---:|---:|---:|---|']
    for row in report['bucket_dispositions']:
        checks = row['static_checks']
        lines.append(f"| {row['name']} | {row['non_runtime_slabs']} / {row['non_runtime_mib']} | "
                     f"{checks['dt-reserved']['overlap_slabs']} | {checks['firmware-policy']['overlap_slabs']} | "
                     f"{checks['smmu-reachable']['overlap_slabs']} | HOLD ({row['static_proof']}) |")
    lines += ['', 'Zero overlap is scoped to verified inputs; missing sources and live owners stay unknown.', '',
             '| Class | Slabs | R-EFI observed/excluded/unknown | R-DMA | R-DPU | R-CPU | R-SEC | R-VM |',
             '|---|---:|---|---|---|---|---|---|']
    for row in report['classes']:
        r = row['receipts']
        efi = '/'.join(str(r['R-EFI'].get(s, 0)) for s in ('observed', 'excluded', 'unknown'))
        lines.append(f"| {row['name']} | {row['slabs']} | {efi} | {r['R-DMA']} | {r['R-DPU']} | "
                     f"{r['R-CPU']} | {r['R-SEC']} | {r['R-VM']} |")
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--efi-map', required=True, type=Path)
    parser.add_argument('--owners-console', required=True, type=Path)
    parser.add_argument('--console', type=Path, help='optional fuller retained console, same boot')
    parser.add_argument('--exclusions', type=Path, help='optional additive physical exclusions JSON; never a release proof')
    parser.add_argument('--run-label', default='unspecified')
    parser.add_argument('--json', action='store_true', help='include descriptors, per-slab joins and numeric observations')
    args = parser.parse_args()
    try:
        report = audit(args.efi_map.read_text(), args.owners_console.read_text(),
                       args.console.read_text() if args.console else '', args.run_label,
                       args.exclusions.read_text() if args.exclusions else None)
    except (OSError, UnicodeError, ValueError):
        # Never echo a private input path or unrecognized raw input on failure.
        print('RAM16G audit: invalid/unreadable input; HOLD; released=0 MiB', file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2) if args.json else matrix(report))
    return 0


if __name__ == '__main__':
    sys.exit(main())
