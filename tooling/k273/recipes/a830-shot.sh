#!/bin/bash
# 2026-11-27 (night): the A830 desktop, with a root shell on the cable and the panel readable through it.
#
# Built on a830-flip.sh (read its header and docs/results-2026-11-27-dpu-flips-4771-latches.md first). That boot proved the DPU
# scans WindowServer's surfaces (flips: 4771 in all, refused: format 0, every frame underrun none: pass) but left two questions
# that the console cannot answer, because in flip mode NativeGopDisplay copies nothing and its non-black line quotes an old frame:
# what pixels the panel actually carries, and whether Data survives a reboot. Both need code running on the tablet.
#
# The obstacle is that this seed has no boot job: out/turnip-a830/stage/scratch-data.img comes from the 27.3 verified-root seed,
# whose /Library/LaunchDaemons holds only amsdstat.plist and com.apple.postfixsetup.plist (checked on the host 2026-11-27 by
# mounting the unpacked seed: no org.droidvm.run.0.plist). So install.sh, which is what the userland pipeline normally asks the
# boot job to run, has never run here, and stage.py refused to build a seed at all.
# stage.py --seed-install-root is the answer: the units files go to their dest paths inside the seed, and they and every directory
# of their path are made root:wheel with sudo (a noowners mount stores this Mac's uid, and launchd refuses a job file that is not
# root's). root:wheel there is what a real Data volume has anyway.
#
# What this boot is for, in order:
#   1. pianousb-shell as a LaunchDaemon: piano-usb-shell=1 makes the kernel create /dev/pianousb, the helper puts /bin/zsh on it,
#      and tooling/native/piano-shell.ps1 on Windows then runs commands. First real shell on this tablet.
#   2. gop-shot (native-gop-display-shot=1 registers machdep.native_gop_display_shot): a PNG of the bytes the DPU is scanning,
#      which is what the panel shows. Run it from that shell.
#   3. Data: PIANO_DATA_STORE_REPLACE=1 rebuilds linux_data's macOS store and binds it to this seed, so NativeRamBlock opens it
#      instead of running volatile for a seed mismatch (every boot so far: Data store state 7 (kept) ... Data is volatile).
#      Write a file, sync, let the image reset itself, boot the SAME image again with SHOT_REPLACE=0: state 2 (resumed) and the
#      file still there is the read-back the goal asks for. data-store-replace starts over on every boot it is in, so it is in the
#      first boot only.
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
# Long enough to drop the console capture and work from the shell before the image resets itself.
export PIANO_RESET_AFTER=${PIANO_RESET_AFTER:-900}
export PIANO_DATA_PERSIST=2
# SHOT_REPLACE=1 (the default) rebuilds linux_data's store for this seed. SHOT_REPLACE=keep leaves the argument out of the command
# line entirely, which is what resumes the store: PIANO_DATA_STORE_REPLACE only accepts 1 or a store seed's 8 hex digits, so an empty
# value is refused, and naming this seed's own digits would replace it as well.
if [ "${SHOT_REPLACE:-1}" = 1 ]; then export PIANO_DATA_STORE_REPLACE=1; fi
# The still costs 26 bytes of a 999-byte command line, so a830-input.sh gives those bytes to the touch screen and the keyboard
# cover by exporting PIANO_SHOT_ARG empty; :+ keeps the space out of the line when it is empty.
SHOT_ARG=${PIANO_SHOT_ARG:-native-gop-display-shot=1}
export PIANO_EXTRA_ARGS="${PIANO_EXTRA_ARGS:-} piano-usb=2 piano-usb-shell=1${SHOT_ARG:+ $SHOT_ARG} piano-dpu=2 piano-dpu-flip=4 native-gop-display-flip-lossless=1 panic_on_crash_disabled=1 data-store-part=linux_data data-store-lba=117735999"
export PIANO_FAST_BOOT=1
export PIANO_DENY_EXEC=/usr/libexec/watchdogd,/usr/libexec/PerfPowerServices
export PIANO_GPU=2
export PIANO_A830_METAL=1
export PIANO_A830_HOST_TESTS=0
export PIANO_A830_TU_DEBUG=sysmem
# The A830 stage first (build-k273-image.sh would run it anyway), then the two units on top of that seed: one seed, one trust cache.
tooling/metal/build-turnip-a830.sh > out/k27/build-turnip-a830.log 2>&1
python3 tooling/userland/stage.py --seed out/turnip-a830/stage/scratch-data.img \
  --trust out/turnip-a830/stage/kernel.macho.trustcache --out out/a830-shot-stage \
  --on pianousb-shell --on gop-shot --seed-install-root pianousb-shell --seed-install-root gop-shot
export PIANO_SEED=/Users/zhangqi/Codex/upstream-tip/out/a830-shot-stage/scratch-data.img
export PIANO_TRUST=/Users/zhangqi/Codex/upstream-tip/out/a830-shot-stage/kernel.macho.trustcache
bash tooling/k273/build-k273-image.sh
rm -rf out/ram-verified-8748-a830-shot out/ram-dump-verified-8748-a830-shot
cp -R out/ram out/ram-verified-8748-a830-shot
cp -R out/ram-dump out/ram-dump-verified-8748-a830-shot
shasum -a 256 out/ram-verified-8748-a830-shot/boot-piano-macos-ram-A-bundled.img out/ram-verified-8748-a830-shot/boot-piano-uefi-ram-A-bundled.img
echo A830-SHOT-8748-BUILD-DONE
