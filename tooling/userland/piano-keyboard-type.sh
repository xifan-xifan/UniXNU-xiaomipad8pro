#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
# piano-keyboard-type: the tablet's own HID keyboards known to loginwindow, so it never takes the keyboard cover for an unknown
# keyboard and opens Keyboard Setup Assistant (2026-11-21: a Data store without this file, a RAM image's or a reinstall from the
# seed, did). loginwindow reads /Library/Preferences/com.apple.keyboardtype.plist, dictionary "keyboardtype", key
# "<ProductID>-<VendorID>-<CountryCode>", value the keyboard type (40 = ANSI). Ours (VendorID 6900, CountryCode 0): the keyboard cover
# 20555 (PianoKeyboardHID), the touch screen 20564, the USB debug keyboard 20565, and 4178.
# Missing keys are added with defaults(1) (through cfprefsd, never a file replacement); a key already there keeps its value (the
# owner's own choice in Keyboard Setup Assistant), and every other key of the file is left as it is. Installed as
# /usr/local/libexec/piano-keyboard-type and run by the one-shot LaunchDaemon com.unixnu.piano-keyboard-type (RunAtLoad,
# LaunchOnlyOnce: once per boot; nothing to do once the four are there).
# Lines go to the system log: log show --last 1h --predicate 'process == "logger"' | grep piano-keyboard-type
# Host tests (tooling/userland/test-stage.py): PIANO_USERLAND_ROOT stands for /; logger comes from PATH.
set -u
R=${PIANO_USERLAND_ROOT:-}
DOMAIN=$R/Library/Preferences/com.apple.keyboardtype
KEYS='20555-6900-0 20564-6900-0 20565-6900-0 4178-6900-0'
TYPE=40

say() {
  echo "piano-keyboard-type: $*"
  logger -t piano-keyboard-type "$*" 2> /dev/null || true
}

# has KEY: the key is in the keyboardtype dictionary (read through cfprefsd, as loginwindow reads it)
has() { defaults read "$DOMAIN" keyboardtype 2> /dev/null | grep -Eq "^[[:space:]]*\"?$1\"?[[:space:]]*="; }

# on_disk KEY: the key in the file itself (cfprefsd writes it out; a write it could not store still reads back through it)
on_disk() { plutil -extract "keyboardtype.$1" raw -o - "$DOMAIN.plist" > /dev/null 2>&1; }

[ -d "$R/Library/Preferences" ] || { say "no $R/Library/Preferences: nothing written"; exit 1; }
added= kept=
for k in $KEYS; do
  if has "$k"; then
    kept="$kept $k"
    continue
  fi
  if ! defaults write "$DOMAIN" keyboardtype -dict-add "$k" -int "$TYPE" 2> /dev/null || ! has "$k"; then
    say "$k could not be added (keyboardtype is not a dictionary?): the file is left as it is"
    exit 1
  fi
  added="$added $k"
done
for k in $added; do
  n=0
  until on_disk "$k"; do
    n=$((n + 1))
    [ "$n" -lt 10 ] || { say "$k reads back but is not in $DOMAIN.plist: not stored"; exit 1; }
    sleep 1
  done
done
if [ -n "$added" ]; then
  say "added$added as type $TYPE (ANSI)${kept:+; already known:$kept}"
else
  say "all four keyboards known already"
fi
exit 0
