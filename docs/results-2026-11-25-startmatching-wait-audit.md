# 2026-11-25: where the synchronous match can wait

Host-side reading of the k27 XNU tree at `E:/Temp/k27-native-candidate-20261119-a/xnu-ac9718fb…` (HEAD `a71b3dd`), against
the ufs170/171 result where `IOBlockStorageDriver` ran every callback but `startMatching(kIOServiceSynchronous)` never returned.

## The code path

`iokit/Kernel/IOService.cpp` `IOService::startMatching` (line 1104) takes the arbitration lock, sets
`kIOServiceNeedConfigState`, and for the synchronous case loops:

- `doServiceMatch(options)` runs the match and calls the driver's `start()`.
- then, while the busy count is still above its entry value, it sets `kIOServiceSyncPubState | kIOServiceBusyWaiterState`,
  `assert_wait(this, THREAD_UNINT)`, `thread_block(THREAD_CONTINUE_NULL)` and repeats (lines 1175-1195).

So the caller blocks only if the match did not settle the busy count. In ufs170/171 the `IOBlockStorageDriver` callbacks
(`open`, `reportRemovability`, `reportMediaState`, `reportBlockSize`, `reportMaxValidBlock`, `reportWriteProtection`) and three
real reads all ran, which means `doServiceMatch` entered the driver. Two cases remain, and they need different fixes:

1. the blocking is inside `IOBlockStorageDriver::start()` itself, so the loop is never reached; or
2. the driver's `start()` returned, some other service in the chain stayed busy, and the loop blocked in `thread_block`.

The earlier runs already showed `thread_block` does not resume in this early environment (ufs104: `thread_block(THREAD_CONTINUE_NULL)`
blocked forever, 84.7 s bite, no spin beats), and that `thread_block_reason` with `AST_YIELD` in a ticker did make progress
(ufs105), so case 2 has a known workaround and case 1 does not.

## Discriminating instrumentation for the next image

- log on entry to and return from `IOBlockStorageDriver::start()` (the IOKit in-kernel class, not our driver), and on entry to
  the `waitAgain` branch in `IOService::startMatching`.
- log the busy count before `doServiceMatch` and in the loop.
- keep our window publish on its own thread, as ufs170/171 did.

No new kernel was built for this: the KC base that the local builder needs (`kernelcache.release.mac17j`) is still not present, so
this note only fixes what the next runnable image has to log. Nothing touched the tablet.
