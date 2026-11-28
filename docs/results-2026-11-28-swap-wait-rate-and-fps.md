# 2026-11-28: what swap_wait is actually costing, measured while the desktop animates

## Correcting the ul10 note

`docs/results-2026-11-28-ul10-long-boot-args.md` recorded that ul10's boot capture held no `swap_wait` line, where ul9's held 23 lines. That
asymmetry is an artefact of the logging, not of the hardware: `NativeGopDisplay::swap_wait` prints during the first 20 failures and then once
per 1000 (`NativeGopDisplay.cpp:1565`), so a boot whose counter never reaches 20 prints nothing. ul10 does have the defect - the counter is at
159,000 fifteen minutes into a desktop session, and 47,000 within the eight-minute window below. The ul9 and ul10 numbers describe the same
behaviour, and the earlier hedge ("not yet a verdict") is now resolved against it.

## The measurement

Capture `logs\ul10-flip-awake.log`, 80 s on ul10 with the display powered (`piano-display-power = Yes`), Safari showing an animated page
(`requestAnimationFrame` counter plus a cycling CSS gradient, i.e. something asking for 60 frames a second):

    swap_wait milestone lines: 39   (= 39,000 refused waits in ~80 s, about 490 a second)
    NativeGopDisplay: swap_wait(2277, 0x80000001, 0) = 0xe00002d5 (45000 failures): last 2276 power 1/1 powered 1 started 0x1 current ... queue 0
    NativeGopDisplay: swap_wait(2338, 0x80000001, 0) = 0xe00002d5 (47000 failures): last 2337 power 1/1 powered 1 started 0x1 current ... queue 0

`0xe00002d5` is `kIOReturnBusy` (`package/NativeUfs/tests/Bridge.h:19`). The call shape never varies: `options = 0x80000001`, `timeout = 0`,
the display powered and started, nothing queued.

What the compositor actually got:

    NativeGopDisplay: damage: 0 of 55 frames from their dirty regions, mean 0.0% of the panel
    NativeGopDisplay: 57 copies waited for the GPU's fence, mean 23038 us max 30347 us, 0 gave up after 1000 ms
    NativeGopDisplay: scheduling: 0 copy wakeups over 20 ms (longest 0 ms), 0 VBL ticks over 20 ms late

about 55 presented frames per 5 s report window, i.e. roughly 11 frames a second while a 60 fps animation is asking. Nothing is overdue and
nothing gave up, which is why the desktop is usable; it is simply slow. The driver's own comment says as much: told the last swap is still
busy, "WindowServer updated at about 10 frames a second however little it drew".

## Why the existing conversion does not fire

`NativeGopDisplay.cpp:1559` answers for the family and already knows the answer for a copy display:

    if (result == kIOReturnBusy && (options & 0x7fffffffu) == 0 && swapID == at<uint32_t>(this, kLastCompletedSwap)) {
        fWaitReleased++;
        return kIOReturnSuccess;
    }

The comment above it states the reasoning: the panel shows a copy, so the swap it is asked about is done. But the guard requires the masked
options to be 0, and CoreAnimation asks with `0x80000001`, so on this boot every one of these questions falls through to the failure path,
is counted, and is answered Busy - about 490 times a second, and the compositor throttles itself accordingly.

## The candidate fix, and what has to be checked before it

The honest answer for `swapID == kLastCompletedSwap` is "done", whatever bits sit in `options` besides the top one, so widening the guard to
`(options & 0x7fffffffu) <= 1` would let the conversion cover the shape actually seen. This is not a boot argument: `NativeGopDisplay` reads
no boot arguments, so it needs the kext rebuilt and the ESP payload reflashed. Two things to check in that same run, because answering "done"
earlier is exactly the case where a surface could be reused while the copy thread is still reading it: `admitSwap` in `GopCompose.h` (the file
says a later swap waits while the copy thread reads the surfaces), and `fWaitReleased` versus the corruption seen on the panel (the green-purple
patch in the top-left corner that the owner has already told me to leave alone - if it grows after this change, it is mine to explain).

This is the cheapest known route to a visible daily-use gain: the same 80 s window should show the frame rate rise from about 11 fps toward
the panel's refresh, and the refusal counter stop climbing at all.