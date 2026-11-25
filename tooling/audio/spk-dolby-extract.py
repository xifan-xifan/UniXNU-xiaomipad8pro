#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""MIPLAY's private file for PianoAudio (package/PianoAudio/AudioSpkProt.h, docs/drivers/audio.md section 12.1), made on the owner's
Mac from the owner's own copies. Its contents, the map below and anything this tool reads are never committed, printed or put in a
published image; the tool prints counts and sizes only.

  spk-dolby-extract.py build ACDB DAX MAP OUT [--profile NAME]
        every parameter of the default calibration of the MIPLAY subgraph 0xb000019f of the deep-buffer speaker graph (graph keys
        STREAMRX 0xA1000000 = deep buffer 0xA1000001, DEVICERX 0xA2000000 = speaker 0xA2000001, INSTANCE 0xAB000000 = 1, DEVICEPP_RX
        0xAC000000 = MIPLAY 0xAC000100), after checking that the subgraph's graph data are the ones PianoAudio sends; the Dolby DAP's
        (0x411f) parameters replaced by the values the Dolby XML DAX (dax-default.xml) gives the speaker for the profile (default
        Music), built through MAP; written as spk-dolby.bin (mode 600)
  spk-dolby-extract.py verify ACDB DAX MAP LOGCAT [--profile Movie]
        the same build for the profile compared with what Android's Dolby HAL sent in LOGCAT (its setParam lines for the speaker 0x2
        and the globals 0x40000000 before a commit, the per-parameter lengths of the commits, the DSP form of the output matrix) and
        with the low-latency MIPLAY's ACDB output matrix: matches and mismatches counted, parameter names of mismatches listed, no
        value printed
  spk-dolby-extract.py check FILE           a spk-dolby.bin's structure and the driver's checks (counts and sizes)
  spk-dolby-extract.py --self-test          the reader, the builder, the writer and every check against synthetic data

MAP (private, never committed: a FourCC to DSP parameter table is the analysis's data) has one line per Dolby parameter, '#'
comments:  FOURCC PID SCOPE SOURCE SHAPE [if:ELEMENT]
  SCOPE   speaker (sent for the speaker device), global (sent for every device), none (never sent: PID and the rest '-')
  SOURCE  profile:ELEMENT   the profile's speaker endpoint_type element (speaker) or its data element (global)
          tuning:ELEMENT    the speaker tuning's data element (a name ending in '-' with SHAPE chans: the prefix of per-channel ones)
          ieq               the band list of the ieq preset the speaker endpoint includes
          geq:ID            the band list of graphic EQ preset ID
          const:V[,V...]    fixed values
          driver            the pregain: PianoAudio sends it (the file's header names its parameter id)
  SHAPE   bool | int | list | nlist (the count first) | attrs:A,B,.. (named attributes) | bands:n:A,B,.. or bands:nc:A,B,.. (the band
          count, with nc also the columns after the first, then each attribute across the bands) | chans:S1,S2,.. (the elements
          ELEMENT+S: their count, their length, then each one's values) | matrix:MODE (the mix_matrix element of MODE in the DSP's
          form: MODE, the tuning's output_channels, its 12 x output_channels coefficients) | -
  if:ELEMENT  only when the profile's speaker endpoint element is true
Every value is a 32-bit signed integer, little endian, as the DSP takes them; every built parameter must have exactly the size of the
same parameter in the ACDB.

spk-dolby.bin (little endian): u32 magic 'PMIP' (0x50494d50), u32 version 1, u32 count, u32 the DAP's pregain parameter id; then per
parameter u32 module instance id, u32 parameter id, u32 size, the size bytes zero-padded to 4. The ACDB container is read with
spk-prot-extract.py's reader (the AudioReach graph services' file format read as a specification).
"""
import argparse
import importlib.util
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

MAGIC, VERSION = 0x50494d50, 1
MAX_ENTRIES, MAX_PARAM = 200, 16384
MIP_GKV = {0xA1000000: 0xA1000001, 0xA2000000: 0xA2000001, 0xAB000000: 1, 0xAC000000: 0xAC000100}
LL_GKV = {0xA1000000: 0xA100000E, 0xA2000000: 0xA2000001, 0xAB000000: 1, 0xAC000000: 0xAC000100}
SG_MIP, SG_LL = 0xb000019f, 0xb00001b0
DAP, DAP_LL = 0x411f, 0x45b8
# AudioSpkProt.c's kMip* tables: instance -> (module, inputs, outputs), the links (from, port, to, port), the container
MODULES = {
    0x4026: (0x0700101A, 1, 1), 0x4351: (0x07001015, 1, 1), 0x4372: (0x0700101A, 1, 1), 0x4373: (0x07001010, 4, 1),
    0x4375: (0x07001038, 3, 4), 0x4377: (0x07001015, 1, 1), 0x437e: (0x0700101B, 1, 1), 0x4461: (0x1000a100, 1, 1),
    0x411f: (0x100108db, 1, 1), 0x4bc4: (0x1000a150, 1, 1), 0x41d9: (0x1000ad00, 1, 1), 0x4bdf: (0x10014000, 1, 1),
}
LINKS = {
    (0x4026, 1, 0x4351, 2), (0x4375, 5, 0x4373, 2), (0x4375, 7, 0x4373, 4), (0x4375, 9, 0x4373, 6), (0x4373, 1, 0x4026, 2),
    (0x4375, 0xb, 0x4373, 8), (0x4372, 1, 0x437e, 2), (0x437e, 1, 0x4377, 2), (0x4461, 1, 0x4372, 2), (0x4bc4, 1, 0x4461, 2),
    (0x411f, 1, 0x41d9, 2), (0x41d9, 1, 0x4bc4, 2), (0x4351, 1, 0x4bdf, 2), (0x4bdf, 1, 0x411f, 2),
}
CONTAINER = (0xe00000b3, {0x08001011: (1, 0x0B001000), 0x08001012: (2,), 0x08001013: (40960,), 0x08001014: (2,), 0x08001174: (1,),
                          0x080010CB: (0xffffffff,), 0x0800124D: (2,)})
SG_CONFIG = {0x0800100E: 1, 0x0800100F: 2, 0x08001010: 0xffffffff}
SHAPES = ('bool', 'int', 'list', 'nlist', 'attrs', 'bands', 'chans', 'matrix', '-')


class DolbyError(Exception):
    pass


def load_reader():
    path = Path(__file__).with_name('spk-prot-extract.py')
    spec = importlib.util.spec_from_file_location('spk_prot_extract', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


SPE = load_reader()


# ------------------------------------------------------------------------------------------------------------------ the ACDB
def spf_blocks(acdb, data_off, want_sg):
    """{parameter id: [u32 words]} of want_sg's graph-open parameters in the graph's subgraph data."""
    o = acdb.at('POOL', data_off)
    _, n = acdb.u32(o, 2)
    o += 8
    for _ in range(n):
        sg, size = acdb.u32(o, 2)
        o += 8
        end = o + size
        if sg == want_sg:
            p = o + 4 + acdb.u32(o)
            spf_end = p + 4 + acdb.u32(p)
            p += 4
            out = {}
            while p < spf_end:
                _, pid, psz = acdb.u32(p, 3)
                p += 12
                out[pid] = acdb.words(p, psz // 4)
                p += psz + (-psz % 8)
            return out
        o = end
    raise DolbyError(f'subgraph {want_sg:#x} has no data in this graph')


def check_graph(acdb):
    """The MIPLAY subgraph's graph data are exactly AudioSpkProt.c's (else PianoAudio would open another graph than this ACDB's)."""
    list_off, data_off = acdb.find_graph(MIP_GKV)
    if SG_MIP not in acdb.subgraphs(list_off):
        raise DolbyError(f'subgraph {SG_MIP:#x} is not in the deep-buffer speaker graph')
    b = spf_blocks(acdb, data_off, SG_MIP)
    w = b.get(0x08001001, [])
    props = {w[3 + 3 * k]: w[5 + 3 * k] for k in range(w[2])} if len(w) >= 3 and w[0] == 1 and w[1] == SG_MIP else {}
    if props != SG_CONFIG:
        raise DolbyError('the subgraph configuration is not the one PianoAudio sends')
    w = b.get(0x08001000, [])
    cont = {}
    if len(w) >= 3 and w[0] == 1 and w[1] == CONTAINER[0]:
        k = 3
        for _ in range(w[2]):
            pid, size = w[k], w[k + 1]
            cont[pid] = tuple(w[k + 2:k + 2 + size // 4])
            k += 2 + size // 4
    if cont != CONTAINER[1]:
        raise DolbyError('the container is not the one PianoAudio sends')
    w = b.get(0x08001002, [])
    mods = {}
    if len(w) >= 4 and w[0] == 1 and w[1] == SG_MIP and w[2] == CONTAINER[0]:
        mods = {w[5 + 2 * k]: w[4 + 2 * k] for k in range(w[3])}
    w = b.get(0x08001003, [])
    ports = {}
    for k in range(w[0] if w else 0):
        iid, n, pid, _, i, o = w[1 + 6 * k:7 + 6 * k]
        if n != 1 or pid != 0x08001015:
            raise DolbyError(f'instance {iid:#x}: module properties PianoAudio does not send')
        ports[iid] = (i, o)
    if {iid: (mods.get(iid),) + ports.get(iid, (None, None)) for iid in MODULES} != MODULES or len(mods) != len(MODULES):
        raise DolbyError('the modules or their ports are not the ones PianoAudio sends')
    w = b.get(0x08001004, [])
    links = {tuple(w[1 + 4 * k:5 + 4 * k]) for k in range(w[0] if w else 0)}
    if links != LINKS or (w and w[0] != len(LINKS)):
        raise DolbyError('the links are not the ones PianoAudio sends')
    if set(b) - {0x08001001, 0x08001000, 0x08001002, 0x08001003, 0x08001004}:
        raise DolbyError('graph data PianoAudio does not send (a control link or another block)')


def calibration(acdb):
    entries = acdb.default_cal(SG_MIP)
    for iid, pid, data in entries:
        if iid not in MODULES:
            raise DolbyError(f'instance {iid:#x} calibrated in MIPLAY but not one of its modules')
    return [(iid, pid, bytes(data)) for iid, pid, data in entries]


# ------------------------------------------------------------------------------------------------------------------- the map
def read_map(text):
    rows = []
    seen = set()
    for n, line in enumerate(text.splitlines(), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        f = line.split()
        if len(f) not in (5, 6):
            raise DolbyError(f'map line {n}: FOURCC PID SCOPE SOURCE SHAPE [if:ELEMENT]')
        fourcc, pid, scope, source, shape = f[:5]
        cond = f[5] if len(f) == 6 else None
        if fourcc in seen:
            raise DolbyError(f'map line {n}: {fourcc} twice')
        seen.add(fourcc)
        if scope not in ('speaker', 'global', 'none'):
            raise DolbyError(f'map line {n}: scope {scope}')
        if scope == 'none':
            rows.append({'fourcc': fourcc, 'scope': scope})
            continue
        if shape.split(':', 1)[0] not in SHAPES:
            raise DolbyError(f'map line {n}: shape {shape}')
        if cond is not None and not cond.startswith('if:'):
            raise DolbyError(f'map line {n}: {cond} (if:ELEMENT)')
        rows.append({'fourcc': fourcc, 'pid': int(pid, 0), 'scope': scope, 'source': source, 'shape': shape,
                     'if': cond[3:] if cond else None})
    pids = [r['pid'] for r in rows if 'pid' in r]
    if len(pids) != len(set(pids)):
        raise DolbyError('the map names a parameter id twice')
    if sum(1 for r in rows if r.get('source') == 'driver') > 1:
        raise DolbyError('the map names more than one driver parameter')
    return rows


# -------------------------------------------------------------------------------------------------------------- the Dolby XML
def ints(text, what):
    out = []
    for v in text.split(','):
        v = v.strip()
        if v in ('true', 'false'):
            out.append(1 if v == 'true' else 0)
        elif re.fullmatch(r'-?\d+', v):
            out.append(int(v))
        else:
            raise DolbyError(f'{what}: not an integer list')
    return out


class Dax:
    def __init__(self, data, profile):
        try:
            self.root = ET.fromstring(data)
        except ET.ParseError as e:
            raise DolbyError(f'the Dolby XML does not parse ({e})')
        profiles = [p for p in self.root.findall('profile') if p.get('name') == profile]
        if len(profiles) != 1:
            raise DolbyError(f'profile {profile!r}: {len(profiles)} in the XML')
        self.profile = profiles[0]
        self.global_data = self.profile.find('data')
        eps = [e for e in self.global_data.findall('endpoint_type') if e.get('id') == 'speaker'] if self.global_data is not None else []
        if len(eps) != 1:
            raise DolbyError(f'profile {profile!r}: no single speaker endpoint')
        self.endpoint = eps[0]
        tunings = [t for t in self.root.findall('tuning') if t.get('name') == 'speaker' and t.get('endpoint_type') == 'speaker']
        if len(tunings) != 1 or tunings[0].find('data') is None:
            raise DolbyError('no single speaker tuning')
        self.tuning = tunings[0]
        self.tuning_data = tunings[0].find('data')

    def element(self, where, name):
        e = where.find(name)
        if e is None:
            raise DolbyError(f'no element {name}')
        return e

    def preset(self, kind, pid):
        p = [x for x in self.root.findall('preset') if x.get('type') == kind and x.get('id') == str(pid)]
        if len(p) != 1:
            raise DolbyError(f'no single {kind} preset {pid}')
        return p[0]

    def flag(self, name):
        return ints(self.element(self.endpoint, name).get('value', ''), name) == [1]


def band_values(bands, spec, what):
    head, _, attrs = spec.partition(':')
    names = attrs.split(',')
    if head not in ('n', 'nc') or not bands or not all(names):
        raise DolbyError(f'{what}: bands:n:A,.. or bands:nc:A,..')
    cols = []
    for a in names:
        col = []
        for b in bands:
            if b.get(a) is None:
                raise DolbyError(f'{what}: a band without {a}')
            col += ints(b.get(a), what)
        cols.append(col)
    out = [len(bands)] + ([len(names) - 1] if head == 'nc' else [])
    for c in cols:
        out += c
    return out


def build_values(dax, row):
    source, shape = row['source'], row['shape']
    kind, _, arg = shape.partition(':')
    what = row['fourcc']
    if source.startswith('const:'):
        return ints(source[6:], what)
    if source == 'ieq':
        inc = dax.endpoint.find('include')
        if inc is None or inc.get('ieq_preset') is None:
            raise DolbyError(f'{what}: the speaker endpoint includes no ieq preset')
        bands = list(dax.preset('ieq', inc.get('ieq_preset')).iter('band_ieq'))
        if kind != 'bands':
            raise DolbyError(f'{what}: an ieq source needs bands')
        return band_values(bands, arg, what)
    if source.startswith('geq:'):
        if kind != 'bands':
            raise DolbyError(f'{what}: a geq source needs bands')
        return band_values(list(dax.preset('geq', source[4:]).iter('band_geq')), arg, what)
    scope, _, name = source.partition(':')
    if scope == 'profile':
        where = dax.endpoint if row['scope'] == 'speaker' else dax.global_data
    elif scope == 'tuning':
        where = dax.tuning_data
    else:
        raise DolbyError(f'{what}: source {source}')
    if kind == 'chans':
        lists = [ints(dax.element(where, name + s).get('value', ''), what) for s in arg.split(',')]
        if len({len(x) for x in lists}) != 1:
            raise DolbyError(f'{what}: channels of different lengths')
        return [len(lists), len(lists[0])] + [v for x in lists for v in x]
    e = dax.element(where, name)
    if kind == 'bands':
        return band_values(list(e), arg, what)
    if kind == 'matrix':
        nout = int(dax.tuning.get('output_channels', '0'))
        for m in e.iter('element'):
            v = ints(m.get('value', ''), what)
            if len(v) >= 2 and str(v[0]) == arg:
                if v[1] != len(v) - 2 or v[1] != 12 * nout:
                    raise DolbyError(f'{what}: the mode {arg} matrix is not 12 x {nout} coefficients')
                return [v[0], nout] + v[2:]
        raise DolbyError(f'{what}: no matrix of mode {arg}')
    if kind == 'attrs':
        out = []
        for a in arg.split(','):
            if e.get(a) is None:
                raise DolbyError(f'{what}: no attribute {a}')
            out += ints(e.get(a), what)
        return out
    v = ints(e.get('value', ''), what)
    if kind in ('bool', 'int'):
        if len(v) != 1:
            raise DolbyError(f'{what}: not one value')
        return v
    if kind == 'list':
        return v
    if kind == 'nlist':
        return [len(v)] + v
    raise DolbyError(f'{what}: shape {shape} for this source')


def dolby_params(dax, rows):
    """{pid: (fourcc, scope, values)} of the parameters the HAL sends for this profile and the speaker; the driver's pregain id."""
    out, preg = {}, 0
    for r in rows:
        if r['scope'] == 'none':
            continue
        if r['source'] == 'driver':
            preg = r['pid']
            continue
        if r['if'] and not dax.flag(r['if']):
            continue
        out[r['pid']] = (r['fourcc'], r['scope'], build_values(dax, r))
    return out, preg


def merge(cal, params, preg):
    """The calibration with the DAP's parameters replaced; each built one must have the ACDB's size for it."""
    sizes = {pid: len(d) for iid, pid, d in cal if iid == DAP}
    for pid, (fourcc, _, values) in params.items():
        if pid not in sizes:
            raise DolbyError(f'{fourcc}: the ACDB has no such DAP parameter')
        if 4 * len(values) != sizes[pid]:
            raise DolbyError(f'{fourcc}: {4 * len(values)} bytes, the ACDB has {sizes[pid]}')
        for v in values:
            if not -2 ** 31 <= v < 2 ** 31:
                raise DolbyError(f'{fourcc}: a value outside 32 bits')
    if preg and sizes.get(preg) != 4:
        raise DolbyError('the pregain is not a 4-byte DAP parameter of the ACDB')
    out, changed = [], 0
    for iid, pid, data in cal:
        if iid == DAP and pid in params:
            new = struct.pack(f'<{len(params[pid][2])}i', *params[pid][2])
            changed += new != data
            data = new
        out.append((iid, pid, data))
    return out, changed


def check_entries(entries, preg):
    if not 1 <= len(entries) <= MAX_ENTRIES:
        raise DolbyError(f'{len(entries)} parameters (1..{MAX_ENTRIES})')
    seen = set()
    for iid, pid, data in entries:
        if iid not in MODULES:
            raise DolbyError(f'instance {iid:#x} is not in the MIPLAY graph')
        if (iid, pid) in seen:
            raise DolbyError(f'{iid:#x}/{pid:#x} twice')
        seen.add((iid, pid))
        if not 1 <= len(data) <= MAX_PARAM:
            raise DolbyError(f'{iid:#x}/{pid:#x}: {len(data)} bytes (1..{MAX_PARAM})')
    if not any(i == DAP for i, _, _ in entries):
        raise DolbyError('no parameter of the Dolby DAP 0x411f')
    if preg and not any(i == DAP and p == preg and len(d) == 4 for i, p, d in entries):
        raise DolbyError('the pregain parameter is not one of the DAP\'s 4-byte parameters')


def pack(entries, preg):
    check_entries(entries, preg)
    out = bytearray(struct.pack('<4I', MAGIC, VERSION, len(entries), preg))
    for iid, pid, data in entries:
        out += struct.pack('<3I', iid, pid, len(data)) + data + bytes(-len(data) % 4)
    return bytes(out)


def unpack(blob):
    if len(blob) < 16:
        raise DolbyError('shorter than the header')
    magic, version, count, preg = struct.unpack_from('<4I', blob)
    if magic != MAGIC or version != VERSION:
        raise DolbyError('not a PMIP version 1 file')
    o, entries = 16, []
    for _ in range(count):
        if o + 12 > len(blob):
            raise DolbyError('truncated')
        iid, pid, size = struct.unpack_from('<3I', blob, o)
        o += 12
        padded = size + (-size % 4)
        if o + padded > len(blob):
            raise DolbyError('truncated parameter')
        entries.append((iid, pid, blob[o:o + size]))
        o += padded
    if o != len(blob):
        raise DolbyError('bytes after the last parameter')
    check_entries(entries, preg)
    return entries, preg


def summary(entries, preg):
    big = sum(1 for _, _, d in entries if 16 + len(d) + (-len(d) % 8) > 472)
    dap = [d for i, _, d in entries if i == DAP]
    return (f'{len(entries)} parameters, {sum(len(d) for _, _, d in entries)} bytes ({len(dap)} of the DAP, {sum(len(d) for d in dap)} bytes); '
            f'{big} out of band; pregain parameter {"named" if preg else "none"}')


def build(acdb_bytes, dax_bytes, map_text, profile):
    acdb = SPE.Acdb(acdb_bytes)
    check_graph(acdb)
    cal = calibration(acdb)
    rows = read_map(map_text)
    params, preg = dolby_params(Dax(dax_bytes, profile), rows)
    entries, changed = merge(cal, params, preg)
    return entries, preg, params, changed, rows


# ------------------------------------------------------------------------------------------------------------ verify (Android)
SETPARAM = re.compile(r'setParam\(device=0x([0-9a-fA-F]+), (\w+) = #(\d+)\[([-\d, ]*)')
COMMIT = re.compile(r'doCommit\(\) Committed (\d+) parameters out of (\d+)')
LENGTH = re.compile(r'payloadACDBTunnelParam: \d+: tag=0x[0-9a-fA-F]+ param id = 0x([0-9a-fA-F]+) param length=0x([0-9a-fA-F]+)')


def read_log(text, speaker_count, global_counts):
    """The values last set per (device, FourCC) before the full speaker commit, and the parameter lengths of that commit and of the
    full global one."""
    sets, lengths, window = {}, {}, {}
    done = {}
    for line in text.splitlines():
        m = SETPARAM.search(line)
        if m and 'speaker' not in done:
            dev, name, n, vals = int(m.group(1), 16), m.group(2), int(m.group(3)), m.group(4)
            got = [int(v) for v in vals.replace(' ', '').split(',') if v not in ('',)]
            sets[(dev, name)] = (n, got[:n])
            continue
        m = LENGTH.search(line)
        if m:
            window[int(m.group(1), 16)] = int(m.group(2), 16)
            continue
        m = COMMIT.search(line)
        if m:
            k, total = int(m.group(1)), int(m.group(2))
            for what, want in (('speaker', {speaker_count}), ('global', global_counts)):
                if what not in done and k == total and k in want and len(window) == k:
                    done[what] = True
                    lengths.update(window)
            window = {}
    return sets, lengths, done


def verify(acdb_bytes, dax_bytes, map_text, log_text, profile):
    entries, preg, params, changed, rows = build(acdb_bytes, dax_bytes, map_text, profile)
    sp = {pid: v for pid, v in params.items() if v[1] == 'speaker'}
    gl = {pid: v for pid, v in params.items() if v[1] == 'global'}
    sets, lengths, done = read_log(log_text, len(sp), {len(gl), len(gl) + (1 if preg else 0)})
    report, bad = [], []
    for scope, dev, table in (('speaker', 0x2, sp), ('global', 0x40000000, gl)):
        same = prefix = missing = 0
        for pid, (fourcc, _, values) in sorted(table.items()):
            got = sets.get((dev, fourcc))
            if got is None:
                missing += 1
                bad.append(f'{fourcc} (not in the log)')
                continue
            n, logged = got
            if n != len(values) and len(logged) == n:   # another form in the HAL (the output matrix): ours must be inside it
                form = [values[0], len(values) - 2] + values[2:]
                if any(logged[i:i + len(form)] == form for i in range(len(logged) - len(form) + 1)):
                    same += 1
                else:
                    bad.append(f'{fourcc} (not found in the HAL form)')
                continue
            if n != len(values):
                bad.append(f'{fourcc} (count)')
            elif logged == values:
                same += 1
            elif len(logged) < n and values[:len(logged)] == logged:
                prefix += 1
            else:
                bad.append(f'{fourcc} (values)')
        report.append(f'{scope}: {len(table)} built, {same} equal to the log, {prefix} equal to its (truncated) logged prefix, {missing} not logged')
    extra = sorted(name for (dev, name) in sets if dev == 0x2 and name not in {v[0] for v in sp.values()})
    report.append(f'speaker parameters the log has and the build does not: {len(extra)}' + (f' ({", ".join(extra)})' if extra else ''))
    for what in ('speaker', 'global'):
        report.append(f'the full {what} commit: {"found" if done.get(what) else "NOT found"}')
    size_ok = sum(1 for pid, n in lengths.items() if pid in params and 4 * len(params[pid][2]) == n)
    size_bad = sorted(params[pid][0] for pid, n in lengths.items() if pid in params and 4 * len(params[pid][2]) != n)
    report.append(f'committed lengths: {len(lengths)} logged, {size_ok} equal to the build' + (f'; DIFFERENT: {", ".join(size_bad)}' if size_bad else ''))
    bad += [f'{n} (committed length)' for n in size_bad]
    # the low-latency MIPLAY's ACDB holds the output matrix in the DSP's form: the same as the built one?
    acdb = SPE.Acdb(acdb_bytes)
    dom = [r for r in rows if r.get('shape', '').startswith('matrix:') and r['pid'] in params]
    if dom:
        ll = {(i, p): d for i, p, d in acdb.default_cal(SG_LL)}.get((DAP_LL, dom[0]['pid']))
        mine = struct.pack(f'<{len(params[dom[0]["pid"]][2])}i', *params[dom[0]['pid']][2])
        report.append(f'the output matrix against the low-latency MIPLAY\'s ACDB: {"equal" if ll == mine else "different" if ll else "absent"}')
    report.append(f'the file: {summary(entries, preg)}; {changed} DAP parameters differ from the ACDB defaults')
    return report, bad


# ------------------------------------------------------------------------------------------------------------------- self-test
def synthetic(dap_extra=(), modules=None, links=None, ctrl=False):
    """A minimal ACDB: the deep-buffer MIPLAY graph and its calibration, the low-latency MIPLAY's. Synthetic values only."""
    pool = bytearray()

    def put(b):
        off = len(pool)
        pool.extend(b)
        return off

    def blk(pid, words):
        body = struct.pack(f'<{len(words)}I', *words)
        return struct.pack('<3I', 1, pid, len(body)) + body + bytes(-len(body) % 8)

    mods = modules or MODULES
    lk = sorted(links or LINKS)
    cont = [CONTAINER[0], len(CONTAINER[1])]
    for pid, v in CONTAINER[1].items():
        cont += [pid, 4 * len(v)] + list(v)
    spf = blk(0x08001001, [1, SG_MIP, 3] + [w for k, v in SG_CONFIG.items() for w in (k, 4, v)])
    spf += blk(0x08001000, [1] + cont)
    spf += blk(0x08001002, [1, SG_MIP, CONTAINER[0], len(mods)] + [w for iid, (mid, _, _) in mods.items() for w in (mid, iid)])
    spf += blk(0x08001003, [len(mods)] + [w for iid, (_, i, o) in mods.items() for w in (iid, 1, 0x08001015, 8, i, o)])
    spf += blk(0x08001004, [len(lk)] + [w for t in lk for w in t])
    if ctrl:
        spf += blk(0x08001061, [0])
    drv = struct.pack('<2I', SG_MIP, 0)
    one = struct.pack('<I', len(drv)) + drv + struct.pack('<I', len(spf)) + spf
    g_list = put(struct.pack('<2I', 0, 2) + struct.pack('<2I', SG_MIP, 0) + struct.pack('<2I', 0xb0000002, 0))
    g_data = put(struct.pack('<2I', 0, 1) + struct.pack('<2I', SG_MIP, len(one)) + one)
    gkvl, tables = bytearray(), []
    for gkv, lo, do in ((MIP_GKV, g_list, g_data), (LL_GKV, 0, 0)):
        keys = sorted(gkv)
        off = len(gkvl)
        gkvl += struct.pack('<2I', len(keys), 1) + struct.pack(f'<{len(keys)}I', *[gkv[k] for k in keys]) + struct.pack('<2I', lo, do)
        tables.append((keys, off))
    gkvt = struct.pack('<I', 1) + struct.pack('<2I', 4, 2) + b''.join(struct.pack('<4I', *k) + struct.pack('<I', o) for k, o in tables)

    def words(*v):
        return struct.pack(f'<{len(v)}i', *v)
    cal = [(0x4026, 0x08001026, words(1)), (0x4351, 0x08001024, words(-1, -1)), (0x4351, 0x0800101F, bytes(40)),
           (DAP, 0x08001026, words(1)), (DAP, 0x20001, words(7)), (DAP, 0x20002, words(0)), (DAP, 0x20003, words(5, 5)),
           (DAP, 0x20004, words(*range(9))), (DAP, 0x20005, words(3, 0, 0, 0, 0, 0, 0)), (DAP, 0x20006, words(1, 1, *([0] * 12))),
           (DAP, 0x20007, words(1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)),
           (DAP, 0x20008, words(-239)), (DAP, 0x20009, bytes(4 * (2 + 3 * 20))), (DAP, 0x2000a, words(9)),
           (0x4bc4, 0x1000a150, words(1)), (0x41d9, 0x1000ad01, bytes(6)), (0x4bdf, 0x10014003, bytes(704))] + list(dap_extra)
    ll_cal = [(DAP_LL, 0x20006, words(3, 1, *range(12)))]
    cakt = bytearray(struct.pack('<I', 0))
    cdlu, cdde, cddo = bytearray(), bytearray(), bytearray()

    def table(c):
        de, do = len(cdde), len(cddo)
        cdde.extend(struct.pack('<I', len(c)) + b''.join(struct.pack('<2I', i, p) for i, p, _ in c))
        cddo.extend(struct.pack('<I', len(c)) + b''.join(struct.pack('<I', put(struct.pack('<I', len(d)) + d)) for _, _, d in c))
        lo = len(cdlu)
        cdlu.extend(struct.pack('<2I', 0, 1) + struct.pack('<3I', de, do, 0))
        return lo
    cslu = struct.pack('<I', 2) + struct.pack('<2I', SG_MIP, 1) + struct.pack('<2I', 0, table(cal)) + \
        struct.pack('<2I', SG_LL, 1) + struct.pack('<2I', 0, table(ll_cal))
    chunks = [('GKVT', gkvt), ('GKVL', gkvl), ('CSLU', cslu), ('CAKT', cakt), ('CDLU', cdlu), ('CDDE', cdde), ('CDDO', cddo),
              ('POOL', pool)]
    return bytes(12) + b''.join(c.encode() + struct.pack('<I', len(b)) + bytes(b) for c, b in chunks), cal


SYN_MAP = """# synthetic: names and ids of no real table
aaaa 0x20001 speaker profile:e-int int
aaab 0x20002 speaker profile:e-bool bool
aaac 0x20003 speaker tuning:t-attrs attrs:lo,hi
aaad 0x20004 speaker tuning:t-list list
aaae 0x20005 global profile:g-list nlist
aaaf 0x20006 speaker tuning:t-mix matrix:3
aaag 0x20007 speaker tuning:t-chan- chans:a,b,c,d
aaah 0x20008 global driver -
aaai 0x20009 speaker tuning:t-bands bands:nc:f,x,y if:e-bool
aaaj 0x2000a global const:4 -
aaak - none - -
"""
SYN_DAX = """<device_data>
  <preset id="1" type="ieq"><data><ieq-bands><band_ieq frequency="10" target="1"/></ieq-bands></data></preset>
  <profile id="0" name="Other"><data><g-list value="9"/><endpoint_type id="speaker"><e-int value="1"/></endpoint_type></data></profile>
  <profile id="1" name="Syn"><data><g-list value="1,2,3,4,5,6"/>
    <endpoint_type id="speaker"><include ieq_preset="1"/><e-int value="42"/><e-bool value="true"/></endpoint_type></data></profile>
  <tuning name="speaker" endpoint_type="speaker" output_channels="1"><data>
    <t-attrs lo="-3" hi="4"/><t-list value="0,1,2,3,4,5,6,7,8"/>
    <t-mix><mix_matrix><element value="1,2,9,9"/><element value="3,12,5,4,3,2,1,0,1,2,3,4,5,6"/></mix_matrix></t-mix>
    <t-chan-a value="1,2,3,4,5,6"/><t-chan-b value="1,2,3,4,5,6"/><t-chan-c value="1,2,3,4,5,6"/><t-chan-d value="1,2,3,4,5,6"/>
    <t-bands>""" + ''.join(f'<band f="{100 * k}" x="{-k}" y="{k}"/>' for k in range(20)) + """</t-bands>
  </data></tuning>
</device_data>"""


def self_test():
    checks = 0

    def check(cond, what):
        nonlocal checks
        if not cond:
            raise SystemExit(f'self-test FAILED: {what}')
        checks += 1

    def refused(fn, what):
        try:
            fn()
        except (DolbyError, SPE.AcdbError) as e:
            check(what in str(e), f'refused for {what} ({e})')
            return
        check(False, f'refused: {what}')
    blob, cal = synthetic()
    entries, preg, params, changed, rows = build(blob, SYN_DAX.encode(), SYN_MAP, 'Syn')
    check(len(rows) == 11 and preg == 0x20008, 'the map: eleven rows, the driver\'s pregain')
    check([(i, p) for i, p, _ in entries] == [(i, p) for i, p, _ in cal], 'the ACDB\'s parameters, all of them, in its order')
    got = {p: struct.unpack(f'<{len(d) // 4}i', d) for i, p, d in entries if i == DAP and len(d) % 4 == 0}
    check(got[0x20001] == (42,) and got[0x20002] == (1,) and got[0x20003] == (-3, 4) and got[0x20004] == tuple(range(9)), 'int, bool, attrs, list')
    check(got[0x20005] == (6, 1, 2, 3, 4, 5, 6) and got[0x2000a] == (4,), 'nlist and a constant (global)')
    check(got[0x20006] == (3, 1, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6), 'the matrix in the DSP form: mode, output channels, coefficients')
    check(got[0x20007] == (4, 6) + (1, 2, 3, 4, 5, 6) * 4, 'per-channel lists: count, length, the four')
    check(got[0x20009][:2] == (20, 2) and got[0x20009][2:22] == tuple(100 * k for k in range(20)) and got[0x20009][22] == 0 and
          got[0x20009][42] == 0 and got[0x20009][23] == -1, 'bands with the column count, then each column')
    check(got[0x20008] == (-239,) and 0x20008 not in params, 'the pregain stays the ACDB\'s (the driver sends its own)')
    check(changed == 8 and params[0x2000a][1] == 'global', 'eight DAP parameters changed (the list is the default)')
    data = pack(entries, preg)
    check(data[:4] == b'PMIP' and struct.unpack_from('<I', data, 12)[0] == 0x20008, 'the header names the pregain')
    check(unpack(data) == (entries, preg), 'pack/unpack round trip')
    check(len(data) == 16 + sum(12 + len(d) + (-len(d) % 4) for _, _, d in entries), 'the size')
    check('2 out of band' not in summary(entries, preg) and '1 out of band' in summary(entries, preg), 'the 704-byte parameter out of band')
    # the conditional parameter: off when its flag is false
    e2, _, p2, _, _ = build(blob, SYN_DAX.replace('<e-bool value="true"/>', '<e-bool value="false"/>').encode(), SYN_MAP, 'Syn')
    check(0x20009 not in p2 and 0x20002 in p2, 'if:ELEMENT leaves the parameter out when false')
    # refusals: the graph data, the map, the XML, sizes
    m = dict(MODULES)
    m[0x4375] = (0x07001038, 3, 3)
    refused(lambda: build(synthetic(modules=m)[0], SYN_DAX.encode(), SYN_MAP, 'Syn'), 'modules or their ports')
    refused(lambda: build(synthetic(links=LINKS - {(0x4026, 1, 0x4351, 2)})[0], SYN_DAX.encode(), SYN_MAP, 'Syn'), 'links')
    refused(lambda: build(synthetic(ctrl=True)[0], SYN_DAX.encode(), SYN_MAP, 'Syn'), 'control link')
    refused(lambda: build(synthetic(dap_extra=[(0x4012, 1, bytes(4))])[0], SYN_DAX.encode(), SYN_MAP, 'Syn'), 'not one of its modules')
    refused(lambda: build(blob, SYN_DAX.encode(), SYN_MAP, 'Nope'), 'profile')
    refused(lambda: build(blob, SYN_DAX.encode(), SYN_MAP.replace('t-list list', 't-list nlist'), 'Syn'), 'bytes, the ACDB has')
    refused(lambda: build(blob, SYN_DAX.encode(), SYN_MAP.replace('0x2000a', '0x2000b'), 'Syn'), 'no such DAP parameter')
    refused(lambda: build(blob, SYN_DAX.encode(), SYN_MAP + 'aaaa 0x20011 speaker profile:e-int int\n', 'Syn'), 'twice')
    refused(lambda: build(blob, SYN_DAX.encode(), SYN_MAP + 'zzzz 0x20001 speaker profile:e-int int\n', 'Syn'), 'parameter id twice')
    refused(lambda: build(blob, SYN_DAX.encode(), SYN_MAP + 'zzzz 0x20011 speaker profile:e-int wobble\n', 'Syn'), 'shape')
    refused(lambda: build(blob, SYN_DAX.replace('<t-attrs lo="-3" hi="4"/>', '').encode(), SYN_MAP, 'Syn'), 'no element')
    refused(lambda: build(blob, SYN_DAX.replace('value="42"', 'value="4x"').encode(), SYN_MAP, 'Syn'), 'not an integer')
    refused(lambda: build(blob, SYN_DAX.replace('<element value="3,12,', '<element value="3,11,').encode(), SYN_MAP, 'Syn'), 'coefficients')
    refused(lambda: build(blob, SYN_DAX.replace('<t-chan-d value="1,2,3,4,5,6"/>', '<t-chan-d value="1"/>').encode(), SYN_MAP, 'Syn'),
            'different lengths')
    refused(lambda: build(blob, b'<device_data', SYN_MAP, 'Syn'), 'does not parse')
    refused(lambda: merge(cal, {0x20001: ('aaaa', 'speaker', [2 ** 31])}, 0), 'outside 32 bits')
    refused(lambda: merge(cal, {}, 0x20003), 'pregain')
    for bad in (data[:-4], data + bytes(4), b'PMIQ' + data[4:], data[:12] + struct.pack('<I', 0x20004) + data[16:]):
        try:
            unpack(bad)
            check(False, 'a damaged file refused')
        except DolbyError:
            check(True, '')
    refused(lambda: pack([(0x4012, 1, b'1234')], 0), 'not in the MIPLAY graph')
    refused(lambda: pack([(0x4026, 1, b'1234')], 0), 'no parameter of the Dolby DAP')
    # verify against a synthetic HAL log: equal, a truncated prefix, the HAL's matrix form, the committed lengths
    sp = [v for v in params.values() if v[1] == 'speaker']
    log = [f'D DlbQdspGefDriver: setParam(device=0x00000002, {f} = #{len(v)}[{", ".join(map(str, v))}])' for f, _, v in sp if f not in ('aaaf', 'aaag')]
    log += ['D DlbQdspGefDriver: setParam(device=0x00000002, aaag = #26[4, 6, 1, 2, 3',
            'setParam(device=0x00000002, aaaf = #17[0, 1, 2, 3, 12, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6])']
    log += [f'setParam(device=0x40000000, {f} = #{len(v)}[{", ".join(map(str, v))}])' for f, s, v in params.values() if s == 'global']
    log += [f'payloadACDBTunnelParam: 1528: tag=0xc000033 param id = {p:#x} param length={4 * len(v):#x}' for p, (_, s, v) in params.items() if s == 'speaker']
    log += [f'doCommit() Committed {len(sp)} parameters out of {len(sp)}, with audio device id 0x2']
    report, bad = verify(blob, SYN_DAX.encode(), SYN_MAP, '\n'.join(log), 'Syn')
    check(not bad and any(f'speaker: {len(sp)} built, {len(sp) - 1} equal to the log, 1 equal to its (truncated)' in r for r in report),
          f'verify: all speaker parameters match ({bad}; {report})')
    check(any(f'{len(sp)} logged, {len(sp)} equal' in r for r in report) and any('full speaker commit: found' in r for r in report), 'verify: lengths')
    check(any('low-latency' in r and 'different' in r for r in report), 'verify: the low-latency matrix compared')
    log[0] = log[0].replace('[42]', '[43]')
    _, bad = verify(blob, SYN_DAX.encode(), SYN_MAP, '\n'.join(log), 'Syn')
    check(bad == ['aaaa (values)'], f'verify: a changed value found ({bad})')
    print(f'PASS spk-dolby-extract self-test: {checks} checks')


def write_private(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_bytes(data)
    tmp.chmod(0o600)
    tmp.replace(path)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--self-test', action='store_true')
    sub = p.add_subparsers(dest='command')
    s = sub.add_parser('build')
    for a in ('acdb', 'dax', 'map', 'out'):
        s.add_argument(a, type=Path)
    s.add_argument('--profile', default='Music')
    s = sub.add_parser('verify')
    for a in ('acdb', 'dax', 'map', 'log'):
        s.add_argument(a, type=Path)
    s.add_argument('--profile', default='Movie')
    s = sub.add_parser('check')
    s.add_argument('file', type=Path)
    a = p.parse_args()
    try:
        if a.self_test:
            return self_test()
        if a.command == 'build':
            entries, preg, params, changed, _ = build(a.acdb.read_bytes(), a.dax.read_bytes(), a.map.read_text(), a.profile)
            data = pack(entries, preg)
            write_private(a.out, data)
            n_sp = sum(1 for v in params.values() if v[1] == 'speaker')
            print(f'{a.out.name}: {len(data)} bytes, {summary(entries, preg)}; profile {a.profile}: {n_sp} speaker and '
                  f'{len(params) - n_sp} global Dolby parameters built, {changed} differ from the ACDB defaults')
        elif a.command == 'verify':
            report, bad = verify(a.acdb.read_bytes(), a.dax.read_bytes(), a.map.read_text(),
                                 a.log.read_text(errors='replace'), a.profile)
            for line in report:
                print(line)
            print(('MISMATCH: ' + ', '.join(bad)) if bad else f'PASS: profile {a.profile} rebuilt as Android sent it')
            return 1 if bad else 0
        elif a.command == 'check':
            entries, preg = unpack(a.file.read_bytes())
            print(f'{a.file.name}: {summary(entries, preg)}: the driver\'s checks pass')
        else:
            p.print_help()
            return 2
    except (DolbyError, SPE.AcdbError) as e:
        raise SystemExit(f'Error: {e}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
