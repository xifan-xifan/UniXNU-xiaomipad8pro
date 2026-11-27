#!/bin/bash
# 2026-11-27 (night, fourth line of this seed): the input boot with the touch panel images built in.
#
# a830-input.sh asked the panel for its firmware and got three good lines and one dead end
# (docs/results-2026-11-27-input-keyboard-and-the-missing-image.md): GENI SE2 up, loopback PASSED 64/64,
# identify (GENI): NT36532 after 1 tries in 17 ms, then lcd-id gpio100 = 1: BOE panel, image 0 bytes and
# FAILED at step 5: firmware: no usable image for this panel (built without it?). The build host is the reason, not the driver:
# tooling/k27/build-native-piano-touch.sh looks for the two pinned images under ../xiaomipad-8pro-mainline/firmware/novatek/piano
# or the same path ending in piano under ~/Downloads/XiaomiPad8Pro-Ubuntu, and on this Mac they sit one level higher
# (firmware/novatek, no piano directory), so every PianoTouch built here has been built without them. Nothing was downloaded for
# this: the two files already on the Mac are the pinned ones, 245760 bytes with the SHA-256 of firmware/piano-input.json
# (boe b7999389d49decb1..., csot 14b30bb7799d273f...), checked again by the build before it stages a copy under out/.
#
# PIANO_TOUCH_CALIBRATION is set empty on purpose: the calibration is private to this unit and we do not have it, and an empty
# value makes the generator write the linear map instead of reading ~/.config/piano/env, so the build here is reproducible.
# Everything else is a830-input.sh unchanged, including SHOT_REPLACE=keep (the store stays resumed) and the 1800 s self-reset.
set -euo pipefail
export PIANO_TOUCH_FW_DIR=${PIANO_TOUCH_FW_DIR:-/Users/zhangqi/Downloads/XiaomiPad8Pro-Ubuntu/xiaomipad-8pro-mainline/firmware/novatek}
export PIANO_TOUCH_CALIBRATION=${PIANO_TOUCH_CALIBRATION-}
bash tooling/k273/recipes/a830-input.sh
cd /Users/zhangqi/Codex/upstream-tip
mv out/ram-verified-8748-a830-input out/ram-verified-8748-a830-touch
mv out/ram-dump-verified-8748-a830-input out/ram-dump-verified-8748-a830-touch
shasum -a 256 out/ram-verified-8748-a830-touch/boot-piano-macos-ram-A-bundled.img
echo A830-TOUCH-8748-BUILT
