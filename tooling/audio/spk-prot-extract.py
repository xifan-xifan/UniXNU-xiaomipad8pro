#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Speaker protection's two private files for PianoAudio (package/PianoAudio/AudioSpkProt.h, docs/drivers/audio.md section 12), made
on the owner's Mac from the owner's own copies. Their contents are never committed, printed or put in a published image.

  spk-prot-extract.py params ACDB OUT       every parameter of the default calibration (the table without calibration keys) of the
                                            V/I subgraph 0xb00000d5 (graph key DEVICETX 0xA3000000 = VI feedback 0xA3000009) and of
                                            the speaker device subgraph 0xb0000002 (STREAMRX 0xA1000000 = low latency 0xA100000E,
                                            DEVICERX 0xA2000000 = speaker 0xA2000001, INSTANCE 0xAB000000 = 1, DEVICEPP_RX
                                            0xAC000000 = MIPLAY 0xAC000100), for the nine module instances PianoAudio opens (not
                                            the SAL 0x4012 PianoAudio leaves out), written as spk-prot.bin
  spk-prot-extract.py re25 FSALGO_RDC OUT   the persist copy's fsalgo_rdc.bin (16 bytes: four u32 Re25 in Q12, CH0..CH3 = TDM slots
                                            0..3) checked (2..16 ohm each) and copied as spk-re25.bin
  spk-prot-extract.py check FILE            a spk-prot.bin's structure and the driver's checks (counts and sizes printed, no byte)
  spk-prot-extract.py --self-test           the reader, the writer and every check against a synthetic ACDB

spk-prot.bin (little endian): u32 magic 'PSPK' (0x4b505350), u32 version 1, u32 count, u32 0; then per parameter u32 module instance
id, u32 parameter id, u32 size, the size bytes zero-padded to 4. The driver refuses a file whose endpoint formats, lanes, V/I channel
map or MFC output differ from the values it sends itself; this tool refuses to write such a file (another ACDB than the one the
graph data in AudioSpkProt.h came from).

The ACDB container, as the AudioReach graph services' file format (BSD-3-Clause-Clear) defines it and read here as a specification:
a 12-byte header, then chunks {4-byte id, u32 length, data}. GKVT {u32 tables; {u32 nkeys, nentries; {keyid[nkeys], GKVL offset}}}
and GKVL {nkeys, nentries; {value[nkeys], subgraph-list POOL offset, subgraph-data POOL offset}} find a graph by its keys (sorted
ascending); POOL+list {size, n; {sg, ndst, dst[]}}; POOL+data {size, n; {sg, size; u32 driver size, driver props; u32 SPF size,
{iid, pid, size, data padded to 8}}} holds the APM graph-open parameters (MODULES_LIST 0x08001002: {n; {sg, container, n;
{module, iid}}}). Calibration: CSLU {n; {sg, ntables; {CAKT offset, CDLU offset}}}, CAKT {nkeys, keyid[]}, CDLU {nkeys, nrows;
{value[nkeys], CDDE offset, CDDO offset, list offset}}, CDDE {n; {iid, pid}}, CDDO {n; POOL offset}, POOL+offset {u32 size, data};
PPTC {n; persistent pid[]}.
"""
import argparse
import struct
import sys
from pathlib import Path

MAGIC, VERSION = 0x4b505350, 1
MAX_ENTRIES, MAX_PARAM = 64, 16384
VI_GKV = {0xA3000000: 0xA3000009}
SPK_GKV = {0xA1000000: 0xA100000E, 0xA2000000: 0xA2000001, 0xAB000000: 1, 0xAC000000: 0xAC000100}
SG_VI, SG_DEV = 0xb00000d5, 0xb0000002
# the modules PianoAudio opens (AudioSpkProt.c's tables): instance -> (module id, subgraph)
MODULES = {
    0x4587: (0x0700101A, SG_VI), 0x4763: (0x07001011, SG_VI), 0x45c2: (0x0700100F, SG_VI), 0x40ee: (0x10001110, SG_VI),
    0x4032: (0x07001015, SG_DEV), 0x4046: (0x10001111, SG_DEV), 0x418e: (0x07001011, SG_DEV), 0x4007: (0x0700101A, SG_DEV),
    0x4023: (0x0700100E, SG_DEV),
}
MF = struct.pack('<IHHI', 48000, 32, 4, 1)
# what the driver sends itself (AudioSpkProt.c kExpect): a file carrying these must carry exactly these bytes
EXPECT = {
    (0x45c2, 0x08001017): MF, (0x4023, 0x08001017): MF,
    (0x45c2, 0x0800106A): struct.pack('<I', 1), (0x4023, 0x0800106A): struct.pack('<I', 2),
    (0x45c2, 0x0800108A): struct.pack('<5H', 4, 48, 49, 50, 51),
    (0x4032, 0x08001024): struct.pack('<IHH4H', 48000, 32, 4, 1, 2, 4, 5),
}
TDM_INTF = 0x0800101B
RE25_MIN, RE25_MAX = 2 * 4096, 16 * 4096


class AcdbError(Exception):
    pass


class Acdb:
    def __init__(self, data):
        self.d = data
        self.chunks = {}
        off = 12
        while off + 8 <= len(data):
            cid = data[off:off + 4].decode('latin1')
            length = self.u32(off + 4)
            if off + 8 + length > len(data):
                raise AcdbError(f'chunk {cid!r} runs past the end')
            self.chunks[cid] = (off + 8, length)
            off += 8 + length
        for need in ('GKVT', 'GKVL', 'POOL', 'CSLU', 'CAKT', 'CDLU', 'CDDE', 'CDDO'):
            if need not in self.chunks:
                raise AcdbError(f'no {need} chunk: not an ACDB of this format')

    def u32(self, off, n=1):
        if off < 0 or off + 4 * n > len(self.d):
            raise AcdbError(f'read past the end at {off:#x}')
        v = struct.unpack_from(f'<{n}I', self.d, off)
        return v if n > 1 else v[0]

    def words(self, off, n):
        return list(self.u32(off, n)) if n > 1 else ([self.u32(off)] if n == 1 else [])

    def at(self, chunk, off=0):
        base, length = self.chunks[chunk]
        if off >= length:
            raise AcdbError(f'{chunk}+{off:#x} outside the chunk')
        return base + off

    def find_graph(self, gkv):
        keys = tuple(sorted(gkv))
        vals = tuple(gkv[k] for k in keys)
        o = self.at('GKVT')
        tables = self.u32(o)
        o += 4
        for _ in range(tables):
            nk, ne = self.u32(o, 2)
            o += 8
            for e in range(ne):
                row = self.words(o + e * (nk + 1) * 4, nk + 1)
                if nk == len(keys) and tuple(row[:nk]) == keys:
                    lut = self.at('GKVL', row[nk])
                    lk, ln = self.u32(lut, 2)
                    for r in range(ln):
                        v = self.words(lut + 8 + r * (lk + 2) * 4, lk + 2)
                        if tuple(v[:lk]) == vals:
                            return v[lk], v[lk + 1]
            o += ne * (nk + 1) * 4
        raise AcdbError('graph keys ' + ' '.join(f'{k:#x}={v:#x}' for k, v in sorted(gkv.items())) + ' not found')

    def subgraphs(self, list_off):
        o = self.at('POOL', list_off)
        _, n = self.u32(o, 2)
        o += 8
        out = []
        for _ in range(n):
            sg, nd = self.u32(o, 2)
            out.append(sg)
            o += 8 + 4 * nd
        return out

    def modules(self, data_off, want_sg):
        """The (module, iid) pairs of want_sg's MODULES_LIST in the graph's subgraph data."""
        o = self.at('POOL', data_off)
        _, n = self.u32(o, 2)
        o += 8
        for _ in range(n):
            sg, size = self.u32(o, 2)
            o += 8
            end = o + size
            if sg == want_sg:
                p = o + 4 + self.u32(o)          # past the driver properties
                spf_end = p + 4 + self.u32(p)
                p += 4
                mods = []
                while p < spf_end:
                    _, pid, psz = self.u32(p, 3)
                    p += 12
                    if pid == 0x08001002:
                        q = p
                        for _ in range(self.u32(q)):
                            _, _, nm = self.u32(q + 4, 3)
                            pairs = self.words(q + 16, 2 * nm)
                            mods += list(zip(pairs[0::2], pairs[1::2]))
                            q += 12 + 8 * nm
                    p += psz + (-psz % 8)
                return mods
            o = end
        raise AcdbError(f'subgraph {want_sg:#x} has no data in this graph')

    def persistent(self):
        if 'PPTC' not in self.chunks:
            return set()
        o = self.at('PPTC')
        return set(self.words(o + 4, self.u32(o)))

    def default_cal(self, want_sg):
        """[(iid, pid, bytes)] of want_sg's calibration table without keys."""
        o = self.at('CSLU')
        n = self.u32(o)
        o += 4
        for _ in range(n):
            sg, ne = self.u32(o, 2)
            o += 8
            tables = [self.u32(o + 8 * k, 2) for k in range(ne)]
            o += 8 * ne
            if sg != want_sg:
                continue
            for cakt, cdlu in tables:
                if self.u32(self.at('CAKT', cakt)):
                    continue                     # keyed by a calibration key: not the default table
                lut = self.at('CDLU', cdlu)
                nk, rows = self.u32(lut, 2)
                if nk or rows != 1:
                    raise AcdbError(f'subgraph {want_sg:#x}: a default table with {rows} rows')
                cdde, cddo, _ = self.u32(lut + 8, 3)
                de, do = self.at('CDDE', cdde), self.at('CDDO', cddo)
                nd, nt = self.u32(de), self.u32(do)
                out = []
                for k in range(nd):
                    iid, pid = self.u32(de + 4 + 8 * k, 2)
                    pool = self.u32(do + 4 + 4 * k) if k < nt else 0xffffffff
                    if pool == 0xffffffff:
                        continue                 # no data for this pair
                    po = self.at('POOL', pool)
                    size = self.u32(po)
                    out.append((iid, pid, self.d[po + 4:po + 4 + size]))
                return out
            raise AcdbError(f'subgraph {want_sg:#x} has no default calibration table')
        raise AcdbError(f'subgraph {want_sg:#x} has no calibration')


def collect(acdb):
    """The entries for the two subgraphs, in the ACDB's order (V/I first), after every structural check."""
    entries = []
    persistent = acdb.persistent()
    for gkv, sg in ((VI_GKV, SG_VI), (SPK_GKV, SG_DEV)):
        list_off, data_off = acdb.find_graph(gkv)
        if sg not in acdb.subgraphs(list_off):
            raise AcdbError(f'subgraph {sg:#x} is not in its graph')
        mods = dict((iid, mid) for mid, iid in acdb.modules(data_off, sg))
        for iid, (mid, msg) in MODULES.items():
            if msg == sg and mods.get(iid) != mid:
                raise AcdbError(f'subgraph {sg:#x}: instance {iid:#x} is not module {mid:#x} (the graph data PianoAudio sends is not this ACDB\'s)')
        for iid, pid, data in acdb.default_cal(sg):
            if iid not in MODULES:
                continue                         # a module PianoAudio does not open (SAL)
            if MODULES[iid][1] != sg:
                raise AcdbError(f'instance {iid:#x} calibrated in the wrong subgraph')
            if pid in persistent:
                raise AcdbError(f'{iid:#x}/{pid:#x} is a persistent parameter (not supported)')
            entries.append((iid, pid, bytes(data)))
    check_entries(entries)
    return entries


def check_entries(entries):
    if not 1 <= len(entries) <= MAX_ENTRIES:
        raise AcdbError(f'{len(entries)} parameters (1..{MAX_ENTRIES})')
    seen = set()
    for iid, pid, data in entries:
        if iid not in MODULES:
            raise AcdbError(f'instance {iid:#x} is not in the protection graphs')
        if pid == TDM_INTF:
            raise AcdbError('a TDM interface configuration (the driver sends its own)')
        if (iid, pid) in seen:
            raise AcdbError(f'{iid:#x}/{pid:#x} twice')
        seen.add((iid, pid))
        if not 1 <= len(data) <= MAX_PARAM:
            raise AcdbError(f'{iid:#x}/{pid:#x}: {len(data)} bytes (1..{MAX_PARAM})')
        want = EXPECT.get((iid, pid))
        if want is not None and data != want:
            raise AcdbError(f'{iid:#x}/{pid:#x}: not the format, lane or map PianoAudio sends')
    if not any(i == 0x4046 for i, _, _ in entries) or not any(i == 0x40ee for i, _, _ in entries):
        raise AcdbError('no SP_RX 0x4046 or SP_TX 0x40ee parameter')


def pack(entries):
    out = bytearray(struct.pack('<4I', MAGIC, VERSION, len(entries), 0))
    for iid, pid, data in entries:
        out += struct.pack('<3I', iid, pid, len(data)) + data + bytes(-len(data) % 4)
    return bytes(out)


def unpack(blob):
    if len(blob) < 16:
        raise AcdbError('shorter than the header')
    magic, version, count, zero = struct.unpack_from('<4I', blob)
    if magic != MAGIC or version != VERSION or zero:
        raise AcdbError('not a PSPK version 1 file')
    o, entries = 16, []
    for _ in range(count):
        if o + 12 > len(blob):
            raise AcdbError('truncated')
        iid, pid, size = struct.unpack_from('<3I', blob, o)
        o += 12
        padded = size + (-size % 4)
        if o + padded > len(blob):
            raise AcdbError('truncated parameter')
        entries.append((iid, pid, blob[o:o + size]))
        o += padded
    if o != len(blob):
        raise AcdbError('bytes after the last parameter')
    check_entries(entries)
    return entries


def check_re25(data):
    if len(data) != 16:
        raise AcdbError(f'{len(data)} bytes, not 16')
    for c, r in enumerate(struct.unpack('<4I', data)):
        if not RE25_MIN <= r <= RE25_MAX:
            raise AcdbError(f'channel {c} outside 2..16 ohm')


def summary(entries):
    big = sum(1 for _, _, d in entries if 16 + len(d) + (-len(d) % 8) > 472)
    per = {}
    for iid, _, d in entries:
        per.setdefault(MODULES[iid][1], [0, 0])
        per[MODULES[iid][1]][0] += 1
        per[MODULES[iid][1]][1] += len(d)
    return (f'{len(entries)} parameters, {sum(len(d) for _, _, d in entries)} bytes ('
            + ', '.join(f'subgraph {sg:#x}: {n} / {b} bytes' for sg, (n, b) in sorted(per.items())) + f'); {big} out of band')


def write_private(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_bytes(data)
    tmp.chmod(0o600)
    tmp.replace(path)


# ------------------------------------------------------------------------------------------------------------------- self-test
def synthetic_acdb(modules_override=None, cal_extra=(), lane_vi=1, keyed_only=False):
    """A minimal ACDB in this format: the two graphs, their module lists, a default and a keyed calibration table. Synthetic
    values only."""
    pool = bytearray()

    def pool_put(b):
        off = len(pool)
        pool.extend(b)
        return off

    def sg_data(sg, mods):
        spf = bytearray()
        lists = {}
        for mid, iid in mods:
            lists.setdefault(0xe0000000 | (iid & 0xfff), []).append((mid, iid))
        body = struct.pack('<I', len(lists))
        for cont, ms in lists.items():
            body += struct.pack('<3I', sg, cont, len(ms)) + b''.join(struct.pack('<2I', m, i) for m, i in ms)
        spf += struct.pack('<3I', 1, 0x08001002, len(body)) + body + bytes(-len(body) % 8)
        spf += struct.pack('<3I', 1, 0x08001004, 4) + struct.pack('<I', 0) + bytes(4)
        drv = struct.pack('<2I', sg, 0)
        one = struct.pack('<I', len(drv)) + drv + struct.pack('<I', len(spf)) + spf
        return struct.pack('<2I', sg, len(one)) + one

    vi_mods = [(m, i) for i, (m, s) in MODULES.items() if s == SG_VI]
    dev_mods = [(0x07001010, 0x4012)] + [(m, i) for i, (m, s) in MODULES.items() if s == SG_DEV]
    if modules_override:
        dev_mods = modules_override
    g1_list = pool_put(struct.pack('<2I', 0, 1) + struct.pack('<2I', SG_VI, 0))
    g1_data = pool_put(struct.pack('<2I', 0, 1) + sg_data(SG_VI, vi_mods))
    sgs = [(0xb0000023, [(0x07001000, 0x40bd)]), (SG_DEV, dev_mods)]
    g2_list = pool_put(struct.pack('<2I', 0, 2) + b''.join(struct.pack('<2I', s, 0) for s, _ in sgs))
    g2_data = pool_put(struct.pack('<2I', 0, 2) + b''.join(sg_data(s, m) for s, m in sgs))
    # GKVT / GKVL: one table per key count
    gkvl = bytearray()
    gkvt_tables = []
    for gkv, lo, do in ((VI_GKV, g1_list, g1_data), (SPK_GKV, g2_list, g2_data), ({0xA3000000: 0xA3000001}, 0, 0)):
        keys = sorted(gkv)
        off = len(gkvl)
        gkvl += struct.pack('<2I', len(keys), 1) + struct.pack(f'<{len(keys)}I', *[gkv[k] for k in keys]) + struct.pack('<2I', lo, do)
        gkvt_tables.append((keys, off))
    gkvt = struct.pack('<I', len(gkvt_tables)) + b''.join(struct.pack('<2I', len(k), 1) + struct.pack(f'<{len(k)}I', *k) +
                                                         struct.pack('<I', o) for k, o in gkvt_tables)

    def val(n, seed):
        return bytes((seed * 7 + i * 13) & 0xff for i in range(n))
    vi_cal = [(0x40ee, 0x10001fc2, val(8, 1)), (0x40ee, 0x11111601, struct.pack('<I', 1)), (0x4587, 0x08001031, val(12, 2)),
              (0x45c2, 0x08001017, MF), (0x45c2, 0x0800106A, struct.pack('<I', lane_vi)),
              (0x45c2, 0x0800108A, struct.pack('<5H', 4, 48, 49, 50, 51))]
    dev_cal = [(0x4012, 0x08001016, val(4, 3)), (0x4023, 0x0800106A, struct.pack('<I', 2)), (0x4032, 0x08001024, EXPECT[(0x4032, 0x08001024)]),
               (0x4046, 0x10001fb0, val(6180, 4)), (0x4046, 0x11111611, struct.pack('<I', 1)), (0x418e, 0x0800103C, val(60, 5))]
    dev_cal += list(cal_extra)
    cakt = bytearray(struct.pack('<I', 0))                 # offset 0: no keys
    keyed = len(cakt)
    cakt += struct.pack('<2I', 1, 0xA4000000)
    cdlu, cdde, cddo = bytearray(), bytearray(), bytearray()

    def table(cal, nkeys):
        de, do = len(cdde), len(cddo)
        cdde.extend(struct.pack('<I', len(cal)) + b''.join(struct.pack('<2I', i, p) for i, p, _ in cal))
        cddo.extend(struct.pack('<I', len(cal)) + b''.join(struct.pack('<I', pool_put(struct.pack('<I', len(d)) + d)) for _, _, d in cal))
        lo = len(cdlu)
        cdlu.extend(struct.pack('<2I', nkeys, 1) + (struct.pack('<I', 0xf) if nkeys else b'') + struct.pack('<3I', de, do, 0))
        return lo
    cslu = bytearray(struct.pack('<I', 2))
    for sg, cal in ((SG_VI, vi_cal), (SG_DEV, dev_cal)):
        decoy = table([(i, p, bytes(len(d))) for i, p, d in cal[:1]], 1)
        tables = [(keyed, decoy)] + ([] if keyed_only else [(0, table(cal, 0))])
        cslu += struct.pack('<2I', sg, len(tables)) + b''.join(struct.pack('<2I', a, b) for a, b in tables)
    chunks = [('GKVT', gkvt), ('GKVL', gkvl), ('CSLU', cslu), ('CAKT', cakt), ('CDLU', cdlu), ('CDDE', cdde), ('CDDO', cddo),
              ('PPTC', struct.pack('<2I', 1, 0x080011d4)), ('POOL', pool)]
    return bytes(12) + b''.join(c.encode() + struct.pack('<I', len(b)) + bytes(b) for c, b in chunks), vi_cal, dev_cal


def self_test():
    checks = 0

    def check(cond, what):
        nonlocal checks
        if not cond:
            raise SystemExit(f'self-test FAILED: {what}')
        checks += 1
    blob, vi_cal, dev_cal = synthetic_acdb()
    entries = collect(Acdb(blob))
    want = vi_cal + [e for e in dev_cal if e[0] != 0x4012]
    check(entries == [(i, p, bytes(d)) for i, p, d in want], 'the default tables of both subgraphs, SAL left out, in order')
    packed = pack(entries)
    check(unpack(packed) == entries, 'pack/unpack round trip')
    check(packed[:4] == b'PSPK' and struct.unpack_from('<I', packed, 8)[0] == len(entries), 'the header')
    check(len(packed) == 16 + sum(12 + len(d) + (-len(d) % 4) for _, _, d in entries), 'the size')
    check('1 out of band' in summary(entries), 'the summary counts the 6180-byte parameter out of band')
    for bad, what in ((synthetic_acdb(lane_vi=2)[0], 'lane'), (synthetic_acdb(keyed_only=True)[0], 'default calibration'),
                      (synthetic_acdb(modules_override=[(0x07001015, 0x4032)])[0], 'is not module'),
                      (synthetic_acdb(cal_extra=[(0x4023, TDM_INTF, bytes(28))])[0], 'TDM interface'),
                      (synthetic_acdb(cal_extra=[(0x4046, 0x11111611, bytes(4))])[0], 'twice'),
                      (synthetic_acdb(cal_extra=[(0x4023, 0x080011d4, bytes(4))])[0], 'persistent'),
                      (synthetic_acdb(cal_extra=[(0x4046, 0x10002000, bytes(MAX_PARAM + 1))])[0], 'bytes'),
                      (blob[:40], 'chunk'), (bytes(12) + b'POOL' + struct.pack('<I', 0), 'no GKVT')):
        try:
            collect(Acdb(bad))
            check(False, f'refused: {what}')
        except AcdbError as e:
            check(what in str(e), f'refused for {what} ({e})')
    for bad in (packed[:-4], packed + bytes(4), b'PSPQ' + packed[4:], packed[:8] + struct.pack('<I', 65) + packed[12:]):
        try:
            unpack(bad)
            check(False, 'a damaged file refused')
        except AcdbError:
            check(True, '')
    re = struct.pack('<4I', 27000, 27500, 28000, 28500)
    check_re25(re)
    for bad in (re[:15], struct.pack('<4I', 27000, 8191, 28000, 28500), struct.pack('<4I', 27000, 27500, 65537, 28500)):
        try:
            check_re25(bad)
            check(False, 'a bad Re25 refused')
        except AcdbError:
            check(True, '')
    print(f'PASS spk-prot-extract self-test: {checks} checks')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--self-test', action='store_true')
    sub = p.add_subparsers(dest='command')
    s = sub.add_parser('params')
    s.add_argument('acdb', type=Path)
    s.add_argument('out', type=Path)
    s = sub.add_parser('re25')
    s.add_argument('rdc', type=Path)
    s.add_argument('out', type=Path)
    s = sub.add_parser('check')
    s.add_argument('file', type=Path)
    a = p.parse_args()
    try:
        if a.self_test:
            return self_test()
        if a.command == 'params':
            entries = collect(Acdb(a.acdb.read_bytes()))
            data = pack(entries)
            write_private(a.out, data)
            print(f'{a.out.name}: {len(data)} bytes, {summary(entries)}')
        elif a.command == 're25':
            data = a.rdc.read_bytes()
            check_re25(data)
            write_private(a.out, data)
            print(f'{a.out.name}: 16 bytes, four channels each within 2..16 ohm')
        elif a.command == 'check':
            print(f'{a.file.name}: {summary(unpack(a.file.read_bytes()))}: the driver\'s checks pass')
        else:
            p.print_help()
            return 2
    except AcdbError as e:
        raise SystemExit(f'Error: {e}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
