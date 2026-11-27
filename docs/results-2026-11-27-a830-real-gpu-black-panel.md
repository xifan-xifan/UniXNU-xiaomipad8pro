# 2026-11-27 - the Adreno 830 answers Metal on this unit, and what it draws never reaches the panel

`ram-k273-a830b-20261127-195858` is a `fastboot boot` of the first A830 image built entirely here: kernel `d921655f...`,
the verified 27.3 assembly, PianoA830 linked, the four pinned gen80000 images, and `NativeAdrenoMetal.bundle` built by
`tooling/metal/build-turnip-a830.sh` on this Mac - our bridge, our Turnip, the ten frontend patches that fit `fb063de`.
Boot 19:59:19, the image reset itself at 420 s back to ABL (20:06:33), the reader staged the mailbox, and the installed
HyperOS OS3.0.309.0.WPYCNXM came up on adb at 20:08:05 with `sys.boot_completed=1`. Nothing was flashed.

## The GPU is the real card, and it does the work

    NativeAdrenoMetal: Installer Progress: device Adreno (TM) 830 (raytracing disabled)     (pid 64,  at us=91 s)
    NativeAdrenoMetal: MenuBarAgent: device Adreno (TM) 830 (raytracing disabled)           (pid 158, at us=95 s)
    NativeAdrenoMetal: loginwindow: device Adreno (TM) 830 (raytracing disabled)            (pid 109, at us=102 s)
    PianoA830: R3b WORK ... submitted=23 completed=23 missed=0 q-us=519800 depth=1          (us=234 s)
    PianoA830: gpu clients 4 (peak 4, refused for want of a slot 0): 0:WindowServer 729M ...
    PianoA830: SMMU routes (60 s later): every GPU-side stream that has an entry is translated or faults; nothing reaches memory as IOVA = PA

WindowServer's Metal device is the Adreno rather than the software accelerator, its swaps carry tens to hundreds of
megabytes of GPU buffers, and jobs submitted equals jobs completed at every sample after the first. Earlier runs on
2026-11-27 used the CPU path (`piano-gpu=0`) and stopped at empty swaps and the backdrop assertion, so the
"no public A830 userspace, so no WindowServer desktop" boundary recorded on 2026-11-26 is closed: the bridge, Turnip and
plugin rebuilt from public inputs on this Mac produce a working device.

## The panel stayed black while that was happening

NativeGopDisplay was the only display driver serving, because the image's command line has no `piano-dpu=` and PianoDpu
is inert without one. Its own report over 11 samples says the copies ran and the content was black:

    NativeGopDisplay: ... 3 swaps submitted, 3 shown, 0 dropped, 0 ticks stalled ..., last frame 0.0% non-black mean #000000 centre #000000
    NativeGopDisplay: period 23827 ms: 34 VBL ticks, 1 times the tick stopped while nothing moved
    NativeGopDisplay: swap_wait(6, 0x80000001, 0) = 0xe00002d5 (1 failures): last 5 power 1/1 powered 1 started 0x1 ...   (21 times)

So swaps are accepted, shown and fenced, and what gets copied is all-black. [dpu.md](drivers/dpu.md) 4 and
[native-gop-display.md](drivers/native-gop-display.md) 5a both name the desktop configuration as PianoDpu serving with
each swap flipped to it, which is `piano-dpu=2 piano-dpu-flip=4`; the image built at 20:18 for the next boot
(`df6bea7c...`) is exactly that recipe, `piano-gpu=2` and the GPU knobs unchanged. The owner separately reported a
green/purple block at the panel's top-left on an earlier boot; it is not part of this run's evidence and has not been
reproduced since.

## How far the session itself got

`loginwindow` (109), `WindowServer` (119), `Dock` (489), `ControlCenter` (491), `SystemUIServer` (493),
`iconservicesagent` (417) and `NotificationCenter` (405) all ran, most with an Adreno device attached. The noise after
that - Core Data store errors and recoveries in `contextstored`, `identityservices`, `dmd`, and WindowServer's
`sandbox_extension_issue_file failed ... (Operation not permitted)` - is behind the display question, not in front of it.

## Data persistence is still unproven, and this run cannot settle it

Root is the shim SystemOS root and Data is RAM-backed for the whole run: `NativeRamBlock: volatile ... 2048 MiB held in
chunks ... peak 196 MiB ... writes refused: 0 no space, 0 no memory` at every sample. `data-persist=2`,
`data-store-part=linux_data` and `data-store-lba=117735999` are on the command line (`build-A-bundled.json`), but the
lines where the store is opened belong to the first minute, and this run's capture does not contain the first minute
(below). Nothing here proves or disproves that Data survives a reboot.

## Two logging faults, both hit in this pair of boots

The capture (`tooling/native/capture-com.ps1`) starts by opening whatever ports exist, and the previous run's reader was
still holding them: the first 1,420 lines of `usb-console.txt` are `open failed COM5` / `open failed COM6` and the first
kernel line arrives at `at-us=77648513`, i.e. 78 s of boot lost. The fix is ordering, not configuration - start the
capture, confirm it holds the port, then reboot. The 20:21 run starts capture at 20:21:37, 27 s before `fastboot boot`.

The pstore mailbox is no longer a usable second channel for these images. `decode2.py decode` on the 7 MiB dump reports

    copies-at-1 histogram [699972, 387029, 639608, 831574, 827482, 656471, 436337, 285456, 227948, 193619, 118030, 34490], threshold 3 of 11
    XNU log: 661294 bytes (start 271966, capacity 667252, 655837 bytes corrected by majority)

and the majority vote reconstructs unreadable text, as it did for the 19:37 boot (656,685 bytes corrected). A 191 MiB
payload with the console mirrored for 420 s decays too far across the reset; treat the live USB console as the record.

## Where the image that reached a desktop still exists

`out/build-a830-8748.sh` ends with `rm -rf out/ram-verified-8748-a830 && cp -R out/ram ...`, so the 20:18 build replaced
it on the Mac. The bytes this run booted are kept on the Windows host at
`E:\project\UniXNU-xiaomipad8pro\out\k273-ram-a830\boot-piano-macos-ram-A-bundled.img`
(sha-256 `65f41bea525c8c3a56a16b19484fd081451fd9be1f8a286f8a1272875d880772`, re-verified 20:21:30), and the DPU variant
at `out\k273-ram-a830-dpu\boot-piano-macos-ram-A-bundled.img` (`df6bea7c52cac80823abf1703c47e2dbbbd322b233da9ea41e289e44736650c2`).
