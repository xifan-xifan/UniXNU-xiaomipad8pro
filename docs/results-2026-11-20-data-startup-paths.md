# First-install Data startup paths (2026-11-20)

The retained 24 GiB Data seed was attached read-only on the Mac. Its APFS UUID matched the deployment manifest (`1B4CEC84-35B3-4AAD-88D0-200DD0956637`). Neither `private/etc/fstab` nor `private/etc/fstab.hd` existed. The two plists in `Library/LaunchDaemons` both specify `RunAtLoad`.

| Job | Static evidence from the 26C5035r Apple OS image |
| --- | --- |
| `com.apple.postfixsetup` | The plist invokes `/usr/libexec/postfix/postfixsetup /`. Its executable embeds a command to remove its own plist from `/Library/LaunchDaemons`. |
| `com.apple.amsdstat` | The executable references its own plist and `/private/var/vm`. Actual writes were not established. |
| Embedded `dirs_cleaner` boot task | `launchd` specifies `/tmp`, `/var/run`, and `/var/boot`. The cleaner binary contains directory creation and swap-rename diagnostics. The corresponding Data directories were empty in this seed. |

Both image attachments were detached. `NativeRamBlock::attachStore` opens a persistent store only when the configured mode is nonzero. The large seed uses the RAM-backed overlay when direct writes are unavailable. That accounts for writes through this Data device with `data-persist=0`. The complete boot candidate still needs separate checks of device enumeration, root remount, fsck, firmware and logs. The existing staging tool can set launchd overrides for individual labels on a derived seed. No overrides or seed changes were needed for this inspection.

The packed seed matched the manifest SHA-256. The inspected raw image's SHA-256 is `7ca2f7b65b0c3c121c4660d25b6d802af4cc2680d2f62a305cb16eb3e664b1b9`.
