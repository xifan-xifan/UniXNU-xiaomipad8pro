#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# pool-device.sh: the tablet side of T-20. The 27.3 System volume (the OS window, 84581E99...) and the SystemOS volume (07AD53B3...)
# copied into two new role-less APFS volumes of the owner's Data container, so that `/`, its SystemOS union and Data share one
# container's free space. Runs as root on the running system (rsudo), the old entry booted, from a work folder the host filled
# (tooling/pool/pool-install.sh). bash 3.2; no python on the tablet.
#
#   pool-device.sh preflight|create|extract|verify  WORK os|systemos
#   pool-device.sh fstab|undo|status|live           WORK
#   pool-device.sh accept-live                      WORK SHA256
#
# WORK: pool-manifest (the binary), state.env, and one folder per volume, WORK/os and WORK/systemos, each with volume.aar,
# expected.manifest, archive.env, afsc-<family>.list (tooling/pool/make-volume-archive.sh). Every step reads and writes WORK/state.env
# (KEY=VALUE lines, the volume's keys prefixed os_ / systemos_); every step is idempotent and refuses on any mismatch:
#   preflight  root, 27.3, the tools, the volume's archive and manifest hashes and its source's UUID, the Data volume (its UUID) and its
#              container's free space, no volume of this name of unknown origin, the old entry booted (`/` on the old SystemOS), the
#              old window's raw hash = the archive's source only with POOL_CHECK_WINDOW=1 (off by default since T-20 v2: the window's raw
#              bytes change while macOS runs; R1a on the host and live/accept-live replace it)
#   live       (T-20 v2 R1b, PLAN-v2.md) the live old volumes through `/`: `pool-manifest --paths WORK/live-paths.txt /` (the host's
#              list, pool-live-check.py list) into WORK/live.txt, made readable for the user's ssh fetch (never through tsudo); refused when
#              `mount` now is not WORK/live-mounts.txt (the mounts the list was made from)
#   accept-live the host's verdict (pool-live-check.py compare EQUAL) for exactly that live.txt (its sha256); create refuses without it
#              (POOL_CHECK_WINDOW=1, plan A's raw window hash, is the other way, for a quiesced window only)
#   create     `diskutil apfs addVolume <Data container> APFS <name> -nomount` (no role, no group); its UUID into the state
#   extract    SF_RESTRICTED settable on the new volume (else refused unless POOL_ACCEPT_UNRESTRICTED=1); the archive extracted, one
#              pass per compression family by exact paths (aa's -include-path is a prefix match: prefixing paths by anchored regexes),
#              hard-link groups whole in one aa run, files aa would store in decmpfs types this kernel is not shown to read (7, 13)
#              stored uncompressed, directories' attributes last
#   verify     the volume mounted read-only, its manifest = expected.manifest (pool-manifest --diff, the relaxations recorded: always the
#              kernel-reserved firmlink markers SF_FIRMLINK / com.apple.fs.firmlink / com.apple.fs.altlink, inert here; SF_RESTRICTED only
#              with POOL_ACCEPT_UNRESTRICTED=1)
#   fstab      (both verified) /etc/fstab: the two copies and the two old windows "ro,noauto" for DiskArbitration; a tagged block, once
#   undo       only the volumes this tool created (UUID from the state, name and container checked) and the tagged fstab block;
#              refused while `/` runs on a copy
# POOL_DATA_UUID is the expected Data volume, supplied by the host.
# Test seams (tooling/pool/test-pool-device.sh, mock-tablet.sh): POOL_DISKUTIL, POOL_MOUNT, POOL_UMOUNT, POOL_AA, POOL_FSTAB,
# POOL_SWVERS, POOL_ID, POOL_CHFLAGS, POOL_PLUTIL, POOL_RAWDEV_PREFIX, POOL_TEST_CONTAINER (the host proof's stand-in container),
# POOL_TEST_ROOT_SYSTEMOS.
set -uo pipefail
STEP=${1:-}
WORK=${2:-}
VOL=${3:-}
DATA_UUID=${POOL_DATA_UUID:-}
OS_OLD=84581E99-85BE-4957-AC53-2947E887B3E4
SYSTEMOS_OLD=07AD53B3-7565-4004-B574-F45FAC3753FB
FORBID=7,13
DISKUTIL=${POOL_DISKUTIL:-/usr/sbin/diskutil}
MOUNT=${POOL_MOUNT:-/sbin/mount}
UMOUNT=${POOL_UMOUNT:-/sbin/umount}
AA=${POOL_AA:-/usr/bin/aa}
FSTAB=${POOL_FSTAB:-/etc/fstab}
SWVERS=${POOL_SWVERS:-/usr/bin/sw_vers}
IDCMD=${POOL_ID:-/usr/bin/id}
CHFLAGS=${POOL_CHFLAGS:-/usr/bin/chflags}
PLUTIL=${POOL_PLUTIL:-/usr/bin/plutil}
RAWDEV=${POOL_RAWDEV_PREFIX:-/dev/r}
TAG='# piano-pool T-20 (tooling/pool/pool-device.sh): the System and SystemOS copies and the old windows kept away from DiskArbitration'

die() { echo "POOL FAIL ($STEP${VOL:+ $VOL}): $*" >&2; echo "POOL FAIL ($STEP${VOL:+ $VOL}): $*" >> "$WORK/pool-device.log" 2>/dev/null; exit 1; }
log() { echo "pool ($STEP${VOL:+ $VOL}): $*"; echo "pool ($STEP${VOL:+ $VOL}): $*" >> "$WORK/pool-device.log" 2>/dev/null; }
usage() { echo 'usage: pool-device.sh preflight|create|extract|verify WORK os|systemos; pool-device.sh fstab|undo|status|live WORK; pool-device.sh accept-live WORK SHA256' >&2; exit 2; }
[ -n "$STEP" ] && [ -n "$WORK" ] && [ -d "$WORK" ] || usage
case "$WORK" in /*) ;; *) die "WORK must be an absolute path";; esac
case "$STEP" in
    preflight|create|extract|verify) case "$VOL" in os|systemos) ;; *) usage;; esac;;
    fstab|undo|status|live) [ -z "$VOL" ] || usage;;
    accept-live) [ -n "$VOL" ] || usage;;
    *) usage;;
esac
STATE=$WORK/state.env
MNT=$WORK/mnt
MANIFEST_TOOL=$WORK/pool-manifest
case "$VOL" in
    os) NAME=PianoOS; OLD_UUID=$OS_OLD; NEED_FREE=${POOL_NEED_FREE:-25769803776};;            # 24 GiB: the copy is ~15 GB
    systemos) NAME=PianoSystemOS; OLD_UUID=$SYSTEMOS_OLD; NEED_FREE=${POOL_NEED_FREE:-8589934592};;   # 8 GiB: ~3.6 GB
esac
V=$WORK/$VOL

get() { [ -f "$STATE" ] && sed -n "s/^$1=//p" "$STATE" | tail -1; }
put() {   # KEY VALUE: the state file rewritten through a new file (a cut leaves the old one)
    { [ -f "$STATE" ] && grep -v "^$1=" "$STATE"; printf '%s=%s\n' "$1" "$2"; } > "$STATE.new" && mv -f "$STATE.new" "$STATE" ||
        die "cannot record $1 in state.env"
}
vget() { get "${VOL}_$1"; }
vput() { put "${VOL}_$1" "$2"; }
plist() {   # TARGET KEY: one value of `diskutil info -plist TARGET`
    "$DISKUTIL" info -plist "$1" 2>/dev/null | "$PLUTIL" -extract "$2" raw -o - - 2>/dev/null
}
meta() { sed -n "s/^$1=//p" "$V/archive.env" | tail -1; }
sha_of() { /usr/bin/shasum -a 256 "$1" | awk '{print $1}'; }

# the container the new volumes go into: the Data volume's (or the host proof's stand-in)
data_container() {
    [[ "$DATA_UUID" =~ ^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$ ]] ||
        { echo 'set POOL_DATA_UUID to the expected Data volume UUID' >&2; return 1; }
    local uuid cont=${POOL_TEST_CONTAINER:-}
    if [ -z "$cont" ]; then
        uuid=$(plist /System/Volumes/Data VolumeUUID)
        [ "$uuid" = "$DATA_UUID" ] || { echo "the volume at /System/Volumes/Data is $uuid, not the owner's Data $DATA_UUID" >&2; return 1; }
        cont=$(plist /System/Volumes/Data APFSContainerReference) || return 1
    fi
    [[ "$cont" =~ ^disk[0-9]+$ ]] || { echo "invalid APFS container target: $cont" >&2; return 1; }
    echo "$cont"
}
# DISK UUID NAME lines of every volume of a container (diskutil apfs list CONTAINER, text)
volumes_of() {
    "$DISKUTIL" apfs list "$1" 2>/dev/null | awk '
        /APFS Volume Disk \(Role\):/ { if (d != "") print d, u, n; d = $5; u = ""; n = "" }
        /Volume UUID:/ { u = $NF }
        /Name:/ { sub(/^.*Name:[ ]*/, ""); sub(/[ ]*\(Case-.*$/, ""); n = $0 }
        END { if (d != "") print d, u, n }'
}
root_systemos() {   # the volume UUID `/` reports (the SystemOS union mount reports MNT_ROOTFS: baseline 0023)
    if [ -n "${POOL_TEST_ROOT_SYSTEMOS:-}" ]; then echo "$POOL_TEST_ROOT_SYSTEMOS"; return 0; fi
    plist / VolumeUUID
}
require_inactive_copies() {
    local now
    now=$(root_systemos) || die 'cannot identify the root volume'
    [[ "$now" =~ ^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$ ]] ||
        die 'cannot identify the root volume'
    if [ "$now" = "$(get os_volume_uuid)" ] || [ "$now" = "$(get systemos_volume_uuid)" ]; then
        die "$1"
    fi
}
unescape_list() {   # manifest paths (bytes outside 0x21-0x7e and the backslash as \xHH) -> raw paths, one per line
    local line
    while IFS= read -r line; do printf '%b\n' "$line"; done
}
format_of() {   # the archive's source personality as diskutil's format name
    case "$(meta source_personality)" in APFS) echo APFS;; Case-sensitive_APFS) echo APFSX;; *) return 1;; esac
}

check_archive() {
    local receipt file want got
    [ -s "$V/archive.env" ] || die "no $V/archive.env (the host stages it)"
    for receipt in archive_sha256:volume.aar manifest_file_sha256:expected.manifest; do
        file=${receipt#*:}
        want=$(meta "${receipt%:*}")
        [[ "$want" =~ ^[0-9a-f]{64}$ ]] && [ -s "$V/$file" ] &&
            got=$(sha_of "$V/$file") && [ "$got" = "$want" ] ||
            die "$file hashes to ${got:-unavailable}, the archive record says $want"
        got=''
    done
    local fam
    for fam in $(meta families); do [ -f "$V/afsc-$fam.list" ] || die "no afsc-$fam.list"; done
}

step_preflight() {
    local usage_output
    [ "$("$IDCMD" -u)" = 0 ] || die 'not root (run it through rsudo)'
    vput preflight no
    [ "$("$SWVERS" -productVersion)" = 27.3 ] || die "not macOS 27.3 ($("$SWVERS" -productVersion))"
    for t in "$AA" "$DISKUTIL" "$PLUTIL" "$MANIFEST_TOOL" /usr/bin/shasum; do [ -x "$t" ] || die "no $t"; done
    usage_output=$("$MANIFEST_TOOL" 2>&1)
    [[ "$usage_output" = 'usage: pool-manifest'* ]] || die 'pool-manifest does not run here'
    check_archive
    [ "$(meta source_volume_uuid)" = "$OLD_UUID" ] || die "the $VOL archive is of volume $(meta source_volume_uuid), not $OLD_UUID"
    format_of > /dev/null || die "the $VOL archive's personality $(meta source_personality) is not APFS"
    local cont free
    cont=$(data_container) || die 'the Data container is not the one the owner chain mounts'
    [ -z "$(get container)" ] || [ "$(get container)" = "$cont" ] || die "the state's container is $(get container), the Data volume's is $cont"
    free=$("$DISKUTIL" apfs list -plist "$cont" 2>/dev/null | "$PLUTIL" -extract Containers.0.CapacityFree raw -o - - 2>/dev/null)
    put container "$cont"
    log "the Data container: $cont (${free:-?} bytes free)"
    local mine others
    mine=$(vget volume_uuid)
    others=$(volumes_of "$cont" | awk -v n="$NAME" '$3 == n')
    if [ -n "$others" ]; then
        [ -n "$mine" ] && [ "$(echo "$others" | awk '{print $2}')" = "$mine" ] ||
            die "a $NAME volume of unknown origin in $cont: $others (not this tool's: refused)"
        log "the $NAME volume of this tool is there already ($mine): resumed"
    else
        [ -n "$free" ] && [ "$free" -ge "$NEED_FREE" ] || die "container $cont has ${free:-?} bytes free, the $VOL copy wants $NEED_FREE"
    fi
    local now
    now=$(root_systemos)
    put root_systemos_at_preflight "${now:-unknown}"
    [ "$now" = "$SYSTEMOS_OLD" ] || die "/ runs on ${now:-an unknown volume}, not the old SystemOS $SYSTEMOS_OLD: boot the old entry (macos-27.3)"
    # the archive's source must be what the tablet's old window holds (else the copy would undo whatever differs)
    vput window_check "$([ "${POOL_CHECK_WINDOW:-0}" = 1 ] && echo raw || echo live)"
    if [ "${POOL_CHECK_WINDOW:-0}" = 1 ]; then
        local whole wsha t0
        whole=$(plist "$OLD_UUID" APFSPhysicalStores.0.APFSPhysicalStore)
        [[ "$whole" =~ ^disk[0-9]+(s[0-9]+)?$ ]] || die "the old $VOL volume $OLD_UUID: invalid physical store"
        t0=$(date +%s)
        wsha=$(dd if="$RAWDEV$whole" bs=4194304 2>/dev/null | /usr/bin/shasum -a 256 | awk '{print $1}') || die "cannot read the old $VOL window $whole"
        vput window_sha256 "$wsha"
        vput window_read_seconds $(( $(date +%s) - t0 ))
        [ "$wsha" = "$(meta source_sha256)" ] ||
            die "the old $VOL window $whole hashes to $wsha, the archive was made from $(meta source_sha256): make the archive from this window"
        log "the old $VOL window $whole = the archive's source ($wsha, read in $(vget window_read_seconds) s)"
    fi
    vput preflight ok
    log PASS
}

step_live() {
    local list=$WORK/live-paths.txt now rc t0
    [ "$("$IDCMD" -u)" = 0 ] || die 'not root (run it through rsudo)'
    [ -s "$list" ] && [ -s "$WORK/live-mounts.txt" ] || die 'no live-paths.txt / live-mounts.txt (the host pushes them)'
    [ -x "$MANIFEST_TOOL" ] || die "no $MANIFEST_TOOL"
    now=$("$MOUNT")
    [ "$now" = "$(cat "$WORK/live-mounts.txt")" ] || die 'the mounts now are not the ones the list was made from: make the list again'
    put live_sha256 ''
    put live_ok ''
    t0=$(date +%s)
    "$MANIFEST_TOOL" --paths "$list" "${POOL_LIVE_ROOT:-/}" > "$WORK/live.txt.new" 2> "$WORK/live.err"
    rc=$?
    mv -f "$WORK/live.txt.new" "$WORK/live.txt"
    chmod 644 "$WORK/live.txt" "$WORK/live.err"
    [ -n "${SUDO_UID:-}" ] && chown "$SUDO_UID" "$WORK/live.txt" "$WORK/live.err" 2>/dev/null
    put live_seconds $(( $(date +%s) - t0 ))
    [ "$rc" = 0 ] || die "pool-manifest --paths: rc $rc ($(head -3 "$WORK/live.err"))"
    put live_sha256 "$(sha_of "$WORK/live.txt")"
    log "live view of $(wc -l < "$list" | tr -d ' ') paths in $(get live_seconds) s: $(tail -1 "$WORK/live.txt")"
}

step_accept_live() {
    local want=$VOL
    [ -n "$(get live_sha256)" ] && [ -f "$WORK/live.txt" ] || die 'no live run'
    [ "$(sha_of "$WORK/live.txt")" = "$(get live_sha256)" ] || die 'live.txt is not the one the live step wrote'
    [ "$want" = "$(get live_sha256)" ] || die "the verdict is for $want, the live output here is $(get live_sha256)"
    put live_ok "$want"
    log "R1b EQUAL accepted for live.txt $want"
}

step_create() {
    [ "$(vget preflight)" = ok ] || die 'preflight has not passed'
    if [ "$(vget window_check)" != raw ]; then
        [ -n "$(get live_ok)" ] && [ "$(get live_ok)" = "$(get live_sha256)" ] || die 'R1b: no accepted live comparison (live, then the host verdict, then accept-live)'
    fi
    local cont mine out disk uuid name fmt volumes
    cont=$(data_container) || die 'the Data container is not the one the owner chain mounts'
    [ "$cont" = "$(get container)" ] || die 'the Data container changed since preflight'
    mine=$(vget volume_uuid)
    if [ -n "$mine" ]; then
        disk=$(mine_disk) || exit 1
        log "already created: $disk ($mine)"
        return 0
    fi
    volumes=$(volumes_of "$cont") || die "cannot list the Data container $cont"
    [ -z "$(printf '%s\n' "$volumes" | awk -v n="$NAME" '$3 == n')" ] ||
        die "a $NAME volume of unknown origin in $cont: refused"
    fmt=$(format_of) || die 'the archive does not declare an APFS personality'
    out=$("$DISKUTIL" apfs addVolume "$cont" "$fmt" "$NAME" -nomount 2>&1) || die "addVolume: $out"
    echo "$out" >> "$WORK/pool-device.log"
    disk=$(echo "$out" | sed -n 's/^Created new APFS Volume \(disk[0-9]*s[0-9]*\).*/\1/p' | tail -1)
    [ -n "$disk" ] || die "addVolume printed no new volume: $out"
    uuid=$(plist "$disk" VolumeUUID)
    name=$(plist "$disk" VolumeName)
    [[ "$uuid" =~ ^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$ ]] &&
        [ "$name" = "$NAME" ] && [ "$(plist "$disk" APFSContainerReference)" = "$cont" ] ||
        die "the new $disk: UUID ${uuid:-?}, name ${name:-?}, not a $NAME volume of $cont"
    case "$uuid" in "$OS_OLD"|"$SYSTEMOS_OLD"|"$DATA_UUID") die "the new volume has the UUID of an existing system volume ($uuid)";; esac
    vput volume_uuid "$uuid"
    vput volume_disk "$disk"
    log "created $disk ($uuid), $fmt, no role, in $cont"
}

extract_list() {   # ALGO LIST: exactly the archive paths of LIST (raw, one per line) extracted again with -afsc ALGO
    local algo=$1 list=$2 rx batch
    # a hard-link group goes whole (one member re-extracted alone would get a new inode and leave the others on the old one)
    awk -F'\t' 'FILENAME == ARGV[1] { if ($1 != "") { m[$1] = m[$1] "\n" $2; r[$2] = $1; r[$1] = $1 }; next }
        { if (!($0 in r)) { print; next } g = r[$0]; if (g in done) next; done[g] = 1; print g
          n = split(substr(m[g], 2), x, "\n"); for (i = 1; i <= n; i++) print x[i] }' "$WORK/x.hl" "$list" |
        LC_ALL=C sort -u > "$WORK/x.sorted"
    LC_ALL=C comm -12 "$WORK/x.prefixes" "$WORK/x.sorted" > "$WORK/x.roots0"
    # a group with one member in the anchored set goes there whole, and into one aa run (two runs would link a member to the other
    # run's old inode): GROUP<TAB>PATH lines, a group's members adjacent
    awk -F'\t' 'FILENAME == ARGV[1] { if ($1 != "") { m[$1] = m[$1] "\n" $2; r[$2] = $1; r[$1] = $1 }; next }
        { if (!($0 in r)) { print $0 "\t" $0; next } g = r[$0]; if (g in done) next; done[g] = 1; print g "\t" g
          n = split(substr(m[g], 2), x, "\n"); for (i = 1; i <= n; i++) print g "\t" x[i] }' "$WORK/x.hl" "$WORK/x.roots0" |
        LC_ALL=C sort -u > "$WORK/x.roots.g"
    cut -f2 "$WORK/x.roots.g" | LC_ALL=C sort -u > "$WORK/x.roots"
    LC_ALL=C comm -23 "$WORK/x.sorted" "$WORK/x.roots" > "$WORK/x.exact"
    if [ -s "$WORK/x.exact" ]; then
        "$AA" extract -d "$MNT" -i "$V/volume.aar" -afsc "$algo" -include-path-list "$WORK/x.exact" || die "aa extract ($algo, list)"
    fi
    local g last='' runs=0
    if [ -s "$WORK/x.roots" ]; then
        grep -q '\^' "$WORK/x.roots" && die "a prefix path with ^ in the $algo list: no regex for it here"
        awk -F'\t' '{ p = $2; gsub(/[^A-Za-z0-9\/]/, "[&]", p); print $1 "\t^" p "$" }' "$WORK/x.roots.g" > "$WORK/x.rx"
        # aa takes a few hundred -include-regex rules per run (800 fail: "archive stream write error"): runs of ~400, cut between groups
        batch=()
        while IFS="$(printf '\t')" read -r g rx; do
            if [ ${#batch[@]} -ge 800 ] && [ "$g" != "$last" ]; then
                "$AA" extract -d "$MNT" -i "$V/volume.aar" -afsc "$algo" "${batch[@]}" || die "aa extract ($algo, anchored)"
                batch=(); runs=$((runs + 1))
            fi
            batch[${#batch[@]}]=-include-regex; batch[${#batch[@]}]=$rx; last=$g
        done < "$WORK/x.rx"
        if [ ${#batch[@]} -gt 0 ]; then
            "$AA" extract -d "$MNT" -i "$V/volume.aar" -afsc "$algo" "${batch[@]}" || die "aa extract ($algo, anchored)"
            runs=$((runs + 1))
        fi
    fi
    log "$algo: $(wc -l < "$WORK/x.exact" | tr -d ' ') paths by list, $(wc -l < "$WORK/x.roots" | tr -d ' ') anchored in $runs run(s)"
}

mount_rw() {
    mkdir -p "$MNT"
    "$MOUNT" -t apfs -o nobrowse "/dev/$1" "$MNT" || die "mount /dev/$1 at $MNT"
}
unmount() { "$UMOUNT" "$MNT" 2>/dev/null || "$UMOUNT" -f "$MNT" 2>/dev/null; }
mine_disk() {   # the device of this tool's volume of $VOL, checked by UUID, name and container
    local uuid disk cont
    uuid=$(vget volume_uuid)
    [[ "$uuid" =~ ^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$ ]] || die "no valid $VOL volume UUID recorded"
    cont=$(data_container) || die 'the Data container is not the one the owner chain mounts'
    [ "$cont" = "$(get container)" ] || die 'the Data container changed since preflight'
    disk=$(plist "$uuid" DeviceIdentifier)
    [[ "$disk" =~ ^disk[0-9]+s[0-9]+$ ]] && [ "$(plist "$uuid" VolumeUUID)" = "$uuid" ] &&
        [ "$(plist "$uuid" VolumeName)" = "$NAME" ] && [ "$(plist "$uuid" APFSContainerReference)" = "$cont" ] ||
        die "$uuid is not this tool's $NAME of $(get container)"
    echo "$disk"
}

step_extract() {
    local disk fam t0 n
    disk=$(mine_disk) || exit 1
    require_inactive_copies '/ runs on a copy (the pool entry): never written while booted'
    vput verified no
    vput extracted no
    check_archive
    mount_rw "$disk"
    trap unmount EXIT
    # SF_RESTRICTED (0x80000) is on most system entries; root sets it only where SIP lets it. Probed on the target.
    local probe=$MNT/.pool-restricted-probe flags
    rm -f "$probe"; : > "$probe" || die "cannot write $MNT"
    if "$CHFLAGS" restricted "$probe" 2>/dev/null && flags=$(stat -f %Xf "$probe") && [ $((0x$flags & 0x80000)) != 0 ]; then
        vput restricted_settable yes
        log 'SF_RESTRICTED can be set on the new volume'
    else
        vput restricted_settable no
        if [ "${POOL_ACCEPT_UNRESTRICTED:-0}" != 1 ]; then
            "$CHFLAGS" norestricted "$probe" 2>/dev/null; rm -f "$probe"
            die 'SF_RESTRICTED cannot be set here (SIP): the copy would lose it; POOL_ACCEPT_UNRESTRICTED=1 (an owner/analysis decision) relaxes verify'
        fi
        log 'SF_RESTRICTED cannot be set here: accepted (POOL_ACCEPT_UNRESTRICTED=1); verify ignores the flag and com.apple.rootless'
    fi
    "$CHFLAGS" norestricted "$probe" 2>/dev/null; rm -f "$probe"
    [ ! -e "$probe" ] || die "the probe file stays in $MNT"
    t0=$(date +%s)
    "$AA" extract -d "$MNT" -i "$V/volume.aar" -afsc lzvn || die 'aa extract (pass A, lzvn)'
    # aa's -include-path is a prefix match ("Localizable.strings" also takes "Localizable.stringsdict"): every archive path that
    # prefixes another one (a contiguous block after it in byte order) goes through an anchored -include-regex instead
    grep -q '\\x0[9a]' "$V/expected.manifest" && die 'a path with a tab or a newline in the manifest: not handled'
    awk -F'\t' 'NF == 14 && $1 != "." { print $1 }' "$V/expected.manifest" | unescape_list | LC_ALL=C sort > "$WORK/x.paths"
    awk 'NR > 1 && index($0, prev) == 1 && $0 != prev { print prev } { prev = $0 }' "$WORK/x.paths" | LC_ALL=C sort -u > "$WORK/x.prefixes"
    { awk -F'\t' 'NF == 14 && $11 != "-" { print $11 "\t" $1 }' "$V/expected.manifest" | unescape_list; printf '\t\n'; } > "$WORK/x.hl"
    for fam in $(meta families); do
        [ "$fam" = lzvn ] && continue
        extract_list "$fam" "$V/afsc-$fam.list"
    done
    # files aa put inline in their decmpfs xattr (types 7/13; the source keeps them in the resource fork, 8/14, the types this kernel
    # reads today) are stored uncompressed instead
    "$MANIFEST_TOOL" --exclude .fseventsd "$MNT" > "$WORK/x.interim" 2> "$WORK/x.interim.err" || die 'interim manifest'
    awk -F'\t' -v forbid=",$FORBID," 'NF == 14 && $2 == "f" && $14 != "-" { split($14, c, ":"); if (index(forbid, "," c[1] ",")) print $1 }' \
        "$WORK/x.interim" | unescape_list > "$WORK/$VOL-inline.list"
    n=$(wc -l < "$WORK/$VOL-inline.list" | tr -d ' ')
    [ "$n" = 0 ] || extract_list none "$WORK/$VOL-inline.list"
    vput inline_uncompressed "$n"
    "$AA" extract -d "$MNT" -i "$V/volume.aar" -include-type d -exclude-field acl || die 'aa extract (directory attributes)'
    # entries with SF_FIRMLINK (0x800000): APFS's own volume-group firmlink markers (with the com.apple.fs.firmlink xattr; the cryptex
    # mount point has com.apple.fs.altlink). The kernel keeps those to itself (EPERM from userland), so aa's one attribute call fails
    # and sets none of the entry's attributes: mode, mtime and the other flags are set here; the bit and the two xattrs stay a
    # recorded relaxation of verify. Inert here: k27 makes its own firmlinks (kern_exec.c native_owner_firmlinks_prepare,
    # vnode_setasfirmlink on the SystemOS union's directories), with no reader of SF_FIRMLINK or these xattrs.
    awk -F'\t' 'NF == 14 { f = $6; n = length(f); if (n >= 6 && index("89abcdef", substr(f, n - 5, 1))) print $1 "\t" $3 "\t" $6 "\t" $8 }' \
        "$V/expected.manifest" > "$WORK/x.firmlink"
    local path mode flags mtime fixed=0
    while IFS="$(printf '\t')" read -r path mode flags mtime; do
        path=$(printf '%b' "$path")
        [ "$path" = . ] && path=''
        chmod "$mode" "$MNT/$path" || die "chmod $mode $path"
        touch -h -d "$(date -u -r "${mtime%%.*}" +%Y-%m-%dT%H:%M:%S).${mtime#*.}Z" "$MNT/$path" || die "touch $path"
        "$CHFLAGS" -h "$(printf '%o' $((0x$flags & ~0x800000)))" "$MNT/$path" 2>/dev/null || [ "$(vget restricted_settable)" = no ] || die "chflags $path"
        fixed=$((fixed + 1))
    done < "$WORK/x.firmlink"
    vput firmlink_markers "$fixed"
    sync
    unmount || die "cannot unmount $MNT after extraction"
    trap - EXIT
    vput extracted yes
    vput extract_seconds $(( $(date +%s) - t0 ))
    log "extracted in $(vget extract_seconds) s; $n inline-compressed files stored uncompressed"
}

step_verify() {
    local disk relax out rc got
    [ "$(vget extracted)" = yes ] || die 'not extracted'
    vput verified no
    disk=$(mine_disk) || exit 1
    mkdir -p "$MNT"
    "$MOUNT" -t apfs -o rdonly,nobrowse "/dev/$disk" "$MNT" || die "mount -r /dev/$disk"
    trap unmount EXIT
    "$MANIFEST_TOOL" --exclude .fseventsd "$MNT" > "$WORK/$VOL-copy.manifest" 2> "$WORK/$VOL-copy.manifest.err"
    rc=$?
    unmount || die "cannot unmount $MNT after verification"
    trap - EXIT
    [ "$rc" = 0 ] || die "the copy's manifest: rc $rc ($(head -3 "$WORK/$VOL-copy.manifest.err"))"
    # always: the firmlink markers (SF_FIRMLINK, com.apple.fs.firmlink, com.apple.fs.altlink), kernel-reserved (step_extract);
    # where SIP keeps SF_RESTRICTED (and com.apple.rootless) from root: those too
    local mask=800000
    relax='--ignore-xattr com.apple.fs.firmlink --ignore-xattr com.apple.fs.altlink'
    if [ "$(vget restricted_settable)" = no ]; then mask=880000; relax="$relax --ignore-xattr com.apple.rootless"; fi
    relax="$relax --ignore-flags $mask"
    # shellcheck disable=SC2086
    out=$("$MANIFEST_TOOL" --diff "$V/expected.manifest" "$WORK/$VOL-copy.manifest" --allow-decompressed --forbid-types "$FORBID" $relax)
    rc=$?
    echo "$out" > "$WORK/$VOL-verify.txt"
    [ "$rc" = 0 ] || die "the copy is not the archive's volume: $(echo "$out" | grep -E '^(EQUAL|DIFFERENT)')"
    got=$(echo "$out" | sed -n 's/.*allow-decompressed on (\([0-9]*\) files stored uncompressed).*/\1/p')
    [ "$got" = "$(vget inline_uncompressed)" ] || die "verify counts $got files stored uncompressed, extract stored $(vget inline_uncompressed)"
    vput copy_manifest_sha256 "$(sha_of "$WORK/$VOL-copy.manifest")"
    vput verified yes
    log "PASS: $(echo "$out" | grep -E '^EQUAL')"
}

fstab_lines() { printf 'UUID=%s none apfs ro,noauto\n' "$(get os_volume_uuid)" "$(get systemos_volume_uuid)" "$OS_OLD" "$SYSTEMOS_OLD"; }

step_fstab() {
    local tmp u
    [ "$(get os_verified)" = yes ] && [ "$(get systemos_verified)" = yes ] || die 'both copies verified first'
    if [ -f "$FSTAB" ] && grep -qF "$TAG" "$FSTAB"; then
        fstab_lines | while IFS= read -r u; do grep -qxF "$u" "$FSTAB" || exit 1; done || die "$FSTAB has the tag but not every line"
        put fstab yes
        log 'fstab block there already'
        return 0
    fi
    if [ -f "$FSTAB" ]; then
        grep -Eq "^UUID=($(get os_volume_uuid)|$(get systemos_volume_uuid)|$OS_OLD|$SYSTEMOS_OLD)[[:space:]]" "$FSTAB" &&
            die "$FSTAB names a system volume already, outside this tool's block"
        [ -f "$WORK/fstab.before" ] || cp -p "$FSTAB" "$WORK/fstab.before"
        put fstab_existed yes
    else
        put fstab_existed no
    fi
    tmp=$FSTAB.pool-new
    { if [ -f "$FSTAB" ]; then cat "$FSTAB" || die "cannot read $FSTAB"; fi; echo "$TAG" && fstab_lines; } > "$tmp" &&
        chmod 644 "$tmp" && mv -f "$tmp" "$FSTAB" || die "cannot install $FSTAB"
    put fstab yes
    log 'fstab: the two copies and the two old windows ro,noauto'
}

step_undo() {
    local v uuid disk cont out name current
    cont=$(get container)
    require_inactive_copies '/ runs on a copy (the pool entry is booted): boot macos-27.3 first'
    if [ -f "$FSTAB" ] && grep -qF "$TAG" "$FSTAB"; then
        fstab_lines > "$WORK/x.fstab-lines"
        echo "$TAG" >> "$WORK/x.fstab-lines"
        awk 'FILENAME == ARGV[1] { drop[$0] = 1; next } !($0 in drop)' "$WORK/x.fstab-lines" "$FSTAB" > "$FSTAB.pool-new"
        if [ "$(get fstab_existed)" = no ] && [ ! -s "$FSTAB.pool-new" ]; then rm -f "$FSTAB.pool-new" "$FSTAB"
        else chmod 644 "$FSTAB.pool-new"; mv -f "$FSTAB.pool-new" "$FSTAB"; fi
        put fstab no
        log 'fstab block removed'
    fi
    for v in systemos os; do
        uuid=$(get "${v}_volume_uuid")
        case "$v" in os) name=PianoOS;; systemos) name=PianoSystemOS;; esac
        [ -n "$uuid" ] || { log "no $v volume of this tool"; continue; }
        [[ "$uuid" =~ ^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$ ]] || die "invalid $v volume UUID: $uuid"
        current=$(data_container) || die 'the Data container is not the one the owner chain mounts'
        [ "$current" = "$cont" ] || die 'the Data container changed since preflight'
        disk=$(plist "$uuid" DeviceIdentifier)
        if [ -z "$disk" ]; then log "the $v volume $uuid is gone already"; put "${v}_volume_uuid" ''; continue; fi
        [[ "$cont" =~ ^disk[0-9]+$ && "$disk" =~ ^disk[0-9]+s[0-9]+$ ]] &&
            [ "$(plist "$uuid" VolumeUUID)" = "$uuid" ] && [ "$(plist "$uuid" VolumeName)" = "$name" ] &&
            [ "$(plist "$uuid" APFSContainerReference)" = "$cont" ] ||
            die "$uuid is not a $name volume of $cont: not deleted"
        unmount
        out=$("$DISKUTIL" apfs deleteVolume "$disk" 2>&1) || die "deleteVolume $disk: $out"
        echo "$out" >> "$WORK/pool-device.log"
        put "${v}_volume_uuid" ''
        put "${v}_verified" no
        log "deleted the $v copy $disk ($uuid)"
    done
    put undone yes
}

case "$STEP" in
    preflight) step_preflight;;
    create) step_create;;
    extract) step_extract;;
    verify) step_verify;;
    fstab) step_fstab;;
    live) step_live;;
    accept-live) step_accept_live;;
    undo) step_undo;;
    status) cat "$STATE" 2>/dev/null || echo 'no state';;
esac
