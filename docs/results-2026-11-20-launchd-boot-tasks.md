# Embedded launchd boot tasks in 26C5035r (2026-11-20)

A read-only attachment of the retained OS image let us parse the embedded XML property lists in `/sbin/launchd`. Two copies contain the same four boot tasks. The image detached successfully.

| Task | Executable and arguments | Configured flags |
| --- | --- | --- |
| `mount-phase-1` | `/sbin/mount -P 1` | `PerformInBaseSystem`, `SkipInNetBoot`, `AssumeSingleUserSuccess`, `RequireSuccess`, `RequireRun` |
| `mount-phase-2` | `/sbin/mount -P 2` | `PerformAfterUserspaceReboot`, `SkipInNetBoot`, `AssumeSingleUserSuccess`, `RequireSuccess`, `RequireRun` |
| `fsck-safe` | `/sbin/fsck -fy` | `SkipInNetBoot`, `RequireSuccess` |
| `fsck` | `/sbin/fsck -qL -R 1` | `SkipInNetBoot`, `RequireSuccess`, `RequireRun` |

The flags describe the task definitions. Their evaluation and execution order on the native startup path remain unverified. The extracted task table also leaves the root remount and mount-all strings unresolved. `fsck -fy` explicitly requests affirmative repair and needs a separate write-path decision before a temporary boot. The upstream APFS phase helper addresses the two phase commands. The fsck tasks still need a verified dispatch policy and the composed root still needs helper path validation.
