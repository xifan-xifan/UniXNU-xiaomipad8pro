#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Host tests of PianoAuto, the macOS-side consumers of PianoSensors (docs/drivers/sensors.md section 5a), all with -Wall -Wextra
-Werror and ASan/UBSan (compilers from the environment's CC and CXX, default cc and c++):

  c        tests/auto_host.c: TouchRotation.h (PianoTouch's transform), AutoRotate.c (piano-rotated's policy), AutoStream.c (a sensor
           stream's off/on lifecycle) and AutoLight.c (the ambient light chain) over PianoSensors' SensorsPolicy.c, which is compiled
           from its own package, unchanged
  ambient  tests/ambient_host.cpp: AmbientBacklight.h (PianoBacklight's automatic mode) against PianoBacklight's own Ktz8866.h step
           ladder (included read only) and end to end from lux through AutoLight.c
  daemon   daemon/piano-rotated.m compiles for arm64 macOS and runs with synthetic boot args and displays on a Mac with xcrun
           shell boot-argument reader tested with a stub sysctl
  kernel   every part a kernel driver includes compiles freestanding with AArch64 general registers only (no C library, no floating
           point): AutoStream.c, AutoLight.c, TouchRotation.h in C and C++, AmbientBacklight.h in C++ (no exceptions, no RTTI),
           AutoRotate.c (portable, though only piano-rotated uses it)

Synthetic inputs only; nothing here talks to a display, a sensor or a driver."""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PKG = HERE.parent
SENSORS = PKG.parent / 'PianoSensors'
BACKLIGHT = PKG.parent / 'PianoBacklight'
POLICY = [SENSORS / n for n in ('SensorsPolicy.c', 'SensorsOrient.c', 'SensorsFixed.c')]
C_FILES = ('AutoRotate.c', 'AutoStream.c', 'AutoLight.c')
INCLUDE = PKG.parent / 'include'   # PianoDspMemory.h (SensorsPolicy.h's arena split)
INCLUDES = ['-I', str(PKG), '-I', str(SENSORS), '-I', str(SENSORS / 'tests'), '-I', str(INCLUDE)]
SANITIZE = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
WARN = ['-Wall', '-Wextra', '-Werror']
KERNEL = ['-target', 'arm64-apple-macos13', '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', *WARN]


def cc():
    return os.environ.get('CC', 'cc')


def run(exe):
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    text = (result.stdout + result.stderr).strip()
    print(text)
    if result.returncode or not text or not text.splitlines()[-1].startswith('PASS'):
        sys.exit(f'FAIL: {exe.name}')


def c_tests(out, defines=(), extra=()):
    exe = out / 'auto_host'
    subprocess.run([cc(), '-std=c11', *WARN, *SANITIZE, *INCLUDES, *defines, str(HERE / 'auto_host.c'), *(str(PKG / n) for n in C_FILES),
                    *map(str, extra), *map(str, POLICY), '-o', str(exe)], check=True)
    run(exe)


def ambient_tests(out):
    objects = []
    for source in [PKG / 'AutoStream.c', PKG / 'AutoLight.c', *POLICY]:
        objects.append(out / (source.stem + '.o'))
        subprocess.run([cc(), '-std=c11', *WARN, *SANITIZE, *INCLUDES, '-c', str(source), '-o', str(objects[-1])], check=True)
    exe = out / 'ambient_host'
    cxx = os.environ.get('CXX', 'c++')
    subprocess.run([cxx, '-std=c++20', *WARN, *SANITIZE, *INCLUDES, '-I', str(BACKLIGHT), str(HERE / 'ambient_host.cpp'),
                    *map(str, objects), '-o', str(exe)], check=True)
    run(exe)


def daemon_check(out):
    if sys.platform != 'darwin' or subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True).returncode:
        print('SKIP daemon: macOS SDK unavailable')
        return
    command = ['xcrun', 'clang', '-mmacosx-version-min=14.0', '-fobjc-arc', *WARN, *INCLUDES]
    subprocess.run([*command, '-arch', 'arm64', '-fsyntax-only', str(PKG / 'daemon/piano-rotated.m')], check=True)
    exe = out / 'daemon_boot'
    subprocess.run([*command, *SANITIZE, str(HERE / 'daemon_boot.m'), str(PKG / 'AutoRotate.c'), *map(str, POLICY),
                    '-framework', 'Foundation', '-framework', 'CoreGraphics', '-framework', 'IOKit', '-o', str(exe)], check=True)
    run(exe)


def shell_bootargs(out):
    helper = PKG.parents[1] / 'tooling/native/piano-usbnet-address/piano-bootargs.sh'
    sysctl = out / 'sysctl'
    sysctl.write_text('''#!/bin/sh
case "$2" in
  kern.piano_bootargs)
    case "$MERGED_MODE" in
      ok) printf '%s\\n' "$MERGED_ARGS" ;;
      absent) echo "sysctl: unknown oid 'kern.piano_bootargs'" >&2; exit 1 ;;
      error) echo "sysctl: permission denied" >&2; exit 1 ;;
    esac ;;
  kern.bootargs) echo 'piano-rotate=1' ;;
  *) exit 2 ;;
esac
''')
    sysctl.chmod(0o755)
    cmd = ['bash', '-c', '. "$1" && piano_bootarg piano-rotate', 'bootargs', str(helper)]
    prefix = 'padding=1 ' * 300
    for mode, args, code, value in (
        ('ok', prefix + 'piano-rotate=2\tpiano-rotated=3', 0, '2\n'),
        ('ok', prefix + 'piano-rotate=2 piano-rotate=0', 0, '0\n'),
        ('ok', 'piano-auto-rotate=0 other=piano-rotate=2', 0, '\n'),
        ('absent', '', 0, '1\n'),
        ('error', '', 1, ''),
    ):
        env = dict(os.environ, PATH=f'{out}{os.pathsep}{os.environ["PATH"]}', MERGED_MODE=mode, MERGED_ARGS=args)
        result = subprocess.run(cmd, env=env, capture_output=True, text=True, check=False)
        if (result.returncode, result.stdout, result.stderr) != (code, value, ''):
            sys.exit(f'FAIL shell bootargs ({mode}): {result}')
    print('PASS shell bootargs: exact late arguments, last override, and legacy fallback')


def kernel_checks(out, c_files=(), headers=(), cpp_headers=()):
    for source in c_files:
        subprocess.run([cc(), '-std=c11', *KERNEL, *INCLUDES, '-c', str(source), '-o', str(out / 'k.o')], check=True)
    for header in headers:
        for language, std in (('c', 'c11'), ('c++', 'c++17')):
            probe = out / f'probe.{"c" if language == "c" else "cpp"}'
            probe.write_text(f'#include "{header}"\n')
            compiler = cc() if language == 'c' else os.environ.get('CXX', 'c++')
            flags = ['-fno-exceptions', '-fno-rtti'] if language == 'c++' else []
            subprocess.run([compiler, f'-std={std}', *KERNEL, *flags, *INCLUDES, '-c', str(probe), '-o', str(out / 'k.o')], check=True)
    for header in cpp_headers:
        probe = out / 'probe.cpp'
        probe.write_text(f'#include "{header}"\nuint16_t probe(pianoauto::Ambient &a) {{ a.target(500, 1); a.step(1); return a.code(); }}\n')
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', *KERNEL, '-fno-exceptions', '-fno-rtti', *INCLUDES, '-c', str(probe),
                        '-o', str(out / 'k.o')], check=True)
    print(f'PASS kernel constraints: {len(c_files)} C files and {len(headers) + len(cpp_headers)} headers freestanding, '
          'general registers only')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--only', action='append', default=[], choices=['c', 'ambient', 'daemon', 'kernel'])
    args = parser.parse_args()
    want = set(args.only) or {'c', 'ambient', 'daemon', 'kernel'}
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        if 'c' in want:
            c_tests(out)
        if 'ambient' in want:
            ambient_tests(out)
        if 'daemon' in want:
            daemon_check(out)
            shell_bootargs(out)
        if 'kernel' in want:
            kernel_checks(out, c_files=[PKG / n for n in C_FILES], headers=['TouchRotation.h'], cpp_headers=['AmbientBacklight.h'])
    print('PASS PianoAuto host tests')


if __name__ == '__main__':
    main()
