# Native SystemOS mount path audit (2026-11-21)

The retained native startup probe in `dispatch-kern_exec_data.c` (SHA-256 `1105bf146b8bfd1b4bf6520e976b5dd3b5b2ce54bfb06430608a8dc0f6488ccb`) contains an APFS mount path before launchd. With `XNU_OWNER_UNION_SYSTEMOS=1`, it selects SystemOS by UUID, requests a read-only union mount over `/` and checks the resulting device, filesystem type, authorization, flags and relationship to the original root. It also rechecks authorization of the lower root. The later cryptex compile arguments enable this option and the bindfs kernel link file list includes the corresponding `kern_exec.o`.

Device validation must establish whether the mount succeeds against the final authenticated root image. The kernel mount and launchd's `mount -P 1/2` role-volume tasks run at different stages. The two launchd fsck requests still need target selection, exit status and write-path validation. No tablet commands were issued.
