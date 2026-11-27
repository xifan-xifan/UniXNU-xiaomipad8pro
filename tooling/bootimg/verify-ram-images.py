#!/usr/bin/env python3
"""Offline verification of the piano RAM-boot images (plan 4(b) step 4). Nothing here touches a device.

  verify-ram-images.py BUILD_JSON [--reference UBUNTU_RAM_IMG] [--avbtool AVBTOOL --pubkey KEY.avbpubkey] [IMG ...]

Per image: the v2 header equals the Ubuntu RAM set's (v1.1.0 boot-ram-maintenance.img) in every field but the sizes,
command line and id; the id is recomputed; the kernel field is BootShim (ARM64 Linux Image header) + the exact FD + the
boot DTB; the v2 dtb section is the boot DTB; the ramdisk is a FAT volume whose files
are listed and, for the macOS image, hashed against the inputs, with the boot addon and the ADT's GpuAddon slot present
together or not at all and every addon entry matching its pin in the payload's collection or the recorded sensor pack
(docs/kc-space.md); an AVB footer,
when present, is shown and verified with the public key. Prints a log; exits 1 on any failure.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HEADER = struct.Struct('<8s10I16s512s32s1024sIQII Q')   # boot_img_hdr_v2
FIELDS = ('magic', 'kernel_size', 'kernel_addr', 'ramdisk_size', 'ramdisk_addr', 'second_size', 'second_addr', 'tags_addr',
          'page_size', 'header_version', 'os_version', 'name', 'cmdline', 'id', 'extra_cmdline', 'recovery_dtbo_size',
          'recovery_dtbo_offset', 'header_size', 'dtb_size', 'dtb_addr')
SAME_AS_REFERENCE = ('magic', 'kernel_addr', 'ramdisk_addr', 'second_size', 'second_addr', 'tags_addr', 'page_size',
                     'header_version', 'os_version', 'name', 'recovery_dtbo_size', 'recovery_dtbo_offset', 'header_size',
                     'dtb_addr')
ROOT_27_0_1 = b'C73696D4-CAC4-40A0-90CB-7D0463E4E3E6'   # configs/k27-adt.toml root_match_volume_uuid (retired line)
ROOT_27_3 = b'84581E99-85BE-4957-AC53-2947E887B3E4'     # configs/k273-adt.toml
IMAGE_MAX_BYTES = int(os.environ.get('PIANO_IMAGE_MAX_BYTES', '250000000'))   # build-ram-images.py's
ROOT = Path(__file__).resolve().parents[2]
WARM_SEEDS = ROOT / 'out/warm-seeds.json'
PAYLOADS = {
    'EFI/UniXNU/Payloads/kernel.macho': 'kc_sha256',
    'EFI/UniXNU/Payloads/kernel.macho.adt': 'adt_sha256',
    'EFI/UniXNU/Payloads/kernel.macho.trustcache': 'trust_sha256',
    'EFI/UniXNU/Payloads/scratch-data.img': 'seed_sha256',
}
failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what)
    if not ok:
        failures.append(what)


def header(data):
    if len(data) < HEADER.size:
        raise ValueError('truncated Android boot image header')
    values = dict(zip(FIELDS, HEADER.unpack_from(data, 0)))
    page = values['page_size']
    if values['magic'] != b'ANDROID!' or values['header_version'] != 2 or values['header_size'] != HEADER.size:
        raise ValueError('expected an Android boot image v2 header')
    if page < HEADER.size or page & (page - 1) or page > len(data):
        raise ValueError('invalid boot image page size')
    if values['second_size'] or values['recovery_dtbo_size'] or values['recovery_dtbo_offset']:
        raise ValueError('unexpected second or recovery DTBO section')
    return values


def sections(data, h):
    page = h['page_size']
    def pages(n):
        return -(-n // page) * page
    offset = page   # header occupies one page (header_size 1660 < 4096)
    kernel = data[offset:offset + h['kernel_size']]; offset += pages(h['kernel_size'])
    ramdisk = data[offset:offset + h['ramdisk_size']]; offset += pages(h['ramdisk_size'])
    offset += pages(h['second_size']) + pages(h['recovery_dtbo_size'])
    dtb = data[offset:offset + h['dtb_size']]; offset += pages(h['dtb_size'])
    if offset > len(data):
        raise ValueError('boot image sections extend beyond the file')
    return kernel, ramdisk, dtb, offset


def mkbootimg_id(kernel, ramdisk, dtb):
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b'', b'', dtb):   # kernel, ramdisk, second, recovery dtbo, dtb (mkbootimg v2 order)
        sha.update(blob)
        sha.update(struct.pack('<I', len(blob)))
    return sha.digest()


def fat_files(ramdisk):
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    with tempfile.TemporaryDirectory() as tmp:
        image = Path(tmp) / 'fat.img'
        image.write_bytes(ramdisk)
        listing = subprocess.run(['mdir', '-/', '-b', '-i', str(image), '::'], capture_output=True, text=True, env=env)
        files = {}
        if listing.returncode:
            return False, files
        for line in sorted(listing.stdout.splitlines()):
            if line.endswith('/'):
                continue
            out = Path(tmp) / 'f'
            subprocess.run(['mcopy', '-n', '-o', '-i', str(image), line, str(out)], check=True, env=env, capture_output=True)
            files[line.removeprefix('::').lstrip('/')] = (out.stat().st_size, hashlib.sha256(out.read_bytes()).hexdigest(),
                                                               out.read_text(errors='replace') if line.endswith('.toml') else None)
        return True, files


def fat_file(ramdisk, name):
    """The bytes of one file of the payload FAT."""
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    with tempfile.TemporaryDirectory() as tmp:
        image = Path(tmp) / 'fat.img'
        image.write_bytes(ramdisk)
        out = Path(tmp) / 'f'
        subprocess.run(['mcopy', '-n', '-o', '-i', str(image), '::/' + name, str(out)], check=True, env=env, capture_output=True)
        return out.read_bytes()


def check_firmware(ramdisk, files, info, adt):
    """The boot addon (docs/kc-space.md, docs/drivers/sensors.md section 5b): present exactly when the build recorded one (firmware
    entries or the sensor pack), then the ADT has the GpuAddon slot and every entry is a pin of the payload's own collection with that
    pin's size and SHA-256, or the recorded sensor pack; absent: no slot. No private hash shown."""
    spec = importlib.util.spec_from_file_location('piano_firmware', Path(__file__).resolve().parents[2] / 'tooling/persist/firmware.py')
    fw = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fw)
    record = obj(info.get('firmware') or {}, 'firmware')
    sensor = obj(info['sensor_pack'], 'sensor_pack') if info.get('sensor_pack') is not None else None
    name = 'EFI/UniXNU/Payloads/kernel.macho.gpuaddon'
    if not record.get('addon') and not sensor:
        check(name not in files, 'no boot addon (the build recorded none)')
        check(not fw.adt_slot(adt), 'the ADT has no /chosen/memory-map GpuAddon slot (DarwinBoot refuses a slot without an addon)')
        return
    if record.get('addon_sha256'):
        check(files.get(name, (0, None))[1] == record['addon_sha256'], f'{name} is the addon the build recorded')
    check(name in files, f'{name} present')
    check(fw.adt_slot(adt), 'the ADT has the /chosen/memory-map GpuAddon slot DarwinBoot fills with the addon')
    if name not in files:
        return
    pins = {f'{fw.ESP_DIR}/{p["path"]}': p for p in fw.pins_in(fat_file(ramdisk, 'EFI/UniXNU/Payloads/kernel.macho'))}
    try:
        entries = fw.parse_addon(fat_file(ramdisk, name))
    except ValueError as problem:
        check(False, f'the addon parses ({problem})')
        return
    for path, mode, data in entries:
        digest = hashlib.sha256(data).hexdigest()
        if path == 'sensors/piano-sensors.pack':   # PianoSensors' SensorsAddon.h SA_PACK_PATH: this unit's pack (private)
            check(bool(sensor) and digest == sensor.get('sha256'), f'addon {path} ({len(data)} bytes) is the sensor pack the build recorded')
            continue
        pin = pins.get(path)
        ok = pin is not None and len(data) == pin['size'] and digest == pin['sha256']
        shown = 'private' if pin and pin['private'] else digest[:16]
        check(ok, f'addon {path} ({len(data)} bytes, {shown}) matches its pin in the payload\'s collection')
    missing = sorted(set(pins) - {path for path, _, _ in entries})
    if missing:
        print('  info  pinned but not in the addon (the kexts try the ESP): ' + ', '.join(missing))


def obj(value, name):
    if not isinstance(value, dict):
        raise ValueError(f'{name} must be an object')
    return value


def required_hash(record, key):
    digest = record.get(key)
    if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest):
        raise ValueError(f'missing or invalid {key} in build record')
    return digest


def filename(name):
    if not isinstance(name, str) or not name or name in ('.', '..') or Path(name).name != name:
        raise ValueError('expected a filename in the build record')
    return name


def store_arguments(args):
    patterns = {'data-persist': r'[012]', 'data-store-replace': r'1|0x[0-9a-f]{8}', 'data-store-flatten': r'1'}
    settings = {}
    for word in args.split():
        key, _, value = word.partition('=')
        if key in patterns:
            if key in settings or not re.fullmatch(patterns[key], value):
                raise ValueError(f'invalid or repeated {key} boot argument')
            settings[key] = value
    return settings


def check_store(settings, store, warm, has_guard):
    mode = int(settings.get('data-persist', '0'))
    if store is not None:
        declared = store.get('data_persist')
        check(type(declared) is int and mode == declared, f'boot data-persist={mode} matches the build record')
    replace, flatten = settings.get('data-store-replace'), settings.get('data-store-flatten')
    if replace or flatten:
        recorded = store or {}
        check(mode == 2 and has_guard and (replace or '').removeprefix('0x') == (recorded.get('replace') or '') and
              bool(flatten) == recorded.get('flatten', False), 'store replacement or flattening requires the recorded guarded mode')
    if warm:
        check(mode == 0, f'a warm test seed runs data-persist=0 (this entry: {mode})')
    if mode == 2:
        seed = store.get('seed_identity') if store is not None else None
        installed = store.get('installed_seed') if store is not None else None
        same_seed = (isinstance(seed, str) and isinstance(installed, str) and
                     re.fullmatch(r'[0-9a-fA-F]{8,64}', installed) and seed.lower().startswith(installed.lower()))
        check(bool(has_guard or same_seed),
              'a persistent Data boot either uses the installed seed or has the store guard')
        if has_guard and not same_seed:
            print('  WARN  this seed differs from the recorded installed seed. A flattened store can still be resumed and written.')


def check_entries(config, macos, adt, ramdisk, info, kc_name):
    spec = importlib.util.spec_from_file_location('persist_common', ROOT / 'tooling/persist/common.py')
    common = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(common)
    entries = common.toml_loads(config).get('entry', [])
    if not isinstance(entries, list) or any(not isinstance(entry, dict) for entry in entries):
        raise ValueError('expected boot menu entry tables')
    xnu = [entry for entry in entries if entry.get('type') == 'xnu']
    check(bool(xnu) == macos, 'XNU boot entries match the presence of a macOS payload')
    if not xnu or adt is None:
        return
    spec = importlib.util.spec_from_file_location('piano_boot_args', ROOT / 'tooling/adt/boot_args.py')
    ba = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ba)
    store = obj(info['data_store'], 'data_store') if 'data_store' in info else None
    registry = obj(json.loads(WARM_SEEDS.read_text()), 'warm seed registry') if WARM_SEEDS.is_file() else {}
    warm = info['seed_sha256'] in registry
    has_guard = None
    for entry in xnu:
        args = entry.get('args')
        if not isinstance(args, str):
            raise ValueError('expected string args in each XNU boot entry')
        if '\0' in args:
            raise ValueError('XNU boot arguments contain an embedded NUL')
        if len(args.encode()) > ba.LEGACY_MAX:
            raise ValueError(f'XNU boot argument line exceeds the {ba.LEGACY_MAX}-byte DarwinBoot limit')
        if not args.isascii() or any(ord(c) < 32 or ord(c) == 127 for c in args):
            raise ValueError('expected printable ASCII args in each XNU boot entry')
        merged, status = ba.merged(args, adt)
        print(f'    boot-args ({status}): {merged}')
        check(status in ('appended', 'none', 'off'), 'ADT boot arguments resolve')
        settings = store_arguments(merged)
        if settings.get('data-persist') == '2' and has_guard is None:
            has_guard = b'data-store-replace' in fat_file(ramdisk, kc_name)
        check_store(settings, store, warm, has_guard)
        if 'native-ufs-root-part=linux_data' in merged.split():
            print('  warn  native-ufs-root-part=linux_data: this image would read its root windows over the Data store '
                  '(docs/k273-line.md §4)')


def verify(a):
    build = obj(json.loads(a.build_json.read_text()), 'build record')
    records = obj(build.get('images'), 'images')
    if not records:
        raise ValueError('the build record has no images')
    fd_record = obj(build.get('fd'), 'fd')
    fd_name = filename(fd_record.get('file'))
    fd_hash = required_hash(fd_record, 'sha256')
    root = a.build_json.parent
    fd = (a.fd or ROOT / 'out/fd' / fd_name).read_bytes()
    dtb_bytes = (a.dtb or ROOT / 'out/inputs/piano-boot.dtb').read_bytes()
    ref_data = a.reference.read_bytes()
    ref = header(ref_data)
    print(f'reference container: {a.reference.name} sha256 {hashlib.sha256(ref_data).hexdigest()}')
    digest = hashlib.sha256(fd).hexdigest()
    print(f'FD {fd_name} sha256 {digest} (build record {fd_hash})')
    check(digest == fd_hash, 'FD file is the one the build used')
    dtb_hash = hashlib.sha256(dtb_bytes).hexdigest()
    print(f'boot DTB sha256 {dtb_hash}')
    if 'dtb_sha256' in build:
        check(dtb_hash == required_hash(build, 'dtb_sha256'), 'boot DTB is the one the build used')
    images = a.images or [root / filename(name) for name in records]
    for path in images:
        with path.open('rb') as stream:
            size = os.fstat(stream.fileno()).st_size
            if size > IMAGE_MAX_BYTES:
                check(False, f'{path.name}: image size {size} exceeds {IMAGE_MAX_BYTES} bytes (PIANO_IMAGE_MAX_BYTES)')
                continue
            data = stream.read(size + 1)
        if len(data) != size:
            check(False, f'{path.name}: image size changed while reading')
            continue
        name = path.name.removesuffix('-signed.img') + '.img' if path.name.endswith('-signed.img') else path.name
        info = obj(records.get(name), f'image record for {name}')
        payload_hash = required_hash(info, 'payload_sha256')
        verify_image(a, path, data, info, ref, fd, dtb_bytes, payload_hash)


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('build_json', type=Path)
    p.add_argument('images', nargs='*', type=Path)
    p.add_argument('--reference', type=Path, default=Path(__file__).resolve().parents[2] / 'out/inputs/ubuntu-boot-ram-maintenance.img')
    p.add_argument('--fd', type=Path)
    p.add_argument('--dtb', type=Path)
    p.add_argument('--avbtool', type=Path)
    p.add_argument('--pubkey', type=Path)
    a = p.parse_intermixed_args(argv)
    failures.clear()
    try:
        verify(a)
    except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as exc:
        check(False, str(exc))
    print(f'\n{"PASS" if not failures else "FAIL"}: {len(failures)} failure(s)')
    return 1 if failures else 0


def verify_image(a, path, data, info, ref, fd, dtb_bytes, payload_hash):
    digest = hashlib.sha256(data).hexdigest()
    print(f'\n== {path.name}  {len(data)} bytes  sha256 {digest}')
    h = header(data)
    kernel, ramdisk, dtb, end = sections(data, h)
    signed = path.name.endswith('-signed.img')
    recorded_size = info.get('bytes', end if signed else len(data))
    if type(recorded_size) is not int or not end <= recorded_size <= len(data) or (
            not signed and recorded_size != len(data)):
        raise ValueError('invalid image size in build record')
    if 'sha256' in info:
        expected = required_hash(info, 'sha256')
        if recorded_size < len(data):
            digest = hashlib.sha256(memoryview(data)[:recorded_size]).hexdigest()
        check(digest == expected, 'image bytes match the build record')
    for field in SAME_AS_REFERENCE:
        check(h[field] == ref[field], f'header {field} = {h[field]!r:.60} (Ubuntu RAM set: {ref[field]!r:.60})')
    print(f'  info  kernel_size {h["kernel_size"]}  ramdisk_size {h["ramdisk_size"]}  dtb_size {h["dtb_size"]}  '
          f'cmdline {h["cmdline"].rstrip(bytes(1))!r}')
    check(h['id'][:20] == mkbootimg_id(kernel, ramdisk, dtb), 'header id is the SHA-1 of the sections (mkbootimg)')
    if len(kernel) < 64:
        raise ValueError('truncated ARM64 image header')
    check(kernel[56:60] == b'ARM\x64', 'kernel field starts with an ARM64 image header')
    text_offset, image_size, flags = struct.unpack_from('<QQQ', kernel, 8)
    shim = info.get('shim_bytes', len(kernel) - len(fd) - len(dtb_bytes))
    if type(shim) is not int or shim < 64 or shim + len(fd) + len(dtb_bytes) != len(kernel):
        raise ValueError('invalid BootShim, FD or DTB size')
    check(text_offset == 0 and flags == 0xa, f'ARM64 header as a Linux Image (text offset {text_offset:#x}, flags {flags:#x}; Ubuntu kernel: 0, 0xa)')
    check(image_size >= shim + len(fd), f'ARM64 header image size {image_size:#x} covers shim {shim:#x} + FD {len(fd):#x}')
    check(kernel[shim:shim + len(fd)] == fd, 'kernel field holds the exact FD after the shim')
    check(kernel[shim + len(fd):] == dtb_bytes, 'the boot DTB is appended after the FD')
    check(dtb == dtb_bytes, 'v2 dtb section is the boot DTB')
    check(hashlib.sha256(ramdisk).hexdigest() == payload_hash, 'ramdisk is the payload the build recorded')
    ok, files = fat_files(ramdisk)
    check(ok and files, f'ramdisk is a FAT volume with {len(files)} files')
    for name, (size, digest, _) in files.items():
        print(f'        {name}  {size}  {digest}')
    kc_name, adt_name = list(PAYLOADS)[:2]
    macos = (path.name.startswith('boot-piano-macos-ram-') or
             any(name in files or key in info for name, key in PAYLOADS.items()))
    raw_adt = None
    if macos:
        for name, key in PAYLOADS.items():
            digest = required_hash(info, key)
            check(files.get(name, (0, None))[1] == digest, f'{name} is the input the build recorded')
        if all(name in files for name in (kc_name, adt_name)):
            raw_adt = fat_file(ramdisk, adt_name)
            check_firmware(ramdisk, files, info, raw_adt)
            spec = importlib.util.spec_from_file_location('adt_extra_ram', ROOT / 'tooling/adt/adt_extra_ram.py')
            extra_ram = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(extra_ram)
            placeholder = extra_ram.extra_ram_property(raw_adt)
            if placeholder is not None:
                check(placeholder == bytes(extra_ram.BYTES),
                      f'/chosen {extra_ram.NAME} is a {extra_ram.BYTES}-byte zeroed placeholder')
            elif info.get('extra_ram'):
                check(False, 'extra-RAM placeholder is missing')
            check(ROOT_27_0_1 not in raw_adt.upper(), 'the ADT does not name the retired 27.0.1 root (configs/k27-adt.toml)')
            check(ROOT_27_3 in raw_adt.upper(), 'the ADT names the 27.3 root (configs/k273-adt.toml root_match_volume_uuid)')
    config = files.get('EFI/UniXNU/unixnu.toml', (0, None, None))[2]
    check(bool(config), 'EFI/UniXNU/unixnu.toml present and nonempty')
    if config:
        print('  unixnu.toml:\n' + '\n'.join('    | ' + line for line in config.splitlines()))
        check_entries(config, macos, raw_adt, ramdisk, info, kc_name)
    rest = data[end:]
    if rest.strip(b'\0'):
        if a.avbtool and a.pubkey:
            infotext = subprocess.run(['python3', str(a.avbtool), 'info_image', '--image', str(path)], capture_output=True, text=True)
            print('\n'.join('    ' + line for line in infotext.stdout.splitlines() if line.strip()))
            # verify_image finds the partition's image as <partition_name>.img next to the one it checks.
            with tempfile.TemporaryDirectory() as tmp:
                (Path(tmp) / 'boot.img').symlink_to(path.resolve())
                v = subprocess.run(['python3', str(a.avbtool), 'verify_image', '--image', str(Path(tmp) / 'boot.img'),
                                    '--key', str(a.pubkey)], capture_output=True, text=True)
            print('    ' + (v.stdout + v.stderr).strip().replace('\n', '\n    '))
            check(v.returncode == 0, 'avbtool verify_image with the piano public key')
        else:
            print('  info  data after the last section (AVB footer?): pass --avbtool and --pubkey to verify it')
    else:
        print('  info  no AVB footer (unsigned, as the Ubuntu RAM set)')


if __name__ == '__main__':
    sys.exit(main())
