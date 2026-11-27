#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline C5 adjudication. Missing evidence fails; webcam frames remain for human GUI/pixel review.

Reads a shared runner result directory; never accesses a device. --tables-only gates the 607 permit.
"""
import argparse
import re
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def fields(line):
    pairs = re.findall(r"(?:^|\s)([\w-]+)=([^\s]+)", line)
    record = dict(pairs)
    require(len(pairs) == len(record), 'duplicate telemetry field')
    return record


def indexed_rows(rows, size, label):
    indexed = {int(i): values for i, *values in rows}
    require(len(rows) == size and set(indexed) == set(range(size)), f'{label} row indices')
    return indexed


def tables(text):
    arcs = {}
    arc_rows = re.findall(r"R3a ARC (\w+) index=(\d+) vlvl=([0-9a-f]+)", text)
    for name, present, count in re.findall(r"R3a ARC (\w+) present=(\d+) count=(\d+)", text):
        count = int(count)
        require(name not in arcs and (present == '0' and count == 0 or present == '1' and 1 <= count <= 16),
                f'ARC {name} header')
        rows = indexed_rows([(i, v) for n, i, v in arc_rows if n == name], count, f'ARC {name}')
        arcs[name] = [int(rows[i][0], 16) for i in range(count)]
        require(all(v <= 0xffff for v in arcs[name]), f'ARC {name} voltage width')
    require(all(arcs.get(n) and arcs[n][0] == 0 for n in ('gfx', 'mx', 'cx')), 'ARC capture incomplete')
    require(all(all(a < b for a, b in zip(v, v[1:])) for v in arcs.values()), 'ARC monotonicity')
    def index(table, corner, clamp=False):
        return next((i for i, v in enumerate(table) if v >= corner), len(table)-1 if clamp else None)
    gx_rows = re.findall(r"R3a GX (\d+) khz=(\d+) gx=([0-9a-f]+) dep=([0-9a-f]+)", text)
    gx = {g: (int(k), int(v, 16), int(d, 16)) for g, (k, v, d) in indexed_rows(gx_rows, 12, 'GX').items()}
    require(gx[0] == (0, 0, 0xffffffff), 'GX off row')
    khz = [160000, 222000, 342000, 389000, 443000, 525000, 607000, 660000, 734000, 832000, 900000]
    corners = [0x32, 0x34, 0x38, 0x3c, 0x40, 0x50, 0x80, 0x90, 0xc0, 0xe0, 0x100]
    secondary = arcs.get('gmxc') or arcs['mx']
    for g, (freq, corner) in enumerate(zip(khz, corners), 1):
        i = index(arcs['gfx'], corner)
        require(i is not None, 'primary coverage')
        rounded = arcs['gfx'][i]
        vote = rounded << 16 | index(secondary, rounded, True) << 8 | i
        dep = 0xffffc0ff | index(arcs['mx'], corner, True) << 8
        if arcs.get('gbx'):
            dep = (dep & ~(63 << 20)) | index(arcs['gbx'], rounded, True) << 20
        require(gx[g] == (freq, vote, dep), f'GX/dependency row {g}')
    cx_rows = re.findall(r"R3a CX (\d+) khz=(\d+) vote=([0-9a-f]+)", text)
    cx = {c: (int(k), int(v, 16)) for c, (k, v) in indexed_rows(cx_rows, 3, 'CX').items()}
    for c, (freq, corner) in enumerate([(0, 0), (500000, 0x40), (650000, 0x80)]):
        i = index(arcs['cx'], corner)
        require(i is not None, 'CX coverage')
        vote = arcs['cx'][i] << 16 | index(arcs['mx'], arcs['cx'][i], True) << 8 | i
        require(cx[c] == (freq, vote), f'CX row {c}')
    bcm_rows = re.findall(r"R3a BCM (\d+) addr=([0-9a-f]+) unit=(\d+) width=(\d+) vcd=(\d+)", text)
    bcms = {i: (int(u), int(w), int(v)) for i, (_, u, w, v) in indexed_rows(bcm_rows, 4, 'BCM').items()}
    require(all(unit > 0 and width > 0 for unit, width, _ in (bcms[0], bcms[1])), 'BCM unit and width')
    rows = re.findall(r"R3a DDR (\d+) kbps=(\d+) wait=([0-9a-f]+) words=([0-9a-f]+),([0-9a-f]+),([0-9a-f]+)", text)
    for i, (kbps, wait, *words) in indexed_rows(rows, 12, 'DDR').items():
        kbps = int(kbps)
        expected, mask = [], 0
        for b in range(3):
            commit = b == 2 or bcms[b][2] != bcms[b+1][2]
            mask |= int(commit) << b
            value = (1 << 30 if commit else 0) | (1 << 29 if kbps else 0)
            if b == 2:
                value |= 4 if i >= 7 else 0
            elif kbps:
                unit, width, _ = bcms[b]
                require(unit > 0 and width > 0, f'BCM {b} unit/width')
                value |= min(16383, max(1, (kbps*width//[16, 4][b])*1000//unit))
            expected.append(value)
        require([int(w, 16) for w in words] == expected and int(wait, 16) == mask, f'DDR row {i}')
    require('C5-607-v1 full-table verified M1=49 GX12 CX3 M2=160' in text, 'verification marker missing')
    return arcs


def heartbeat(records, opened, end, label):
    times = [(int(record['at-us']), record) for record in records]
    window = [(at, record) for at, record in times if opened <= at < end]
    require(len(window) >= 590, f'{label} heartbeat incomplete')
    stamps = [at for at, _ in window]
    max_gap = 2_500_000
    require(stamps[0] - opened <= max_gap and end - stamps[-1] <= max_gap and
            all(0 < b - a <= max_gap for a, b in zip(stamps, stamps[1:])), f'{label} heartbeat order or gaps')
    return [record for _, record in window]


def adjudicate(root):
    text = (root/'console-for-checks.txt').read_text(errors='replace')
    arcs = tables(text)
    def records(kind):
        return [fields(l) for l in text.splitlines() if 'R3a '+kind+' ' in l]
    counters, temps, res, acks = (records(k) for k in ('COUNTERS', 'TEMP', 'RES', 'ACK'))
    require(all((counters, temps, res, acks)), 'positive telemetry missing')
    require(re.search(r'PianoThermal: start: piano-thermal=2 .*shutdown=1', text), 'SoC shutdown protection')
    require(re.search(r'adc: running: a round every 5 s', text), 'ADC protection')
    inj = re.search(r'R3a injection armed from ACK level=(\d+) at-us=(\d+)', text)
    require(inj and int(inj[1]) > 3, 'injection must start from a high ACK')
    inj_at = int(inj[2])
    opened = min(int(t['opened-us']) for t in temps if int(t['opened-us']))
    require(600_000_000 <= inj_at-opened <= 602_000_000, 'full 600-second soak, bounded injection tail')
    before = heartbeat(counters, opened, inj_at, 'counter')
    require(all(1 <= int(c['level']) <= 7 for c in before), 'normal acknowledged level exceeds607')
    for key in ('failures', 'timeout', 'errors', 'stale', 'malformed', 'queuefull', 'forceoff', 'recover', 'recoverfail', 'gbif'):
        require(len({c[key] for c in before}) == 1, f'normal delta {key}')
    for t in heartbeat(temps, opened, inj_at, 'temperature'):
        require(t['gpu-valid'] == 'ff' and int(t['gpu-max']) < 700, 'GPU temperature invalid or too hot')
        require(int(t['msm']) < 65000 and int(t['quiet']) < 40000, 'board temperature too hot')
        require(t['board-ok'] == '1' and t['valid'] == '3' and t['disabled'] == '0' and t['state'] == '2', 'invalid board temperature')
        ages = [int(a) for a in t['age-ms'].split(',')]
        require(len(ages) == 2 and all(0 <= a <= 10000 for a in ages) and t['reason'] == '0', 'stale or unsafe board temperature')
    windows = {}
    for r in res:
        at = int(r['at-us'])
        if opened <= at < inj_at:
            windows[(at-opened)//10_000_000] = r
            require(all(int(r['l'+str(i)]) == 0 for i in range(8, 12)), 'residency above607')
    require(sum(sum(int(r['l'+str(i)]) for i in range(4, 8)) > 0 for w,r in windows.items() if 1 <= w <= 58) >= 3, 'three high residency windows')
    require(any(int(r['l7']) > 0 for r in windows.values()), '607 residency')
    normal_acks = [a for a in acks if opened <= int(a['at-us']) < inj_at]
    require(normal_acks and any(int(a['l7']) > 0 for a in normal_acks), 'matching607 ACK')
    require(all(all(int(a['l'+str(i)]) == 0 for i in range(8, 12)) for a in normal_acks), 'ACK above607 even if residency rounded to0')
    require(all(a['host-fail'] == '0' and a['ddr'] == '18597656' and int(a['gpu-peak']) < 700 for a in normal_acks), 'rail/BCM/peak bound')
    require(all(int(a['mx']) == len(arcs['mx'])-1 and int(a['cx']) == len(arcs['cx'])-1 for a in normal_acks), 'host rails at top corners')
    post = [c for c in counters if int(c['at-us']) >= inj_at]
    require(post and int(post[-1]['at-us'])-inj_at >= 60_000_000, 'post-fallback load60s')
    first, last = before[-1], post[-1]
    for key in ('failures', 'timeout', 'forceoff', 'recover'):
        require(int(last[key])-int(first[key]) == 1, f'exactly one injection {key}')
    require(last['dropped'] == '1' and last['sticky'] == '1' and last['reason'] == '1', 'injection fallback state')
    low = re.search(r'R3a fresh-M9 ACK level=1 bus=2 header=[0-9a-f]+ at-us=(\d+) elapsed-us=\d+ high-acks=(\d+)', text[inj.start():])
    require(low, 'fresh low M9')
    for c in post:
        if int(c['at-us']) < int(low[1]):
            continue
        require(int(c['level']) <= 3, 'post-injection high ACK')
    for a in acks:
        if int(a['at-us']) >= int(low[1]):
            require(sum(int(a['l'+str(i)]) for i in range(4, 12)) == int(low[2]), 'post-M9 high ACK count grew')
    for marker in ('R3a strict-off result=0', 'R3a fresh-M9 ACK level=1 bus=2', 'R3a gate=0'):
        require(marker in text[inj.start():], marker)
    progress = []
    for phase in ('low', 'normal', 'post'):
        require('PIXELS_OK commands=3 pixels=196608 fresh-client=1' in (root/f'pixels-{phase}.txt').read_text(), f'{phase} fresh GPU client/pixels')
        lines = (root/f'redraw-{phase}.txt').read_text().splitlines()
        records_ = [fields(l) for l in lines if l.startswith('REDRAW ')]
        require(records_ and int(records_[-1]['moves']) > 0 and int(records_[-1]['draws']) > 0, f'{phase} redraw positive')
        progress.append(int(records_[-1]['moves']))
    require(progress[0] < progress[1] < progress[2], 'redraw stopped progressing')
    for key in ('WS_PID', 'BOOT_SESSION'):
        values = []
        for p in root.glob('session-*.txt'):
            m = re.search(r'^'+key+r'=(\S+)', p.read_text(), re.M)
            require(m, f'{p.name} {key} missing')
            values.append(m[1])
        require(len(values) >= 3 and len(set(values)) == 1, f'{key} changed')
    require(not re.search(r'R3a UNSAFE|GPU LOST', text), 'loss/escalation')
    require('FIVE_APPS_OK' in (root/'apps.txt').read_text(), 'app readiness')
    for phase in ('before', 'low', 'normal', 'post'):
        require('GUI_SESSION_OK' in (root/f'session-{phase}.txt').read_text(), f'{phase} live GUI/console user')
    for name, total in (('asset-hashes.txt', 14), ('load-hashes.txt', 5)):
        require((root/name).read_text().count(': OK') == total, 'payload hash positive count')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('result', type=Path)
    parser.add_argument('--tables-only', action='store_true')
    args = parser.parse_args()
    try:
        if args.tables_only:
            tables(args.result.read_text(errors='replace'))
        else:
            adjudicate(args.result)
    except (KeyError, ValueError, OSError) as exc:
        message = 'FAIL C5 evidence: '+str(exc)
        rc = 1
    else:
        message = ('PASS C5 table encoding' if args.tables_only else
                   'PASS C5 automated gates; review webcam pixels and service/q/copy benefit separately')
        rc = 0
    if not args.tables_only and args.result.is_dir():
        try:
            with (args.result/'verdict.txt').open('a') as stream:
                stream.write(message+'\n')
        except OSError as exc:
            if not rc:
                message = 'FAIL C5 evidence: '+str(exc)
                rc = 1
    print(message)
    return rc


if __name__ == '__main__':
    raise SystemExit(main())
