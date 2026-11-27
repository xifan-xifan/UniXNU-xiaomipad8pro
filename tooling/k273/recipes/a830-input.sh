#!/bin/bash
# 2026-11-27 (night, third line of this seed): the shell, the store and the desktop, plus the tablet's own input.
#
# a830-shot.sh is the base and its header explains the seed, the boot job and the store; read that first. This recipe only changes
# what goes on the command line and how long the boot stays up, because that recipe has run twice and answered its two questions
# (docs/results-2026-11-27-store-resumed-across-a-reboot.md): the store resumes, and the panel still is a file.
#
#   - Input: piano-touch=2 and piano-keyboard=2, the first two words of the desk profile in tooling/k27/build-desktop-images.sh.
#     No PIANO_TOUCH_CALIBRATION is staged, so PianoTouch runs its approximate linear map (docs/drivers/touch.md): enough to tell
#     whether the controller speaks to us, not the final calibration answer. piano-keyboard-gestures, piano-backlight-keys and
#     piano-backlight wait for a line with room.
#   - No ADT split. PIANO_BOOT_ARGS_SPLIT=1 was tried first and is the right long-term shape, but the builder then refuses
#     ("pianousb-shell cannot read piano-usb-shell through kern.bootargs. rebuild the kernel collection with k27 0076"), because on
#     a machine with no installed-seed record the boot-argument readers are read from the Data-side layer, which this unit does not
#     have, so a reader moved into the tail counts as unreadable. Instead the still goes away: native-gop-display-shot=1 costs 26 of
#     the 999 bytes and its question is already answered (source=flip on the second boot), and those 26 bytes buy the two input words.
#     That keeps the command line at 983 bytes for the 8-CPU entry, with no split, no piano-boot-args selector and no 0076 dependency.
#   - PIANO_RESET_AFTER=1800, one extra byte, because the work is done from the shell now: at 900 s the pipe was live for about six
#     minutes after the desktop came up, and fetching a few hundred KiB eats that.
#   - SHOT_REPLACE stays keep: seed bb6dd1c3's store is resumed, and boot two left /private/var/db/piano-persist2.txt plus
#     /private/var/db/piano-persist.d/{nested.txt,blob.bin,blob.sha} to read back. Command line changes do not change the seed identity.
set -euo pipefail
cd /Users/zhangqi/Codex/upstream-tip
export SHOT_REPLACE=keep
export PIANO_RESET_AFTER=${PIANO_RESET_AFTER:-1800}
export PIANO_SHOT_ARG=
unset PIANO_BOOT_ARGS_SPLIT
export PIANO_EXTRA_ARGS="${PIANO_EXTRA_ARGS:-} piano-touch=2 piano-keyboard=2"
bash tooling/k273/recipes/a830-shot.sh
mv out/ram-verified-8748-a830-shot out/ram-verified-8748-a830-input
mv out/ram-dump-verified-8748-a830-shot out/ram-dump-verified-8748-a830-input
echo A830-INPUT-8748-BUILT