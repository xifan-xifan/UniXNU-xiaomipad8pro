#!/bin/sh
# Tablet side of tooling/persist/maint-session.py, run in the Ubuntu release's maintenance RAM image (BusyBox sh, dd, sha256sum,
# wget, nc, od, blkdiscard; Ubuntu's sgdisk, partx, blockdev). The host downloads it to /run/pp/tablet.sh (sha256 checked) and calls
# one subcommand per telnet line. Every subcommand prints key=value lines; nothing writes unless its name says so (copy, write,
# restore, discard, zero, job).
# Devices are found from sysfs every time (LUN 0 = the disk on UFS target 0, partitions by PARTNAME), never assumed.
#   map                                   LUN 0 disk, block size, size; boot_a/boot_b/vendor_boot_a/init_boot_a; linux_rootfs/esp
#   hash DEV SKIP COUNT [BS]              sha256 of COUNT blocks of BS (default 4096) bytes at block SKIP, page cache dropped first
#   zero-sha COUNT [BS]                   sha256 of COUNT zero blocks (to tell an all-zero region)
#   peek DEV LBA OFFSET LENGTH            hex of LENGTH bytes at OFFSET in 4096-byte block LBA
#   send DEV SKIP COUNT BS PORT           stream the blocks to the host's TCP PORT (maint-session.py receives and checks);
#                                         fails when the read (dd) or nc fails, both statuses reported (the shell has no pipefail)
#   send-list FILE DEV PORT               FILE's runs streamed as one stream; the same failure rule
#   fetch NAME SHA256 PORT                wget http://HOST:PORT/NAME to /run/pp/NAME and check its sha256
#   file-sha FILE                         sha256 of a file in /run/pp
#   copy DEV SKIP SEEK COUNT              WRITE: dd within DEV, 4096-byte blocks, forward (SEEK < SKIP is safe), conv=notrunc,fsync
#   write FILE DEV SEEK BS                WRITE: dd FILE onto DEV at block SEEK of BS bytes, conv=notrunc,fsync
#   restore NAME DEV SEEK BS PORT         WRITE: stream http://HOST:PORT/NAME (a verified backup) onto DEV at block SEEK;
#                                         fails when wget fails (a short body fails wget through the server's Content-Length)
#   restore-list FILE DEV PORT NAME       WRITE: FILE's runs from the host's NAME@K, at their DST blocks; the same failure rule
#   job NAME SUBCOMMAND...                run a subcommand detached (setsid): /run/pp/NAME.{log,rc}; survives the telnet session
#   job-status NAME                       rc=<exit status> when done, else running=1
#   drop-caches                           sync; echo 3 > /proc/sys/vm/drop_caches (read-backs then come from the medium)
#   rereadpt DEV                          blockdev --rereadpt, else partx -u
#   discard-info NODE                     whether the disk of partition NODE takes discards and which blkdiscard there is
#   discard NODE NAME START SIZE          WRITE: blkdiscard the whole partition NODE, only when NODE is the LUN 0 partition named NAME
#                                         at START with SIZE (512-byte units, sysfs; blockdev too), never a whole disk
#   zero NODE NAME START SIZE             WRITE: zeros over the whole partition NODE (dd from /dev/zero, conv=fsync), the same guard
set -u
export PATH=${PIANO_PATH:-/usr/sbin:/usr/bin:/sbin:/bin}
P=${PIANO_PP_DIR:-/run/pp}   # (PIANO_PP_DIR, PIANO_DEV_DIR, PIANO_PATH and PIANO_HOST_IP only for a host-side test of this script)
DEV=${PIANO_DEV_DIR:-/dev}   # discard and zero only
mkdir -p "$P"
host_ip() { [ -n "${PIANO_HOST_IP:-}" ] || . /etc/piano-network; echo "$PIANO_HOST_IP"; }
die() { echo "error=$*"; exit 1; }
lun0() {
	for b in /sys/block/sd*; do
		[ -e "$b" ] || continue
		case "$(readlink -f "$b/device")" in *1d84000*:0) echo "${b##*/}" ;; esac
	done
}
part_by_name() {   # print the node, start and size (512-byte units) of the partition named $1 (on disk $2, else on any disk)
	for d in /sys/class/block/${2:-sd*}[0-9]*; do
		[ -e "$d/partition" ] || continue
		n=$(sed -n 's/^PARTNAME=//p' "$d/uevent")
		[ "$n" = "$1" ] && echo "${d##*/} $(cat "$d/start") $(cat "$d/size")"
	done
}
part_guard() {   # the whole-partition writes: $1 must be a partition of LUN 0, named $2, at start $3 with size $4 (512-byte units)
	case "$1" in ''|*/*|*[!a-z0-9]*) die "node '$1'" ;; esac
	d=/sys/class/block/$1
	[ -e "$d/partition" ] || die "$1 is not a partition"
	l=$(lun0)
	case "$l" in ''|*[!a-z]*) die "LUN 0 not found or ambiguous: '$l'" ;; esac
	case "$1" in "$l"[0-9]*) ;; *) die "$1 is not a partition of LUN 0 ($l)" ;; esac
	[ "${1#"$l"}" != "" ] && [ -z "$(echo "${1#"$l"}" | tr -d 0-9)" ] || die "$1 is not a partition of LUN 0 ($l)"
	n=$(sed -n 's/^PARTNAME=//p' "$d/uevent")
	[ "$n" = "$2" ] || die "$1 is named '$n', not $2"
	[ "$(cat "$d/start")" = "$3" ] || die "$1 starts at $(cat "$d/start"), not $3"
	[ "$(cat "$d/size")" = "$4" ] || die "$1 has $(cat "$d/size") sectors, not $4"
	if command -v blockdev >/dev/null 2>&1; then
		[ "$(blockdev --getsize64 "$DEV/$1")" = "$(( $4 * 512 ))" ] || die "$DEV/$1 is not $(( $4 * 512 )) bytes"
	fi
	[ "$(grep -c "^$DEV/$1 " /proc/mounts 2>/dev/null)" = "0" ] || [ ! -e /proc/mounts ] || die "$1 is mounted"
}
blkdiscard_tool() {
	if command -v blkdiscard >/dev/null 2>&1; then echo blkdiscard
	elif busybox --list 2>/dev/null | grep -qx blkdiscard; then echo "busybox blkdiscard"
	else echo none; fi
}
cmd=${1:-}; [ $# -gt 0 ] && shift
case $cmd in
map)
	l=$(lun0)
	case "$l" in ''|*[!a-z]*) die "LUN 0 not found or ambiguous: '$l'" ;; esac
	echo "lun0=$l"
	echo "lun0_block_size=$(cat /sys/block/$l/queue/logical_block_size)"
	echo "lun0_bytes=$(blockdev --getsize64 /dev/$l)"
	echo "mounted=$(grep -c '^/dev/sd' /proc/mounts)"
	for n in boot_a boot_b vendor_boot_a init_boot_a linux_rootfs linux_data esp; do
		case $n in linux_*|esp) disk=$l ;; *) disk= ;; esac   # LUN 0's own entries only on LUN 0
		set -- $(part_by_name "$n" $disk)
		[ $# -eq 0 ] && { echo "$n=none"; continue; }
		[ $# -ne 3 ] && die "partition name $n is not unique"
		echo "$n=$1 $2 $3"
	done
	;;
piano-309-gpt-preflight|piano-309-gpt-readback|piano-309-gpt-write)
	if [ "$cmd" = piano-309-gpt-write ]; then
		lock=/run/piano-309-gpt-write.lock
		mkdir "$lock" || die 'GPT write lock already held or unavailable'
		trap 'rmdir "$lock"' EXIT
	fi
	# Refuse active mounts, swap and stacked block devices before a GPT trial.
	l=$(lun0)
	case "$l" in sd[a-z]) ;; *) die "LUN 0 missing or ambiguous: '$l'" ;; esac
	[ "$(cat "/sys/block/$l/queue/logical_block_size")" = 4096 ] || die 'unexpected LUN 0 block size'
	[ "$(blockdev --getsize64 "/dev/$l")" = 510165778432 ] || die 'unexpected LUN 0 capacity'
	[ "$(blockdev --getro "/dev/$l")" = 0 ] || die 'LUN 0 is not writable'
	mounts=$(awk '{print $3}' /proc/self/mountinfo) || die 'cannot inspect mounts'
	[ -n "$mounts" ] || die 'mount inventory is empty'
	awk 'END { exit NR != 1 }' /proc/swaps || die 'active swap or unreadable swap inventory'
	for b in "/sys/block/$l" "/sys/block/$l"/"$l"[0-9]*; do
		[ -d "$b" ] || continue
		devno=$(cat "$b/dev") || die "cannot identify ${b##*/}"
		if printf '%s\n' "$mounts" | grep -Fxq "$devno"; then die "${b##*/} is mounted"; fi
		[ -d "$b/holders" ] || die "cannot inspect ${b##*/} holders"
		for holder in "$b"/holders/*; do
			[ ! -e "$holder" ] || die "${b##*/} has block holder ${holder##*/}"
		done
	done
	if [ "$cmd" = piano-309-gpt-write ]; then
		[ "$#" = 1 ] || die 'specify stock or candidate'
		stock_head=9c096761ee7b9c12c1a5c3ef3f100bc27abccbbafb798a141aa4320eb81af25d
		stock_tail=f8b53b4538913ccf2ac385d8bd87eb8ad689aa3f439222480a95d05c625ca4fe
		case "$1" in
		stock) head=$stock_head; tail=$stock_tail ;;
		candidate)
			head=12710135635beee2a5de1dd910924a3d2d26e09a6f368c191b879fd6b41a499b
			tail=9acf222558a6766c4f1fcd09509a41cfd5be10d53f34a2a0f84a8cfe0a18c2f5 ;;
		*) die 'specify stock or candidate' ;;
		esac
		umask 077
		work=$(mktemp -d /run/piano-gpt.XXXXXX) || die 'cannot allocate RAM write directory'
		trap 'rm -f "$work/head" "$work/tail" "$work/check"; rmdir "$work"; rmdir "$lock"' EXIT
		cp "$P/gpt-head.bin" "$work/head" && cp "$P/gpt-tail.bin" "$work/tail" || die 'cannot stage GPT fragments'
		check_hash() {
			digest=$(sha256sum "$1") || die 'cannot hash GPT fragment'
			[ "${digest%% *}" = "$2" ] || die "GPT hash mismatch: $3"
		}
		verify_region() {
			dd if="/dev/$l" of="$work/check" bs=4096 iflag=direct skip="$1" count="$2" || die "GPT read failed at LBA $1"
			check_hash "$work/check" "$3" "LBA $1"
		}
		check_hash "$work/head" "$head" 'staged primary'
		check_hash "$work/tail" "$tail" 'staged backup'
		if [ "$1" = candidate ]; then
			verify_region 0 6 "$stock_head"
			verify_region 124552187 5 "$stock_tail"
		fi
		dd if="$work/tail" of="/dev/$l" bs=4096 seek=124552187 conv=notrunc,fsync || die 'backup GPT write failed'
		verify_region 124552187 5 "$tail"
		dd if="$work/head" of="/dev/$l" bs=4096 conv=notrunc,fsync || die 'primary GPT write failed'
		verify_region 0 6 "$head"
		verify_region 124552187 5 "$tail"
		verify_region 0 6 "$head"
		printf 'gpt_written=%s\n' "$1"
	fi
	if [ "$cmd" = piano-309-gpt-readback ]; then
		umask 077
		work=$(mktemp -d /run/piano-gpt.XXXXXX) || die 'cannot allocate RAM readback directory'
		trap 'rm -f "$work/primary" "$work/backup"; rmdir "$work"' EXIT
		dd if="/dev/$l" of="$work/primary" bs=4096 iflag=direct count=6 || die 'cannot read primary GPT'
		dd if="/dev/$l" of="$work/backup" bs=4096 iflag=direct skip=124552187 count=5 || die 'cannot read backup GPT'
		[ "$(wc -c < "$work/primary")" -eq 24576 ] || die 'short primary GPT read'
		[ "$(wc -c < "$work/backup")" -eq 20480 ] || die 'short backup GPT read'
		primary=$(sha256sum "$work/primary") || die 'cannot hash primary GPT'
		backup=$(sha256sum "$work/backup") || die 'cannot hash backup GPT'
		printf 'primary_sha256=%s\nbackup_sha256=%s\n' "${primary%% *}" "${backup%% *}"
	fi
	echo "lun0=$l"
	echo 'gpt_preflight=ready'
	;;
hash)   # from the medium: the page cache is dropped first
	sync; { echo 3 > /proc/sys/vm/drop_caches; } 2>/dev/null
	status=$(mktemp "$P/hash.XXXXXX") || die 'cannot create hash status file'
	digest=$({ LC_ALL=C dd if="/dev/$1" bs="${4:-4096}" skip="$2" count="$3" 2> "$status" && printf '%s\n' 'read-ok' >> "$status"; } | sha256sum)
	hash_rc=$?
	records=$(awk '/^[0-9]+\+[0-9]+ records in$/ {print $1}' "$status")
	read_status=$(tail -n 1 "$status")
	rm -f "$status"
	[ "$read_status" = read-ok ] && [ "$hash_rc" = 0 ] || die 'cannot hash requested device region'
	[ "$records" = "$3+0" ] || die 'incomplete read of requested device region'
	printf 'sha256=%s\n' "${digest%% *}"
	;;
zero-sha)
	dd if=/dev/zero bs="${2:-4096}" count="$1" 2>/dev/null | sha256sum | cut -d' ' -f1 | sed 's/^/sha256=/'
	;;
peek)
	dd if="/dev/$1" bs=4096 skip="$2" count=1 2>/dev/null | od -A n -t x1 -v -j "$3" -N "$4" | tr -d ' \n' | sed 's/^/hex=/'
	echo
	;;
send)   # both statuses collected in files: the pipeline's own status would be nc's alone (no pipefail in this shell)
	rm -f "$P/dd.rc" "$P/nc.rc"
	{ dd if="/dev/$1" bs="$4" skip="$2" count="$3" 2>/dev/null; echo $? > "$P/dd.rc"; } | { nc "$(host_ip)" "$5"; echo $? > "$P/nc.rc"; }
	if [ "$(cat "$P/dd.rc" 2>/dev/null)" != "0" ] || [ "$(cat "$P/nc.rc" 2>/dev/null)" != "0" ]; then
		die "send $1+$3: dd rc $(cat "$P/dd.rc" 2>/dev/null), nc rc $(cat "$P/nc.rc" 2>/dev/null)"
	fi
	echo "sent=0"
	;;
fetch)
	rm -f "$P/$1"
	wget -q -O "$P/$1" "http://$(host_ip):$3/$1" || die "wget $1"
	got=$(sha256sum "$P/$1" | cut -d' ' -f1)
	[ "$got" = "$2" ] || die "$1 sha256 $got, expected $2"
	echo "fetched=$1 sha256=$got bytes=$(wc -c < "$P/$1")"
	;;
file-sha)
	echo "sha256=$(sha256sum "$P/$1" | cut -d' ' -f1)"
	;;
copy)
	dd if="/dev/$1" of="/dev/$1" bs=4096 skip="$2" seek="$3" count="$4" conv=notrunc,fsync 2>"$P/dd.err" || { cat "$P/dd.err"; die "dd copy"; }
	tail -1 "$P/dd.err"; echo "copied=$4"
	;;
copy-list)   # FILE DEV: FILE's lines "SRC DST COUNT" (4096-byte blocks of /dev/DEV), each run copied, then one sync
	[ -f "$P/$1" ] || die "no $1"
	n=0
	while read -r s d c || [ -n "$s" ]; do   # the || takes a last line with no final newline too
		dd if="/dev/$2" of="/dev/$2" bs=4096 skip="$s" seek="$d" count="$c" conv=notrunc 2>"$P/dd.err" || { cat "$P/dd.err"; die "dd copy-list"; }
		n=$((n + c))
	done < "$P/$1"
	sync
	echo "copied=$n"
	;;
hash-list)   # FILE DEV COLUMN: the SHA-256 of FILE's runs read from the medium in order (COLUMN 1: the SRC blocks, 2: the DST blocks)
	[ -f "$P/$1" ] || die "no $1"
	sync; { echo 3 > /proc/sys/vm/drop_caches; } 2>/dev/null
	while read -r s d c || [ -n "$s" ]; do
		if [ "$3" = 1 ]; then at=$s; else at=$d; fi
		dd if="/dev/$2" bs=4096 skip="$at" count="$c" 2>/dev/null
	done < "$P/$1" | sha256sum | cut -d' ' -f1 | sed 's/^/sha256=/'
	;;
send-list)   # FILE DEV PORT: FILE's runs (column 1: the SRC blocks, COUNT) read from /dev/DEV in order, one stream to the host
	[ -f "$P/$1" ] || die "no $1"
	rm -f "$P/dd.rc" "$P/nc.rc"
	while read -r s d c || [ -n "$s" ]; do
		{ dd if="/dev/$2" bs=4096 skip="$s" count="$c" 2>/dev/null; echo $? >> "$P/dd.rc"; }
	done < "$P/$1" | { nc "$(host_ip)" "$3"; echo $? > "$P/nc.rc"; }
	if grep -qv '^0$' "$P/dd.rc" 2>/dev/null || [ "$(cat "$P/nc.rc" 2>/dev/null)" != "0" ]; then
		die "send-list: dd rcs '$(tr '\n' ' ' < "$P/dd.rc" 2>/dev/null)', nc rc $(cat "$P/nc.rc" 2>/dev/null)"
	fi
	echo "sent=0"
	;;
restore-list)   # FILE DEV PORT NAME: run K of FILE from the host's NAME@K, written at its DST blocks (column 2) of /dev/DEV; one sync
	[ -f "$P/$1" ] || die "no $1"
	k=0; n=0
	while read -r s d c || [ -n "$s" ]; do
		rm -f "$P/wget.rc"
		{ wget -q -O - "http://$(host_ip):$3/$4@$k" || echo $? > "$P/wget.rc"; } | dd of="/dev/$2" bs=4096 seek="$d" conv=notrunc 2>"$P/dd.err" || { cat "$P/dd.err"; die "restore-list run $k: dd"; }
		if [ -s "$P/wget.rc" ]; then die "restore-list run $k: $4@$k: wget rc $(cat "$P/wget.rc")"; fi
		k=$((k + 1)); n=$((n + c))
	done < "$P/$1"
	sync
	echo "restored=$n"
	;;
write)
	dd if="$P/$1" of="/dev/$2" bs="$4" seek="$3" conv=notrunc,fsync 2>"$P/dd.err" || { cat "$P/dd.err"; die "dd write"; }
	tail -1 "$P/dd.err"; echo "written=$1"
	;;
restore)
	rm -f "$P/wget.rc"
	{ wget -q -O - "http://$(host_ip):$5/$1" || echo $? > "$P/wget.rc"; } | dd of="/dev/$2" bs="$4" seek="$3" conv=notrunc,fsync 2>"$P/dd.err" || { cat "$P/dd.err"; die "restore $1: dd"; }
	if [ -s "$P/wget.rc" ]; then die "restore $1: wget rc $(cat "$P/wget.rc")"; fi
	tail -1 "$P/dd.err"; echo "restored=$1"
	;;
job)
	name=$1; shift
	rm -f "$P/$name.rc" "$P/$name.log"
	setsid sh -c "sh $P/tablet.sh $*; echo \$? > $P/$name.rc" > "$P/$name.log" 2>&1 < /dev/null &
	echo "started=$name"
	;;
job-status)
	if [ -f "$P/$1.rc" ]; then echo "rc=$(cat "$P/$1.rc")"; cat "$P/$1.log"; else echo "running=1"; fi
	;;
drop-caches)
	sync; echo 3 > /proc/sys/vm/drop_caches; echo "dropped=1"
	;;
rereadpt)
	blockdev --rereadpt "/dev/$1" 2>/dev/null && echo "reread=blockdev" || { partx -u "/dev/$1" && echo "reread=partx"; }
	;;
discard-info)
	l=$(lun0)
	q=/sys/block/$l/queue
	max=$(cat "$q/discard_max_bytes" 2>/dev/null || echo 0)
	tool=$(blkdiscard_tool)
	echo "discard_max_bytes=$max"
	echo "discard_granularity=$(cat "$q/discard_granularity" 2>/dev/null || echo 0)"
	echo "tool=$(echo "$tool" | tr ' ' '-')"
	[ "$max" != "0" ] && [ "$tool" != "none" ] && echo "discard=1" || echo "discard=0"
	;;
discard)   # the whole partition: the host reads it back and zeroes it when the device does not read zeros after a discard
	part_guard "$@"
	tool=$(blkdiscard_tool)
	[ "$tool" != none ] || die "no blkdiscard"
	sync
	$tool "$DEV/$1" 2>"$P/discard.err" || { cat "$P/discard.err"; die "blkdiscard $1"; }
	sync; { echo 3 > /proc/sys/vm/drop_caches; } 2>/dev/null
	echo "discarded=$1 bytes=$(( $4 * 512 )) tool=$(echo "$tool" | tr ' ' '-')"
	;;
zero)
	part_guard "$@"
	bytes=$(( $4 * 512 ))
	if [ $(( bytes % 1048576 )) -eq 0 ]; then bs=1048576; else bs=4096; fi
	[ $(( bytes % bs )) -eq 0 ] || die "$1: $bytes bytes are no whole number of 4096-byte blocks"
	dd if=/dev/zero of="$DEV/$1" bs=$bs count=$(( bytes / bs )) conv=notrunc,fsync 2>"$P/dd.err" || { cat "$P/dd.err"; die "dd zero $1"; }
	sync; { echo 3 > /proc/sys/vm/drop_caches; } 2>/dev/null
	tail -1 "$P/dd.err"; echo "zeroed=$1 bytes=$bytes"
	;;
*)
	die "usage: tablet.sh map|hash|zero-sha|peek|send|fetch|file-sha|copy|copy-list|hash-list|send-list|restore-list|write|restore|job|job-status|drop-caches|rereadpt|discard-info|discard|zero"
	;;
esac
