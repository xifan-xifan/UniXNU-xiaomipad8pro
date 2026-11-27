#!/usr/bin/env python3
"""Fault-test actual patched Turnip allocation functions, without a GPU/tablet.

Only three source files are copied from the existing staged Mesa tree. The new
patch is applied to those copies. Tests extract the actual functions, replacing
only the kernel/VA/retirement peer. This is not WindowServer/capture validation.
"""
import argparse
import json
import re
import shlex
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, timeout=60, **kwargs)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)
    return result.stdout


def function(source, name):
    match = re.search(r'(?m)^(?:static )?(?:VkResult|bool|void)\n' + re.escape(name) + r'\(', source)
    if not match:
        raise SystemExit(f'missing actual function: {name}')
    end = source.index('\n}\n', match.end()) + 3
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesa', type=Path, default=ROOT / 'out/turnip-a830/mesa')
    parser.add_argument('--out', type=Path, default=ROOT / 'out/ws-oom2-capacity-test')
    parser.add_argument('--syntax', action='store_true', help='also compile both full translation units using staged build flags')
    args = parser.parse_args()
    output = args.out.resolve()
    if output == args.mesa.resolve() or args.mesa.resolve() in output.parents:
        raise SystemExit('test output must be separate from staged Mesa')
    source = output / 'src/freedreno/vulkan'
    source.mkdir(parents=True, exist_ok=True)
    for name in ['tu_knl.h', 'tu_knl_darwin.cc', 'tu_device.cc']:
        shutil.copyfile(args.mesa / 'src/freedreno/vulkan' / name, source / name)
    patch = ROOT / 'tooling/metal/turnip-a830/mesa/0012-turnip-darwin-capacity-recovery.patch'
    if 'tu_darwin_capacity_retry' not in (source / 'tu_knl.h').read_text():
        run(['patch', '-s', '-p1', '-d', str(output), '-i', str(patch)])
    backend = (source / 'tu_knl_darwin.cc').read_text()
    device = (source / 'tu_device.cc').read_text()
    generated = 'static uint64_t darwin_retry_failed_ns;\n'
    for name in ['tu_darwin_capacity_retry', 'tu_darwin_capacity_result', 'tu_darwin_capacity_done', 'darwin_map_failure',
                 'darwin_bo_init', 'darwin_bo_init_host']:
        generated += function(backend, name) + '\n'
    generated += function(device, 'tu_add_to_heap')
    generated += function(device, 'tu_allocate_lazy_memory')
    (output / 'capacity_backend.inc').write_text(generated)
    executable = output / 'capacity_recovery_host'
    test = ROOT / 'tooling/metal/turnip-a830/capacity_recovery_host.cpp'
    run(['xcrun', 'clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-Wno-c99-designator', '-I', str(output), str(test), '-o', str(executable)])
    print(run([str(executable)]), end='')
    if args.syntax:
        build = args.mesa.parent / 'build'
        commands = json.loads((build / 'compile_commands.json').read_text())
        for name in ['tu_knl_darwin.cc', 'tu_device.cc']:
            entry = next(e for e in commands if e['file'].endswith('/' + name))
            command = shlex.split(entry['command'])
            for i, arg in enumerate(command):
                if arg == entry['file']:
                    command[i] = str(source / name)
                elif arg == '-o':
                    command[i + 1] = str(output / (name + '.o'))
            for flag in ['-MD', '-MMD']:
                if flag in command:
                    command.remove(flag)
            for flag in ['-MF', '-MQ', '-MT']:
                if flag in command:
                    i = command.index(flag)
                    del command[i:i + 2]
            # Staged command databases can name an unplugged build worktree's
            # ABI directory. Use the same staged ABI here, without restaging it.
            command[1:1] = ['-I' + str((args.mesa.parent / 'abi').resolve()),
                            '-include', str(source / 'tu_knl.h')]
            run(command, cwd=entry['directory'])
            print(f'PASS full translation unit: {name}')


if __name__ == '__main__':
    main()
