#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""A material directory (scratch-data.img, kernel.macho.trustcache, userland.json) that is an existing one plus the GPU addon:
the seed's own /Library/GPUBundles/<name>.bundle cloned to /private/var/ngpu/addons/<name>.bundle (stage.py's seed_gpu_addon), the
source of the k27 kernel's boot-time view over /System/Library/Extensions (boot-arg sheng-gpu-addon=1).

    seed-gpu-addon.py MATERIAL_DIR OUT_DIR [--mode copy|move] [--name NativeAdrenoMetal]

Why a separate step (gpu/safari1, private/host-checks/20261124-browser-gpu/ANALYSIS.md section 10.8): the daily image builder replaces
the seed with its pinned material (PIANO_V131_MATERIAL) after PAD_USERLAND's staging, so a PIANO_GPU_ADDON staging never reached the
safari1a image. This adds the addon to the material itself; build with PIANO_V131_MATERIAL=OUT_DIR.

Only the addon changes: no userland restaging, the trust cache copied byte for byte (the clone's CDHashes must already be in it, else
refused), userland.json the material's plus `seed_gpu_addon` and `addon_of` (the material's seed and trust sha256). MATERIAL_DIR is
only read. OUT_DIR must not exist.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('stage', HERE / 'stage.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


def sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('material', type=Path)
    p.add_argument('out', type=Path)
    p.add_argument('--mode', choices=('copy', 'move'), default='copy')
    p.add_argument('--name', default='NativeAdrenoMetal')
    a = p.parse_args()
    material, out = a.material.resolve(), a.out.resolve()
    seed, trust, record = material / 'scratch-data.img', material / 'kernel.macho.trustcache', material / 'userland.json'
    for path in (seed, trust):
        if not path.is_file():
            raise SystemExit(f'{path}: no such file (a material directory holds scratch-data.img and kernel.macho.trustcache)')
    if out.exists():
        raise SystemExit(f'{out} exists')
    if not stage.GPU_ADDON_NAME.fullmatch(a.name):
        raise SystemExit(f'--name {a.name!r}: letters, digits, _ and - only')
    seed_sha, trust_sha = sha(seed), sha(trust)
    out.mkdir(parents=True)
    try:
        shutil.copyfile(trust, out / 'kernel.macho.trustcache')
        packed = {}
        with stage.mounted_seed(seed, out, packed) as (mount, volume):
            addon = stage.seed_gpu_addon(mount, a.mode, a.name, out / 'kernel.macho.trustcache', [])
            listing = stage.sh(['ls', '-lnR', str(mount / stage.GPU_ADDONS.lstrip('/'))], timeout=60, capture_output=True,
                               text=True).stdout.replace(str(mount), '')
        if sha(seed) != seed_sha or sha(trust) != trust_sha:
            raise SystemExit('the material changed during the run')
        doc = json.loads(record.read_text()) if record.is_file() else {}
        doc['seed_gpu_addon'] = addon
        doc['addon_of'] = {'material': str(material), 'seed_sha256': seed_sha, 'trust_sha256': trust_sha,
                           'seed_sha256_out': sha(out / 'scratch-data.img')}
        (out / 'userland.json').write_text(json.dumps(doc, indent=1) + '\n')
    except BaseException:
        shutil.rmtree(out, ignore_errors=True)
        raise
    print(listing.strip())
    print(f'{out / "scratch-data.img"}: sha256 {doc["addon_of"]["seed_sha256_out"]} (material {seed_sha[:16]}); '
          f'addon {addon["mode"]} {addon["dest"]}, {len(addon["files"])} files; trust cache unchanged ({trust_sha[:16]})')
    print(f'build with PIANO_V131_MATERIAL={out} and boot-arg {addon["boot_arg"]}; preflight: the image\'s build-A-bundled.json '
          f'seed_sha256 must be {doc["addon_of"]["seed_sha256_out"]}')


if __name__ == '__main__':
    sys.exit(main())
