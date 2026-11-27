#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# A mock tablet for tooling/pool's host tests: DIR/bin/{diskutil,mount,umount,chflags,id,sw_vers,df} as pool-device.sh and
# pool-install.sh call them. State in $MOCKD: a fixture Data volume in container disk3; the old windows' volumes, OS
# 84581E99... on disk4 and SystemOS 07AD53B3... on disk5 (their raw bytes: $MOCKD/raw-disk4, raw-disk5); a created volume NAME in
# vol-NAME.uuid / vol-NAME.disk with its files in vols/DISK (moved to the mount point while "mounted"); `/`'s volume UUID from root.uuid,
# else $MOCK_ROOT_UUID.
#
#   mock-tablet.sh DIR
set -eu
M=$1
mkdir -p "$M/bin" "$M/vols"
cat > "$M/bin/diskutil" <<'EOF'
#!/bin/bash
M=$MOCKD
DATA_UUID=${MOCK_DATA_UUID:-DA7A0000-0000-4000-8000-000000000001}
OS_OLD=84581E99-85BE-4957-AC53-2947E887B3E4
SYS_OLD=07AD53B3-7565-4004-B574-F45FAC3753FB
kv() { printf '<key>%s</key><string>%s</string>' "$1" "$2"; }
pl() { printf '<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict>%s</dict></plist>\n' "$1"; }
created() { for f in "$M"/vol-*.uuid; do [ -f "$f" ] || continue; n=${f#$M/vol-}; n=${n%.uuid}; echo "$n $(cat "$f") $(cat "$M/vol-$n.disk")"; done; }
lookup() { created | awk -v t="${1#/dev/}" '$2 == t || $3 == t'; }
root=$(cat "$M/root.uuid" 2>/dev/null || echo "$MOCK_ROOT_UUID")
case "$1 $2" in
  "info -plist")
    case "$3" in
      /System/Volumes/Data) pl "$(kv VolumeUUID "$DATA_UUID")$(kv APFSContainerReference disk3)";;
      /) pl "$(kv VolumeUUID "$root")";;
      "$SYS_OLD"|disk5s1|/dev/disk5s1) pl "$(kv VolumeUUID $SYS_OLD)$(kv DeviceIdentifier disk5s1)$(kv VolumeName SystemOS)<key>APFSPhysicalStores</key><array><dict>$(kv APFSPhysicalStore disk5)</dict></array>";;
      "$OS_OLD"|disk4s1|/dev/disk4s1) pl "$(kv VolumeUUID $OS_OLD)$(kv DeviceIdentifier disk4s1)$(kv VolumeName 'Macintosh HD')<key>APFSPhysicalStores</key><array><dict>$(kv APFSPhysicalStore disk4)</dict></array>";;
      *) set -- $(lookup "$3"); [ $# = 3 ] || exit 1
         pl "$(kv VolumeUUID "$2")$(kv DeviceIdentifier "$3")$(kv VolumeName "${MOCK_NEWNAME:-$1}")$(kv APFSContainerReference disk3)";;
    esac;;
  "info $SYS_OLD"|"info $OS_OLD")
    if [ "$root" = "$SYS_OLD" ]; then echo '   Mounted:                   Yes'; else echo '   Mounted:                   No'; fi;;
  "apfs list")
    if [ "$3" = -plist ]; then pl "<key>Containers</key><array><dict><key>CapacityFree</key><integer>${MOCK_FREE:-200000000000}</integer></dict></array>"
    else
      echo "+-- Container disk3"
      echo "    +-> Volume disk3s1"; echo "        APFS Volume Disk (Role):   disk3s1 (Data)"; echo "        Name:                      Data (Case-insensitive)"
      echo "        Volume UUID:               $DATA_UUID"
      created | while read -r n u d; do echo "    +-> Volume $d"; echo "        APFS Volume Disk (Role):   $d (No specific role)"
        echo "        Name:                      $n (Case-insensitive)"; echo "        Volume UUID:               $u"; done
      if [ -n "${MOCK_FOREIGN:-}" ]; then echo "    +-> Volume disk3s8"; echo "        APFS Volume Disk (Role):   disk3s8 (No specific role)"
        echo "        Name:                      $MOCK_FOREIGN (Case-insensitive)"; echo "        Volume UUID:               11111111-2222-3333-4444-555555555555"; fi
    fi;;
  "apfs addVolume")
    [ "$3" = disk3 ] && [ "$6" = -nomount ] && [ $# = 6 ] || { echo "mock: unexpected addVolume $*" >&2; exit 1; }
    case "$5" in PianoSystemOS) d=disk3s9; u=9A0B0C0D-1111-2222-3333-444455556666;; PianoOS) d=disk3s10; u=5E6F7A8B-1111-2222-3333-444455556666;;
      *) echo "mock: unexpected name $5" >&2; exit 1;; esac
    echo "$*" >> "$M/calls"
    echo "$u" > "$M/vol-$5.uuid"; echo "$d" > "$M/vol-$5.disk"; mkdir -p "$M/vols/$d"
    echo "Created new APFS Volume $d";;
  "apfs deleteVolume")
    set -- $(created | awk -v t="$3" '$3 == t'); [ $# = 3 ] || { echo "mock: no volume $*" >&2; exit 1; }
    echo "apfs deleteVolume $3" >> "$M/calls"; rm -f "$M/vol-$1.uuid" "$M/vol-$1.disk"; chmod -R u+w "$M/vols/$3" 2>/dev/null; rm -rf "$M/vols/$3";;
  *) echo "mock diskutil: $*" >&2; exit 1;;
esac
EOF
cat > "$M/bin/mount" <<'EOF'
#!/bin/bash
if [ $# = 0 ]; then cat "$MOCKD/mounts.txt" 2>/dev/null || printf '/dev/disk4s1 on / (apfs, local, read-only, journaled)\n/dev/disk5s1 on / (apfs, local, read-only, union)\n'; exit 0; fi
echo "mount $*" >> "$MOCKD/calls"
dev=${*: -2:1}; mnt=${*: -1}; d=${dev#/dev/}
[ -d "$MOCKD/vols/$d" ] || { echo "mock mount: no volume $d" >&2; exit 1; }
[ ! -s "$MOCKD/mounted" ] || { echo "mock mount: $(cat "$MOCKD/mounted") is mounted" >&2; exit 1; }
rmdir "$mnt" 2>/dev/null || { echo "mock mount: $mnt is not an empty directory" >&2; exit 1; }
mv "$MOCKD/vols/$d" "$mnt" && printf '%s %s\n' "$d" "$mnt" > "$MOCKD/mounted"
EOF
cat > "$M/bin/umount" <<'EOF'
#!/bin/bash
echo "umount $*" >> "$MOCKD/calls"
[ -s "$MOCKD/mounted" ] || exit 1
read -r d mnt < "$MOCKD/mounted"
mv "$mnt" "$MOCKD/vols/$d" && mkdir "$mnt" && : > "$MOCKD/mounted"
EOF
cat > "$M/bin/df" <<'EOF'
#!/bin/bash
# df -k PATH: / reports its SystemOS's container: the Data container once / runs on a copy, else the 6 GiB window
root=$(cat "$MOCKD/root.uuid" 2>/dev/null || echo "$MOCK_ROOT_UUID")
echo 'Filesystem 1024-blocks Used Available Capacity Mounted on'
case "${*: -1}" in
  /) if [ "$root" = 07AD53B3-7565-4004-B574-F45FAC3753FB ]; then echo "/dev/disk5s1 6291456 3538724 2538724 59% /"
     else echo "/dev/disk3s9 209715200 30000000 ${MOCK_AVAIL_KIB:-190000000} 14% /"; fi;;
  *) echo "/dev/disk3s1 209715200 30000000 ${MOCK_AVAIL_KIB:-190000000} 14% /System/Volumes/Data";;
esac
EOF
printf '#!/bin/sh\necho "chflags $*" >> "$MOCKD/calls"\n[ -n "$MOCK_CHFLAGS_OK" ] || exit 1\n' > "$M/bin/chflags"
printf '#!/bin/sh\necho "${MOCK_UID:-0}"\n' > "$M/bin/id"
printf '#!/bin/sh\necho "${MOCK_VERSION:-27.3}"\n' > "$M/bin/sw_vers"
chmod 755 "$M/bin/"*
