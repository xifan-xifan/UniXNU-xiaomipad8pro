#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
"""Read captured V6 evidence; never contacts a device or runs a test."""
import argparse
import json
import re
from pathlib import Path


def reference_hashes(manifest, clip):
    reference = {}
    for frame in manifest['fixtures'][clip]['output']:
        pts, digest = str(frame['pts']), frame['sha256']
        if (not re.fullmatch(r'-?[0-9]{1,19}', pts) or pts in reference
                or not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest)):
            raise ValueError(f'invalid {clip} reference frame')
        reference[pts] = digest
    if not reference:
        raise ValueError(f'empty {clip} reference')
    return reference


def evaluate(console, tool, manifest):
    errors = []
    def require(ok, label):
        if not ok:
            errors.append(label)
    require(bool(console.strip()) and bool(tool.strip()), 'nonempty captures')
    require('V6 bounded AVC streaming ready' in console, 'current V6 kernel banner')
    require('V3 first-HFI PASS:' in console, 'known-positive INIT')
    require('video-v6: begin pid=' in tool, 'fresh tool begin')
    exits = re.findall(r'^V6_TOOL_EXIT=.*$', tool, re.M)
    require(re.search(r'^video-v6: PASS qualification failures=0(?: |$)', tool, re.M)
            and exits == ['V6_TOOL_EXIT=0'], 'tool completed all checks')
    require(not re.search(r'video-v6: FAIL ', tool), 'zero tool failures')
    phases = {'ibp-async': ('ibp', 60, 0), 'ibp-temporal': ('ibp', 60, 0),
              'ip-temporal': ('ip', 60, 0), 'ibp-no-output': ('ibp', 60, 1), 'recreate-at-idr': ('ibp', 30, 0)}
    summaries, phase_frames = {}, {phase: [] for phase in phases}
    for line in tool.splitlines():
        if not line.startswith(('video-v6: summary ', 'video-v6: frame ')):
            continue
        pairs = re.findall(r'(?:^|\s)([\w-]+)=([^\s]+)', line)
        fields = dict(pairs)
        phase = fields.get('phase')
        if len(pairs) != len(fields) or not phase:
            errors.append('malformed tool frame or summary')
        elif phase in phases:
            if line.startswith('video-v6: frame '):
                phase_frames[phase].append(fields)
            elif phase in summaries:
                errors.append(phase+' duplicate summary')
            else:
                summaries[phase] = fields
    references = {clip: reference_hashes(manifest, clip) for clip in ('ip', 'ibp')}
    for phase, (clip, count, suppressed) in phases.items():
        summary = summaries.get(phase, {})
        for key, n in [('submitted', count), ('callbacks', count), ('images', count-suppressed), ('nulls', suppressed), ('errors', 0)]:
            require(summary.get(key) == str(n), phase+' '+key)
        require(summary.get('early') in {str(n) for n in range(1, count+1)}, phase+' output before finish')
        require(summary.get('high-water') in '1 2 3 4 5 6 7'.split(), phase+' bounded producer')
        frames = phase_frames[phase]
        require(len(frames) == count, phase+' captured every token')
        require(sum(f.get('image') == '0' for f in frames) == suppressed, phase+' captured no-output count')
        tokens = set()
        timestamps = set()
        reference = references[clip]
        for f in frames:
            token = f.get('token', '')
            require(re.fullmatch(r'[1-9][0-9]{0,19}', token) and token not in tokens
                    and f.get('callback') == '1', phase+' unique token')
            tokens.add(token)
            match = re.fullmatch(r'(-?[0-9]{1,19})/([0-9]{1,10})', f.get('pts', ''))
            if not match:
                errors.append(phase+' exact PTS scale')
                continue
            value, scale = map(int, match.groups())
            require(scale > 0 and value*90000 % scale == 0, phase+' exact PTS scale')
            pts = str(value*90000//scale) if scale > 0 else 'invalid'
            require(pts not in timestamps, phase+' unique PTS')
            timestamps.add(pts)
            if f.get('image') == '1':
                require(f.get('status') == '0' and pts in reference and reference[pts] == f.get('sha256'),
                        phase+' per-PTS software hash')
            else:
                require(f.get('image') == '0' and suppressed == 1 and f.get('status') == '0' and pts in reference,
                        phase+' explicit no-output')
    require('PASS original-one-golden-callback ' in tool, 'original golden')
    require('PASS cancel-had-pending ' in tool and 'PASS no-callback-after-invalidate ' in tool, 'cancel and joined new epoch')
    require(re.search(r'V6 resume session .* BITSTREAM RAW cause DRAIN', console) is not None, 'actual drain resume')
    require(re.search(r'V6: drain barrier session .* ACK LAST pending0 terminal0', console) is not None, 'nonterminal barrier')
    # A retained console may replay its boot. Exact duplicate evidence lines do
    # not count twice; protocol duplicates still fail the real kernel ledger.
    closes = set(re.findall(r'V6 native close session ([^\n\r]+)', console))
    require(len(closes) >= 6, 'all persistent streams closed (cancel may precede OPEN)')
    full, session_ids = 0, set()
    for close in sorted(closes):
        match = re.fullmatch(r'([0-9a-f]{1,8}) result (-?\d+) ETB (\d+) EBD (\d+) PSC (\d+) decoded (\d+).*STOP (\d+)/(\d+) CLOSE (\d+) teardown (-?\d+) dma-held (\d+) watchdog (\d+) fault-bits ([0-9a-f]+)\s*', close)
        require(bool(match), 'close stage evidence')
        if not match:
            continue
        sid = match[1]
        session = int(sid, 16)
        require(session not in session_ids, 'one close per persistent session')
        session_ids.add(session)
        rc, etb, ebd, psc, decoded, raw, inp, closed, teardown, held, watchdog = map(int, match.groups()[1:-1])
        require(rc == 0 and psc == 1 and raw == inp == closed == 1 and teardown == held == watchdog == 0 and int(match[13], 16) == 0, 'safe close')
        if etb == 60:
            full += 1
            require(ebd == decoded == 60, 'persistent 60 ETB/EBD/FBD')
            decimal_sid = str(session)
            admitted = set(re.findall(r'V6: accepted session '+decimal_sid+r' token (\d+) flags \d+ normalized-bytes (\d+) SHA256 ([0-9a-f]{64})', console))
            submitted = set(re.findall(r'V6 native submit session '+sid+r' token (\d+) bytes (\d+) SHA256 ([0-9a-f]{64})', console))
            returned = set(re.findall(r'V6 EBD session '+sid+r' index \d+ token (\d+) count \d+', console))
            pictures = set(re.findall(r'V6 FBD session '+sid+r' token (\d+) index \d+ picture (\d+) count \d+', console))
            require(len(admitted) == len(submitted) == 60 and admitted == submitted, 'same caller AU digest/token through HFI session')
            tokens = {x[0] for x in admitted}
            require(tokens == returned == {x[0] for x in pictures} and len(pictures) == 60, 'every EBD/FBD opaque token matched')
            b_count = sum(int(x[1]) == 4 for x in pictures)
            p_count = sum(int(x[1]) == 2 for x in pictures)
            require((b_count, p_count) in ((38, 20), (0, 58)), 'actual IP/IBP picture types')
    require(full == 4, 'four 60-frame persistent hardware sessions')
    ownership = set(re.findall(r'V6 ownership session [^\n\r]+', console))
    reused = [x for x in ownership if re.search(r'input-reuse [1-9]\d* raw-reuse [1-9]\d*', x)]
    require(len(reused) >= 5, 'real input and RAW reuse across streams')
    copies = set(re.findall(r'V6: worker close session [^\n\r]+', console))
    require(len(copies) >= 7, 'worker cleanup records')
    for line in copies:
        m = re.search(r'status (-?\d+).*surface-open (\d+) surface-close (\d+) maps (\d+) high-water (\d+)', line)
        require(m is not None and m[1] == '0' and m[2] == m[3] and m[4] == '0' and int(m[5]) <= 8, 'surface refs/maps/credit balanced')
    require(not re.search(r'PianoVideo(?:VT V5a: first-error|: V4 first-error|: smmu: FAULT on the video bank|: V3 teardown result .*watchdog 1)', console), 'zero kernel errors/faults/watchdog')
    return errors


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('console', type=Path)
    p.add_argument('tool', type=Path)
    p.add_argument('manifest', type=Path)
    a = p.parse_args()
    try:
        errors = evaluate(a.console.read_text(errors='replace'), a.tool.read_text(errors='replace'),
                          json.loads(a.manifest.read_text()))
    except (OSError, KeyError, TypeError, ValueError) as exc:
        errors = [f'invalid capture or manifest: {exc}']
    for error in errors:
        print('V6_EVIDENCE_FAIL '+error)
    print('V6_EVIDENCE_'+('FAIL' if errors else 'PASS')+' checks='+str(len(errors))+'-failures')
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
