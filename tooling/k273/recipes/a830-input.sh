#!/bin/bash
# 2026-11-27 (night, third line of this seed): the shell, the store and the desktop, plus the tablet's own input.
#
# a830-shot.sh is the base and its header explains the seed, the boot job and the store; read that first. This only changes what
# goes on the command line and how long the boot stays up, because that recipe has run twice and answered its two questions
# (docs/results-2026-11-27-store-resumed-across-a-reboot.md): the store resumes, and the panel still is a file.
#
#   - Input, the desk profile from tooling/k27/build-desktop-images.sh: piano-touch=2 piano-keyboard=2 piano-keyboard-gestures=1
#     piano-backlight=2 piano-backlight-keys=1 and piano-gpu-hang-ms=10000. No PIANO_TOUCH_CALIBRATION here, so PianoTouch runs its
#     approximate linear map (docs/drivers/touch.md): usable for the first touch checks, not the final calibration answer.
#   - The base recipe's line is already 999 bytes and the builder's budget is under 1000, so PIANO_BOOT_ARGS_SPLIT=1 forces the k27
#     0066 split: the loader keeps the early and legacy tokens and the rest goes to /chosen/piano-boot-args-ram in the payload ADT
#     (docs/boot-args-length.md). The piano-* knobs here are all post-merge readers, so the split is exactly what they allow.
#   - PIANO_RESET_AFTER=1800 because the work is now done from the shell: 900 s left about six minutes of pipe after the desktop
#     was up, and a file fetch of a few hundred KiB eats that.
#   - SHOT_REPLACE stays keep. The store of this seed (identity bb6dd1c3) is resumed, and boot two left
#     /private/var/db/piano-persist2.txt plus /private/var/db/piano-persist.d/{nested.txt,blob.bin,blob.sha} to read back.
#     Changing command line does not change the seed identity, so the store still belongs to us.
set -euo pipefail
cd /Users/zhangqi/Codex/upstream-tip
export SHOT_REPLACE=keep
export PIANO_RESET_AFTER=${PIANO_RESET_AFTER:-1800}
export PIANO_BOOT_ARGS_SPLIT=${PIANO_BOOT_ARGS_SPLIT:-1}
export PIANO_EXTRA_ARGS="piano-touch=2 piano-keyboard=2 piano-keyboard-gestures=1 piano-backlight=2 piano-backlight-keys=1 piano-gpu-hang-ms=10000"
bash tooling/k273/recipes/a830-shot.sh
mv out/ram-verified-8748-a830-shot out/ram-verified-8748-a830-input
mv out/ram-dump-verified-8748-a830-shot out/ram-dump-verified-8748-a830-input
echo A830-INPUT-8748-BUILT