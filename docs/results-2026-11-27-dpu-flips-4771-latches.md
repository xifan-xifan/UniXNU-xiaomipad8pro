# 2026-11-27 - the DPU flips WindowServer's frames: 4771 latches, no underrun, desktop-rate GPU work

`ram-k273-a830flip-20261127-210911` is `701e1abd46b965fced2e129ed84c62666b5e6ca94c80c9cf5c716e427db5d9d9`
(`tooling/k273/recipes/a830-flip.sh`: the A830 recipe plus `native-gop-display-flip-lossless=1`, nothing else changed).
Boot 21:09:32, self-reset at the image's own 420 s, ABL 21:16:45, mailbox staged, HyperOS `sys.boot_completed=1` 21:18:17.
`fastboot boot` only; no partition written. The capture again holds the ports from the start, so this is the whole boot.

## The switch takes effect and the refusals stop

    NativeGopDisplay: flip mode: '&BGA' plane-0 flips enabled (native-gop-display-flip-lossless=1): plane 0's offset and row bytes are offered as linear 'BGRA'; other layouts are copied
    NativeGopDisplay: flip mode: machdep.native_gop_flip_lossless=1 (root writes 0/1: copied or flipped '&BGA' frames)
    NativeGopDisplay: flip mode: 2 swaps flipped (no copy, no refresh copies; 2 '&BGA' plane 0), 0 copied (PianoDpu refused them, is not serving, or a cursor was on the panel: 0)

Against the previous boot's `refused: format 287`, the refusal counters are all zero:

    PianoDpu: flips: 2810 in the last 71 s (4741 in all ...); refused: format 0 planes 0 size 0 stride 0 align 0 memory 0 slots 0

The first latch, in full, with no underrun:

    PianoDpu: flip 1: reflush 1/1: ctl0 flush 00000003 latched in 6873 us (1 vsync interrupts, 1 waits), frames 1979 -> 1979, underrun none: pass
    PianoDpu: flip 8: 3200x2136 row 12800 -> window 1 iova 0x12000000, clean 2363 us, ctl0 flush 00000003 latched in 5002 us (1 vsync interrupts)

The first flip needs a 12.9 ms clean of the surface and 6.9 ms to latch; steady state is a ~2.4 ms clean and a 5-7.7 ms latch,
one vsync interrupt each, and the panel's own frame counter (`intf1 frames`) moving. `underrun none` on every flip logged.

## What was being flipped

    t gpu meter rs 2421 2.7/258.0ms fw 2421 0.0/0.0ms ... conn 5/5 nos 0 ovf 0 vmr 0/0 pf 0/0/0/0
    t gpu meter rs 2117 2.1/111.8ms ...

About 35-40 render submissions a second at 2.1-2.7 ms mean GPU time, against the ~10/s of the maintainer's login run: the
compositor is drawing continuously, the DPU is presenting at the same rate (2810 flips in 71 s), and the session grew into
`loginwindow`, `Dock`, `DockControls`, `ControlCenter`, `SystemUIServer`, `NotificationCenter`, `iconservicesagent` and
`Finder` (503). WindowServer started once and never exited; no assertion, abort or panic in seven minutes.

So the panel is scanning WindowServer's own surfaces for the whole boot. What it cannot yet be is *evidence of the picture*:
with the DPU scanning, NativeGopDisplay copies nothing (`0 copies`), and its `last frame 0.0% non-black` line still quotes the
sample taken before the first flip. Fourteen times it said `the boot framebuffer (an older frame) is back until the GPU
finishes the frame`, which is the flip path's own rule while a swap is not ready.

## Reading the picture instead of describing it

The next image (`tooling/k273/recipes/a830-flip2.sh`) adds `native-gop-display-shot=1`, which registers
`machdep.native_gop_display_shot` - "a root-only still capture of the internal panel's scanout backing"
[native-gop-display.md](drivers/native-gop-display.md) 9 - and gets root to it through `piano-usb-shell=1` with the
`pianousb-shell` helper, staged by `tooling/userland/stage.py` (which is also what installs the `gop-shot` unit now in
`configs/userland.toml`). That path answers two of the goal's standing items rather than only the screen: the PNG is the
panel's bytes, and a shell on the pipe is what a "write a file, reboot, read it back" check needs. That recipe also sets
`PIANO_DATA_STORE_REPLACE=1`, because every boot so far ran `NativeRamBlock ... volatile` for the reason
[data-store.md](data-store.md) gives - the store guard keeps a store made for another seed untouched - so until the store is
bound to the image's own seed, persistence cannot be tested at all.
