#!/bin/bash
# 2026-11-26: the verified-root 27.3 assembly of out/build-verified-420.sh, with the owner Data store opened on this unit.
#  1. PIANO_DATA_PERSIST=2 so NativeUfs opens the store window and NativeRamBlock binds a fresh one (state 1/3): the store is what
#     makes the owner Data volume exist, and without it kern_exec.c cannot find it (offline scratch Data mount failed: 2).
#  2. PIANO_UNIT_PROFILE is this unit, not the reference one: its linux_data ends at 124289791, so the window for this seed starts
#     at 117735999 (the installed ESP entry says the same). Without the profile the tooling insists on the reference unit's 117998394.
set -euo pipefail
cd /Users/zhangqi/Codex/upstream-tip
export PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:/Users/zhangqi/bin
export PIANO_UNIT_PROFILE=/Users/zhangqi/Codex/upstream-tip/out/unit-profile/piano-7ccdd855.json
export PIANO_UFS_VERIFIED=1
python3 tooling/k27/make-root-policy-source.py out/root-policies-26C5035r --out out/root-policy-src --name OS --name SystemOS
export PIANO_ROOT_POLICY_SOURCE=/Users/zhangqi/Codex/upstream-tip/out/root-policy-src/root-policies.cpp
export K273_INPUTS=/Users/zhangqi/Codex/upstream-tip/out/inputs/k273-stock
export K273_ROOT=k273
export K273_ROOT_PART=linux_rootfs
export PIANO_RESET_AFTER=420
export PIANO_GPU=0
export PIANO_DATA_PERSIST=2
export PIANO_EXTRA_ARGS="${PIANO_EXTRA_ARGS:-} data-store-part=linux_data data-store-lba=117735999"
export PIANO_KEXT_LIST=out/kexts-no-softgpu.txt
export PIANO_A830_METAL=0
export PIANO_A830_HOST_TESTS=0
export PIANO_GPU_NO_FIRMWARE=1
bash tooling/k273/build-k273-image.sh
rm -rf out/ram-verified-store out/ram-dump-verified-store
cp -R out/ram out/ram-verified-store
cp -R out/ram-dump out/ram-dump-verified-store
shasum -a 256 out/ram-verified-store/boot-piano-macos-ram-A-bundled.img out/ram-verified-store/boot-piano-uefi-ram-A-bundled.img
echo VERIFIED-STORE-BUILD-DONE