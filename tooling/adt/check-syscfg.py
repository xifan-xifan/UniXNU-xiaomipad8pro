#!/usr/bin/env python3
"""Check the /chosen properties of an ADT file that macOS 27.3's libMobileGestalt reads before it would wait 5 s (docs/identity.md
section 9): syscfg-v2-data and mac-address-wifi0.

  check-syscfg.py ADT          PASS when /chosen has syscfg-v2-data (an erased region or a table MobileGestalt accepts) and a
                               usable mac-address-wifi0 (6 bytes, unicast, not all zeros; never printed)
  check-syscfg.py --self-test  the rules on built-in erased, good and bad regions and addresses

Without the property, every process's first syscfg-backed MobileGestalt answer (AppKit's hardware accent color in every app's
NSApplication init) waits 5 s for an AppleDiagnosticDataAccessReadOnly service that piano never has; with any value MobileGestalt
does not wait. Two values are well formed:
  erased  at least 24 bytes, all 0xFF: what a Mac's NOR syscfg partition reads; MobileGestalt logs "syscfg is not initialized!",
          its sysconfig dictionary stays empty and the raw-syscfg answer is nothing, as on that Mac (piano's value);
  table   at least the 24-byte header, the magic 'SCfg' as a little-endian u32 at offset 0, the entry count at offset 0x14,
          24 + 20 * count bytes; an entry is a 4-byte key and 16 bytes of data, or 'CNTB' with key, size and offset of data that
          must lie inside the table.
Anything else (shorter, or neither) is refused. Without mac-address-wifi0, every Wi-Fi address question (the UDID's: akd asks
about every 5 s) waits 5 s for an IONetworkController under a service named wlan, sdio or marconi-wifi. Exit 0 = PASS, 1 = FAIL.
"""
import importlib.util
import struct
import sys
from pathlib import Path

MAGIC = 0x53436667   # 'SCfg'
CNTB = 0x434E5442    # 'CNTB'
HEADER, ENTRY = 0x18, 0x14


def key_text(value):
    """A syscfg key as MobileGestalt names it (the u32 byte-swapped: stored 'SHLC' reads as CLHS)."""
    return struct.pack('>I', value).decode('latin-1')


def classify(blob):
    """'erased' or the entry keys of a table MobileGestalt accepts; raises ValueError with the reason otherwise."""
    if len(blob) < HEADER:
        raise ValueError(f'{len(blob)} bytes, less than the {HEADER} bytes MobileGestalt reads ("syscfg length ... is too small")')
    if blob == b'\xff' * len(blob):
        return 'erased'
    magic, = struct.unpack_from('<I', blob, 0)
    if magic != MAGIC:
        raise ValueError(f'neither erased (all 0xFF) nor a table: magic 0x{magic:08x}, not 0x{MAGIC:08x}')
    count, = struct.unpack_from('<I', blob, 0x14)
    if len(blob) < HEADER + ENTRY * count:
        raise ValueError(f'{len(blob)} bytes cannot hold {count} entries ({HEADER + ENTRY * count} needed)')
    keys = []
    for i in range(count):
        at = HEADER + ENTRY * i
        key, = struct.unpack_from('<I', blob, at)
        if key == CNTB:
            real, size, start = struct.unpack_from('<III', blob, at + 4)
            if start > len(blob) or start + size > len(blob):
                raise ValueError(f'entry {i} ({key_text(real)}): data at {start}+{size} outside the {len(blob)}-byte table')
            key = real
        keys.append(key_text(key))
    return keys


def check_mac(value):
    """The kind of a usable Wi-Fi address; raises ValueError otherwise (the value itself is never in the message)."""
    if len(value) != 6:
        raise ValueError(f'{len(value)} bytes, not 6')
    if value[0] & 1:
        raise ValueError('a group address')
    if value in (bytes(6), b'\xff' * 6):
        raise ValueError('all zeros or all ones')
    return 'locally administered' if value[0] & 2 else 'universally administered'


def self_test():
    empty = struct.pack('<6I', MAGIC, HEADER, HEADER, 0, 0, 0)
    one = struct.pack('<6I', MAGIC, HEADER + ENTRY, HEADER + ENTRY, 0, 0, 1) + struct.pack('<I', 0x434C4853) + bytes(16)
    big = struct.pack('<6I', MAGIC, 0, 0, 0, 0, 1) + struct.pack('<5I', CNTB, 0x52656720, 4, HEADER + ENTRY, 0) + b'abcd'
    good = {'erased 24': (b'\xff' * 24, 'erased'), 'erased 128 KiB': (b'\xff' * 0x20000, 'erased'), 'empty table': (empty, []),
            'one inline entry': (one, ['CLHS']), 'one CNTB entry': (big, ['Reg '])}
    bad = {'short erased': b'\xff' * 23, 'short table': empty[:20], 'zeroed': bytes(24), 'count too big': empty[:0x14] + struct.pack('<I', 1),
           'CNTB out of range': big[:-1], 'mostly erased': b'\xff' * 23 + b'\0'}
    failures = []
    for name, (mac, want) in {'local': (bytes.fromhex('025049414e01'), 'locally administered'),
                              'universal': (bytes.fromhex('001122334455'), 'universally administered')}.items():
        try:
            if check_mac(mac) != want:
                failures.append(f'address {name}: {check_mac(mac)}')
        except ValueError as e:
            failures.append(f'address {name}: refused ({e})')
    for name, mac in {'5 bytes': bytes.fromhex('025049414e'), 'group': bytes.fromhex('015049414e01'), 'zeros': bytes(6)}.items():
        try:
            check_mac(mac)
            failures.append(f'address {name}: accepted')
        except ValueError:
            pass
    for name, (blob, want) in good.items():
        try:
            got = classify(blob)
            if got != want:
                failures.append(f'{name}: {got}, wanted {want}')
        except ValueError as e:
            failures.append(f'{name}: refused ({e})')
    for name, blob in bad.items():
        try:
            classify(blob)
            failures.append(f'{name}: accepted')
        except ValueError:
            pass
    print('SELF-TEST ' + ('FAIL: ' + '; '.join(failures) if failures else f'PASS ({len(good)} good, {len(bad)} bad values, 2 good and 3 bad addresses)'))
    return 1 if failures else 0


def main():
    if sys.argv[1:] == ['--self-test']:
        return self_test()
    if len(sys.argv) != 2:
        print(__doc__.strip().split('\n\n')[1])
        return 2
    spec = importlib.util.spec_from_file_location('adt_dump', Path(__file__).with_name('adt-dump.py'))
    adt = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adt)
    try:
        _, children = adt.parse_tree(Path(sys.argv[1]).read_bytes())
    except (OSError, ValueError) as error:
        print(f'FAIL: {error}')
        return 1
    chosen = [props for props in (dict(props) for props, _ in children)
              if props.get('name', b'').rstrip(b'\0') == b'chosen']
    if len(chosen) != 1:
        print(f'FAIL: {len(chosen)} /chosen nodes')
        return 1
    blob = chosen[0].get('syscfg-v2-data')
    if blob is None:
        print('FAIL: no /chosen syscfg-v2-data: every process waits 5 s in MobileGestalt (docs/identity.md section 9)')
        return 1
    try:
        kind = classify(blob)
    except ValueError as e:
        print(f'FAIL: /chosen syscfg-v2-data: {e}')
        return 1
    mac = chosen[0].get('mac-address-wifi0')
    if mac is None:
        print('FAIL: no /chosen mac-address-wifi0: every Wi-Fi address question waits 5 s in MobileGestalt (docs/identity.md section 9)')
        return 1
    try:
        mac_kind = check_mac(mac)
    except ValueError as e:
        print(f'FAIL: /chosen mac-address-wifi0: {e}')
        return 1
    what = 'erased (all 0xFF)' if kind == 'erased' else f'a table of {len(kind)} entries{" " + " ".join(kind) if kind else ""}'
    print(f'PASS: /chosen syscfg-v2-data, {len(blob)} bytes, {what}; /chosen mac-address-wifi0, 6 bytes, {mac_kind}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
