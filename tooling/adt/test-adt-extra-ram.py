#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""adt_extra_ram.py: the placeholder lands in /chosen only, zero-filled, the tree stays well formed, a second add is refused,
other nodes and properties are byte-identical."""
import importlib.util
import struct
from pathlib import Path

spec = importlib.util.spec_from_file_location('x', Path(__file__).with_name('adt_extra_ram.py'))
x = importlib.util.module_from_spec(spec); spec.loader.exec_module(x)


def prop(name, data):
    b = name.encode().ljust(32, b'\0') + struct.pack('<I', len(data)) + data
    return b + b'\0' * ((-len(b)) % 4)


def node(props, children=()):
    return struct.pack('<II', len(props), len(children)) + b''.join(props) + b''.join(children)


cpus = node([prop('name', b'cpus\0'), prop('reg', b'\1\2\3')])
chosen = node([prop('name', b'chosen\0'), prop('darwinboot-memory-size', struct.pack('<Q', 0x33fe00000))],
              [node([prop('name', b'memory-map\0'), prop('RAMDisk', bytes(16))])])
tree = node([prop('name', b'device-tree\0')], [cpus, chosen, node([prop('name', b'arm-io\0')])])
out = x.add_extra_ram(tree)
assert x.BYTES == 384
assert len(out) == len(tree) + 36 + x.BYTES
assert x.has_extra_ram(out) and not x.has_extra_ram(tree)
off, _ = x.find_chosen(out)
assert struct.unpack_from('<I', out, off)[0] == 3
i = out.index(b'piano-extra-ram\0')
assert struct.unpack_from('<I', out, i + 32)[0] == x.BYTES and out[i + 36:i + 36 + x.BYTES] == bytes(x.BYTES)
assert out.startswith(tree[:len(node([prop('name', b'device-tree\0')])) + len(cpus)])   # root props and cpus unchanged
assert out.endswith(tree[-(len(node([prop('name', b'arm-io\0')])) + len(chosen) - len(prop('name', b'chosen\0')) - 8 - len(prop('darwinboot-memory-size', bytes(8)))):])
try:
    x.add_extra_ram(out); raise SystemExit('FAIL: second add accepted')
except ValueError:
    pass
try:
    x.add_extra_ram(node([prop('name', b'device-tree\0')], [cpus])); raise SystemExit('FAIL: no /chosen accepted')
except ValueError:
    pass
assert x.ensure_extra_ram_placeholder(tree) == out
assert x.ensure_extra_ram_placeholder(out) == out
bad = bytearray(out)
bad[i + 36] = 1
try:
    x.ensure_extra_ram_placeholder(bad); raise SystemExit('FAIL: nonzero placeholder accepted')
except ValueError:
    pass
legacy = node([prop('name', b'device-tree\0')], [
    node([prop('name', b'chosen\0'), prop('piano-extra-ram', bytes(136))]),
])
try:
    x.ensure_extra_ram_placeholder(legacy); raise SystemExit('FAIL: legacy R1 placeholder accepted')
except ValueError:
    pass
print('PASS: /chosen piano-extra-ram placeholder added (384 zero bytes), tree well formed, other nodes unchanged, second add and no /chosen refused')


def refused(blob, message):
    try:
        x.ensure_extra_ram_placeholder(blob)
    except ValueError as error:
        assert message in str(error), error
    else:
        raise AssertionError(f'expected refusal: {message}')


placeholder = prop(x.NAME, bytes(x.BYTES))
root_name = prop('name', b'device-tree\0')
chosen_name = prop('name', b'chosen\0')
refused(out + b'\0', 'trailing bytes')
refused(out[:7], 'node header')
refused(out[:8 + 35], 'property header')
refused(node([root_name], [node([chosen_name, placeholder])])[:-1], 'value or padding')
refused(tree[:-1], 'value or padding')
refused(node([root_name], [node([chosen_name, placeholder, placeholder])]), 'duplicate')
refused(node([root_name], [node([chosen_name]), node([chosen_name])]), 'duplicate /chosen')
refused(node([root_name, placeholder], [chosen]), 'outside /chosen')
bad = bytearray(out)
struct.pack_into('<I', bad, i + 32, 0x80000000 | x.BYTES)
refused(bad, 'flagged length')

# Image staging only accepts a zeroed placeholder. Published ranges belong to DarwinBoot.
slab = 2 << 20
record = struct.Struct('<QQII')
misaligned = record.pack(4 * slab + 1, slab, 1, 1)
overlapping = record.pack(4 * slab, 2 * slab, 1, 1) + record.pack(5 * slab, slab, 1, 1)
for records in (misaligned, overlapping):
    prefilled = prop(x.NAME, records.ljust(x.BYTES, b'\0'))
    refused(node([root_name], [node([chosen_name, prefilled])]), 'zeroed placeholder')
