# 2026-11-27 15:39 (image f27f7deb): the framebuffer format is fixed, WindowServer survives, and the panel stays black

Read `results-2026-11-27-27-3-framebuffer-compression-bga.md` first: it is why this image exists.
The image is `f27f7debb95610f34098dc4984a592a5258ccacec5a66f06b35deb779cee8c18` (Mac `out/ram-verified-8748-sm/`,
Windows `out/k273-ram-8748-sm/`), the run is `logs/native-runs/ram-k273-fbc-20261127-153912/`
(decoded `console.txt`, 7,340,032-byte mailbox, 4,380,674 bytes non-zero). Booted 15:39:32, the image's own
`piano-reset-after=420` fired (`piano-reset-after: 420 s reached, PSCI SYSTEM_RESET`), ABL back after 447 s, HyperOS on adb
again at 15:48:25. Nothing was written to the medium by this run: it is a `fastboot boot` RAM image.

## What the fix did

The plugin set Core Animation's option by name at load, and the boot line carried it for WindowServer as well
(`exec-env=...,WindowServer:CA_DISABLE_FRAMEBUFFER_COMPRESSION=1`):

```
fd2: pid 122 WindowServer: NativeSoftMetal: WindowServer: load: CA option 66 CA_DISABLE_FRAMEBUFFER_COMPRESSION set
NativeGopDisplay: first frame (swap 1); boot console released from the screen (at 154386 ms)
```

- `Unimplemented pixel format of 641877825`: **gone** (the previous run had 15 of them and 15 WindowServer restarts).
- The display driver's own thumbnail now reads the framebuffer as `'BGRA'`, which is the format our compositor writes.
- No kernel panic in the run.

## WindowServer: one crash instead of a crash loop

```
exec: pid 122 WindowServer      -> exit: pid 122 WindowServer status 0        (the boot-session handoff, normal)
exec: pid 163 WindowServer      -> Assertion failed: (r.update()->_backdrop_invalidations_finalized),
                                   function prepare_layers_roi, file ogl-layer.cpp, line 7151
                                -> exit: pid 163 WindowServer status 0x6
exec: pid 301 WindowServer      -> device made, registry ID 0x100000157, port 0, no queue: software compositing
                                -> alive when the 420 s reset came
```

Two WindowServer exits in the whole run, one of them an abort, in Core Animation's software renderer reached from
`CompositorSW::CompositeLayersToDevice -> layer_blit_entry -> CARenderOGLRender` (`CA::OGL::prepare_layers_roi`,
`0x18df6cd44` in the pinned 27.3 SystemOS cache). That is the assertion `docs/drivers/soft-metal.md` 11 describes.

## The wall now: the swaps carry nothing

```
NativeGopDisplay: damage: 0 of 2 frames from their dirty regions, mean 0.0% of the panel ...
                  5 swaps submitted, 5 shown, 0 dropped ... last frame 0.0% non-black mean #000000 centre #000000
NativeGopDisplay: thumb frame 1: 3200x2136 'BGRA', 0.0% non-black, mean #000000, centre #000000
```

Five full-frame swaps over the rest of the boot, all-black, and none of them came with a dirty region: the software
compositor is handing over an empty surface rather than a badly-drawn one. The path to a visible desktop through it is
already measured upstream - 11 wrote that Reduce transparency (managed preferences) did not remove the assertion, and 12
that `soft-metal-all=1` did not either, with the conclusion that Metal compositing avoids Core Animation's software
renderer altogether. So one more image stages Reduce transparency as `root:wheel` files inside the seed itself (this seed
has no `org.droidvm.run.0.plist` for the boot-job route) purely to check that on 27.3, and the desktop line goes to the A830
GPU: see `results-2026-11-27-turnip-inputs-are-public.md`.

## Side effects of restaging the seed, and what the userland still reports

The staged plugin bundle changes the seed's identity, so the installed Data store keeps out of this boot:

```
NativeRamBlock: the store belongs to seed 8e1a87e1 (this seed 58c6beb4): kept; Data is volatile this boot
NativeRamBlock: Data store state 7 (1 blank, 2 resumed, 3 fresh, 4 failed, 7 kept), ... generation 2, present 262911
```

Every build that restages gives a new seed, so Data is volatile until an ESP is built for that root and seed
(`state 2 resumed`). Persistence is blocked behind this, not the other way round.

Also in the console, none of them new: LaunchServices `couldn't find system appex for
file:///System/Library/PrivateFrameworks/ViewBridge.framework/Versions/A/XPCServices/ViewBridgeAuxiliary.xpc/`
(`_LSErrorMessage=kLSApplicationNotFoundErr`) in the crash-message stream, `ViewBridgeAuxili` pid 284 exiting 0x9, and
`cryptexd` / `AppSSODaemon` restarting until launchd reports `Too many corpses being created`.
