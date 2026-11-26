#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
# piano-userland install.sh: puts the userland staged next to it (manifest.tsv, files/) in place on the tablet's macOS, as root
# (docs/userland.md section 5). tooling/userland/stage.py copies it into the Data seed with the files and adds
#   sh /private/var/piano/userland/install.sh --boot
# to the seed's boot job, so every boot from that seed runs it; a live update runs it with --live (docs/userland.md section 7).
#
#   install.sh [--boot|--live]   install what differs, load the jobs that are on, restart the loaded jobs whose files changed
#   install.sh --check           change nothing: every file against the manifest (sha256, owner, mode) and every job's state;
#                                exit status 1 on any difference
#
# Rules:
#   - A staged file whose sha256 is not the manifest's is not installed (a torn copy); its unit counts as failed.
#   - A file is replaced only when its bytes differ, by install -S (a new file renamed over the old one: a running program keeps
#     its pages). Owner and mode are set as the manifest says, also on an unchanged file.
#   - A job that is on (default_on in configs/userland.toml) is loaded (launchctl bootstrap) when it is not; when the owner turned
#     it off (launchctl disable), launchd refuses the bootstrap and it stays off. A job that is off has Disabled in its plist (written
#     by stage.py) and is never loaded here; `launchctl enable system/<label>` (an agent: gui/<uid>/<label>) turns it on for good.
#   - A loaded job whose files changed is restarted (kickstart -k; bootout and bootstrap when its plist changed).
#   - Agents go to the console user's session; with nobody logged in, the next login window or login loads them.
#   - A missing directory is made one level at a time, every level 0755 root:wheel (install -d -m 755 a/b/c sets c only; a and b took the
#     caller's umask: the layer apply runs with 077, which left v13.1y7's PianoMicNS.driver/ at 0700 and the HAL plug-in unreadable for
#     coreaudiod's driver service). Every directory of a plugin bundle (/Library/GPUBundles/<x>.bundle/..., /Library/Audio/Plug-Ins/HAL/
#     <x>.driver/...: from the bundle root down to the file) is made 0755 root:wheel when it is not, also on an unchanged file;
#     --check reports such a directory.
#   - A file this installer put in place earlier (the receipt) that the manifest no longer lists is removed, its job booted out,
#     when it is unchanged since; anything else is left alone.
#   - kind link (LT-B3, gpu/cryptex2): DEST is a symbolic link; the staged file holds its absolute target (no newline) and the
#     sha256 column is that target's. It is made as DEST.piano-link and renamed over DEST (mv -fh: a regular file there, ours or a
#     kept one, is replaced; a running program keeps its mapping); owner set with chown -h. --check and the receipt compare the
#     link's target, never the bytes behind it; a file row whose DEST is a symlink is replaced by the file, never adopted through it.
#   - A file already there that this installer did not put in place (not in its receipt: the base seed's Metal plugin, an owner's
#     own file) is copied to $STATE/prev/<its path> before it is taken over (replaced, or adopted with the same bytes), once; when the
#     manifest no longer lists it, that copy is put back instead of the file being removed (docs/data-store.md section 9). A file
#     whose copy cannot be made is not taken over.
# SIGHUP is ignored: run through the USB root shell, a restart of pianousb-shell ends that shell but not this script.
# Lines go to stdout (the boot job's: the kernel log with stderr-log=1), the system log (logger -t piano-userland) and
# /private/var/db/piano-userland/last.txt.
#
# Host tests (tooling/userland/test-stage.py): PIANO_USERLAND_ROOT is a directory standing for /, PIANO_USERLAND_NOCHOWN=1 leaves
# owners alone (a user cannot chown to root), launchctl and logger come from PATH.
set -u
trap '' HUP
MODE=${1:---boot}
case "$MODE" in --boot|--live|--check) ;; *) echo "usage: install.sh [--boot|--live|--check]" >&2; exit 2 ;; esac
R=${PIANO_USERLAND_ROOT:-}
HERE=$(cd "$(dirname "$0")" && pwd)
S=${PIANO_USERLAND_STAGE:-$HERE}
STATE=$R/private/var/db/piano-userland
NOCHOWN=${PIANO_USERLAND_NOCHOWN:-0}
TAB=$(printf '\t')
LOG=
FAILED=0
CHANGED=' '
PLIST_CHANGED=' '
PASSD_KEEP=0
PASSD=/usr/local/libexec/piano-passd-policy

# The passd policy changes persistent launchd state as well as files. Revert it
# BEFORE removing either of its files. A failed revert retains the whole unit
# and its receipt, including when gui/501 is absent, for a safe retry at login.
passd_revert() {
  [ -f "$STATE/receipt.tsv" ] || return 0
  want=$(awk -F '\t' -v p="$PASSD" '$9 == p {print $7}' "$STATE/receipt.tsv")
  [ -n "$want" ] || return 0
  if [ ! -f "$R$PASSD" ] || [ "$(sum "$R$PASSD")" != "$want" ]; then
    say 'passd policy: rollback helper missing or changed; unit and receipt kept'; return 1
  fi
  sh "$R$PASSD" --remove
}

say() {
  echo "piano-userland: $*"
  logger -t piano-userland "$*" 2> /dev/null || true
  LOG="$LOG$*
"
}

sum() {
  if command -v shasum > /dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
  elif command -v openssl > /dev/null 2>&1; then openssl dgst -sha256 -r "$1" | cut -d' ' -f1
  else echo unknown; fi
}

# lsum LINK: the sha256 of a symbolic link's target string (what a link row's sha256 column holds); "none" when LINK is not a link
lsum() {
  [ -L "$1" ] || { echo none; return; }
  lt=$(readlink "$1")
  if command -v shasum > /dev/null 2>&1; then printf '%s' "$lt" | shasum -a 256 | cut -d' ' -f1
  elif command -v openssl > /dev/null 2>&1; then printf '%s' "$lt" | openssl dgst -sha256 -r | cut -d' ' -f1
  else echo unknown; fi
}

# placed KIND DEST WANT: 0 when DEST is in place as the row says (a link with that target; a regular file, not a link, with those bytes)
placed() {
  if [ "$1" = link ]; then [ -L "$2" ] && [ "$(lsum "$2")" = "$3" ]
  else [ -f "$2" ] && [ ! -L "$2" ] && [ "$(sum "$2")" = "$3" ]; fi
}

# The owner and mode a file has: "user:group mode" (mode in octal, as the manifest writes it, e.g. 0755).
has() {
  stat -f '%Su:%Sg 0%Lp' "$1" 2> /dev/null
}

domain_of() {   # domain_of KIND: system, gui/<uid> of the console user, or nothing (an agent with nobody logged in)
  if [ "$1" = daemon ]; then echo system; return; fi
  u=$(stat -f %u "$R/dev/console" 2> /dev/null || echo 0)
  [ "$u" != 0 ] && echo "gui/$u"
}

loaded() { launchctl print "$1/$2" > /dev/null 2>&1; }

ours() { [ -f "$STATE/receipt.tsv" ] && cut -f9 "$STATE/receipt.tsv" | grep -qxF "$1"; }

# keep DEST DST: before the first take-over of a file this installer did not put in place, a copy of it in $STATE/prev/DEST (cp -p:
# its owner, mode and times). Never replaced once made: it is the file of before.
keep() {
  [ -e "$2" ] || [ -L "$2" ] || return 0
  ! ours "$1" || return 0
  [ ! -e "$STATE/prev$1" ] && [ ! -L "$STATE/prev$1" ] || return 0
  if [ -L "$2" ]; then cpflags=-pPR; else cpflags=-p; fi
  mkdir -p "$(dirname "$STATE/prev$1")" && cp $cpflags "$2" "$STATE/prev$1.new" && mv -fh "$STATE/prev$1.new" "$STATE/prev$1" || return 1
  say "$1: the file in place was not installed by this installer: kept as $STATE/prev$1 (back when the manifest drops it)"
}

disabled() { launchctl print-disabled "$1" 2> /dev/null | grep -Eq "\"$2\" => (disabled|true)"; }

# mkdirs DIR: DIR and every missing parent, one level at a time, each 0755 (root:wheel)
mkdirs() {
  [ -d "$1" ] && return 0
  mkdirs "$(dirname "$1")" || return 1
  if [ "$NOCHOWN" = 1 ]; then install -d -m 755 "$1"; else install -d -o root -g wheel -m 755 "$1"; fi
}

# bundle_dirs DEST: the directories of DEST's plugin bundle, from DEST's parent up to the bundle root (none outside a bundle)
bundle_dirs() {
  case "$1" in /Library/GPUBundles/*.bundle/*|/Library/Audio/Plug-Ins/HAL/*.driver/*) ;; *) return 0 ;; esac
  bd=$(dirname "$1")
  while [ "$bd" != / ]; do
    echo "$bd"
    case "${bd##*/}" in *.bundle|*.driver) return 0 ;; esac
    bd=$(dirname "$bd")
  done
}

# bundle_fix DEST: every directory of DEST's plugin bundle 0755 root:wheel (repaired and logged; --check: reported). 1 when one is not.
bundle_fix() {
  bf=0
  while IFS= read -r bdir; do
    [ -n "$bdir" ] && [ -d "$R$bdir" ] || continue
    if [ "$NOCHOWN" = 1 ]; then bnow=$(has "$R$bdir" | cut -d' ' -f2); bexp=0755
    else bnow=$(has "$R$bdir"); bexp='root:wheel 0755'; fi
    [ "$bnow" = "$bexp" ] && continue
    if [ "$MODE" = --check ]; then say "$bdir: $bnow, a plugin bundle directory is $bexp"; bf=1; continue; fi
    if [ "$NOCHOWN" = 1 ]; then chmod 755 "$R$bdir"; else chown root:wheel "$R$bdir" && chmod 755 "$R$bdir"; fi ||
      { say "$bdir: could not be made $bexp"; bf=1; continue; }
    say "$bdir: directory $bnow -> $bexp (a plugin bundle directory)"
  done <<EOF
$(bundle_dirs "$1")
EOF
  return "$bf"
}

[ -f "$S/manifest.tsv" ] || { say "no manifest at $S/manifest.tsv"; exit 1; }
[ "$MODE" = --check ] || mkdir -p "$STATE"

# 1. Files.
while IFS=$TAB read -r unit kind on mode owner group want staged dest label; do
  case "$unit" in ''|'#'*) continue ;; esac
  src=$S/$staged
  dst=$R$dest
  if [ "$MODE" = --check ] && [ "$kind" = link ]; then
    if [ ! -L "$dst" ]; then say "$dest: not a symbolic link ($unit)"; FAILED=1; continue; fi
    got=$(lsum "$dst")
    [ "$got" = "$want" ] || [ "$got" = unknown ] || { say "$dest: link target $(readlink "$dst"), sha256 $got, the manifest's $want ($unit)"; FAILED=1; }
    if [ "$NOCHOWN" != 1 ]; then now=$(has "$dst" | cut -d' ' -f1); [ "$now" = "$owner:$group" ] || { say "$dest: link owner $now, the manifest's $owner:$group ($unit)"; FAILED=1; }; fi
    continue
  fi
  if [ "$MODE" = --check ]; then
    if [ -L "$dst" ]; then say "$dest: a symbolic link, the manifest's is a file ($unit)"; FAILED=1; continue; fi
    if [ ! -f "$dst" ]; then say "$dest: missing ($unit)"; FAILED=1; continue; fi
    got=$(sum "$dst")
    [ "$got" = "$want" ] || [ "$got" = unknown ] || { say "$dest: sha256 $got, the manifest's $want ($unit)"; FAILED=1; }
    if [ "$NOCHOWN" = 1 ]; then now=$(has "$dst" | cut -d' ' -f2); exp=$mode
    else now=$(has "$dst"); exp="$owner:$group $mode"; fi
    [ "$now" = "$exp" ] || { say "$dest: $now, the manifest's $exp ($unit)"; FAILED=1; }
    bundle_fix "$dest" || FAILED=1
    continue
  fi
  if [ ! -f "$src" ]; then say "$dest: the staged file $staged is missing: $unit not installed"; FAILED=1; continue; fi
  got=$(sum "$src")
  if [ "$got" != "$want" ] && [ "$got" != unknown ]; then
    say "$dest: the staged file's sha256 is $got, not the manifest's $want (a torn copy?): $unit not installed"
    FAILED=1
    continue
  fi
  dir=$(dirname "$dst")
  mkdirs "$dir" || { say "$dest: its directory could not be made: $unit not installed"; FAILED=1; continue; }
  if [ "$kind" = link ]; then
    target=$(cat "$src")
    case "$target" in /*) ;; *) say "$dest: link target $target is not absolute: $unit not installed"; FAILED=1; continue ;; esac
    if [ -L "$dst" ] && [ "$(readlink "$dst")" = "$target" ]; then
      if [ "$NOCHOWN" != 1 ] && [ "$(has "$dst" | cut -d' ' -f1)" != "$owner:$group" ]; then
        chown -h "$owner:$group" "$dst" && say "$dest: link owner -> $owner:$group"
      fi
      continue
    fi
    if [ -d "$dst" ] && [ ! -L "$dst" ]; then say "$dest: a directory is there: link not installed ($unit)"; FAILED=1; continue; fi
    keep "$dest" "$dst" || { say "$dest: no copy of what it replaces could be kept: link not installed ($unit)"; FAILED=1; continue; }
    rm -f "$dst.piano-link"
    if ln -s "$target" "$dst.piano-link" && { [ "$NOCHOWN" = 1 ] || chown -h "$owner:$group" "$dst.piano-link"; } &&
       mv -fh "$dst.piano-link" "$dst"; then
      say "$dest: link -> $target"
      CHANGED="$CHANGED$unit "
    else
      rm -f "$dst.piano-link"; say "$dest: link install failed ($unit)"; FAILED=1
    fi
    continue
  fi
  bundle_fix "$dest" || FAILED=1
  if [ -L "$dst" ]; then
    # a link where the manifest has a file (a link row of an earlier layer, or anything else): the file replaces the link itself
    keep "$dest" "$dst" || { say "$dest: no copy of the link in place could be kept: not installed ($unit)"; FAILED=1; continue; }
    rm -f "$dst.piano-new"
    if [ "$NOCHOWN" = 1 ]; then install -m "$mode" "$src" "$dst.piano-new"; else install -o "$owner" -g "$group" -m "$mode" "$src" "$dst.piano-new"; fi &&
      mv -fh "$dst.piano-new" "$dst" || { rm -f "$dst.piano-new"; say "$dest: install over the link failed ($unit)"; FAILED=1; continue; }
    say "$dest: installed over a symbolic link (sha256 ${want%"${want#????????????????}"})"
    CHANGED="$CHANGED$unit "
    continue
  fi
  if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
    keep "$dest" "$dst" || { say "$dest: no copy of the file in place could be kept: not taken over ($unit)"; FAILED=1; continue; }
    if [ "$NOCHOWN" = 1 ]; then now=$(has "$dst" | cut -d' ' -f2); exp=$mode
    else now=$(has "$dst"); exp="$owner:$group $mode"; fi
    if [ "$now" != "$exp" ]; then
      if [ "$NOCHOWN" = 1 ]; then chmod "$mode" "$dst"; else chown "$owner:$group" "$dst" && chmod "$mode" "$dst"; fi
      say "$dest: owner/mode $now -> $exp"
    fi
    continue
  fi
  keep "$dest" "$dst" || { say "$dest: no copy of the file it replaces could be kept: not installed ($unit)"; FAILED=1; continue; }
  if [ "$NOCHOWN" = 1 ]; then install -S -m "$mode" "$src" "$dst"; else install -S -o "$owner" -g "$group" -m "$mode" "$src" "$dst"; fi
  if [ $? != 0 ]; then say "$dest: install failed ($unit)"; FAILED=1; continue; fi
  say "$dest: installed (sha256 ${want%"${want#????????????????}"})"
  CHANGED="$CHANGED$unit "
  case "$kind" in daemon|agent) PLIST_CHANGED="$PLIST_CHANGED$unit " ;; esac
done < "$S/manifest.tsv"

# A removed policy must restore its persistent state before losing its files.
passd_on=$(awk -F '\t' -v p="$PASSD" '$9 == p {print $3}' "$S/manifest.tsv")
if [ -z "$passd_on" ] && [ "$MODE" != --check ]; then
  passd_revert || { FAILED=1; PASSD_KEEP=1; }
fi

# 2. Files of the previous receipt that the manifest no longer lists: removed when unchanged since, their jobs booted out first.
if [ "$MODE" != --check ] && [ -f "$STATE/receipt.tsv" ]; then
  while IFS=$TAB read -r unit kind on mode owner group want staged dest label; do
    case "$unit" in ''|'#'*) continue ;; esac
    [ "$unit" != piano-passd-policy ] || [ "$PASSD_KEEP" != 1 ] || continue
    cut -f9 "$S/manifest.tsv" | grep -qxF "$dest" && continue
    dst=$R$dest
    prev=$STATE/prev$dest
    if [ "$kind" = link ]; then
      if [ -L "$dst" ] && [ "$(lsum "$dst")" = "$want" ]; then
        if [ -e "$prev" ] || [ -L "$prev" ]; then
          if cp -pPR "$prev" "$dst.piano-prev" && mv -fh "$dst.piano-prev" "$dst"; then
            rm -f "$prev" && say "$dest: no longer in the manifest: what the link had replaced is back"
          else
            rm -f "$dst.piano-prev"; say "$dest: no longer in the manifest; putting back $prev failed: left as it is"; FAILED=1
          fi
        else
          rm -f "$dst" && say "$dest: link removed (no longer in the manifest)"
        fi
      elif [ -e "$dst" ] || [ -L "$dst" ]; then
        say "$dest: no longer in the manifest, but not the link it installed: left"
      fi
      continue
    fi
    if [ -L "$dst" ]; then say "$dest: no longer in the manifest, but a symbolic link now: left"; continue; fi
    if [ -f "$dst" ] && [ "$(sum "$dst")" != "$want" ]; then say "$dest: no longer in the manifest, but changed since it was installed: left"; continue; fi
    if [ -f "$dst" ] && [ "$kind" != file ]; then
      d=$(domain_of "$kind")
      if [ -n "$d" ] && loaded "$d" "$label"; then launchctl bootout "$d/$label" 2> /dev/null; fi
    fi
    if [ -f "$prev" ]; then
      if cp -p "$prev" "$dst.piano-prev" && mv -f "$dst.piano-prev" "$dst"; then
        rm -f "$prev" && say "$dest: no longer in the manifest: the file it had replaced is back"
      else
        rm -f "$dst.piano-prev"; say "$dest: no longer in the manifest; putting back $prev failed: left as it is"; FAILED=1
      fi
    elif [ -f "$dst" ]; then
      rm -f "$dst" && say "$dest: removed (no longer in the manifest)"
    fi
  done < "$STATE/receipt.tsv"
fi

# The receipt: what is in place now (before any job is touched, so a run cut short below still knows its files).
if [ "$MODE" != --check ]; then
  : > "$STATE/receipt.tsv.new"
  while IFS=$TAB read -r unit kind on mode owner group want staged dest label; do
    case "$unit" in ''|'#'*) continue ;; esac
    placed "$kind" "$R$dest" "$want" &&
      printf '%s\n' "$unit$TAB$kind$TAB$on$TAB$mode$TAB$owner$TAB$group$TAB$want$TAB$staged$TAB$dest$TAB$label" >> "$STATE/receipt.tsv.new"
  done < "$S/manifest.tsv"
  if [ "$PASSD_KEEP" = 1 ] && [ -f "$STATE/receipt.tsv" ]; then
    awk -F '\t' '$1 == "piano-passd-policy"' "$STATE/receipt.tsv" >> "$STATE/receipt.tsv.new"
  fi
  mv -f "$STATE/receipt.tsv.new" "$STATE/receipt.tsv"
fi

# Install/check only after the file receipt is written in the installer tree:
# an interrupted first install must not leave an override with no file receipt.
# With a GUI this is synchronous; without one, prepare for the Aqua agent.
if [ -n "$passd_on" ]; then
  passd_valid=1
  while IFS=$TAB read -r unit kind on mode owner group want staged dest label; do
    [ "$unit" = piano-passd-policy ] || continue
    [ -f "$R$dest" ] && [ "$(sum "$R$dest")" = "$want" ] || passd_valid=0
  done < "$S/manifest.tsv"
  if [ "$passd_valid" != 1 ]; then say 'passd policy: incomplete unit; hook refused'; FAILED=1
  elif [ "$MODE" != --check ] && ! ours "$PASSD"; then say 'passd policy: file receipt missing; hook refused'; FAILED=1
  elif [ "$passd_on" = 0 ]; then
    if [ "$MODE" != --check ]; then sh "$R$PASSD" --remove || FAILED=1; fi
  elif [ "$MODE" = --check ]; then sh "$R$PASSD" --check || FAILED=1
  else
    # The policy drops to UID 501. The boot hook's umask can leave its
    # parent root-only even when the policy directory itself is owned by 501.
    if chmod a+x "$STATE"; then sh "$R$PASSD" --install || FAILED=1
    else say 'passd policy: state parent is not searchable; hook refused'; FAILED=1; fi
  fi
fi

# 3. Jobs.
while IFS=$TAB read -r unit kind on mode owner group want staged dest label; do
  case "$kind" in daemon|agent) ;; *) continue ;; esac
  d=$(domain_of "$kind")
  if [ -z "$d" ]; then
    [ "$on" = 1 ] && say "$label: an agent; nobody is logged in: the next login window or login loads it"
    continue
  fi
  if [ "$MODE" = --check ]; then
    if loaded "$d" "$label"; then say "$label: loaded in $d"
    elif [ "$on" = 1 ] && disabled "$d" "$label"; then say "$label: on in the manifest, turned off by the owner (launchctl disable)"
    # launchd removes a LaunchOnlyOnce job after its one run of the boot (launchctl print then fails), so "not loaded" is its normal state
    elif [ "$on" = 1 ] && [ "$(/usr/bin/plutil -extract LaunchOnlyOnce raw -o - "$R$dest" 2>/dev/null)" = true ]; then say "$label: a run-once job (LaunchOnlyOnce): launchd drops it after its run"
    elif [ "$on" = 1 ]; then say "$label: on in the manifest but not loaded in $d"; FAILED=1
    else say "$label: off (launchctl enable $d/$label turns it on)"; fi
    continue
  fi
  case "$CHANGED" in *" $unit "*) changed=1 ;; *) changed=0 ;; esac
  if loaded "$d" "$label"; then
    [ "$changed" = 1 ] || continue
    case "$PLIST_CHANGED" in
      *" $unit "*) launchctl bootout "$d/$label" 2> /dev/null
                   if launchctl bootstrap "$d" "$R$dest" 2> /dev/null; then say "$label: reloaded in $d (its job file changed)"
                   elif [ "$on" = 0 ] || disabled "$d" "$label"; then say "$label: off now: booted out of $d"
                   else say "$label: bootstrap in $d failed after the bootout"; FAILED=1; fi ;;
      *) if launchctl kickstart -k "$d/$label" > /dev/null 2>&1; then say "$label: restarted in $d (its files changed)"
         else say "$label: kickstart -k in $d failed"; FAILED=1; fi ;;
    esac
    continue
  fi
  [ "$on" = 1 ] || continue
  if disabled "$d" "$label"; then say "$label: turned off by the owner (launchctl disable): left off"; continue; fi
  if launchctl bootstrap "$d" "$R$dest" 2> /dev/null; then say "$label: loaded in $d"
  else say "$label: launchctl bootstrap $d failed"; FAILED=1; fi
done < "$S/manifest.tsv"

result=ok
[ "$FAILED" = 0 ] || result=failed
say "$MODE done: $result (changed:${CHANGED% })"
if [ "$MODE" != --check ]; then
  { date -u '+%Y-%m-%dT%H:%M:%SZ' 2> /dev/null; printf '%s' "$LOG"; } > "$STATE/last.txt.new" && mv -f "$STATE/last.txt.new" "$STATE/last.txt"
fi
exit "$FAILED"
