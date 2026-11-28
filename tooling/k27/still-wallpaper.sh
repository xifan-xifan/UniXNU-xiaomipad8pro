#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The test images' still wallpaper, the power-saving default (docs/drivers/gpu-userspace.md, "Allocation limits and the video wallpaper,
# 2026-11-12"): staged by tooling/k27/build-desktop-images.sh and tooling/k27/build-gfx-test-images.sh (DESK_WALLPAPER=still, the
# default) as /private/var/piano/still-wallpaper.sh with its files (tooling/k27/still-wallpaper.py) in /private/var/piano/wallpaper,
# and run as root by the seed's boot job after settime.sh. SecurityAgent reads the login window's picture about 7 s after settime.sh
# (run 20261112-104004). Without the picture it plays the default aerial wallpaper, a 4096x2160 video decoded in software.
#  - /private/var/db/Wallpapers/<GeneratedUID>/Wallpaper.png and Metadata.plist, root's, as wallpaperexportd leaves them: the picture
#    the login window shows for that account, for every local account (uid 501 or more; the GeneratedUID read from its record in the
#    local directory node, which only root may read, at boot: it is not known when the seed is staged) and for root's and
#    _securityagent's own ids (the login window's fallback for its process's uid).
#  - The account's wallpaper store ~/Library/Application Support/com.apple.wallpaper/Store/Index.plist, the same stock picture as its
#    choice, so the desktop after the login is still as well.
#  - The picture comes from macOS 27.3's own /System/Library/CoreServices/DefaultDesktop.heic, converted by sips, in the background, to the
#    panel's 3200x2136 PNG (scaled to its height, the middle cropped: no stretching). The conversion is cached at /private/var/piano/wallpaper-stock.png and a later boot uses the cache; until it exists the staged still is what lands, so the login window never waits on a conversion. Nothing from Apple is staged or copied into the repository. The staged one-colour still.png is the fallback
#    when the stock picture is absent or sips cannot convert it.
# An absent choice is written, and our earlier ochre choice (the string "UniXNU Still" in its binary plist) or the current generated
# choice is replaced. Anything else is the user's and stays byte for byte. One line per account on the console (stderr-log=1):
# "still-wallpaper: uid 501: login picture replaced ours, store replaced ours" (or written / kept the user's).
# The installed system (the owner's Data, whose seed is the store's base and never restaged: docs/data-store.md section 9) gets the
# same script and files from the system layer instead (configs/userland.toml unit piano-still-wallpaper): the script as
# /usr/local/libexec/piano-still-wallpaper, its files in /usr/local/share/piano/wallpaper, run once a boot by its launchd job. The
# files are taken from the directory given as $1, else the first of those two places that has all three.
# Host tests may override the wallpaper database, stock picture and local-directory users path with PIANO_WALLPAPER_DB,
# PIANO_WALLPAPER_STOCK and PIANO_WALLPAPER_DSLOCAL; PIANO_WALLPAPER_NOCHOWN=1 leaves ownership alone on the fake root.
S=
for d in ${1:+"$1"} /private/var/piano/wallpaper /usr/local/share/piano/wallpaper; do
  [ -f "$d/still.png" ] && [ -f "$d/Metadata.plist" ] && [ -f "$d/Index.plist" ] && { S=$d; break; }
done
W=${PIANO_WALLPAPER_DB:-/private/var/db/Wallpapers}
STOCK=${PIANO_WALLPAPER_STOCK:-/System/Library/CoreServices/DefaultDesktop.heic}
DSLOCAL=${PIANO_WALLPAPER_DSLOCAL:-/private/var/db/dslocal/nodes/Default/users}
RO='-o root -g wheel'
[ -n "${PIANO_WALLPAPER_NOCHOWN:-}" ] && RO=
[ -n "$S" ] || { echo "still-wallpaper: no files in ${1:+$1, }/private/var/piano/wallpaper or /usr/local/share/piano/wallpaper"; exit 0; }
install -d $RO -m 755 "$W" || exit 0
# The picture has to be a file that already exists when this script exits: SecurityAgent reads the login window's picture seconds
# after the boot job's predecessor, and a foreground conversion of the 4096x2160 stock heic ran for minutes on this machine (sips
# still going minutes into the boot, 2026-11-28), so the choice was never in place and the login window played the default aerial
# wallpaper -- a video decoded in software, ~40 % of the CPU every boot. So: cached conversion if there is one, else the staged still
# immediately, and the stock conversion runs in the background to be cached for a later boot.
CACHE=${PIANO_WALLPAPER_CACHE:-/private/var/piano/wallpaper-stock.png}
rm -f "$W"/.piano-still-wallpaper.*.png "$W"/.piano-stock.*.png 2> /dev/null
I="$S/still.png"
if [ -s "$CACHE" ]; then
  I=$CACHE
  echo "still-wallpaper: picture from the stock conversion cached on an earlier boot"
elif [ -f "$STOCK" ]; then
  echo "still-wallpaper: staged still picture now, stock picture converting in the background for a later boot"
  ( T="$W/.piano-stock.$$.png"
    if sips -s format png --resampleHeight 2136 "$STOCK" --out "$T" >/dev/null 2>&1 && sips -c 2136 3200 "$T" >/dev/null 2>&1 &&
        [ -s "$T" ]; then
      install -d -o root -g wheel -m 755 "${CACHE%/*}" 2> /dev/null
      mv -f "$T" "$CACHE" 2> /dev/null || rm -f "$T"
    else
      rm -f "$T"
    fi ) &
fi
# The login window's picture of one GeneratedUID: absent and ours are written; a different existing choice is the user's.
put() {
  d="$W/$1"
  m="$d/Metadata.plist"
  p="$d/Wallpaper.png"
  state=written
  if [ -e "$m" ] || [ -e "$p" ]; then
    if [ -f "$m" ] && { grep -a -Fq 'UniXNU Still' "$m" || cmp -s "$m" "$S/Metadata.plist"; }; then
      state='replaced ours'
    else
      echo "kept the user's"
      return
    fi
  fi
  install -d $RO -m 755 "$d" && install $RO -m 644 "$I" "$p" &&
    install $RO -m 644 "$S/Metadata.plist" "$m" && echo "$state" || echo failed
}
r=$(put FFFFEEEE-DDDD-CCCC-BBBB-AAAA00000000); a=$(put FFFFEEEE-DDDD-CCCC-BBBB-AAAA0000005C)
echo "still-wallpaper: root and _securityagent: login picture $r, $a"
for f in "$DSLOCAL"/*.plist; do
  [ -f "$f" ] || continue
  u=$(plutil -extract uid.0 raw -o - "$f" 2>/dev/null) || continue
  [ "$u" -ge 501 ] 2>/dev/null || continue
  g=$(plutil -extract generateduid.0 raw -o - "$f" 2>/dev/null) || continue
  [ "${#g}" -eq 36 ] && printf '%s\n' "$g" | grep -Exq '[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}' || continue
  h=$(plutil -extract home.0 raw -o - "$f" 2>/dev/null) || h=
  login=$(put "$(printf '%s' "$g" | tr a-f A-F)")
  store=unavailable
  L="$h/Library/Application Support"
  if [ -n "$h" ] && [ -d "$h/Library" ]; then
    i="$L/com.apple.wallpaper/Store/Index.plist"
    if [ -e "$i" ]; then
      if [ -f "$i" ] && { grep -a -Fq 'UniXNU Still' "$i" || cmp -s "$i" "$S/Index.plist"; }; then
        store='replaced ours'
      else
        store="kept the user's"
      fi
    fi
    if [ "$store" != "kept the user's" ]; then
      [ "$store" = 'replaced ours' ] || store=written
      gid=$(plutil -extract gid.0 raw -o - "$f" 2>/dev/null) || gid=20   # the account's primary group (staff)
      UO="-o $u -g $gid"
      [ -n "${PIANO_WALLPAPER_NOCHOWN:-}" ] && UO=
      [ -d "$L" ] || install -d $UO -m 700 "$L"
      install -d $UO -m 755 "$L/com.apple.wallpaper" "$L/com.apple.wallpaper/Store" &&
        install $UO -m 644 "$S/Index.plist" "$i" || store=failed
    fi
  fi
  echo "still-wallpaper: uid $u: login picture $login, store $store"
done
exit 0
