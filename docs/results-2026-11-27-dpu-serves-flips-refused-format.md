# 2026-11-27 - PianoDpu serves the panel, and 27.3's surface tag kept every flip refused

`ram-k273-a830dpu-20261127-202143` is the A830 image plus `piano-dpu=2 piano-dpu-flip=4`
(`df6bea7c52cac80823abf1703c47e2dbbbd322b233da9ea41e289e44736650c2`, same kernel `d921655f...`, same plugin `e749f767...`,
GPU arguments unchanged). Boot 20:22:04, self-reset 20:29:19 (440 s), reader, HyperOS `sys.boot_completed=1` at 20:30:51.
`fastboot boot` only, nothing flashed. This time the capture held the ports from the start
(`OPEN COM5`/`OPEN COM6` at 20:22:12, then `reset-reason: armed v1 producer=kernel copies=11 generation=1`), so the whole
boot is on record - the ordering fix from the previous section works.

## PianoDpu came up and served

    PianoDpu: start: piano-dpu=2 smmu=2 measure=1000 ms delay=0 repeat=0 samples=3 (MDSS 0xae00000, DPU 0xae01000, DSI 0xae94000/0xae96000)
    PianoDpu: mdss hw version c0000000 (12.0.0) intr status 00000000 ...
    PianoDpu: pipe VIG0 rect 0: 1600x2136 @0,0 -> 1600x2136 @0,0 addr fc800000 stride 12800 format 000237ff unpack 03020001 'BGRA' 8888 4 B linear alpha flippable [splash at 0,0]
    PianoDpu: B1b: serving flips (NativeGopDisplay with native-gop-display-flip=1): 4 windows, clean 1, latch timeout 50 ms, watch 10 s, vsync interrupt
    PianoDpu: B1b: 14 DPU stores, 8 SMMU stores (all to bank page 0x82000), 0 refused stores

That is the first time this driver has run on the tablet at all (dpu.md's status line still said "built and host-tested,
not run on the tablet"), and the DPU side is clean: MDSS powered, the splash pipe read back, its own page tables installed.

## Every swap was refused, on format, and that is why nothing flipped

    NativeGopDisplay: flip refused (format): 3200x2136 row 3200 format 0x26424741; copied
    PianoDpu: flips: 0 in the last 60 s (0 in all ...); refused: format 287 planes 0 size 0 stride 0 align 0 memory 0 slots 0 clean 0 off 0
    NativeGopDisplay: flip mode: 0 swaps flipped (no copy, no refresh copies; 0 '&BGA' plane 0), 959 copied (PianoDpu refused them, ...)

0x26424741 is `'&BGA'`, the tag 27.3's WindowServer puts on its framebuffer surfaces; `docs/k273-line.md:134` records the
same tag on the maintainer's own first 27.3 boot, and `GopShot.h` defines it as "linear plane 0 with BGRA8 pixels".
`checkSurface` in [DpuFlip.h](../package/PianoDpu/DpuFlip.h) takes only plain `'BGRA'`, and says what it expects instead:

    // Only plain 'BGRA' is taken: an '&BGA' frame (27.3's WindowServer framebuffers) arrives here
    // already normalized by its producer (NativeGopDisplay, native-gop-display-flip-lossless=1: plane 0's offset and rows as 'BGRA')

That producer switch is `native-gop-display-flip-lossless` (NativeGopDisplay.cpp, default 0, "the '&BGA' plane-0 flip switch").
No image built here had ever passed it, so flip mode could not engage on 27.3 surfaces at any point - which also explains why
every claim about flips in dpu.md and native-gop-display.md is host-tested rather than tablet-tested. The fix is one boot
argument, not a patch: `native-gop-display-flip-lossless=1`, which is also item 1 of native-gop-display.md's RAM-boot read
list next to `piano-dpu=2 piano-dpu-flip=4`. It is safe to read plane 0 as BGRA here because the image already runs
WindowServer with `CA_DISABLE_FRAMEBUFFER_COMPRESSION=1` (visible in its own boot-args line), the fix recorded in
[the framebuffer-compression note](results-2026-11-27-27-3-framebuffer-compression-bga.md).

## What the session did while that was going on

WindowServer started once (pid 119) and never exited; no assertion, abort or panic anywhere in 7 minutes. The GPU is doing
real work - `t gpu meter rs 1303 45.7/1018.3ms fw 1303 0.0/0.0ms conn 5/5 nos 0 ovf 0 vmr 0/0 pf 0/0/0/0` in one minute,
against the ~620 jobs/minute the maintainer's login run reports - and NativeGopDisplay copied about ten frames a second
the whole time. Those copies still read `0.0% non-black mean #000000 centre #000000` at all 11 samples, so the CPU copy
path saw black surfaces both with and without the DPU attached. The flip path is what settles whether that is the surfaces
or the CPU's view of them: the DPU scans plane 0 straight out of DRAM, with no CPU read in the way.

## Next boot

`tooling/k273/recipes/a830-flip.sh` (this repository) is that image: the same recipe, plus
`native-gop-display-flip-lossless=1`, written to `out/ram-verified-8748-a830-flip` so the two earlier images stay bootable.
Judged by a `PianoDpu` latch and `flips: N` above zero, and by whether the panel stops being black.
