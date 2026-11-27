#!/usr/bin/env python3
"""Build UniXNU's native Metal bridge (drivers/NativeMetal/bridge/NativeMetalBridge.rs: the pinned metal2vulkan runtime behind a C ABI)
as a static library for macOS arm64e, on macOS.

This is tooling/metal/build-metal-bridge-macos-host.py: the k27 tree's scripts/build-metal-bridge-macos.py, which is written for the
author's Linux build host, with the host-side substitutions of this machine listed in docs/drivers/gpu-userspace.md. Everything about
the built artefact is unchanged: same source pin, same patch set and order, same arm64e-apple-darwin target spec, same RUSTFLAGS, same
release profile. Only the toolchain and SDK paths differ, because a Mac has none of /usr/lib/llvm-21, /Users/... /toolchains/sdks or
rustup's +nightly.

    K27_TREE=/Users/zhangqi/Codex/k27-tree build-metal-bridge-macos-host.py

Inputs, all read in place (see tooling/metal/UPSTREAM for provenance):
  K27_TREE/sources/UniXNU/drivers/NativeMetal          NativeMetal main (bridge/*.rs, bridge/patches, shaders/scanout_convert.ll)
  K27_TREE/sources/UniXNU/scripts/data/gpu-metal-vulkan-inputs.json   the translator pin (the monorepo record NativeMetal is used with)
  K27_TREE/sources/UniXNU/.cache/gpu-reference/metal2vulkan           the translator source at that pin, a git checkout
  K27_TREE/sources/UniXNU-turnip/patches/metal2vulkan                the board's patches
  K27_TREE/patches/metal2vulkan-macos                                this project's own
  K27_TREE/configs/arm64e-apple-darwin.json                          the tier-3 target spec
Output: K27_TREE/out/metal/bridge/libnative_metal_bridge.a
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(os.environ.get('K27_TREE', '/Users/zhangqi/Codex/k27-tree'))
UNIXNU = ROOT / 'sources/UniXNU'
SOURCE = UNIXNU / '.cache/gpu-reference/metal2vulkan'
OUT = ROOT / 'out/metal/bridge'
# Host substitutions: Homebrew LLVM for llvm-as/clang/llvm-ar, the Xcode SDK, and a rustup-less nightly toolchain with rust-src.
LLVM = Path(os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm/bin'))
SDK = Path(os.environ.get('BRIDGE_SDK', subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()))
CARGO = Path(os.environ.get('CARGO', '/Users/zhangqi/rust-nightly/bin/cargo'))
TRIPLE = 'arm64e-apple-darwin'
# rustc's arm64e-apple-darwin leaves pointer authentication off: function pointers from C (dlsym, vkGetInstanceProcAddr: signed with IA,
# discriminator 0) were called as raw addresses, and C would call Rust's unsigned ones. rustc implements clang's ABI (blraaz, paciza,
# pacibsp/retab, signed static function pointers) for cfg(target_abi = "pauthtest") only, so this is the built-in target with that ABI;
# init-fini off: dyld signs Mach-O initializers itself.
TARGET_SPEC = ROOT / 'configs/arm64e-apple-darwin.json'
# The translator pin as the monorepo records it. The fallback is this repository's copy of that record, for a k27 checkout whose
# sources/UniXNU is the drivers tree rather than the whole monorepo.
PIN_JSON = UNIXNU / 'scripts/data/gpu-metal-vulkan-inputs.json'
PIN_FALLBACK = Path(__file__).resolve().parent / 'metal2vulkan-pin.json'
# NativeMetal's own patches, then the board's, in the order upstream's build applies them. The upstream script drops NativeMetal's
# 0020-written-host-page-textures-in-place-at-any-size.patch, because the Mesa at that recipe's revision had no
# turnip-render-mode-for-attachment-memory.patch. At this line (AdrenoMetal fb063de, the 21 board patches of UniXNU 527b8b7) the series is
# coupled: with 0020 out, the board's 0013-trim-kept-memory.patch no longer applies, so the whole series goes in, and the Mesa patch it
# needs goes into the Turnip work tree too (tooling/metal/build-mesa-work.sh --extra-patch). NMB_SKIP_PATCHES=<names,comma> to override.
BOARD = ROOT / 'sources/UniXNU-turnip'
SKIP = {name for name in os.environ.get('NMB_SKIP_PATCHES', '').split(',') if name}
PATCHES = [patch for patch in sorted((UNIXNU / 'drivers/NativeMetal/bridge/patches').glob('*.patch')) if patch.name not in SKIP] \
    + sorted((BOARD / 'patches/metal2vulkan').glob('*.patch')) \
    + sorted((ROOT / 'patches/metal2vulkan-macos').glob('*.patch'))

for needed in (SOURCE / 'Cargo.toml', TARGET_SPEC, UNIXNU / 'drivers/NativeMetal/bridge/NativeMetalBridge.rs',
               UNIXNU / 'drivers/NativeMetal/shaders/scanout_convert.ll', CARGO, LLVM / 'llvm-as', SDK):
    if not needed.exists():
        raise SystemExit(f'missing input: {needed}')

pin_source = PIN_JSON if PIN_JSON.exists() else PIN_FALLBACK
if not PIN_FALLBACK.exists() and not PIN_JSON.exists():
    raise SystemExit('no translator pin record')
pin = json.loads(pin_source.read_text())['translator']
commit = pin['commit']

# The upstream script checks the checkout by commit, which needs the object. Where the translator source was staged from GitHub's
# source archive at that commit instead - git over this network does not complete - the recorded per-file hashes are the check, and
# only with NMB_ALLOW_HASH_PIN=1, so the substitution is a deliberate, logged step rather than a silent one.
head = subprocess.run(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True, capture_output=True).stdout.strip()
if head == commit:
    pass
elif os.environ.get('NMB_ALLOW_HASH_PIN') == '1':
    for name, want in pin['files'].items():
        got = hashlib.sha256((SOURCE / name).read_bytes()).hexdigest()
        if got != want:
            raise SystemExit(f'{SOURCE / name}: {got} is not the pinned {want}')
    print(f'{SOURCE}: translator {commit[:12]} verified by the recorded file hashes (archive checkout, HEAD {head[:12]})')
else:
    raise SystemExit(f'{SOURCE} is at {head}, not the pinned translator commit {commit}; stage the archive at the pin and set NMB_ALLOW_HASH_PIN=1')


def tree(*steps):
    """The tree SOURCE's HEAD makes after the git commands STEPS, in a scratch index."""
    index = OUT / 'patch-index'
    env = dict(os.environ, GIT_INDEX_FILE=str(index))
    try:
        for step in [['read-tree', 'HEAD'], *steps]:
            subprocess.run(['git', '-C', str(SOURCE), *step], env=env, check=True, stdout=subprocess.DEVNULL)
        return subprocess.check_output(['git', '-C', str(SOURCE), 'write-tree'], env=env, text=True).strip()
    finally:
        index.unlink(missing_ok=True)


# The source is the pin with every patch, or it is reset and patched again: a later patch may change what an earlier one wrote
# (NativeMetal's 0010 to 0012), so whether each patch is in cannot be told one at a time.
OUT.mkdir(parents=True, exist_ok=True)
if tree(['add', '-A']) != tree(*[['apply', '--cached', str(patch)] for patch in PATCHES]):
    subprocess.run(['git', '-C', str(SOURCE), 'checkout', '--', '.'], check=True)
    subprocess.run(['git', '-C', str(SOURCE), 'clean', '-fdq'], check=True)
    for patch in PATCHES:
        subprocess.run(['git', '-C', str(SOURCE), 'apply', str(patch)], check=True)
# The translator source the runtime is built from (its pin and the patches applied), which keys the shader cache: NativeMetal's
# shader priming keeps what it finished while this stays the same (nmb_runtime_build, PrimeShaders.h).
runtime_id = hashlib.sha256((commit + '\n' + ''.join(patch.read_text() for patch in PATCHES)).encode()).hexdigest()[:32]
# the runtime's example library and function-constant fixture, assembled to AIR for the bridge's self-tests
air, fc_air = OUT / 'runtime_examples.air', OUT / 'function_constant.air'
fixtures = SOURCE / 'crates/metal2vulkan-validation/fixtures/public'
subprocess.run([str(LLVM / 'llvm-as'), '-o', str(air), str(fixtures / 'examples/runtime_examples.ll')], check=True)
subprocess.run([str(LLVM / 'llvm-as'), '-o', str(fc_air), str(fixtures / 'kernel_function_constant_gate_fold.ll')], check=True)
# the GPU scanout's half-float conversion kernel (NativeMetal shaders/scanout_convert.ll, GpuScanoutPresenter.h)
scanout_air = OUT / 'scanout_convert.air'
subprocess.run([str(LLVM / 'llvm-as'), '-o', str(scanout_air), str(UNIXNU / 'drivers/NativeMetal/shaders/scanout_convert.ll')], check=True)
manifest = OUT / 'Cargo.toml'
manifest.write_text('\n'.join([
    '[package]', 'name = "native-metal-bridge"', 'version = "0.1.0"', 'edition = "2024"', '[workspace]',
    '[lib]', 'crate-type = ["staticlib"]', 'path = ' + json.dumps(str(UNIXNU / 'drivers/NativeMetal/bridge/NativeMetalBridge.rs')),
    '[dependencies]',
    'metal2vulkan = { path = ' + json.dumps(str(SOURCE / 'crates/metal2vulkan')) + ', features = ["ash"] }',
    'metal2vulkan-runtime = { path = ' + json.dumps(str(SOURCE / 'crates/metal2vulkan-runtime')) + ' }',
    'libc = "0.2"',
    # WindowServer's update thread spends much of a frame in the runtime's small functions across the translator's crates: one
    # optimisation unit with link-time optimisation inlines them across crate boundaries (as build-adreno-metal-bridge.py)
    '[profile.release]', 'panic = "unwind"', 'debug = 1', 'lto = "fat"', 'codegen-units = 1', '',
]))
key = TRIPLE.replace('-', '_')
env = dict(os.environ, NMB_RUNTIME_ID=runtime_id, RUSTFLAGS='-Zpointer-authentication=-init-fini',
           NMB_FIXTURE_AIR=str(air), NMB_FC_AIR=str(fc_air), NMB_SCANOUT_AIR=str(scanout_air),
           MACOSX_DEPLOYMENT_TARGET='14.0', SDKROOT=str(SDK),
           **{f'CC_{key}': str(LLVM / 'clang'), f'CXX_{key}': str(LLVM / 'clang++'), f'AR_{key}': str(LLVM / 'llvm-ar'),
              f'CFLAGS_{key}': f'-target arm64e-apple-macos14.0 -isysroot {SDK} -O2',
              f'CXXFLAGS_{key}': f'-isysroot {SDK} -target arm64e-apple-macos14.0'},
           # dependencies' C goes through cc-rs, which would derive a Linux triple from the target spec's target_abi (pauthtest): its
           # defaults off, the triple and SDK are CFLAGS' above
           CRATE_CC_NO_DEFAULTS='1')
target_dir = OUT / 'target'
log = OUT / 'build.log'
with log.open('w') as stream:
    result = subprocess.run([str(CARGO), 'build', '--release', '-Zbuild-std=std,panic_unwind', '-Zjson-target-spec',
                             '--manifest-path', str(manifest), '--target', str(TARGET_SPEC), '--target-dir', str(target_dir)],
                            env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=5400)
if result.returncode:
    raise SystemExit(log.read_text()[-6000:])
library = OUT / 'libnative_metal_bridge.a'
shutil.copyfile(target_dir / TRIPLE / 'release/libnative_metal_bridge.a', library)
print(f'{library}: {library.stat().st_size} bytes (translator {commit[:12]})')
