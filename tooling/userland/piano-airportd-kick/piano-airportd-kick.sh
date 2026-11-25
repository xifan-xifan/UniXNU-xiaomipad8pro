#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Normal launchd/IOKit discovery gets the first opportunity to adopt the
# interface. Start a missing daemon or restart one that failed to adopt it.
# The CoreWLAN job starts only after the current PID has read the driver.
say() {
  /usr/bin/logger -t piano-airportd-kick "$*"
  echo "piano-airportd-kick: $(/bin/date +%T) $*" > /dev/console 2> /dev/null
  echo "piano-airportd-kick: $(/bin/date +%T) $*"
}
case "$(/usr/sbin/sysctl -n machdep.piano_wifi_apple80211 2> /dev/null)" in
[1-9]*) ;;
*) exit 0 ;;
esac
[ "$(/usr/sbin/sysctl -n machdep.piano_wifi_native_boot 2>/dev/null)" = 1 ] || exit 0
boot=$(/usr/sbin/sysctl -n kern.bootsessionuuid) || exit 1
[ -n "$boot" ] || exit 1
marker=/var/run/piano-airportd-kick
[ "$(/bin/cat "$marker" 2>/dev/null)" != "$boot" ] || exit 0
# the controller answers and its interface has a name (the BSD name on PianoWifi80211's interface child)
bsd=""
i=0
while [ $i -lt 600 ]; do
  tree=$(/usr/sbin/ioreg -l -r -c PianoWifi80211 -d 2 2> /dev/null)
  if echo "$tree" | /usr/bin/grep -q '"piano-wifi-apple80211-answering"'; then
    bsd=$(echo "$tree" | /usr/bin/sed -n 's/.*"BSD Name" = "\(en[0-9]*\)".*/\1/p' | /usr/bin/head -1)
    [ -n "$bsd" ] && break
  fi
  sleep 1
  i=$((i + 1))
done
[ -n "$bsd" ] || exit 0
airportd_pid() {
  /bin/launchctl print system/com.apple.airportd 2>/dev/null |
    /usr/bin/awk '$1 == "pid" && $2 == "=" && $3 ~ /^[1-9][0-9]*$/ { print $3; exit }'
}
wait_for_adoption() {
  n=0
  while [ "$n" -lt "$1" ]; do
    pid=$(airportd_pid)
    adopted=$(/usr/sbin/ioreg -l -r -c PianoWifi80211 -d 1 2>/dev/null |
      /usr/bin/awk '$1 == "\"piano-wifi-airportd-adopted\"" && $2 == "=" { print $3; exit }')
    if [ -n "$pid" ] && [ "$pid" = "$adopted" ]; then
      return 0
    fi
    sleep 1
    n=$((n + 1))
  done
  return 1
}
say "PianoWifi80211 answers and its interface is $bsd (after ${i} s)"
mode=observed
if ! wait_for_adoption 10; then
  pid=$(airportd_pid)
  if [ -n "$pid" ]; then
    mode=restarted
    say "PianoWifiAirportdKickV1 restart-needed pid=$pid"
    /bin/launchctl kickstart -k system/com.apple.airportd >/dev/null 2>&1 || exit 1
  else
    mode=started
    say "PianoWifiAirportdKickV1 start-needed"
    /bin/launchctl kickstart system/com.apple.airportd >/dev/null 2>&1 || exit 1
  fi
  if ! wait_for_adoption 30; then
    say "PianoWifiAirportdKickV1 adoption-timeout mode=$mode"
    exit 1
  fi
fi
receipt=$(/usr/bin/mktemp /var/run/piano-airportd-kick.XXXXXXXX) || exit 1
if ! printf '%s\n' "$boot" > "$receipt" || ! /bin/mv -f "$receipt" "$marker"; then
  /bin/rm -f "$receipt"
  exit 1
fi
say "PianoWifiAirportdKickV1 complete mode=$mode pid=$pid"
# On first provisioning, the staged job may still be getting registered.
# kickstart without -k will never interrupt an already running fallback.
i=0
while [ "$i" -lt 30 ]; do
  if /bin/launchctl kickstart system/com.unixnu.piano-wifi-join >/dev/null 2>&1; then
    say "PianoWifiAirportdKickV1 helper=started"
    exit 0
  fi
  sleep 1
  i=$((i + 1))
done
say "PianoWifiAirportdKickV1 helper=unavailable"
exit 1
