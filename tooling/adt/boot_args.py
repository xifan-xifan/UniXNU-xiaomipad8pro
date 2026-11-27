#!/usr/bin/env python3
"""k27 0066's host side: the kernel arguments beyond DarwinBoot's budget live in the ADT (docs/bringup-debugging.md §5.2).

DarwinBoot passes at most 1023 bytes of command line, and the tools normally keep the line under LINE_MAX (1000). With k27 0066 in the kernel,
the line's piano-boot-args=NAME makes it append the ADT's /chosen/piano-boot-args-NAME to the boot-args (piano-boot-args=0 none; no
selector: /chosen/piano-boot-args), and every PE_parse_boot_argn() sees both (tooling/k27/piano-bootargs/piano_bootargs_policy.h).

  boot_args.py --show ADT [--line LINE]    the ADT's piano-boot-args sets; with LINE, the boot-args the kernel makes of it
  boot_args.py --self-test                 the split, the ADT edit and the merge against the kernel header's numbers

Library (tooling/bootimg/build-ram-images.py):
  fit(args, set_name, reserve=0, force=False) -> (line args, tail args)   the tail is [] when the line fits (or nothing moves)
  with_set(adt_bytes, set_name, tail) -> ADT bytes with /chosen/piano-boot-args-NAME = tail
  sets(adt_bytes) -> {property: text}
  merged(line, adt_bytes) -> (boot-args, status)   what the kernel makes of LINE with this ADT
  kc_has_boot_args(kc_path)                        the collection's kernel has 0066 (its console line's format)

What moves: every piano-* argument but the ones the boot's safety rests on even under a kernel without 0066 (piano-reset-after,
piano-reset-to, piano-boot-guard*, piano-extra-ram-stage, and the selector itself); everything else (the k27 arguments, native-ufs-*, data-*, pstore-*,
droidvm-*, cpus, sheng-smp) stays on the line. static-if keys (validation_disables, serverperfmode, lcks, wdt) must stay on the
line: static_if_init() reads it before the device tree is up. Order is kept within each part, and the kernel appends the tail after
the line, so for one name the last value still wins as before.

piano-long-boot-args=1 selects a 1008-byte loader line and keeps early and legacy-reader arguments on it. Display arguments
with only post-merge PE readers may also move to the ADT. See docs/boot-args-length.md.
"""
import argparse
import hashlib
import json
import mmap
import os
import re
import struct
import sys
import tempfile
from pathlib import Path

DEVICE = Path(__file__).resolve().parents[2]
HEADER = DEVICE / 'tooling/k27/piano-bootargs/piano_bootargs_policy.h'
LINE_MAX = 1000        # the tools' budget for DarwinBoot's line (it passes at most 1023 bytes)
LONG_LINE_MAX = 1008   # inclusive, including the ADT selector and the entry suffix
LEGACY_MAX = 1023     # BOOT_LINE_LENGTH - 1, k27 0066's kern.bootargs clip
MERGED_MAX = 4095      # PIANO_BOOT_ARGS_SIZE - 1
PROP = 'piano-boot-args'
LONG_ARG = 'piano-long-boot-args'
SET_MAX = 15           # PIANO_BOOT_ARGS_SET_MAX
KC_MARKER = b'piano-boot-args: /chosen/%s (%u bytes): %u bytes appended'
MERGED_SYSCTL_MARKER = b'Piano merged boot arguments (0066 ADT tail)\0'
READER_ABI = b'kern.piano_bootargs\0PianoBootArgs ABI 1\0'
KEEP = ('piano-reset-after', 'piano-reset-to', 'piano-boot-guard', 'piano-extra-ram-stage', PROP)
SET_RE = re.compile(r'[A-Za-z0-9._-]{1,%d}' % SET_MAX)

MERGED_READERS = {
    'pianousb-shell': ('piano-usb-shell',),
    'piano-usbnet-address': ('piano-usb-net',),
    'piano-rotated': ('piano-rotate', 'piano-rotate-map', 'piano-rotate-settle', 'piano-rotate-interval',
                      'piano-rotate-detach', 'piano-rotate-cover', 'piano-rotate-test'),
    'piano-brightness-sync': ('piano-backlight-native', 'piano-backlight-auto'),
}
# For these readers only the legacy prefix is guaranteed by this build.
LEGACY_READERS = {
    'piano-camera-grab': ('piano-camera-grab',),
    'piano-lw-quiet': ('piano-lw-quiet',),
    'NativeSoftMetal': ('soft-metal', 'soft-metal-report', 'soft-metal-fb8', 'soft-metal-stubs',
                        'soft-metal-all', 'soft-metal-family'),
    'NativeAdrenoMetal': ('piano-metal-report', 'piano-metal-barriers', 'piano-tu-debug'),
    'Turnip surface residency': ('piano-surface-residency',),
    'Turnip BO clear': ('piano-tu-bo-clear',),
    'settime.sh': ('droidvm-utc', 'droidvm-tz'),
}
SHORT_READERS = {'NativeSoftMetal', 'NativeAdrenoMetal'}  # pass 1023 bytes to sysctl, including the NUL
LEGACY_KEYS = {key for keys in LEGACY_READERS.values() for key in keys}
ADT_DISPLAY_KEYS = {'native-gop-display-builtin', 'native-gop-display-flip-lossless',
                    'native-gop-display-shot', 'native-gop-display-night-shift'}


def name_of(arg):
    return arg.split('=', 1)[0]


def movable(arg):
    n = name_of(arg)
    return n.startswith('piano-') and n not in KEEP and not n.startswith('piano-boot-guard-')


def line_max(args):
    mode = next((word for word in reversed(args) if name_of(word) == LONG_ARG), None)
    return LONG_LINE_MAX if mode == f'{LONG_ARG}=1' else LINE_MAX - 1


def movable_long(arg):
    name = name_of(arg)
    if name in ADT_DISPLAY_KEYS:
        return True
    return (movable(arg) and name not in LEGACY_KEYS and name != LONG_ARG
            and not name.startswith(('piano-metal-', 'piano-tu-')))


def prop_name(set_name):
    if set_name is None:
        return PROP
    if set_name == '0' or not SET_RE.fullmatch(set_name):
        raise SystemExit(f'piano-boot-args set {set_name!r}: 1-{SET_MAX} of A-Z a-z 0-9 . _ - (and not "0")')
    return f'{PROP}-{set_name}'


def fit(args, set_name, reserve=0, force=False, *, extra_tail_keys=()):
    """(line args, tail args) for ARGS: unchanged with an empty tail when the line plus RESERVE fits (and
    not FORCE). Otherwise movable arguments and the caller's extra kernel-only keys go in the tail with
    piano-boot-args=SET_NAME in the line. SystemExit when either the loader or merged line would exceed its budget."""
    args = list(args)
    if any(name_of(x) == PROP for x in args):
        raise SystemExit(f'the arguments already have {PROP}=: give the whole (merged) arguments, the tools make the selector')
    line = ' '.join(args)
    if '\0' in line:
        raise SystemExit('bootloader arguments contain an embedded NUL')
    line_bytes = len(line.encode()) + reserve
    limit = line_max(args)
    long_layout = limit == LONG_LINE_MAX
    if long_layout and any(not 0x20 <= ord(c) <= 0x7e or c in '"\\' for c in line):
        raise SystemExit(f'{LONG_ARG}=1 needs printable ASCII arguments without quotes or backslashes')
    if not force and line_bytes <= limit:
        return args, []
    moves = movable_long if long_layout else movable
    kept, tail = [], []
    for word in args:
        (tail if moves(word) or name_of(word) in extra_tail_keys else kept).append(word)
    if not tail:
        if line_bytes > limit:
            if long_layout:
                raise SystemExit(f'required bootloader prefix is {line_bytes} bytes; {LONG_ARG}=1 allows {limit}')
            raise SystemExit(f'kernel command line is {line_bytes} bytes; the builder allows {limit}')
        return args, []
    prop_name(set_name)
    kept.append(f'{PROP}={set_name}')
    line_bytes = len(' '.join(kept).encode()) + reserve
    if line_bytes > limit:
        if long_layout:
            raise SystemExit(f'required bootloader prefix, selector and entry suffix are {line_bytes} bytes; '
                             f'{LONG_ARG}=1 allows {limit}')
        raise SystemExit(f'kernel command line is {line_bytes} bytes even with every piano-* argument in the ADT '
                         f'({len(" ".join(tail))} bytes); DarwinBoot passes at most 1023 (the tools keep it under {LINE_MAX})')
    text = ' '.join(tail)
    merged_bytes = line_bytes + 1 + len(text.encode())
    if merged_bytes > MERGED_MAX:
        raise SystemExit(f'the boot-args with the ADT\'s tail would be {merged_bytes} bytes; the kernel takes '
                         f'{MERGED_MAX} (k27 0066 PIANO_BOOT_ARGS_SIZE)')
    return kept, tail


# --- the ADT (the format of tooling/adt/set-identity.py: nodes of props [name, size word, value, raw name] and children) ---

def parse(data, at=0, depth=0):
    if depth > 64 or not 0 <= at <= len(data) - 8:
        raise ValueError('invalid ADT node header or nesting depth')
    nprops, nchildren = struct.unpack_from('<II', data, at)
    at += 8
    props = []
    for _ in range(nprops):
        if len(data) - at < 36:
            raise ValueError('truncated ADT property header')
        name = data[at:at + 32].split(b'\0', 1)[0].decode()
        word = struct.unpack_from('<I', data, at + 32)[0]
        size = word & 0x7fffffff
        end = at + 36 + ((size + 3) & ~3)
        if end > len(data):
            raise ValueError('truncated ADT property value or padding')
        props.append([name, word, data[at + 36:at + 36 + size], data[at:at + 32]])
        at = end
    children = []
    for _ in range(nchildren):
        child, at = parse(data, at, depth + 1)
        children.append(child)
    return [props, children], at


def serialize(node):
    props, children = node
    out = bytearray(struct.pack('<II', len(props), len(children)))
    for name, word, value, raw_name in props:
        out += raw_name if raw_name is not None else name.encode().ljust(32, b'\0')
        out += struct.pack('<I', word)
        out += value + bytes(((len(value) + 3) & ~3) - len(value))
    for child in children:
        out += serialize(child)
    return bytes(out)


def load(adt):
    data = bytes(adt)
    try:
        root, end = parse(data)
    except ValueError as error:
        raise SystemExit(f'invalid ADT: {error}') from None
    if end != len(data) or serialize(root) != data:
        raise SystemExit('the ADT does not parse and write back byte for byte; not an ADT this tool can edit')
    return root


def chosen(root):
    nodes = [c for c in root[1] if any(p[0] == 'name' and p[2].rstrip(b'\0') == b'chosen' for p in c[0])]
    if not nodes:
        raise SystemExit('the ADT has no /chosen')
    if len(nodes) != 1:
        raise SystemExit('the ADT has duplicate /chosen nodes')
    return nodes[0]


def tail_props(node):
    props = {}
    for p in node[0]:
        name = p[0]
        if name == PROP or name.startswith(PROP + '-'):
            if name in props:
                raise SystemExit(f'/chosen has duplicate {name} properties')
            props[name] = p
    return props


def sets(adt):
    """{property: text} of the ADT's /chosen/piano-boot-args*."""
    return {name: p[2].split(b'\0', 1)[0].decode('ascii', 'replace')
            for name, p in tail_props(chosen(load(adt))).items()}


def with_set(adt, set_name, tail):
    """ADT bytes with /chosen/piano-boot-args-SET_NAME (or /chosen/piano-boot-args for None) = the arguments TAIL (a list or a
    string), added or replaced."""
    text = ' '.join(tail) if isinstance(tail, (list, tuple)) else tail
    if not text or text != text.strip(' ') or any(not 0x20 <= ord(c) <= 0x7e for c in text):
        raise SystemExit(f'{PROP}: the tail must be printable ASCII without outer spaces, not empty')
    if len(text) > MERGED_MAX:
        raise SystemExit(f'{PROP}: {len(text)} bytes, more than the kernel\'s {MERGED_MAX}')
    root = load(adt)
    node = chosen(root)
    name = prop_name(set_name)
    value = text.encode('ascii') + b'\0'
    p = tail_props(node).get(name)
    if p:
        if p[1] & 0x80000000:
            raise SystemExit(f'/chosen/{name}: a placeholder property (DarwinBoot fills it)')
        p[1], p[2] = len(value), value
    else:
        node[0].append([name, len(value), value, None])
    out = serialize(root)
    if sets(out).get(name) != text:
        raise SystemExit(f'/chosen/{name}: not read back')
    return out


def selector(args):
    """The line's piano-boot-args value as PE_parse_boot_arg_str reads it (the last one; a bare name is "1"), or None."""
    value = None
    for x in args:
        if name_of(x) == PROP:
            value = x.split('=', 1)[1] if '=' in x else '1'
    return value


def merged(line, adt):
    """(boot-args, status) as k27 0066 makes them of LINE with this ADT: status appended, none, off, bad-set, missing, empty,
    bad-char or too-long (the merged text is LINE then)."""
    line_bytes = line.encode().partition(b'\0')[0][:LEGACY_MAX]
    line = line_bytes.decode(errors='ignore')
    sel = selector(line_bytes.decode(errors='replace').split())
    if sel == '0':
        return line, 'off'
    if sel is not None and not SET_RE.fullmatch(sel):
        return line, 'bad-set'
    name = PROP if sel is None else f'{PROP}-{sel}'
    p = tail_props(chosen(load(adt))).get(name)
    if p is None:
        return line, 'none' if sel is None else 'missing'
    raw = p[2].split(b'\0', 1)[0]
    if any(not 0x20 <= b <= 0x7e for b in raw):
        return line, 'bad-char'
    text = raw.decode('ascii').strip(' ')
    if not text:
        return line, 'empty'
    if len(line_bytes) + bool(line_bytes) + len(text) > MERGED_MAX:
        return line, 'too-long'
    return (f'{line} {text}' if line_bytes else text), 'appended'


def kc_has_boot_args(kc):
    return KC_MARKER in Path(kc).read_bytes()


def merged_reader_destinations(files):
    """Destinations of the migrated readers, based on the files the stager built."""
    libexec = '/usr/local/libexec/'
    found = {libexec + name for name in MERGED_READERS if READER_ABI in files.get(libexec + name, b'')}
    usbnet = libexec + 'piano-usbnet-address'
    if (b'/piano-bootargs.sh"' in files.get(usbnet, b'') and
            b'sysctl -n kern.piano_bootargs' in files.get(libexec + 'piano-bootargs.sh', b'')):
        found.add(usbnet)
    return found


def recorded_readers(artifact, record_path, seed=False):
    if not record_path.is_file():
        return set()
    try:
        record = json.loads(record_path.read_text())
        expected = record['outputs']['seed']['file_sha256'] if seed else record['archive_sha256']
        digest = hashlib.sha256()
        with artifact.open('rb') as stream:
            while chunk := stream.read(8 << 20):
                digest.update(chunk)
        if digest.hexdigest() != expected:
            raise ValueError(f'{artifact.name} differs from {record_path.name}')
        return {Path(f['dest']).name for f in record['files'] if f.get('merged_bootargs') is True}
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit(f'{record_path}: cannot verify the staged boot-argument readers ({error})') from error


def staged_readers(seed, live, layer):
    sources = []
    if seed is not None:
        sources.append(recorded_readers(seed, seed.with_name('userland.json'), seed=True))
    if live:
        path = layer or os.environ.get('PIANO_BOOTARGS_LIVE_DIR')
        if path:
            path = Path(path)
            sources.append(recorded_readers(path / 'userland-update.tar', path / 'live.json'))
        else:
            sources.append(set())
    return set.intersection(*sources) if sources else set()


def legacy_clip(full):
    """k27 0066's whole-argument clip, using byte offsets."""
    end = LEGACY_MAX
    if len(full) > end and full[end] != ord(' '):
        end = full.rfind(b' ', 0, end) + 1
    return full[:end].rstrip(b' ')


def check_userland(entries, adt, kc, *, seed=None, live=False, layer=None):
    """Check the final lines against the KC and userland that can run on each entry."""
    with Path(kc).open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
        has_tail = image.find(KC_MARKER) >= 0
        has_full = image.find(MERGED_SYSCTL_MARKER) >= 0
    readers = None
    for entry, line in entries.items():
        raw = line.encode()
        limit = line_max(line.split())
        if len(raw) > limit or b'\0' in raw:
            raise SystemExit(f'{entry}: bootloader argument line must have at most {limit} bytes without embedded NUL')
        full, status = merged(line, adt)
        if status not in ('appended', 'none', 'off'):
            raise SystemExit(f'{entry}: kernel rejects the ADT boot-argument tail ({status})')
        if status == 'appended' and not has_tail:
            raise SystemExit(f'{entry}: kernel needs k27 0066 to append the ADT boot-argument tail')
        full = full.encode()
        clipped = legacy_clip(full)
        if len(clipped) == len(full) and len(clipped) < LEGACY_MAX:
            continue
        names = {name_of(word) for word in full.decode().split(' ') if word}
        hidden = {name_of(word) for word in full[len(clipped):].decode().split(' ') if word}
        for reader, keys in (LEGACY_READERS | MERGED_READERS).items():
            unreadable = names if reader in SHORT_READERS and len(clipped) == LEGACY_MAX else hidden
            blocked = sorted(unreadable.intersection(keys))
            if not blocked:
                continue
            detail = f'{entry}: {reader} cannot read {", ".join(blocked)} through kern.bootargs'
            if reader in MERGED_READERS and has_full:
                if readers is None:
                    readers = staged_readers(seed, live, layer)
                if reader in readers:
                    continue
            if reader == 'piano-brightness-sync':
                print(f'{detail}. The driver properties still supply brightness readiness.', file=sys.stderr)
                continue
            if reader not in MERGED_READERS:
                reason = 'keep its arguments in the legacy prefix and within its reader buffer'
            elif not has_full:
                reason = 'rebuild the kernel collection with k27 0076'
            else:
                reason = 'provide the matching staged userland (seed record or PIANO_BOOTARGS_LIVE_DIR for a resumed store)'
            raise SystemExit(f'{detail}. {reason}.')


def header_numbers():
    text = HEADER.read_text()
    size = int(re.search(r'#define PIANO_BOOT_ARGS_SIZE\s+(\d+)', text).group(1))
    set_max = int(re.search(r'#define PIANO_BOOT_ARGS_SET_MAX\s+(\d+)', text).group(1))
    prop = re.search(r'#define PIANO_BOOT_ARGS_PROP\s+"([^"]+)"', text).group(1)
    return size, set_max, prop


def tiny_adt():
    def prop(name, value):
        return name.encode().ljust(32, b'\0') + struct.pack('<I', len(value)) + value + bytes(((len(value) + 3) & ~3) - len(value))
    chosen_node = struct.pack('<II', 2, 0) + prop('name', b'chosen\0') + prop('placeholder', b'\0' * 8)
    other = struct.pack('<II', 1, 0) + prop('name', b'arm-io\0')
    return struct.pack('<II', 1, 2) + prop('name', b'device-tree\0') + other + chosen_node


def test_userland_visibility():
    patch, = (DEVICE / 'patches/k27').glob('0076-*.patch')
    text = patch.read_bytes()
    assert b'sysctl_io_string(req, PE_boot_args(), 0, 0, NULL)' in text
    assert b'SYSCTL_PROC(_kern, OID_AUTO, piano_bootargs,' in text
    assert b'CTLFLAG_LOCKED | CTLFLAG_RD | CTLFLAG_KERN | CTLTYPE_STRING' in text
    assert MERGED_SYSCTL_MARKER.rstrip(b'\0') in text
    assert legacy_clip(b'x' * 1023 + b' y') == b'x' * 1023
    assert legacy_clip(b'x' * 1022 + b' y') == b'x' * 1022

    line = 'piano-boot-args=ram'
    pad = 'piano-pad=' + 'a' * (1016 - len(line) - len(' piano-pad= '))
    args = ['piano-usb-shell=1', 'piano-usb-net=1', 'piano-rotate=2']
    adt = with_set(tiny_adt(), 'ram', [pad, *args])
    full, status = merged(line, adt)
    assert status == 'appended' and full.index(args[0]) == 1016
    assert len(legacy_clip(full.encode())) == 1015

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        kc = root / 'kernel'
        layer = root / 'layer'
        layer.mkdir()
        archive = layer / 'userland-update.tar'
        archive.write_bytes(b'host fixture')
        libexec = '/usr/local/libexec/'
        staged = {libexec + 'pianousb-shell': READER_ABI, libexec + 'piano-rotated': READER_ABI,
                  libexec + 'piano-usbnet-address': b'. "$(dirname "$0")/piano-bootargs.sh"'}
        assert libexec + 'piano-usbnet-address' not in merged_reader_destinations(staged)
        staged[libexec + 'piano-bootargs.sh'] = b'sysctl -n kern.piano_bootargs'
        files = [{'dest': dest, 'merged_bootargs': True} for dest in sorted(merged_reader_destinations(staged))]
        (layer / 'live.json').write_text(json.dumps({'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
                                                    'files': files}))
        seed = root / 'seed'
        seed.write_bytes(b'host seed fixture')
        (root / 'userland.json').write_text(json.dumps({
            'outputs': {'seed': {'file_sha256': hashlib.sha256(seed.read_bytes()).hexdigest()}}, 'files': files,
        }))

        def refused(wanted, **sources):
            try:
                check_userland({'fixture': line}, adt, kc, **sources)
            except SystemExit as error:
                assert wanted in str(error), error
            else:
                raise AssertionError(f'expected refusal: {wanted}')

        kc.write_bytes(KC_MARKER)
        refused('with k27 0076', live=True, layer=layer)
        kc.write_bytes(KC_MARKER + MERGED_SYSCTL_MARKER)
        refused('pianousb-shell cannot read piano-usb-shell')
        check_userland({'fixture': line}, adt, kc, seed=seed, live=True, layer=layer)
        archive.write_bytes(b'stale fixture')
        refused('userland-update.tar differs from live.json', live=True, layer=layer)
        adt = with_set(tiny_adt(), 'ram', [pad, 'piano-tu-debug=none'])
        refused('NativeAdrenoMetal cannot read piano-tu-debug', seed=seed)


def self_test():
    size, set_max, prop = header_numbers()
    assert (size - 1, set_max, prop) == (MERGED_MAX, SET_MAX, PROP), 'boot_args.py and piano_bootargs_policy.h disagree'
    test_userland_visibility()
    # nothing moves while the line fits
    short = ['debug=0x14e', 'piano-gpu=2', 'data-persist=0']
    assert fit(short, 'ram') == (short, [])
    # forced: the piano-* arguments go, the safety ones and the rest stay, order kept, the selector last
    args = ['debug=0x14e', 'piano-gpu=2', 'piano-reset-after=600', 'piano-reset-to=bootloader', 'native-ufs-root=1',
            'piano-boot-guard=180', 'piano-boot-guard-ping=0', 'piano-audio=1', 'data-persist=0', 'piano-gpu=3']
    line, tail = fit(args, 'ram', force=True)
    assert line == ['debug=0x14e', 'piano-reset-after=600', 'piano-reset-to=bootloader', 'native-ufs-root=1', 'piano-boot-guard=180',
                    'piano-boot-guard-ping=0', 'data-persist=0', 'piano-boot-args=ram'], line
    assert tail == ['piano-gpu=2', 'piano-audio=1', 'piano-gpu=3'], tail
    # a long line splits by itself, and RESERVE counts
    long = ['debug=0x14e'] + [f'piano-x{i}={"y" * 40}' for i in range(30)]
    line, tail = fit(long, 'ram')
    assert len(' '.join(line)) < LINE_MAX and len(tail) == 30
    edge = ['a' * 990]
    assert fit(edge, 'ram', reserve=9) == (edge, [])
    try:
        fit(edge + ['piano-q=1'], 'ram', reserve=10)
    except SystemExit:
        pass
    else:
        raise AssertionError('a line that cannot get under the budget must be refused')
    for bad in ('0', '', 'a' * 16, 'a b', 'a/b'):
        try:
            prop_name(bad)
        except SystemExit:
            continue
        raise AssertionError(f'set name {bad!r} accepted')
    try:
        fit(['piano-boot-args=x', 'piano-a=1'], 'ram', force=True)
    except SystemExit:
        pass
    else:
        raise AssertionError('arguments with a selector must be refused')
    # the ADT: added, replaced, read back, the rest of the tree byte for byte
    adt = tiny_adt()
    one = with_set(adt, 'ram', ['piano-gpu=2', 'piano-audio=1'])
    assert sets(one) == {'piano-boot-args-ram': 'piano-gpu=2 piano-audio=1'}
    two = with_set(one, 'ram', 'piano-audio=2')
    assert sets(two) == {'piano-boot-args-ram': 'piano-audio=2'} and len(two) == len(one) - 12
    before, after = load(adt), load(two)
    assert before[0] == after[0] and before[1][0] == after[1][0], 'the root and the other nodes must not change'
    assert after[1][1][0][:2] == before[1][1][0] and after[1][1][1] == before[1][1][1], '/chosen keeps its properties, one added'
    both = with_set(two, None, 'piano-x=1')
    assert sets(both) == {'piano-boot-args-ram': 'piano-audio=2', 'piano-boot-args': 'piano-x=1'}
    # the kernel's merge, redone: the selector picks the set, no selector the plain property, =0 none
    assert merged('debug=0x14e piano-boot-args=ram', one) == ('debug=0x14e piano-boot-args=ram piano-gpu=2 piano-audio=1', 'appended')
    assert merged('debug=0x14e', one) == ('debug=0x14e', 'none')
    assert merged('debug=0x14e', both) == ('debug=0x14e piano-x=1', 'appended')
    assert merged('a piano-boot-args=0', both)[1] == 'off'
    assert merged('a piano-boot-args=other', both)[1] == 'missing'
    assert merged('a piano-boot-args=a/b', both)[1] == 'bad-set'
    full = with_set(adt, None, 'x' * (MERGED_MAX - 2))
    assert merged('a', full)[1] == 'appended' and len(merged('a', full)[0]) == MERGED_MAX
    assert merged('ab', full)[1] == 'too-long'
    for bad in ('', ' x', 'x ', 'a\tb', 'é'):
        try:
            with_set(adt, 'ram', bad)
        except SystemExit:
            continue
        raise AssertionError(f'tail {bad!r} accepted')
    print('boot_args.py: self-test passed (split, ADT set, merge; header numbers PIANO_BOOT_ARGS_SIZE '
          f'{size}, SET_MAX {set_max})')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--show', type=Path, metavar='ADT')
    p.add_argument('--line', help='a command line: print the boot-args the kernel makes of it with --show\'s ADT')
    p.add_argument('--self-test', action='store_true')
    a = p.parse_args()
    if a.self_test:
        self_test()
        return
    if not a.show:
        p.error('--show ADT or --self-test')
    data = a.show.read_bytes()
    found = sets(data)
    for name, text in found.items():
        print(f'/chosen/{name} ({len(text) + 1} bytes): {text}')
    if not found:
        print('no /chosen/piano-boot-args* in this ADT')
    if a.line is not None:
        text, status = merged(a.line, data)
        print(f'{status}: boot-args {len(text)} bytes (line {len(a.line)}): {text}')


if __name__ == '__main__':
    main()
