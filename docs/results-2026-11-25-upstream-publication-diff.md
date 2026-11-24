# Upstream root publication vs the piano deviation (2026-11-25)

Read-only comparison of the pinned production driver (`UniXNU/ShengUfs`, commit
`fd420df000f43213d9cd019725abff0849c98c72`, the revision `docs/porting/ufs.md` and
`docs/drivers/sheng-ufs.md` in `CrossDarwin/UniXNU_macOS` point at) against
`package/NativeUfs`.

## What upstream does

`NativeUfsBlock::start (IOService *provider)` calls `IOBlockStorageDevice::start (provider)`, sets
`online`, calls `registerService ()` and returns. `publishThread`'s adapter then runs
`ufsPublishRootWindows`: for each window `start (index)` (that `registerService`) followed by
`wait (index)`, and `wait` polls `mediaWithUuidExists (uuid, window)` for up to 300 x 100 ms. The
`IOMedia` node is created by `IOStorageFamily`, never by the driver, and **the driver never calls
`IOService::startMatching` anywhere**.

Local check: `package/NativeUfs/RootPublication.h` is byte-identical to the upstream header
(3,224 B, reflowed line endings aside), so the plan/verify logic is not where the two differ.

## Where the piano tree deviates

`package/NativeUfs/NativeUfs.cpp` deliberately stops `IOBlockStorageDriver` from matching and
publishes the media itself:

- `NativeUfsBlock::start` ends with `setProperty ("Content", "Apple_APFS")` and
  `setProperty ("Content Hint", "EF57347C-...")` and returns without `registerService`, so the
  window is not in the service plane while its own `start` runs (ufs167 comment).
- `publishMedia ()` allocates a bare `IOMedia`, `attach`es it to the window, calls
  `registerService`, and `publishThread` then calls `media->startMatching (kIOServiceSynchronous)`
  after both windows exist (ufs181/ufs185 comments).

That is the only structural difference: the same `RootPublication.h` drives a path in which the
storage stack does the matching upstream, while here the driver replaces `IOBlockStorageDriver`
with a hand-built media and then has to drive matching by hand.

## The stack is present, so absence is not the cause

Counting bundle/class strings in the pinned RAM kernel collection
(`out/k27-smprobe/piano-ufs266.kc`, 147,095,552 B):

| string | occurrences |
| --- | --- |
| `com.apple.iokit.IOStorageFamily` | 29 |
| `IOBlockStorageDriver` | 329 |
| `IOMedia` | 464 |
| `com.apple.iokit.IOAPFSFamily` | 0 |
| `AppleAPFSContainer` | 120 |

`IOStorageFamily` and `IOBlockStorageDriver` are in the collection; the APFS classes are present
under a different kext identifier. A missing kext therefore does not explain "matched drivers 0",
which matches the probe design rather than replacing it.

## Consequence for the next device session

The production sequence is the target state: `IOBlockStorageDevice::start` + `registerService` on
the window, then poll for an `IOMedia` carrying the signed volume UUID, and let `IOStorageFamily`
and the APFS personality match it. The uncommitted 2026-11-24 edit to `NativeUfs.cpp`
(`registerService ()` on the window, drop the monitor loop, skip `SystemOS`) is the first half of
exactly that change and is the closest existing candidate; it was never built or run.

The 0xA0-0xA8 and 0xB0-0xB7 probes stay the instrument that says which step of that sequence
fails when the device is next on USB. Nothing here was booted or written.
