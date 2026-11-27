# 2026-11-27 - our frontend patch set against `AdrenoMetal fb063de`, and the plugin bundle the pinned tree builds from it

`tooling/metal/build-turnip-a830.sh` builds the GPU plugin out of three parts: the k27 tree's `overlay/metal/NativeAdrenoMetalPlugin.mm`,
the frontend `drivers/AdrenoMetal` (our pin: `UniXNU/AdrenoMetal fb063de55cbb5429bde12a8bb55538726f8b1a0f`, the revision
`Droid-VM/UniXNU 527b8b7a3119` records for it) after `$K/scripts/piano-frontend-patch.py`, and then this repository's
`tooling/metal/turnip-a830/frontend/*.patch` on top. The last step is the one that had never been run on this frontend: the patches were
written against the VM's tree, and until this session the plugin had always been taken from the author's prebuilt bundle.

## The set fits, except one patch that describes code this frontend does not have

Measured on the Mac build host, in order, against a frontend produced by `piano-frontend-patch.py` from `fb063de`
(`/tmp/fework/work.mm`, 373,220 bytes; `piano-frontend-patch.py` reports one skipped edit - the YCbCr `convertedYCbCrTexture`
tail call, which only exists in the iOS/VM frontend):

```
APPLY 0001-frontend-console-rate-limit-serial-concurrent-compute.patch
FAIL  0002-frontend-render-barrier-policy.patch        1 out of 1 hunks failed
APPLY 0003-frontend-surface-texture-sizes-limits.patch
APPLY 0004-frontend-function-formatted-description.patch
APPLY 0005-frontend-quartzcore-generic-texcoords.patch
APPLY 0006-frontend-bounded-import-reclaim.patch
APPLY 0007-pressure-surface-residency.patch
APPLY 0008-frontend-windowserver-encode-thread.patch
APPLY 0009-frontend-compiler-additional-arguments.patch
APPLY 0010-frontend-function-reflection.patch
APPLY 0011-frontend-browser-gpu-mac2.patch
applied=10 of 11
```

`0002-frontend-render-barrier-policy.patch` rewrites a block that is not in this frontend: its context is

```
 static constexpr uint64_t kBarrierMaxPassPixels = 4000000;
-static bool nbRenderBarriers() {
```

and neither `kBarrierMaxPassPixels` nor `nbRenderBarriers` nor any render-barrier policy appears anywhere in `fb063de`
(`grep -rn` over the tree, no matches). The patch was written for the barrier knob of the iOS/VM frontend, where the render-pass
barriers of panel-sized `IOSurface` passes could lock the VM up; this frontend has no such code, so there is nothing for the knob to
select. It is retired rather than deleted: it moved to `tooling/metal/turnip-a830/frontend/not-on-this-frontend/`, which the build does
not glob, so it stays available if a frontend that has the block is ever pinned. The consequence for the build is direct - step 5 of
`build-turnip-a830.sh` globs `frontend/*.patch`, so with `0002` in the directory the plugin build stopped at
"frontend patch ... does not apply", which is what it should do.

`docs/drivers/gpu-userspace.md` 1 and the bring-up table still describe the knob, because it is what the *VM-era* logs record;
both now point here.

## The bundle the pinned tree's own script makes, on this Mac

`$K/build-metal-plugin-piano.sh` (the k27 tree's plugin recipe, the upstream-parity path: overlay plugin + the transformed `fb063de`
frontend + the bridge built here + Turnip) produced, in `$K/out/metal/plugin/NativeAdrenoMetal.bundle/Contents/MacOS/`:

- `NativeAdrenoMetal` - 23,174,448 bytes, Mach-O thin arm64e, ad hoc (`adhoc, linker-signed`), sha256
  `65563e6a90623d2be7edfb0ee2a0c3f22a21d0ba31a062f1d3ad6fa627163711`;
- `libvulkan_freedreno.dylib` - 17,667,328 bytes, sha256 `ffae67cb0a238df81d322d739942da38766c6541616f57392ae82a68da8f607d`,
  the re-signed copy of the Turnip below.

It reads its inputs from `$K` and from the shim directory `$HOME/Codex/UniXNU-turnip/{drivers/AdrenoMetal, build/mesa-tu-macos/src/freedreno/vulkan/libvulkan_freedreno.dylib}`,
which is where the last Mesa rebuild put the dylib it expects. This bundle is the parity check that the inputs are wired correctly;
the plugin that goes on the tablet is the one `build-turnip-a830.sh` links, because that is the one carrying our ten frontend patches,
the abort reporter and the plugin-side patch.

## Turnip, rebuilt with the Mesa half of the bridge's in-place writes

`out/turnip-a830/build/src/freedreno/vulkan/libvulkan_freedreno.dylib` on the Mac build host is now 17,770,472 bytes, sha256
`c360c30dbc0b32bed7e025e797259bb020129fdaac5b63e87158af442e39e639`, superseding `6eeb3d2dffa4f7f709cc976235f2f71d3459b89c2bcc0527fa296b762cd069ac`
recorded earlier today. The difference is `tooling/metal/turnip-a830/mesa/0016-turnip-render-mode-for-attachment-memory.patch`, the Mesa
side of the bridge patch `0020-written-host-page-textures...`, which had to stay in the bridge series because board patch `0013` depends on
it. Built, to be run: never on the tablet.
