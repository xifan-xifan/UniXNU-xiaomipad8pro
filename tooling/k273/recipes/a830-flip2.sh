#!/bin/bash
# 2026-11-27 (late night): the A830 desktop with the panel's own pixels readable and a shell on the USB pipe.
#
# Built on a830-flip.sh (read its header and docs/results-2026-11-27-dpu-serves-flips-refused-format.md first). What that boot proved:
#   NativeGopDisplay: flip mode: '&BGA' plane-0 flips enabled (native-gop-display-flip-lossless=1)
#   PianoDpu: flip 1: reflush 1/1: ctl0 flush 00000003 latched in 6873 us (1 vsync interrupts, 1 waits), frames 1979 -> 1979, underrun none: pass
#   PianoDpu: flips: 132 in the last 60 s (140 in all ...); refused: format 0 planes 0 size 0 stride 0 align 0 memory 0 slots 0
# so WindowServer's surfaces are being scanned out by the DPU, and the CPU sampler can no longer say anything about it (0 copies, and its
# "last frame" line still quotes the pre-flip sample). Two things are missing to read the truth instead of guessing at it:
#
#   1. the picture. native-gop-display-shot=1 registers machdep.native_gop_display_shot, "a still capture of the internal panel's scanout
#      backing" (native-gop-display.md 9), and the gop-shot CLI turns it into a PNG with source flip - the bytes the DPU is scanning,
#      which is what the panel shows. It needs root, so
#   2. a way in: piano-usb-shell=1 plus the pianousb-shell helper, which is why the boot goes through tooling/userland/stage.py instead of
#      stage-soft-metal.py: it installs the units with the modes and owners launchd wants and adds their CDHashes to the trust cache.
#      gop-shot becomes a unit of the same manifest (off by default, --on gop-shot here).
#      Reply on the Mac side: tooling/native/piano-shell.ps1.
#
# PIANO_DATA_STORE_REPLACE=1: this seed is a new identity, and NativeRamBlock's store guard keeps a store made for another seed untouched
# and runs Data volatile (docs/data-store.md; every boot today was volatile for exactly that reason). Replacing the store is what binds it
# to this seed, which is the precondition for the "write a file, reboot, read it back" acceptance. The tablet's HyperOS carries no owner
# data (factory reset before this work; the owner authorised flashing freely), and only linux_data's macOS store region is replaced.
set -euo pipefail
cd /Users/zhangqi/Codex/upstream-tip
export PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:/Users/zhangqi/bin:/opt/homebrew/opt/llvm/bin
export PIANO_K27_TREE=/Users/zhangqi/Codex/k27-tree
export TURNIP_TREE=/Users/zhangqi/Codex/turnip-native
export TURNIP_K27_TREE=/Users/zhangqi/Codex/k27-tree
export UNIXNU_RESEARCH=/Users/zhangqi/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research
export PIANO_UNIT_PROFILE=/Users/zhangqi/Codex/upstream-tip/out/unit-profile/piano-7ccdd855.json
export PIANO_GPU_FW=/Users/zhangqi/Codex/piano-gpu-fw
export PIANO_UFS_VERIFIED=1
python3 tooling/k27/make-root-policy-source.py out/root-policies-shim --out out/root-policy-src-shim --name OS --name SystemOS
export PIANO_ROOT_POLICY_SOURCE=/Users/zhangqi/Codex/upstream-tip/out/root-policy-src-shim/root-policies.cpp
export K273_INPUTS=/Users/zhangqi/Codex/upstream-tip/out/inputs/k273-shim
export K273_ROOT=k273
export K273_ROOT_PART=linux_rootfs
export K273_KERNEL=/Users/zhangqi/Codex/upstream-tip/out/k273/k27-tree/out/k27/kernel-k273-0075-s0001-native.macho
export PIANO_RESET_AFTER=420
export PIANO_DATA_PERSIST=2
export PIANO_DATA_STORE_REPLACE=1
export PIANO_EXTRA_ARGS="${PIANO_EXTRA_ARGS:-} piano-usb=2 piano-usb-shell=1 native-gop-display-shot=1 piano-dpu=2 piano-dpu-flip=4 native-gop-display-flip-lossless=1 panic_on_crash_disabled=1 data-store-part=linux_data data-store-lba=117735999"
export PIANO_FAST_BOOT=1
export PIANO_DENY_EXEC=/usr/libexec/watchdogd,/usr/libexec/PerfPowerServices
export PIANO_GPU=2
export PIANO_A830_METAL=1
export PIANO_A830_HOST_TESTS=0
export PIANO_A830_TU_DEBUG=sysmem
# The A830 stage first (build-native-image.sh would run it anyway), then userland on top of that seed, so the plugin and the units
# are in one seed with one trust cache.
tooling/metal/build-turnip-a830.sh > out/k27/build-turnip-a830.log 2>&1
python3 tooling/userland/stage.py --seed out/turnip-a830/stage/scratch-data.img \
  --trust out/turnip-a830/stage/kernel.macho.trustcache --out out/a830-flip2-stage --on pianousb-shell --on gop-shot
export PIANO_SEED=/Users/zhangqi/Codex/upstream-tip/out/a830-flip2-stage/scratch-data.img
export PIANO_TRUST=/Users/zhangqi/Codex/upstream-tip/out/a830-flip2-stage/kernel.macho.trustcache
bash tooling/k273/build-k273-image.sh
rm -rf out/ram-verified-8748-a830-flip2 out/ram-dump-verified-8748-a830-flip2
cp -R out/ram out/ram-verified-8748-a830-flip2
cp -R out/ram-dump out/ram-dump-verified-8748-a830-flip2
shasum -a 256 out/ram-verified-8748-a830-flip2/boot-piano-macos-ram-A-bundled.img out/ram-verified-8748-a830-flip2/boot-piano-uefi-ram-A-bundled.img
echo A830-FLIP2-8748-BUILD-DONE