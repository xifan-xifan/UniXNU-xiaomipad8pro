#!/bin/bash
# 2026-11-28: the persistent pair of this unit's own k273 line: boot_a (BootShim + FD) and the 1 GiB ESP (skills gap G8, second unit
# piano-7ccdd855). Nothing here touches the tablet; the writes are tooling/persist/maint-session.py's, per WRITE-PLAN.md.
#
# Why these inputs. The kernel collection out/k27/kc-k27-native.bin and the device tree out/adt/piano-k27.adt are the ones tonight's
# RAM images booted with, so the ESP carries the collection that has PianoTouch with the two pinned panel images and this unit's
# reversed Y, and PianoKeyboard with the cover MCU - not the 2026-11-24 pair (piano-ufs264.kc, paclr53.adt) that the ESP now on the
# medium came from. The seed and the trust cache come from out/a830-shot-stage, the pair the store on linux_data is bound to
# (NativeRamBlock: first LBA 117735999, seed bb6dd1c3, resumed across three reboots). boot_a is built with --same-kernel-as that
# RAM record, so the shim, the FD and the DTB are byte for byte the ones that ran on the glass tonight.
#
# PIANO_UNIT_PROFILE is what makes this a second-unit build at all: common.py takes ESP_FIRST, DATA_LAST and the rest from this
# unit's own GPT rather than the reference unit's constants, so the FAT hidden sectors, the dd window and the store LBA are ours.
# The profile comes from tooling/persist/unit_profile.py over this unit's GPT heads and stays outside the repository.
#
# PIANO_BOOT_FD_RAM_WINDOW=1 is a true statement about the FD, not a way past the check: this ADT asks for a 13310 MiB XNU window,
# and out/fd/piano-uefi-A-bundled.fd is the FD that boots exactly that ADT on this tablet in the RAM images (the same kernel field
# build-boot.py copies into boot_a). The 8 GiB cap in the check is the older DarwinBoot's.
#
# Arguments: the gpu profile's (piano-gpu=2, piano-gpu-delay=20, panic_on_crash_disabled=1, the A830 kext check) plus the ones the
# RAM line ran with, and nothing else: no piano-usb-hid (the tablet's own keyboard and touch are what is being proven), no charging
# and no power policy yet. --default macos-diag keeps the unattended rule, so a cold start that does not reach a healthy desktop
# returns to ABL by itself. Unsigned on purpose: WRITE-PLAN.md section "Slot A boot triplet write" - ABL on this unlocked unit
# takes a v4 container whose AVB chain does not match, and the reference signing host is not ours (G7).
set -euo pipefail
cd /Users/zhangqi/Codex/upstream-tip
export PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:/Users/zhangqi/bin:/opt/homebrew/opt/llvm/bin:/Users/zhangqi/Codex/mtools/install/bin
export PIANO_K27_TREE=/Users/zhangqi/Codex/k27-tree
export PIANO_UNIT_PROFILE=${PIANO_UNIT_PROFILE:-/Users/zhangqi/Codex/piano-unit/piano-7ccdd855-shim.json}
export PIANO_DATA_STORE_RELAYOUT=1
export PIANO_BOOT_FD_RAM_WINDOW=1
python3 tooling/persist/build-boot.py --series 5 --fd out/fd/piano-uefi-A-bundled.fd --dtb out/inputs/piano-boot.dtb --same-kernel-as out/ram/build-A-bundled.json --out out/persistent-k273
python3 tooling/persist/build-esp.py --profile gpu --trust out/a830-shot-stage/kernel.macho.trustcache --seed out/a830-shot-stage/scratch-data.img --default macos-diag --diag-reset-after 240 --timeout 3 --fast-boot 1 --boot-guard 180 --deny-exec /usr/libexec/watchdogd --deny-exec /usr/libexec/PerfPowerServices --arg piano-usb=2 --arg piano-usb-shell=1 --arg piano-touch=2 --arg piano-keyboard=2 --arg piano-dpu=2 --arg piano-dpu-flip=4 --arg native-gop-display-flip-lossless=1 --firmware-dir "${PIANO_ESP_FIRMWARE_DIR:-/Users/zhangqi/Codex/k273-esp/inputs/firmware}" --name esp-7ccdd855-k273 --out out/persistent-k273
shasum -a 256 out/persistent-k273/boot-piano-macos-persistent-v5.img out/persistent-k273/esp-7ccdd855-k273.img
echo PERSISTENT-K273-BUILT
