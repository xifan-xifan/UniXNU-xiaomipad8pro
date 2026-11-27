#!/usr/bin/env python3
"""Exercise the RAM and 27.3 ESP argument builders without creating images."""

import argparse
from collections import defaultdict
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ram = load('ram', 'tooling/bootimg/build-ram-images.py')
esp = load('esp', 'tooling/k273/build-k273-esp.py')
ba = esp.boot_args
OPT_IN = 'piano-long-boot-args=1'
SMP = 'sheng-smp=1'


def refused(message, build, *args, **kwargs):
    try:
        build(*args, **kwargs)
    except SystemExit as error:
        assert message in str(error), error
    else:
        raise AssertionError(f'expected refusal: {message}')


def values(words):
    by_key = defaultdict(list)
    for word in words:
        by_key[ba.name_of(word)].append(word)
    return by_key


def check_layout(loader, tail, set_name, words):
    wire = loader.split()
    assert OPT_IN in wire and len(loader.encode()) <= 1008, loader
    adt = ba.with_set(ba.tiny_adt(), set_name, tail)
    full, status = ba.merged(loader, adt)
    assert status == 'appended'
    assert full == loader + ' ' + ' '.join(tail)
    assert values(w for w in full.split() if ba.name_of(w) != ba.PROP) == values(words)
    clip = ba.legacy_clip(full.encode()).decode()
    for word in words:
        if not ba.movable_long(word):
            assert word in wire and word in clip.split(), word
    return len(full), len(clip), clip


def test_boundaries():
    reserve = len(' ' + SMP)
    keep = [OPT_IN, 'wdt=-1', 'piano-extra-ram-stage=0', 'piano-boot-guard-ping=0', 'piano-reset-after=600',
            'droidvm-utc=1794497308', 'piano-camera-grab=1', 'piano-lw-quiet=1', 'piano-tu-debug=none',
            'piano-metal-report=0', 'piano-surface-residency=1', 'soft-metal-fb8=0']
    tail = ['piano-usb-shell=0', 'native-gop-display-night-shift=1', 'piano-usb-shell=1']
    size = 1008 - len(' '.join([*keep, '', 'piano-boot-args=ram', SMP]))
    prefix = [*keep, 'f' * size]
    one, smp, got = ram.boot_entry_args(ba, [*tail, *prefix])
    assert (len(one), len(smp), got) == (1003, 1008, tail)
    check_layout(smp, got, 'ram', [*prefix, SMP, *tail])
    refused('are 1009 bytes', ram.boot_entry_args, ba, [*tail, *keep, 'f' * (size + 1)])

    exact = [OPT_IN, 'f' * (1008 - reserve - len(OPT_IN) - 1)]
    _, smp, got = ram.boot_entry_args(ba, exact)
    assert smp == ' '.join([*exact, SMP]) and not got
    refused('is 1009 bytes', ram.boot_entry_args, ba, [exact[0], exact[1] + 'f'])
    refused('needs printable ASCII', ram.boot_entry_args, ba, [OPT_IN, 'piano-test=é'])
    assert ba.line_max(['other=1', OPT_IN, 'piano-long-boot-args=0']) == 999
    plain = 'f' * (999 - reserve)
    assert ram.boot_entry_args(ba, [plain])[1:] == (plain + ' ' + SMP, [])
    refused('the builder allows 999', ram.boot_entry_args, ba, [plain + 'f'])
    refused('the builder allows 999', ba.fit, ['é' * 500], 'ram')
    refused('embedded NUL', ba.fit, ['piano-test=a\0b'], 'ram')
    assert esp.split_line('plain=1', force=True) == ('plain=1', None)
    stage = 'piano-extra-ram-stage=0'
    one, smp, got = ram.boot_entry_args(ba, [stage, 'piano-test=1'], force=True)
    assert stage in one.split() and stage in smp.split()
    assert got == ['piano-test=1']

    # 0066's limit includes the selector, space and reserved CPU suffix.
    tail_word = 'piano-test=' + 'z' * (4095 - len(' '.join([OPT_IN, 'piano-boot-args=ram', SMP, 'piano-test='])))
    _, smp, got = ram.boot_entry_args(ba, [OPT_IN, tail_word])
    assert check_layout(smp, got, 'ram', [OPT_IN, tail_word, SMP])[0] == 4095
    refused('would be 4096 bytes', ram.boot_entry_args, ba, [OPT_IN, tail_word + 'z'])
    daily, got = esp.split_line(' '.join([OPT_IN, SMP, tail_word[:-2]]))  # macos selector is two bytes longer than ram
    assert check_layout(daily, got, 'macos', [OPT_IN, SMP, tail_word[:-2]])[0] == 4095
    assert not esp.diag_budget(esp.diag_line(daily, 240), len(' '.join(got)))['included']
    print('PASS limits, opt-out, required readers, duplicate ordering and ADT round trip')


def test_merge_loader_bytes():
    default = ba.with_set(ba.tiny_adt(), None, 'piano-test=1')
    assert ba.merged('plain=1\0 piano-boot-args=0', default) == ('plain=1 piano-test=1', 'appended')

    # The last byte of the loader field holds half a UTF-8 character. XNU
    # still counts that byte when sizing the merged C string.
    line = 'x' * (ba.LEGACY_MAX - 1) + 'é'
    tail = 'y' * (ba.MERGED_MAX - ba.LEGACY_MAX)
    assert ba.merged(line, ba.with_set(default, None, tail))[1] == 'too-long'
    assert ba.merged(line, ba.with_set(default, None, tail[:-1]))[1] == 'appended'


def test_adt_bounds():
    adt = ba.with_set(ba.tiny_adt(), 'ram', 'piano-test=1')
    refused('truncated ADT property value or padding', ba.sets, adt[:-1])
    refused('invalid ADT node header', ba.sets, adt[:7])
    refused('byte for byte', ba.sets, adt + b'\0')
    root = ba.load(adt)
    props, _ = ba.chosen(root)
    props.append(props[-1])
    duplicate = ba.serialize(root)
    refused('duplicate piano-boot-args-ram', ba.sets, duplicate)
    refused('duplicate piano-boot-args-ram', ba.merged, 'piano-boot-args=ram', duplicate)
    refused('truncated ADT property header', ba.sets, struct.pack('<II', 1, 0) + bytes(35))


def test_identity_overlay():
    root = ba.load(ba.tiny_adt())
    root[1].append([[['name', 8, b'product\0', None]], []])
    with tempfile.TemporaryDirectory() as scratch:
        source = Path(scratch) / 'in.adt'
        output = Path(scratch) / 'out.adt'
        source.write_bytes(ba.serialize(root))
        interpreters = {sys.executable}
        if sys.platform == 'darwin':
            interpreters.add('/usr/bin/python3')
        for python in sorted(interpreters):
            result = subprocess.run([python, str(ROOT / 'tooling/adt/set-identity.py'),
                                     str(source), str(output), '--identity', 'piano'], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            props, _ = ba.load(output.read_bytes())
            values = {name: value for name, _, value, _ in props}
            assert values['compatible'] == b'PianoAP\0'


def test_qual(record):
    images = json.loads(record.read_text())['images']
    image, = (v for v in images.values() if 'cmdline_8cpu' in v)
    base = [w for w in image['cmdline_8cpu'].split() if w != SMP and ba.name_of(w) != ba.PROP]
    tail = image['boot_args']['tail'].split()
    words = [*base, *tail]
    assert ram.boot_entry_args(ba, words) == (image['cmdline_1cpu'], image['cmdline_8cpu'], tail)
    one, smp, got = ram.boot_entry_args(ba, [*words, OPT_IN])
    size, clipped, clip = check_layout(smp, got, 'ram', [*words, OPT_IN, SMP])
    check_layout(one, got, 'ram', [*words, OPT_IN, 'cpus=1'])
    assert 'piano-usb-shell=1' in got and 'piano-usb-shell=1' not in clip.split()
    print(f'PASS {record.parent.name} legacy replay, opt-in {len(one)}/{len(smp)} loader, {size} merged, {clipped} clipped')

    # Reproduce a post-split prefix of 1013 bytes from the current qual line.
    key = 'test-bootline-padding='
    filler = 'x' * (1013 - len(image['cmdline_8cpu'].encode()) - 1 - len(key))
    extended = [*words, key + filler]
    refused('command line is 1013 bytes', ram.boot_entry_args, ba, extended)
    one, smp, got = ram.boot_entry_args(ba, [*extended, OPT_IN])
    size, clipped, clip = check_layout(smp, got, 'ram', [*extended, OPT_IN, SMP])
    assert 'piano-usb-shell=1' not in clip.split()
    print(f'PASS 1013-byte refusal replay, opt-in {len(one)}/{len(smp)} loader, {size} merged, {clipped} clipped')


def test_daily(record):
    image = json.loads(record.read_text())
    entry = image['default']
    old = image['cmdlines'][entry]
    base = [w for w in old.split() if ba.name_of(w) != ba.PROP]
    tail = image['boot_args_split']['tail'].split()
    whole = [*base, *tail]
    assert esp.split_line(' '.join(whole), force=True) == (old, tail)
    assert esp.diag_line(old, 240) == image['cmdlines'][f'{entry}-diag']
    assert esp.fastboot_line(old) == image['cmdlines']['fastboot']

    words = [*whole, OPT_IN]
    line, got = esp.split_line(' '.join(words), force=True)
    size, clipped, _ = check_layout(line, got, 'macos', words)
    diag = esp.diag_line(line, 240)
    assert esp.diag_budget(diag, len(' '.join(got)))['included']
    check_layout(diag, got, 'macos', esp.diag_line(' '.join(words), 240).split())
    fastboot = esp.fastboot_line(line)
    assert ba.selector(fastboot.split()) is None and OPT_IN not in fastboot.split()
    assert not set(('native-ufs-root', 'data-store-lba')) & values(fastboot.split()).keys()
    assert {'data-persist=0', 'piano-reset-after=1', 'piano-reset-to=bootloader'} <= set(fastboot.split())
    assert len(fastboot.encode()) <= ba.line_max(fastboot.split())
    print(f'PASS {record.stem} legacy replay, opt-in {len(line)}/{len(diag)} loader, {size} merged, {clipped} clipped')


def test_daily_combo():
    # Recreate the installed prefix size and USB argument positions without
    # needing an installed image or a staged userland manifest.
    base = ['test-prefix=' + 'x' * 770, 'sheng-smp=1', 'piano-test-pad=' + 'y' * 107, 'piano-usb-net=1', 'piano-usb-shell=1']
    baseline, _ = esp.split_line(' '.join(base), force=True)
    assert len(baseline) == 816
    with tempfile.TemporaryDirectory() as tmp:
        kc = Path(tmp) / 'kc'
        kc.write_bytes(ba.KC_MARKER + ba.MERGED_SYSCTL_MARKER)
        for hz in (120, 144):
            delta = ['native-gop-display-night-shift=1', 'piano-dpu-clean-skip=1', f'piano-dpu-modes={hz}', 'data-store-write-back=1']
            words = base + delta
            # The previous split kept Night Shift and write-back in the
            # loader, moving USB shell beyond the legacy clip.
            before, tail = ba.fit(words, 'macos', force=True)
            refused('pianousb-shell', ba.check_userland, {'main': ' '.join(before)},
                    ba.with_set(ba.tiny_adt(), 'macos', tail), kc, live=True)
            line, tail = esp.split_line(' '.join(words), force=True)
            assert line == baseline and tail[-4:] == delta
            adt = ba.with_set(ba.tiny_adt(), 'macos', tail)
            diag = esp.diag_line(line, 240)
            assert esp.diag_budget(diag, len(' '.join(tail)))['included']
            ba.check_userland({'main': line, 'diag': diag}, adt, kc, live=True)
            for prefix in (line, diag):
                full, status = ba.merged(prefix, adt)
                assert status == 'appended' and set(delta) <= set(full.split())
                assert set(base[-2:]) <= set(ba.legacy_clip(full.encode()).decode().split())
    print('PASS y12u fixed 120/144 ESP replay keeps the installed USB arguments visible')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qual-record', type=Path, help='record from an existing RAM qualification image')
    parser.add_argument('--daily-record', type=Path, help='record from an existing k27.3 daily ESP')
    args = parser.parse_args()
    test_boundaries()
    test_merge_loader_bytes()
    test_adt_bounds()
    ba.test_userland_visibility()
    test_identity_overlay()
    test_daily_combo()
    if args.qual_record:
        test_qual(args.qual_record)
    if args.daily_record:
        test_daily(args.daily_record)


if __name__ == '__main__':
    main()
