#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the k27 boot handoff with synthetic payloads and the checked-in patches."""

import hashlib
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCHES = ROOT / 'patches/k27'
OPT_IN = 'piano-long-boot-args=1'
USB_SHELL = 'piano-usb-shell=1'
SELECTOR = 'piano-boot-args=ram'
SMP = 'sheng-smp=1'
PREFIX = (OPT_IN, 'data-persist=0', 'native-ufs-root=1', 'probe=' + 'x' * 850)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ram = load('ram', 'tooling/bootimg/build-ram-images.py')
verify = load('verify', 'tooling/bootimg/verify-ram-images.py')
ba = ram.boot_args_module()
extra_ram = load('adt_extra_ram', 'tooling/adt/adt_extra_ram.py')
identity = load('installed_boot_identity', 'tooling/persist/installed_boot_identity.py')
manifests = load('repo_manifests', 'scripts/ci/test-repo-manifests.py')
kc_check = ROOT / 'tooling/k27/check-piano-bootargs-kc.py'


def sized_tail(size):
    """Tail arguments for an SMP entry with exactly SIZE merged ASCII bytes."""
    tail = ['piano-power-b4pd=0', 'piano-gpu-tier=0']
    if size > 1024:
        tail += [f'piano-probe-{i:02d}=' + 'x' * 50 for i in range(44)]
    base_size = len(' '.join([*PREFIX, SELECTOR, SMP, *tail, 'piano-pad=', USB_SHELL]))
    return [*tail, 'piano-pad=' + 'x' * (size - base_size), USB_SHELL]


class BootArgs(unittest.TestCase):
    def test_ram_entries_at_merged_byte_boundaries(self):
        for size in (1023, 1024, 3993, 4094, 4095):
            with self.subTest(size=size):
                expected_tail = sized_tail(size)
                # With no selector overhead, the two smallest cases fit on the loader line.
                one, smp, tail = ram.boot_entry_args(ba, [*PREFIX, *expected_tail], force=size <= 1024)
                self.assertEqual(tail, expected_tail)
                tree = ba.with_set(ba.tiny_adt(), 'ram', tail)
                for line, expected_size in ((smp, size), (one, size - 5)):
                    self.assertLessEqual(len(line.encode('ascii')), 1008)
                    self.assertIn(OPT_IN, line.split())
                    full, status = ba.merged(line, tree)
                    self.assertEqual(status, 'appended')
                    self.assertEqual(full, ' '.join([line, *tail]))
                    raw = full.encode('ascii')
                    self.assertEqual(len(raw), expected_size)
                    clip = ba.legacy_clip(raw)
                    if expected_size <= 1023:
                        self.assertEqual(clip, raw)
                    else:
                        self.assertEqual(clip, raw[:1023].rsplit(b' ', 1)[0])
                        self.assertNotIn(USB_SHELL.encode('ascii'), clip.split())
                    if size >= 3993:
                        self.assertGreater(raw.index(USB_SHELL.encode('ascii')), 1023)

        overflow_tail = sized_tail(4096)
        with self.assertRaisesRegex(SystemExit, '4096 bytes.*4095'):
            ram.boot_entry_args(ba, [*PREFIX, *overflow_tail])
        _, line, _ = ram.boot_entry_args(ba, [*PREFIX, *sized_tail(4095)])
        tree = ba.with_set(ba.tiny_adt(), 'ram', overflow_tail)
        self.assertEqual(ba.merged(line, tree), (line, 'too-long'))

    def test_loader_field_clips_at_1023_bytes(self):
        prefix = SELECTOR + ' probe='
        line = prefix + 'x' * (1023 - len(prefix))
        tree = ba.with_set(ba.tiny_adt(), 'ram', USB_SHELL)
        self.assertEqual(ba.merged(line, tree), (line + ' ' + USB_SHELL, 'appended'))
        self.assertEqual(ba.merged(line + 'y', tree), (line + ' ' + USB_SHELL, 'appended'))

    def test_duplicate_and_conflicting_flags(self):
        for suffix in ('data-persist=0', 'data-persist=2'):
            with self.subTest(suffix=suffix):
                mode = suffix[-1]
                self.assertEqual(verify.store_arguments(suffix), {'data-persist': mode})
                self.assertEqual(ram.exact_store_mode([suffix]), mode)
                line = 'data-persist=0 ' + SELECTOR
                tree = ba.with_set(ba.tiny_adt(), 'ram', suffix)
                full, status = ba.merged(line, tree)
                self.assertEqual(status, 'appended')
                with self.assertRaisesRegex(ValueError, 'repeated data-persist'):
                    verify.store_arguments(full)
                with self.assertRaisesRegex(SystemExit, 'data-persist must occur at most once'):
                    ram.exact_store_mode(full.split())

        for flag, enabled in (('piano-power-b4pd', '1'), ('piano-gpu-tier', '2')):
            off, on = f'{flag}=0', f'{flag}={enabled}'
            for pair in ((off, off), (on, off), (off, on)):
                with self.subTest(args=pair):
                    _, line, tail = ram.boot_entry_args(ba, [OPT_IN, *pair], force=True)
                    self.assertEqual(tail, list(pair))
                    tree = ba.with_set(ba.tiny_adt(), 'ram', tail)
                    full, status = ba.merged(line, tree)
                    self.assertEqual(status, 'appended')
                    self.assertEqual(full.split()[-2:], list(pair))
                    if pair[0] == pair[1]:
                        identity.need_args(full, off)
                    else:
                        with self.assertRaisesRegex(ValueError, f'conflicting installed boot arguments for {flag}'):
                            identity.need_args(full, off)

    def test_non_ascii_and_control_bytes(self):
        for bad in ('é', '\u0085', '\t', '\n', '\r', '\x1b', '\x7f', '\0'):
            value = f'piano-probe=a{bad}b'
            with self.subTest(bad=repr(bad)):
                with self.assertRaisesRegex(SystemExit, 'printable ASCII|embedded NUL'):
                    ram.boot_entry_args(ba, [OPT_IN, value])
                with self.assertRaisesRegex(SystemExit, 'printable ASCII'):
                    ba.with_set(ba.tiny_adt(), 'ram', value)
                if bad == '\0':
                    continue  # In an existing ADT property the first NUL terminates the value.
                root = ba.load(ba.tiny_adt())
                raw = value.encode('utf-8') + b'\0'
                ba.chosen(root)[0].append(['piano-boot-args-ram', len(raw), raw, None])
                self.assertEqual(ba.merged(SELECTOR, ba.serialize(root)), (SELECTOR, 'bad-char'))

    def test_late_usb_shell_requires_kernel_and_reader_support(self):
        _, line, tail = ram.boot_entry_args(ba, [*PREFIX, *sized_tail(3993)])
        tree = ba.with_set(ba.tiny_adt(), 'ram', tail)
        entries = {'macos-8cpu': line}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            kc = root / 'kernel'
            seed = root / 'seed'
            kc.write_bytes(b'baseline fixture')
            with self.assertRaisesRegex(SystemExit, 'needs k27 0066'):
                ba.check_userland(entries, tree, kc)
            kc.write_bytes(ba.KC_MARKER)
            with self.assertRaisesRegex(SystemExit, 'pianousb-shell.*k27 0076'):
                ba.check_userland(entries, tree, kc)
            kc.write_bytes(ba.KC_MARKER + b'\0' + ba.MERGED_SYSCTL_MARKER)
            with self.assertRaisesRegex(SystemExit, 'pianousb-shell.*matching staged userland'):
                ba.check_userland(entries, tree, kc)
            seed.write_bytes(b'seed fixture')
            (root / 'userland.json').write_text(json.dumps({
                'outputs': {'seed': {'file_sha256': hashlib.sha256(seed.read_bytes()).hexdigest()}},
                'files': [{'dest': '/usr/local/libexec/pianousb-shell', 'merged_bootargs': True}],
            }), encoding='utf-8')
            ba.check_userland(entries, tree, kc, seed=seed)


class AdtPacking(unittest.TestCase):
    def test_boot_tail_next_to_extra_ram_and_memory_map(self):
        firmware = ram.firmware_module()
        root = ba.load(ba.tiny_adt())
        ramdisk = struct.pack('<QQ', 0x880000000, 0x200000)
        memory_map = [['name', 11, b'memory-map\0', None], ['RAMDisk', 16, ramdisk, None]]
        ba.chosen(root)[1].append([memory_map, []])
        original = ba.serialize(root)
        packed = extra_ram.ensure_extra_ram_placeholder(firmware.adt_with_slot(original))
        _, start = extra_ram.find_chosen(packed)

        # The combined handoff must preserve the following node for every 4-byte padding case.
        for size in (3993, 3996, 4094, 4095):
            with self.subTest(size=size):
                _, line, tail = ram.boot_entry_args(ba, [*PREFIX, *sized_tail(size)])
                tree = ba.with_set(packed, 'ram', tail)
                _, end = extra_ram.find_chosen(tree)
                raw = ' '.join(tail).encode('ascii') + b'\0'
                header = b'piano-boot-args-ram'.ljust(32, b'\0') + struct.pack('<I', len(raw))
                self.assertEqual(tree[start:end], header + raw + bytes(-len(raw) % 4))
                self.assertEqual(tree[end:], packed[start:])
                self.assertEqual(ba.sets(tree), {'piano-boot-args-ram': raw[:-1].decode('ascii')})
                self.assertEqual(ba.merged(line, tree), (' '.join([line, *tail]), 'appended'))
                self.assertEqual(extra_ram.extra_ram_property(tree), struct.pack('<QQII', 0, 0, 0, 0) * 16)
                self.assertEqual(extra_ram.ensure_extra_ram_placeholder(tree), tree)
                self.assertTrue(firmware.adt_slot(tree))
                self.assertEqual(ba.chosen(ba.load(tree))[1][0][0][1][2], ramdisk)
                with self.assertRaisesRegex(SystemExit, 'truncated ADT property'):
                    ba.with_set(tree[:-1], 'ram', tail)


class PatchGuards(unittest.TestCase):
    def test_native_patch_sequence_and_headers(self):
        patches = sorted(PATCHES.glob('*.patch'))
        numbers = []
        for path in patches:
            with self.subTest(patch=path.name):
                name = re.fullmatch(r'([0-9]{4})-.+\.patch', path.name)
                self.assertIsNotNone(name)
                number = int(name[1])
                numbers.append(number)
                patch = path.read_text(encoding='utf-8')
                # 0074 names the target OS's runtime mount source.
                runtime_path = r'(?<![\w/.-])/private/var/ngpu/app-library(?=["\s]|$)'
                self.assertNotIn('private/', re.sub(runtime_path, '', patch) if number == 74 else patch)
                diffs = re.split(r'^diff --git ', patch, flags=re.M)[1:]
                self.assertTrue(diffs, 'missing Git file headers')
                for diff in diffs:
                    old, new = diff.splitlines()[0].split()
                    self.assertTrue(old.startswith('a/') and new.startswith('b/'))
                    for path_name in (old, new):
                        self.assertTrue(all(part not in ('', '.', '..', 'private') for part in path_name.split('/')))
                    headers = re.search(r'^--- (\S+)\n\+\+\+ (\S+)\n@@ ', diff, re.M)
                    self.assertIsNotNone(headers, f'missing unified headers for {new}')
                    self.assertIn(headers[1], (old, '/dev/null'))
                    self.assertIn(headers[2], (new, '/dev/null'))
        # The baseline 0001–0052 lives upstream. docs/bringup-debugging.md records the 0072 gap.
        self.assertGreaterEqual(max(numbers, default=0), 77)
        self.assertEqual(numbers, [n for n in range(53, max(numbers) + 1) if n != 72])
        result = subprocess.run(['git', 'apply', '--numstat', *map(str, patches)], cwd=ROOT,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_0077_build_requires_literal_opt_in(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for relative in ('tooling/k273/prepare-k27-tree.sh', 'tooling/k27/k27-tree.sh'):
                dest = root / relative
                dest.parent.mkdir(parents=True)
                shutil.copyfile(ROOT / relative, dest)
            patch_dir = root / 'patches/k27'
            patch_dir.mkdir(parents=True)
            for number in ('0076', '0077'):
                path, = PATCHES.glob(f'{number}-*.patch')
                (patch_dir / path.name).touch()
            source = root / 'source'
            source.mkdir()
            stub_kc = root / 'out/inputs/k273/kc/kc273-mac17j.bin'
            stub_kc.parent.mkdir(parents=True)
            stub_kc.touch()
            trees = root / 'out/k273'
            for name in ('k27-tree', 'k27-tree-gpu-tier-wc'):
                clone = trees / name / 'k27'
                clone.mkdir(parents=True)
                (clone / 'kc27-layout.py').write_text(
                    'from pathlib import Path\nprint(Path.cwd().name)\nraise SystemExit(42)\n', encoding='utf-8')
            tools = root / 'bin'
            tools.mkdir()
            (tools / 'python3').symlink_to(sys.executable)
            git = tools / 'git'
            git.write_text('#!/bin/sh\ncase "$3:$4" in\n'
                           '  apply:--reverse) exit 1 ;;\n'
                           '  apply:*) printf "selected %s\\n" "${4##*/}" ;;\n'
                           'esac\n', encoding='utf-8')
            git.chmod(0o755)
            env = {'PATH': os.pathsep.join((str(tools), os.defpath)), 'HOME': tmp,
                   'PIANO_K27_TREE': str(source), 'K27_TREE_TOP': '0075'}
            command = ['bash', str(root / 'tooling/k273/prepare-k27-tree.sh')]
            for flag in (None, '', '0', 'true', '01', '2', '1'):
                with self.subTest(flag=flag):
                    run_env = dict(env)
                    if flag is not None:
                        run_env['PIANO_K27_GPU_TIER_WC'] = flag
                    result = subprocess.run(command, env=run_env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 42, result.stdout + result.stderr)
                    self.assertIn('selected 0076-', result.stdout)
                    self.assertEqual('selected 0077-' in result.stdout, flag == '1', result.stdout)
                    self.assertEqual(result.stdout.splitlines()[-1], 'k27-tree-gpu-tier-wc' if flag == '1' else 'k27-tree')

            header = trees / 'k27-tree/out/k27/xnu/osfmk/arm/pmap/pmap_piano_gpu_wc.h'
            header.parent.mkdir(parents=True)
            header.touch()
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn('0077 is already present', result.stderr)

        ci_build = (ROOT / 'tooling/ci/k27-kernel.sh').read_text(encoding='utf-8')
        default = re.search(r'\bTOP=\$\{K27_TREE_TOP:-([0-9]{4})\}', ci_build)
        self.assertIsNotNone(default, 'CI needs an explicit default patch level')
        self.assertLess(int(default[1]), 77, 'the default CI kernel must omit 0077')


class KextChecks(unittest.TestCase):
    def test_checked_driver_families_are_linked(self):
        names = manifests.kext_names(ROOT)
        manifests.validate_descriptors(ROOT, [*names, 'NativeSoftGpu'])
        manifests.validate_kext_build(ROOT, [*names, 'NativeSoftGpu'])
        # The live identity checker names the driver whose console evidence it accepts.
        for key, pattern in identity.DRIVER_ARGS.items():
            with self.subTest(key=key):
                driver = re.match(r'\^([A-Za-z0-9_]+):', pattern)
                self.assertIsNotNone(driver, pattern)
                self.assertIn(driver[1], names)

        # Userland readers can also rely on a companion driver. NativeSoftMetal uses the optional NativeSoftGpu.
        companions = {
            'pianousb-shell': 'PianoDwc3',
            'piano-usbnet-address': 'PianoDwc3',
            'piano-rotated': 'PianoSensors',
            'piano-brightness-sync': 'PianoBacklight',
            'piano-camera-grab': 'PianoCamera',
            'NativeAdrenoMetal': 'PianoA830',
            'Turnip surface residency': 'PianoA830',
            'Turnip BO clear': 'PianoA830',
        }
        for reader, kext in companions.items():
            self.assertIn(kext, names, reader)
        self.assertEqual(set(ba.MERGED_READERS) | set(ba.LEGACY_READERS),
                         set(companions) | {'NativeSoftMetal', 'piano-lw-quiet', 'settime.sh'})
        soft_gpu = json.loads((ROOT / 'tooling/k27/kext/NativeSoftGpu.json').read_text(encoding='utf-8'))
        self.assertIn('NativeSoftMetal', {p.get('MetalPluginName') for p in soft_gpu['personalities'].values()})

        sources = {
            'NativeUfs/NativeUfs.cpp': ('data-persist', 'native-ufs-root-part'),
            'NativeRamBlock/NativeRamBlock.cpp': ('data-store-replace', 'data-store-flatten'),
            'PianoPower/PianoPower.cpp': ('piano-power-b4pd',),
            'PianoA830/adreno830/A830GpuTier.h': ('piano-gpu-tier',),
            'PianoDwc3/PianoDwc3.cpp': (*ba.MERGED_READERS['pianousb-shell'],
                                       *ba.MERGED_READERS['piano-usbnet-address']),
            'PianoSensors/PianoSensors.cpp': ('piano-rotate', 'piano-rotate-test'),
            'PianoBacklight/PianoBacklight.cpp': ba.MERGED_READERS['piano-brightness-sync'],
            'NativeGopDisplay/NativeGopDisplay.cpp': sorted(ba.ADT_DISPLAY_KEYS),
        }
        for path, keys in sources.items():
            self.assertIn(path.split('/')[0], names)
            source = (ROOT / 'package' / path).read_text(encoding='utf-8')
            for key in keys:
                with self.subTest(key=key, source=path):
                    self.assertRegex(source, re.escape(f'"{key}') + r'[="]')

    def test_0076_symbols_must_be_defined(self):
        names = ('_sysctl__kern_bootargs', '_sysctl_piano_bootargs', '_sysctl__kern_piano_bootargs')
        legacy_strings = b'\0' + ba.KC_MARKER + b'\0'
        merged_strings = legacy_strings + b'piano_bootargs\0' + ba.MERGED_SYSCTL_MARKER
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp) / 'kernel'
            for baseline, types, sections, short_strings, expected in (
                (False, (0x0e, 0x0e, 0x0f), (1, 1, 1), False, 0),  # Local and external section symbols.
                (False, (0x0e, 0x01, 0x0f), (1, 0, 1), False, 1),  # Undefined handler.
                (False, (0x0e, 0x0e, 0x01), (1, 1, 0), False, 1),  # Undefined OID.
                (False, (0x01, 0x0e, 0x0f), (0, 1, 1), False, 1),  # Undefined legacy OID.
                (False, (0x0e, 0xee, 0x0f), (1, 1, 1), False, 1),  # Debug entry with the handler's name.
                (False, (0x0e, 0x0e, 0x0f), (1, 0, 1), False, 1),  # N_SECT with no section ordinal.
                (False, (0x0e, 0x0e, 0x0f), (1, 1, 1), True, 1),   # OID name extends beyond the string table.
                (True, (0x0e,), (1,), False, 0),
            ):
                with self.subTest(baseline=baseline, types=types, sections=sections, short_strings=short_strings):
                    symbols, strings = bytearray(), bytearray(b'\0')
                    for name, kind, section in zip(names, types, sections):
                        symbols.extend(struct.pack('<IBBHQ', len(strings), kind, section, 0, 0))
                        strings.extend(name.encode('ascii') + b'\0')
                    header = struct.pack('<IiiIIIII', 0xfeedfacf, 0x0100000c, 0, 2, 1, 24, 0, 0)
                    strsize = len(strings) - int(short_strings)
                    symtab = struct.pack('<6I', 2, 24, 56, len(types), 56 + len(symbols), strsize)
                    kernel.write_bytes(header + symtab + symbols + strings + (legacy_strings if baseline else merged_strings))
                    args = ['--baseline'] if baseline else []
                    result = subprocess.run([sys.executable, str(kc_check), str(kernel), *args],
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                    message = 'check-piano-bootargs-kc: ' + ('FAIL' if expected else 'PASS')
                    if short_strings:
                        self.assertIn('symbol name outside the string table', result.stderr)
                    else:
                        self.assertIn(message, result.stdout)


if __name__ == '__main__':
    unittest.main()
