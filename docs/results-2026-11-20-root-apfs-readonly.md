# Read-only APFS checks for the root windows (2026-11-20)

On the Mac, attach each retained raw image separately with `hdiutil attach -readonly -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage`. Resolve the synthesized APFS volume with `disk-of.py volume`, then run the host `/sbin/fsck_apfs -n` on the matching `/dev/rdisk…` volume node. Both checks completed and the images detached successfully.

| Image | APFS volume UUID | Host check |
| --- | --- | --- |
| Apple OS `141-22233-021.dmg` | `84581E99-85BE-4957-AC53-2947E887B3E4` | Exit 0. Host checker 2317.81.2 warned that the image was written by 2332.120.31.100.1. |
| `systemos.unshimmed.img` | `284548A8-FC87-434E-A0C8-A4038E83704D` | Exit 0. |

Separate read-only mounts found the stock `fsck`, its `fsck_apfs` symlink into the APFS bundle, and `apfs_boot_util` in the Apple OS image. The SystemOS image carries only `mount.real` among the helpers checked. Neither standalone image has an fstab in `private/etc`. The composed namespace can expose additional files through Data and requires its own fstab and helper lookup check. These volume checks do not exercise launchd or its boot-time filesystem checks. A candidate must account for the `fsck -fy` repair request and the actual block devices made visible before launchd runs.
