#!/bin/bash
# 2026-11-28: the ESP of this unit's own k273 line (skills/install-piano-macos/SKILL.md gap G8, second unit piano-7ccdd855).
#
# Why these four inputs and no others. The kernel collection out/k27/kc-k27-native.bin and the device tree out/adt/piano-k27.adt are
# the ones tonight's RAM images booted with - the same collection carries PianoTouch with the two pinned panel images and this unit's
# reversed Y, and PianoKeyboard with the cover MCU - so the ESP has to carry that pair, not the 2026-11-24 pair (piano-ufs264.kc and
# paclr53.adt) that the ESP now on the medium was built from. The seed and the trust cache come from out/a830-shot-stage, which is the
# pair the store on linux_data is bound to (NativeRamBlock: first LBA 117735999, seed bb6dd1c3, resumed across three reboots); the
# gpu profile's own turnip-a830/stage pair would be a different install, and the ESP is what has to agree with the store.
#
# PIANO_UNIT_PROFILE is what makes this a second-unit build at all: tooling/persist/common.py takes ESP_FIRST, DATA_LAST and the rest
# from the unit's own GPT instead of the reference unit's constants, so the hidden-sector count, the dd window and the store LBA are
# this tablet's. The profile is a product of tooling/persist/unit_profile.py from this unit's own GPT heads; it is private to the unit
# and lives outside the repository (~/Codex/piano-unit).
#
# Arguments: the gpu profile's (piano-gpu=2, piano-gpu-delay=20, panic_on_crash_disabled=1, the A830 kext check) plus the ones tonight's
# RAM line ran with, and nothing else - no piano-usb-hid (the tablet's own keyboard and touch are what we are proving), no charging and
# no power policy yet. --default macos-diag keeps the unattended rule: the first cold starts return to ABL by themselves.
set -euo pipefail
cd /Users/zhangqi/Codex/upstream-tip
export PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin:/Users/zhangqi/bin:/opt/homebrew/opt/llvm/bin:/Users/zhangqi/Codex/mtools/install/bin
export PIANO_K27_TREE=/Users/zhangqi/Codex/k27-tree
export PIANO_UNIT_PROFILE=${PIANO_UNIT_PROFILE:-/Users/zhangqi/Codex/piano-unit/piano-7ccdd855-shim.json}
export PIANO_DATA_STORE_RELAYOUT=1
python3 tooling/persist/build-esp.py --profile gpu --trust out/a830-shot-stage/kernel.macho.trustcache --seed out/a830-shot-stage/scratch-data.img --default macos-diag --diag-reset-after 240 --timeout 3 --fast-boot 1 --boot-guard 180 --deny-exec /usr/libexec/watchdogd --deny-exec /usr/libexec/PerfPowerServices --arg piano-usb=2 --arg piano-usb-shell=1 --arg piano-touch=2 --arg piano-keyboard=2 --arg piano-dpu=2 --arg piano-dpu-flip=4 --arg native-gop-display-flip-lossless=1 --firmware-dir "${PIANO_ESP_FIRMWARE_DIR:-/Users/zhangqi/Codex/k273-esp/inputs/firmware}" --name esp-7ccdd855-k273 --out out/persistent-k273
shasum -a 256 out/persistent-k273/esp-7ccdd855-k273.img
echo ESP-K273-BUILT
