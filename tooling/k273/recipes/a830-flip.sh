#!/bin/bash
# 2026-11-27 (night): the A830 desktop with PianoDpu actually flipping.
#
# Same verified 27.3 assembly as out/build-a830-8748.sh (kernel d921655f..., the shimmed root that reaches Early boot complete,
# 420 s self-reset, PianoA830 + the NativeAdrenoMetal.bundle built here). Two boots and their logs decided what is added here:
#
#   logs/native-runs/ram-k273-a830b-20261127-195858 (65f41bea...): WindowServer, loginwindow, MenuBarAgent, NotificationCenter and
#     iconservicesagent each hold `device Adreno (TM) 830`, `submitted=23 completed=23`, Dock/ControlCenter/SystemUIServer exec. No
#     piano-dpu= on the line, so PianoDpu was inert and NativeGopDisplay copied; its 11 samples read 0.0% non-black.
#   logs/native-runs/ram-k273-a830dpu-20261127-202143 (df6bea7c...): piano-dpu=2 piano-dpu-flip=4, so PianoDpu serves (MDSS 12.0.0,
#     VIG0 'BGRA' 1600x2136 splash) and NativeGopDisplay enters flip mode - and every flip is refused:
#     `flip refused (format): 3200x2136 row 3200 format 0x26424741; copied`, 287 refusals, `0 swaps flipped ... 959 copied`.
#     0x26424741 is '&BGA', 27.3's tag for WindowServer's framebuffers (docs/k273-line.md:134 records the same tag on the
#     maintainer's first 27.3 boot). DpuFlip.h checkSurface takes only plain 'BGRA' and says why: an '&BGA' frame is expected to
#     arrive "already normalized by its producer (NativeGopDisplay, native-gop-display-flip-lossless=1)". That switch defaults to 0
#     and was never passed, so flip mode could not engage on 27.3 at all.
#
# So the only change is native-gop-display-flip-lossless=1 (docs/drivers/native-gop-display.md, "RAM-boot read list" item 1).
# CA_DISABLE_FRAMEBUFFER_COMPRESSION=1 is already in the WindowServer exec-env, which is what makes plane 0 lossless BGRA.
# Output goes to its own directory so 65f41bea (GPU, no DPU) and df6bea7c (DPU, no lossless) stay bootable next to it.
set -euo pipefail
cd /Users/zhangqi/Codex/upstream-tip
export PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:/Users/zhangqi/bin:/opt/homebrew/opt/llvm/bin
export PIANO_K27_TREE=/Users/zhangqi/Codex/k27-tree
export TURNIP_TREE=/Users/zhangqi/Codex/turnip-native
export TURNIP_K27_TREE=/Users/zhangqi/Codex/k27-tree
export UNIXNU_RESEARCH=/Users/zhangqi/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research
export PIANO_UNIT_PROFILE=/Users/zhangqi/Codex/upstream-tip/out/unit-profile/piano-7ccdd855.json
export PIANO_GPU_FW=/Users/zhangqi/Codex/piano-gpu-fw   # the four pinned gen80000 images, verified against package/PianoA830/board/piano-gpu-firmware.json
export PIANO_UFS_VERIFIED=1
python3 tooling/k27/make-root-policy-source.py out/root-policies-shim --out out/root-policy-src-shim --name OS --name SystemOS
export PIANO_ROOT_POLICY_SOURCE=/Users/zhangqi/Codex/upstream-tip/out/root-policy-src-shim/root-policies.cpp
export K273_INPUTS=/Users/zhangqi/Codex/upstream-tip/out/inputs/k273-shim
export K273_ROOT=k273
export K273_ROOT_PART=linux_rootfs
export K273_KERNEL=/Users/zhangqi/Codex/upstream-tip/out/k273/k27-tree/out/k27/kernel-k273-0075-s0001-native.macho
export PIANO_RESET_AFTER=420
export PIANO_DATA_PERSIST=2
# 882 bytes without the display keys; the builder's budget and the ADT tail are docs/boot-args-length.md.
export PIANO_EXTRA_ARGS="${PIANO_EXTRA_ARGS:-} piano-usb=2 piano-dpu=2 piano-dpu-flip=4 native-gop-display-flip-lossless=1 panic_on_crash_disabled=1 exec-log=1 stderr-log=1 data-store-part=linux_data data-store-lba=117735999"
export PIANO_FAST_BOOT=1
export PIANO_DENY_EXEC=/usr/libexec/watchdogd,/usr/libexec/PerfPowerServices
export PIANO_GPU=2
export PIANO_A830_METAL=1
export PIANO_A830_HOST_TESTS=0
export PIANO_A830_TU_DEBUG=sysmem
bash tooling/k273/build-k273-image.sh
rm -rf out/ram-verified-8748-a830-flip out/ram-dump-verified-8748-a830-flip
cp -R out/ram out/ram-verified-8748-a830-flip
cp -R out/ram-dump out/ram-dump-verified-8748-a830-flip
shasum -a 256 out/ram-verified-8748-a830-flip/boot-piano-macos-ram-A-bundled.img out/ram-verified-8748-a830-flip/boot-piano-uefi-ram-A-bundled.img
echo A830-FLIP-8748-BUILD-DONE