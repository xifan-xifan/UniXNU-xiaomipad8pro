# 2026-11-27: what the tablet's boot surface actually is, read from ABL

`fastboot getvar all` on this unit, taken from ABL at 23:01 with `adb reboot bootloader` (no write of any kind). It settles two
questions that had been open all week: whether this tablet has the UniXNU partition layout at all, and what the A/B slot bits
say before anything touches `boot_a`.

## The UniXNU partitions are here

| Partition | Size | Note |
| --- | --- | --- |
| `esp` | `0x40000000` = 1 GiB | the same size as the reference unit's ESP entry, so this GPT came from the same tooling |
| `linux_rootfs` | `0x500000000` = 20 GiB | a 20,935,868,416-byte root image (OS + SystemOS windows plus the manifest block) fits |
| `linux_data` | `0x1000000000` = 64 GiB | our 24 GiB store window sits inside it |
| `boot_a` | `0x6000000` = 100,663,296 B | equals `out/backup-20261125/boot_a.img` and both persistent candidates byte for byte |
| `init_boot_a` | `0x800000` = 8,388,608 B | equals the backup |
| `vendor_boot_a` | `0x6000000` = 100,663,296 B | equals the backup |

All three A-slot sizes match `out/backup-20261125/{boot_a,init_boot_a,vendor_boot_a}.img`, which is the size check
`tools/piano-switch.py` refuses to skip. The full `getvar all` is kept at `out/getvar-all-20261127.txt` (run evidence, not
committed).

## The slot bits, which the ESP guide says to check first

```
current-slot: a          slot-count: 2
slot-successful:a: yes   slot-retry-count:a: 6   slot-unbootable:a: no
slot-successful:b: yes   slot-retry-count:b: 6   slot-unbootable:b: yes
product: piano           unlocked: yes           is-userspace: no
```

So the four identity checks that `piano-switch.py` makes all pass, `boot_a` is the partition a cold boot actually reads, and
`slot-unbootable:b` is already set - this unit only boots A. There is no B to fall back to, so "Android can come back" has to
mean the verified rollback triplet, not a slot switch. `slot-retry-count:a` is 6, not the 0 the ESP guide's `abl-final` gate
expects, which is another reason to read and restore that attribute deliberately rather than assume it.

The `dd`-level GPT read that was going to confirm all of this from inside a macOS RAM boot did not happen: the six `rdisk`
nodes on that boot are the RAM root, the Data store and their APFS slices, not the UFS LUNs, and `/dev/rdisk0` read as zeros
at LBA 1. The media map has to be taken from `ioreg` before any block of a UFS LUN is read. The dumps that attempt produced
(`gpt/lun[0-5].bin`, 24 KiB each) are all-zero or unrelated and are not evidence of anything.

## What this changes about the cold-boot plan

The persistent path no longer needs a GPT change: `esp`, `linux_rootfs` and `linux_data` exist at the sizes the tools expect.
What it still needs, in order, is

1. read what the ESP holds now (`EFI/UniXNU/unixnu.toml` and the payload it names) - read-only, from a macOS RAM boot once the
   UFS whole-disk node is known, or from the ESP's FAT volume mounted read-only;
2. put the root windows on `linux_rootfs` if the ESP entry is meant to boot from it, since the RAM images have always carried
   the root in memory and never read it from disk;
3. produce a `boot_a` whose FD carries the UFS stack (`3fcc8429...`, built and AVB-signed as `out/persistent-v6-macfd/boot.img`
   `946adfe0...`, never run) and check the ESP's own view of the payload before writing it;
4. `piano-switch.py --images <macOS triplet> --rollback <backup-20261125 triplet>` for the swap, and the same tool in the other
   direction for the way back to HyperOS, plus the `slot-success` step around the `boot_a` write because writing `boot_a`
   through fastboot clears bit 54 of the GPT entry.

Steps 2 and 3 are the expensive ones and both are checkable before they are written: the root windows have a manifest the host
can verify, and the FD has its stamped module checks. Nothing in this section was executed against the tablet.