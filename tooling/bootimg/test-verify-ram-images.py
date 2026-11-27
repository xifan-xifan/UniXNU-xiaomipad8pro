#!/usr/bin/env python3
"""Verify synthetic Android v2 containers with an in-memory FAT file listing."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


vr = load('verify-ram-images')
builder = load('build-ram-images')
PAGE = 2048


def digest(data):
    return hashlib.sha256(data).hexdigest()


def pages(data):
    return data + bytes(-len(data) % PAGE)


def node(name, properties=(), children=()):
    props = [('name', name.encode() + b'\0'), *properties]
    return (struct.pack('<II', len(props), len(children)) +
            b''.join(key.encode().ljust(32, b'\0') + struct.pack('<I', len(data)) + data + bytes(-len(data) % 4)
                     for key, data in props) + b''.join(children))


class VerifyRamImages(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.image = self.root / 'boot.img'
        self.record = self.root / 'build.json'
        self.registry = self.root / 'warm.json'
        self.fd, self.dtb, self.payload = b'fixture FD', b'fixture DTB', b'fixture FAT'
        (self.root / 'fd').write_bytes(self.fd)
        (self.root / 'dtb').write_bytes(self.dtb)
        shim = bytearray(64)
        struct.pack_into('<QQQ', shim, 8, 0, PAGE, 0xa)
        shim[56:60] = b'ARM\x64'
        kernel = bytes(shim) + self.fd + self.dtb
        header = vr.HEADER.pack(b'ANDROID!', len(kernel), 0x8000, len(self.payload), 0x1000000, 0, 0, 0x100,
                                PAGE, 2, 0, b'', b'', vr.mkbootimg_id(kernel, self.payload, self.dtb), b'',
                                0, 0, vr.HEADER.size, len(self.dtb), 0x1f00000)
        self.data = b''.join(map(pages, (header, kernel, self.payload, self.dtb)))
        self.image.write_bytes(self.data)
        (self.root / 'reference.img').write_bytes(self.data)
        self.info = {'shim_bytes': len(shim), 'payload_sha256': digest(self.payload)}
        self.build = {'fd': {'file': 'fd', 'sha256': digest(self.fd)}, 'dtb_sha256': digest(self.dtb),
                      'images': {self.image.name: self.info}}
        self.files = {'EFI/UniXNU/unixnu.toml': b'version = 1\n[[entry]]\ntype = "action"\n'}

    def verify(self, *args, write_record=True):
        if write_record:
            self.record.write_text(json.dumps(self.build))
        files = {name: (len(data), digest(data), data.decode() if name.endswith('.toml') else None)
                 for name, data in self.files.items()}
        output = io.StringIO()
        with contextlib.redirect_stdout(output), patch.object(vr, 'WARM_SEEDS', self.registry), \
                patch.object(vr, 'fat_files', return_value=(True, files)), \
                patch.object(vr, 'fat_file', side_effect=lambda _, name: self.files[name]):
            code = vr.main([str(self.record), '--reference', str(self.root / 'reference.img'),
                            '--fd', str(self.root / 'fd'), '--dtb', str(self.root / 'dtb'), *map(str, args)])
        return code, output.getvalue()

    def check_result(self, status, item, *args, **kwargs):
        code, output = self.verify(*args, **kwargs)
        self.assertEqual(code, status, output)
        self.assertIn(item, output)

    def macos(self, args, guard=False, tail=None):
        self.files['EFI/UniXNU/unixnu.toml'] = f'[[entry]]\ntype = "xnu"\nargs = "{args}"\n'.encode()
        props = [('test-root', vr.ROOT_27_3 + b'\0')]
        if tail:
            props.append(('piano-boot-args-ram', tail.encode() + b'\0'))
        kc, adt, trust, seed = vr.PAYLOADS
        self.files.update({
            kc: b'fixture KC' + (b'\0data-store-replace\0' if guard else b''),
            adt: node('device-tree', children=[node('chosen', props, [node('memory-map')])]),
            trust: b'fixture trust',
            seed: b'fixture seed',
        })
        self.info.update({key: digest(self.files[name]) for name, key in vr.PAYLOADS.items()})

    def test_unsigned_and_explicit_path(self):
        self.check_result(0, 'PASS: 0 failure(s)')
        signed = self.root / 'boot-signed.img'
        signed.write_bytes(self.data)
        self.check_result(0, 'PASS: 0 failure(s)', signed)
        unrecorded = self.root / 'other.img'
        unrecorded.write_bytes(self.data)
        self.check_result(1, 'image record for other.img', unrecorded)

    def test_bad_record(self):
        self.record.write_text('{')
        self.check_result(1, 'FAIL: 1 failure(s)', write_record=False)
        with patch.dict(self.build, {'images': {}}):
            self.check_result(1, 'build record has no images')
        del self.info['payload_sha256']
        self.check_result(1, 'missing or invalid payload_sha256')

    def test_container_bounds(self):
        with patch.object(vr, 'IMAGE_MAX_BYTES', len(self.data)):
            self.check_result(0, 'PASS: 0 failure(s)')
            self.image.write_bytes(self.data + b'\0')
            self.check_result(1, 'exceeds')
        self.image.write_bytes(self.data[:-1])
        self.check_result(1, 'sections extend beyond the file')
        invalid = bytearray(self.data)
        struct.pack_into('<I', invalid, 36, 0)
        self.image.write_bytes(invalid)
        self.check_result(1, 'invalid boot image page size')
        self.image.write_bytes(self.data[:vr.HEADER.size - 1])
        self.check_result(1, 'truncated Android boot image header')

    def test_recorded_image_hash_covers_padding_and_signed_prefix(self):
        self.info.update(sha256=digest(self.data), bytes=len(self.data))
        self.check_result(0, 'image bytes match the build record')
        signed = self.root / 'boot-signed.img'
        signed.write_bytes(self.data + b'fixture footer')
        self.check_result(0, 'image bytes match the build record', signed)
        modified = bytearray(self.data)
        modified[PAGE - 1] = 1  # Outside the sections hashed by mkbootimg.
        self.image.write_bytes(modified)
        signed.write_bytes(modified + b'fixture footer')
        self.check_result(1, 'image bytes match the build record')
        self.check_result(1, 'image bytes match the build record', signed)

    def test_mkbootimg_id_known_encoding(self):
        self.assertEqual(vr.mkbootimg_id(b'kernel', b'ramdisk', b'dtb').hex(),
                         '7bfbb9fb2b9e8de49aea126a957fba6c0418c856')

    def test_builder_loads_unixnu_payload_on_this_python(self):
        scripts = self.root / 'UniXNU/scripts/unixnu'
        scripts.mkdir(parents=True)
        (scripts / '__init__.py').touch()
        (scripts / 'payload.py').write_text(
            'import os\n'
            'import tomllib\n'
            'def write_volume(stage, image, label, size_mib):\n'
            '    epoch = int(os.environ["SOURCE_DATE_EPOCH"])\n'
            '    assert all(path.stat().st_mtime == epoch for path in stage.rglob("*"))\n'
            '    image.write_text(tomllib.loads(\'value = "ready"\')["value"])\n')
        source = ('import runpy, sys; from pathlib import Path; '
                  'b = runpy.run_path(sys.argv[1]); b["uefi_payload"](Path(sys.argv[2]))')
        env = dict(os.environ, UNIXNU_RESEARCH=str(self.root))
        result = subprocess.run([sys.executable, '-c', source, builder.__file__, str(self.root)],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / 'payload-uefi.img').read_text(), 'ready')

    def test_missing_macos_payload_hash(self):
        self.macos('data-persist=0')
        del self.info['trust_sha256']
        self.check_result(1, 'missing or invalid trust_sha256')

    def test_verifies_every_xnu_entry_with_inline_toml_comments(self):
        self.macos('data-persist=0', guard=True)
        self.info['data_store'] = {'data_persist': 0}
        menu = 'EFI/UniXNU/unixnu.toml'
        self.files[menu] += b'[[entry]]\ntype = "xnu" # additional boot option\nargs = "data-persist=2"\n'
        self.check_result(1, 'FAIL  boot data-persist=2 matches the build record')

    def test_loader_line_byte_limit_with_adt_tail(self):
        start = 'piano-boot-args=ram '
        args = start + 'x' * (1023 - len(start))
        self.macos(args, tail='data-persist=0')
        self.check_result(0, 'ADT boot arguments resolve')
        self.macos(args[:-1] + 'é', tail='data-persist=0')
        self.check_result(1, 'exceeds the 1023-byte DarwinBoot limit')

    def test_extra_ram_handoff_requires_zeroed_descriptor_table(self):
        self.macos('data-persist=0')
        adt = list(vr.PAYLOADS)[1]
        self.info['extra_ram'] = True
        self.check_result(1, 'extra-RAM placeholder is missing')
        slab = 2 << 20
        record = struct.Struct('<QQII')
        misaligned = record.pack(0x880000001, slab, 1, 1)
        overlapping = record.pack(0x880000000, 2 * slab, 1, 1) + record.pack(0x880200000, slab, 1, 1)
        for records, status in ((b'', 0), (misaligned, 1), (overlapping, 1)):
            with self.subTest(records=records):
                props = [('test-root', vr.ROOT_27_3 + b'\0'), ('piano-extra-ram', records.ljust(384, b'\0'))]
                self.files[adt] = node('device-tree', children=[node('chosen', props)])
                self.info['adt_sha256'] = digest(self.files[adt])
                self.check_result(status, '384-byte zeroed placeholder')

    def test_truncated_or_corrupt_firmware_headers(self):
        self.macos('data-persist=0')
        adt = list(vr.PAYLOADS)[1]
        chosen = node('chosen', [('test-root', vr.ROOT_27_3 + b'\0')],
                      [node('memory-map', [('GpuAddon', bytes(16))])])
        self.files[adt] = node('device-tree', children=[chosen])
        self.info['adt_sha256'] = digest(self.files[adt])
        self.info['firmware'] = {'addon': True}
        name = 'EFI/UniXNU/Payloads/kernel.macho.gpuaddon'
        fw = builder.firmware_module()
        stream = zlib.compress(b'firmware', 9)

        def addon(body):
            return struct.pack('<8sIIQ', fw.ADDON_MAGIC, 1, 0, 24 + len(body)) + body

        def entry(packed, size=8):
            return struct.pack('<IIQQ', 2, 0o644, size, len(packed)) + fw.pad8(b'f\0') + fw.pad8(packed)

        cases = (addon(b'')[:23], addon(entry(stream)[:23]), addon(entry(b'\0' + stream[1:])),
                 addon(entry(stream + b'junk')), addon(entry(stream, 2)))
        for i, blob in enumerate(cases):
            with self.subTest(case=i):
                self.files[name] = blob
                self.check_result(1, 'the addon parses')
        valid = fw.container([('f', 0o644, b'firmware')])
        self.assertEqual(valid, addon(entry(stream)))
        self.assertEqual(fw.parse_addon(valid), [('f', 0o644, b'firmware')])

    def test_malformed_gpuaddon_slot_is_rejected(self):
        fw = builder.firmware_module()
        for props in ([('GpuAddon', bytes(15))], [('GpuAddon', bytes(16))] * 2):
            with self.subTest(props=props), self.assertRaisesRegex(ValueError, 'GpuAddon'):
                tree = node('device-tree', children=[node('chosen', children=[node('memory-map', props)])])
                fw.adt_with_slot(tree)

    def test_effective_store_guard_and_warm_seed(self):
        self.macos('piano-boot-args=ram', tail='data-persist=2')
        self.info['data_store'] = {'data_persist': 2, 'same_seed': True, 'kc_has_store_guard': True}
        self.check_result(1, 'uses the installed seed or has the store guard')
        self.macos('piano-boot-args=ram', guard=True, tail='data-persist=2')
        self.check_result(0, 'PASS: 0 failure(s)')
        self.info['data_store']['data_persist'] = 0
        self.check_result(1, 'boot data-persist=2 matches the build record')
        del self.info['data_store']
        self.registry.write_text(json.dumps({self.info['seed_sha256']: {}}))
        self.check_result(1, 'a warm test seed runs data-persist=0')
        self.macos('data-persist=0')
        self.check_result(0, 'PASS: 0 failure(s)')

    def test_exact_args_cannot_bypass_store_options(self):
        for arg in ('data-store-replace=1', 'data-store-flatten=1', 'data-persist=0 data-persist=2', 'data-persist'):
            with self.subTest(arg=arg), self.assertRaises(SystemExit):
                builder.exact_store_mode(arg.split())
        self.assertEqual(builder.exact_store_mode(['data-persist=2']), '2')

    def test_replacement_must_be_recorded(self):
        self.macos('data-persist=2 data-store-replace=1', guard=True)
        self.info['data_store'] = {'data_persist': 2}
        self.check_result(1, 'replacement or flattening requires the recorded guarded mode')
        self.info['data_store']['replace'] = '1'
        self.check_result(0, 'PASS: 0 failure(s)')


if __name__ == '__main__':
    unittest.main()
