#!/usr/bin/env python3
"""Host-only tests of tooling/k273/build-k273-esp.py on a base whose entry line is already split (k27 0066): the base line ends
piano-boot-args=macos and the piano-* arguments live in the base ADT's /chosen/piano-boot-args-macos (the v5 shape,
private/host-checks/20261114-v6). No tablet, no fastboot, and no file of out/: every fixture is made here, from the fixtures of
tooling/persist/test-k273-only.py.

1. a split base rebuilt with no edit: the same entry line, the same tail, the same ADT bytes (reassembled and split again), and
   the record's base_split_rebuilt.
2. edits over a split base (the v6 story): --arg of a tail argument (piano-gpu-delay) replaces it by name and lands in the new
   tail exactly once; a new non-piano argument stays on the line; every entry's names stay unique over line + tail; the fastboot
   entry keeps only its own piano-* arguments; the -diag entry keeps the selector.
3. a fresh --kc/--adt over a split base (the old tool's silent loss: a selector pointing at a tail the new ADT did not carry):
   the tail is written into the given ADT and the entry line still selects it.
4. a hand-split base whose record has no boot_args_split: the tail read out of the base ADT alone.
5. refusals, loudly: a selector whose tail is in neither the record nor the base ADT; a record whose tail is not the base ADT's;
   a name carried by both the line and the tail; an invalid set name; piano-boot-args=0 (the off selector) carried as it is (a
   --split over it stays fit()'s refusal); --arg twice for one name, --arg without a name, and --arg data-store-lba with --seed.
  test-esp-split.py [--keep DIR]
"""
import argparse
import importlib.util
import json
import os
import shutil
import struct
import sys
import tempfile
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
D = HERE.parents[1]
sys.path.insert(0, str(D / 'tooling/persist'))
import common  # noqa: E402

spec = importlib.util.spec_from_file_location('tk', D / 'tooling/persist/test-k273-only.py')
tk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tk)
spec = importlib.util.spec_from_file_location('boot_args', D / 'tooling/adt/boot_args.py')
boot_args = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_args)

ADT = 'EFI/UniXNU/Payloads/k273/kernel.macho.adt'
TOML = 'EFI/UniXNU/unixnu.toml'
KC66 = b'piano-boot-args: /chosen/%s (%u bytes): %u bytes appended'   # k27 0066's console marker
failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what, flush=True)
    if not ok:
        failures.append(what)


def arg_map(text):
    return {x.split('=', 1)[0]: (x.split('=', 1)[1] if '=' in x else None) for x in text.split()}


def dup_names(*texts):
    names = [x.split('=', 1)[0] for t in texts for x in t.split()]
    return sorted({n for n in names if names.count(n) > 1})


def whole_of(record):
    """The macos-27.3 entry's whole arguments of RECORD: its line + its ADT tail (the tail is '' when unsplit)."""
    split = record.get('boot_args_split') or {}
    return record['cmdlines']['macos-27.3'] + ' ' + (split.get('tail') or '')


def split_inputs():
    """test-k273-only.py's dual-ESP fixtures with k27 0066 in the 27.3 collection (the split needs its marker)."""
    fsm, old_fw = os.urandom(4348), os.urandom(1000)
    markers = [b'com.unixnu.driver.NativeUfs', b'com.unixnu.driver.NativeRamBlock', b'data-store-lba=%llu',
               b'piano-boot-guard: healthy after %u s', b'root-part=%s', b'piano-power-rp-max', b'data-store-replace',
               b'piano-rtc: count %u + offset %u']
    return {'fsm': fsm, 'old_fw': old_fw,
            'kc273': tk.make_kc(markers + [KC66], [tk.pin('PianoAudio/fs19xx.fsm', fsm)]),
            'adt273': tk.make_adt(tk.WINDOW13), 'trust273': os.urandom(3000),
            'seed273': tk.make_seed(24 << 30, 3),
            'kc': tk.make_kc(markers[:1] + [b'data-store-replace'],
                             [tk.pin('PianoAudio/fs19xx.fsm', fsm), tk.pin('Old/old.bin', old_fw)]),
            'adt': tk.make_adt(tk.WINDOW13), 'trust': os.urandom(2000), 'seed': tk.make_seed(12 << 30, 2)}


def make_adt_tail(window, tail):
    """tk.make_adt plus /chosen/piano-boot-args-macos = TAIL (a hand-split base's ADT)."""
    chosen = tk.adt_node([('name', b'chosen\0'), ('darwinboot-memory-size', struct.pack('<Q', window)),
                          ('piano-boot-args-macos', tail.encode() + b'\0')], [tk.adt_node([('name', b'memory-map\0')])])
    return tk.adt_node([('name', b'device-tree\0')], [chosen])


def tail_of(image):
    return boot_args.sets(common.fat32_read(image)['files'][ADT]).get('piano-boot-args-macos')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--keep', type=Path)
    a = p.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix='piano-esp-split-')) if not a.keep else a.keep
    tmp.mkdir(parents=True, exist_ok=True)
    try:
        out = tmp / 'out'
        out.mkdir()
        inputs = split_inputs()

        # -- a split base -------------------------------------------------------------------------------------------------
        print('== a split base: reassembled, edited, split again')
        tk.make_dual(out, 'esp-dual66', tk.line_273(), inputs)
        rc, log = tk.build(out, 'esp-dual66', '--split', '--name', 'esp-split')
        check(rc == 0, f'the split base builds ({log.strip().splitlines()[-1][:90] if log.strip() else ""})')
        base = json.loads((out / 'esp-split.json').read_text())
        b_line, b_tail = base['cmdlines']['macos-27.3'], base['boot_args_split']['tail']
        check(base['boot_args_split']['set'] == 'macos' and b_line.split()[-1] == 'piano-boot-args=macos' and
              not dup_names(b_line, b_tail) and len(b_line) + 1 + len(b_tail) < 1000,
              f'the base entry is split: a {len(b_line)}-B line selecting a {len(b_tail)}-B tail (the merge would fit '
              f'{len(b_line) + 1 + len(b_tail)} B: only the forced split again keeps it split)')
        base_adt = common.fat32_read(out / 'esp-split.img')['files'][ADT]
        check(tail_of(out / 'esp-split.img') == b_tail, 'the base ADT carries the tail, as the record says')

        rc, log = tk.build(out, 'esp-split', '--name', 'esp-same')
        same = json.loads((out / 'esp-same.json').read_text())
        merged = ' '.join([x for x in b_line.split() if x != 'piano-boot-args=macos'] + b_tail.split())
        check(rc == 0 and same['cmdlines']['macos-27.3'] == b_line and same['boot_args_split']['tail'] == b_tail and
              common.fat32_read(out / 'esp-same.img')['files'][ADT] == base_adt,
              'no edit: reassembled and split again into the same line, the same tail, the same ADT bytes')
        rb = same.get('base_split_rebuilt') or {}
        check(rb.get('set') == 'macos' and rb.get('property') == 'piano-boot-args-macos' and rb.get('full_bytes') == len(merged)
              and rb.get('line_bytes') == len(b_line) and rb.get('tail_bytes') == len(b_tail),
              'the record notes the reassembly (base_split_rebuilt)')

        # -- a line that splits by size: the reassembled merge is over the budget on its own ------------------------------------------------------------
        long_line = tk.line_273() + ' piano-pad0=1234567890123 piano-pad1=1234567890123'
        tk.make_dual(out, 'esp-dual66l', long_line, inputs)
        rc, log = tk.build(out, 'esp-dual66l', '--name', 'esp-split2')
        base2 = json.loads((out / 'esp-split2.json').read_text())
        merge2 = len(' '.join(base2['cmdlines']['macos-27.3'].split()[:-1] + base2['boot_args_split']['tail'].split()))
        check(rc == 0 and base2['boot_args_split'] and base2['cmdlines']['macos-27.3'].split()[-1] == 'piano-boot-args=macos'
              and merge2 >= 1000, f'a {len(long_line)}-B base line: split by size alone (its merge is {merge2} B)')
        rc, log = tk.build(out, 'esp-split2', '--arg', 'piano-gpu=8', '--name', 'esp-v6l')
        v6l = json.loads((out / 'esp-v6l.json').read_text())
        check(rc == 0 and v6l['boot_args_split'] and v6l['cmdlines']['macos-27.3'].split()[-1] == 'piano-boot-args=macos' and
              arg_map(whole_of(v6l)) == dict(arg_map(whole_of(base2)), **{'piano-gpu': '8'}) and
              'piano-gpu=8' in v6l['boot_args_split']['tail'].split() and v6l['base_split_rebuilt'] is not None,
              'rebuilt from the size-split base without --split: split again by size, the edit in the tail')

        # -- edits, the v6 story -------------------------------------------------------------------------------------------------------------------
        rc, log = tk.build(out, 'esp-split', '--split', '--arg', 'piano-gpu-delay=0', '--arg', 'cs-native4k-skip=1',
                           '--name', 'esp-v6')
        check(rc == 0, f'edits over the split base ({log.strip().splitlines()[-1][:90] if log.strip() else ""})')
        v6 = json.loads((out / 'esp-v6.json').read_text())
        n_line, n_tail = v6['cmdlines']['macos-27.3'], v6['boot_args_split']['tail']
        check(arg_map(whole_of(v6)) == dict(arg_map(whole_of(base)), **{'piano-gpu-delay': '0', 'cs-native4k-skip': '1'}),
              'the whole arguments: the base\'s plus exactly the two --arg edits, no name duplicated')
        check(n_line.split()[-1] == 'piano-boot-args=macos' and 'cs-native4k-skip=1' in n_line.split() and
              'piano-gpu-delay' not in arg_map(n_line) and n_tail.split().count('piano-gpu-delay=0') == 1,
              'the line keeps the selector and the non-piano edit; the piano edit lands in the tail once')
        check(tail_of(out / 'esp-v6.img') == n_tail and not dup_names(n_line, n_tail) and
              v6['boot_args_split']['full_bytes'] == len(' '.join(
                  [x for x in merged.split() if x.split('=', 1)[0] not in ('piano-gpu-delay', 'cs-native4k-skip')] +
                  ['piano-gpu-delay=0', 'cs-native4k-skip=1'])),
              'the image\'s ADT carries the edited tail; full_bytes is the edited, reassembled line')
        menu = tomllib.loads(common.fat32_read(out / 'esp-v6.img')['files'][TOML].decode())
        ids = [e['id'] for e in menu['entry']]
        diag = next(e for e in menu['entry'] if e['id'] == 'macos-27.3-diag')
        fb = next(e for e in menu['entry'] if e['id'] == 'fastboot')
        check(ids == ['macos-27.3', 'macos-27.3-diag', 'fastboot', 'reset', 'shutdown', 'tools'] and
              'piano-boot-args=macos' in diag['args'].split() and 'piano-reset-after=240' in diag['args'].split(),
              f'the menu {ids}: the -diag entry keeps the selector (its tail applies at boot)')
        check(all(not dup_names(e.get('args', '')) for e in menu['entry']) and
              [x for x in fb['args'].split() if x.startswith('piano-')] == ['piano-reset-after=1', 'piano-reset-to=bootloader']
              and 'cs-native4k-skip=1' in fb['args'].split(),
              'no entry repeats a name; fastboot: only its own piano-* arguments, the non-piano edit carried')

        # -- a fresh --kc/--adt: the old tool's silent loss --------------------------------------------------------------------------------------------------
        print('== a fresh --kc/--adt over the split base (the old tool kept the selector and lost the tail)')
        fsm = inputs['fsm']
        kc2 = tk.make_kc([b'com.unixnu.driver.NativeUfs', b'com.unixnu.driver.NativeRamBlock', b'data-store-lba=%llu',
                          b'piano-boot-guard: healthy after %u s', b'root-part=%s', b'piano-power-rp-max', b'data-store-replace',
                          b'piano-rtc: count %u + offset %u', KC66], [tk.pin('PianoAudio/fs19xx.fsm', fsm)])
        (tmp / 'kc2.macho').write_bytes(kc2)
        (tmp / 'adt2.adt').write_bytes(tk.make_adt(tk.WINDOW13))
        rc, log = tk.build(out, 'esp-split', '--kc', str(tmp / 'kc2.macho'), '--adt', str(tmp / 'adt2.adt'), '--split',
                           '--arg', 'piano-gpu-delay=0', '--name', 'esp-fresh')
        fresh = json.loads((out / 'esp-fresh.json').read_text())
        files = common.fat32_read(out / 'esp-fresh.img')['files']
        check(rc == 0 and 'adt' in fresh.get('overrides', {}) and
              files['EFI/UniXNU/Payloads/k273/kernel.macho'] == kc2 and
              boot_args.sets(files[ADT]).get('piano-boot-args-macos') == fresh['boot_args_split']['tail'] and
              'piano-gpu-delay=0' in fresh['boot_args_split']['tail'].split(),
              'the given ADT gets the tail and the given KC the entry: no piano-* argument lost')

        # -- a hand-split base: the tail from the ADT alone --------------------------------------------------------------------------------------------------
        print('== a hand-split base (no boot_args_split in its record)')
        hand_line, tail_h = tk.line_273(long=False) + ' piano-boot-args=macos', 'piano-audio=2 piano-touch=2 piano-wifi=7'
        tk.make_dual(out, 'esp-hand', hand_line, dict(inputs, adt273=make_adt_tail(tk.WINDOW13, tail_h)))
        rc, log = tk.build(out, 'esp-hand', '--name', 'esp-hand-only')
        hand = json.loads((out / 'esp-hand-only.json').read_text())
        check(rc == 0 and arg_map(whole_of(hand)) == arg_map(hand_line + ' ' + tail_h) and
              hand['boot_args_split']['tail'].split() == [x for x in (hand_line + ' ' + tail_h).split() if x.startswith('piano-')
                                                          and x.split('=', 1)[0] not in boot_args.KEEP] and
              tail_of(out / 'esp-hand-only.img') == hand['boot_args_split']['tail'] and
              (hand.get('base_split_rebuilt') or {}).get('tail_bytes') == len(tail_h),
              'split again even without --split: every piano-* argument in the tail, none lost, none doubled')
        check(hand['cmdlines']['macos-27.3'].split()[-1] == 'piano-boot-args=macos' and
              'piano-usb=2' not in hand['cmdlines']['macos-27.3'].split(),
              'the hand-split base\'s own movable argument (piano-usb) moved to the tail too')

        # -- refusals, loudly -------------------------------------------------------------------------------------------------------------------------
        print('== refusals (a selector never points at a tail that is not there)')
        tk.make_dual(out, 'esp-notail', tk.line_273(long=False) + ' piano-boot-args=macos', inputs)
        rc, log = tk.build(out, 'esp-notail', '--name', 'x', expect=1)
        check(rc != 0 and 'refused rather than drop' in log, f'a selector with no tail anywhere: refused ({log.strip()[-90:]})')
        rec = json.loads((out / 'esp-split.json').read_text())
        common.write_json(out / 'esp-split.json', dict(
            rec, boot_args_split=dict(rec['boot_args_split'], tail=rec['boot_args_split']['tail'].replace('piano-gpu=2', 'piano-gpu=9', 1))))
        rc, log = tk.build(out, 'esp-split', '--name', 'x', expect=1)
        check(rc != 0 and 'is not the /chosen/piano-boot-args-macos' in log, 'a record whose tail is not the base ADT\'s: refused')
        tk.make_dual(out, 'esp-both', tk.line_273(long=False) + ' piano-gpu=9 piano-boot-args=macos',
                     dict(inputs, adt273=make_adt_tail(tk.WINDOW13, 'piano-gpu=2')))
        rc, log = tk.build(out, 'esp-both', '--name', 'x', expect=1)
        check(rc != 0 and 'both carry' in log and 'piano-gpu' in log, 'a name on the line and in the tail: refused')
        tk.make_dual(out, 'esp-badset', tk.line_273(long=False) + ' piano-boot-args=a/b', inputs)
        rc, log = tk.build(out, 'esp-badset', '--name', 'x', expect=1)
        check(rc != 0 and 'piano-boot-args set' in log, 'an invalid set name: refused')
        tk.make_dual(out, 'esp-off', tk.line_273(long=False) + ' piano-boot-args=0', inputs)
        rc, log = tk.build(out, 'esp-off', '--name', 'esp-off-only')
        off = json.loads((out / 'esp-off-only.json').read_text()) if rc == 0 else {}
        check(rc == 0 and off.get('cmdlines', {}).get('macos-27.3', '').endswith('piano-boot-args=0') and
              not off.get('boot_args_split') and not off.get('base_split_rebuilt'),
              'piano-boot-args=0 (off): carried as it is, nothing reassembled')
        rc, log = tk.build(out, 'esp-off', '--split', '--name', 'x', expect=1)
        check(rc != 0 and 'already have piano-boot-args=' in log, '--split over the off selector: fit()\'s own loud refusal')
        rc, log = tk.build(out, 'esp-dual66', '--arg', 'piano-x=1', '--arg', 'piano-x=2', '--name', 'x', expect=1)
        check(rc != 0 and 'given more than once' in log and 'piano-x' in log, '--arg twice for one name: refused')
        rc, log = tk.build(out, 'esp-dual66', '--arg', '=5', '--name', 'x', expect=1)
        check(rc != 0 and 'NAME=VALUE' in log, '--arg without a name: refused')
        (tmp / 'seed-x.img').write_bytes(tk.make_seed(24 << 30, 1))
        rc, log = tk.build(out, 'esp-dual66', '--seed', str(tmp / 'seed-x.img'), '--arg', f'data-store-lba={tk.STORE_LBA}',
                           '--name', 'x', expect=1)
        check(rc != 0 and 'given more than once' in log, '--arg data-store-lba with --seed (its own edit): refused')
        rc, log = tk.build(out, 'esp-dual66', '--data-extent', '107512576', '124289791',
                           '--expect-store-lba', '117735999', '--name', 'esp-target')
        check(rc == 0, f'24 GiB seed binds to the target Data extent ({log.strip()[-120:]})')
        rc, log = tk.build(out, 'esp-dual66', '--data-extent', '124000000', '124289791',
                           '--name', 'x', expect=1)
        check(rc != 0 and 'do not fit' in log, 'undersized target Data extent refused')
        for suffix in ('.img', '.json'):
            (out / ('x' + suffix)).unlink(missing_ok=True)
    finally:
        if not a.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
