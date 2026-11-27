# 2026-11-27: Turnip for the A830 built on this Mac from public inputs; only the Metal bridge is still missing

Follows [results-2026-11-27-turnip-inputs-are-public.md](results-2026-11-27-turnip-inputs-are-public.md), which established that
"UniXNU-turnip" is the public fork `Droid-VM/UniXNU` at `527b8b7a3119`.

## The work tree

`Droid-VM/mesa` `cecc0e96` (106 MB tarball) plus that commit's `patches/mesa` plus the k27 tree's `patches/mesa-macos`:
applied in the published alphabetical order it stops early, because `turnip-builtin-shader-pack` rewrites the `libtu_files`
line that `turnip-darwin-kmd` adds and lands its `meson.options` hunk at the end of the file (right after
`offline-adreno-compiler-probe`'s two options), and `turnip-darwin-deadline-queue` patches the `tu_knl_darwin_virtgpu.cc`
that `turnip-darwin-virtgpu` creates. Rather than guess an order, the reconstruction picks it: take the first remaining patch
that checks clean; where several fit, apply each in turn (`patch -R` to undo), count how many of the others then fit, and keep
the one that unblocks the most, ties by published name. Result: **23 patches applied, none with fuzz**, order in
`/tmp/mesa-order.log`, tree at `/Users/zhangqi/Codex/turnip-rebuilt/.cache/mesa-work` (12,842 files). Two published patches
never fit at any point in that search - `turnip-gmem-mutable-swap` and `turnip-render-mode-for-attachment-memory` - and the
21 that did is exactly the count `tooling/metal/UPSTREAM` records for the pinned work tree, so those two are not part of it.
The stronger check came after: **all 15 of this repository's own `tooling/metal/turnip-a830/mesa/*.patch` apply cleanly on
top**, and they were written against the VM-era tree.

`DroidVMVirtgpuABI.h` (UPSTREAM: `macOS-DroidVM out/x12377/src-gpu/drivers/NativeAdreno`) is not a published file, but it is a
published product: applying the k27 tree's six public `patches/droidvm-gpu/NativeAdreno/*.patch` to `Droid-VM/NativeAdreno`
`main` with `git apply` creates it (0001 adds it, 0002/0003/0006 widen it). Placed at
`/Users/zhangqi/Codex/k27-tree/out/x12377/src-gpu/drivers/NativeAdreno/`.

One host tool was missing: Mesa's builtin shader pack asks for `glslangValidator` (the cross file declares no `[binaries]`, so
meson needs it on the build PATH). Installed the Khronos macOS universal release (`Glslang Version: 11:16.6.0`) as
`/Users/zhangqi/bin/glslangValidator` (a symlink to `glslang`); the build must be started with that directory on `PATH`.

## What came out

```
out/turnip-a830/build/src/freedreno/vulkan/libvulkan_freedreno.dylib
17,770,312 bytes  sha256 6eeb3d2dffa4f7f709cc976235f2f71d3459b89c2bcc0527fa296b762cd069ac  Mach-O 64-bit dynamically linked shared library arm64e
PASS: 5633 weak flat-namespace imports of 5907 bound to the image itself
```

That is the A830 Vulkan driver, arm64e, built and post-processed by `tooling/metal/build-turnip-a830.sh` with
`TURNIP_TREE=/Users/zhangqi/Codex/turnip-rebuilt`, on this Mac (Xcode with the SDK at `MacOSX27.2.sdk`, target
`arm64e-apple-macos14.0`). The script then stops at `no bridge at
/Users/zhangqi/Codex/k27-tree/out/metal/bridge/libnative_metal_bridge.a` - the one input that has to be built.

## The bridge's inputs, each checked public

`scripts/build-metal-bridge-macos.py` of the published k27 tree needs, and this host can get:

| Input | Where |
| --- | --- |
| `drivers/NativeMetal/bridge/NativeMetalBridge.rs`, `bridge/patches/*.patch` (7), `shaders/scanout_convert.ll` | `Droid-VM/NativeMetal` (public, `main` lists all of them) |
| `patches/metal2vulkan/*.patch` (the board's, applied between NativeMetal's and ours) | `Droid-VM/UniXNU` branch `wip/droidvm` - 28 files |
| our own `patches/metal2vulkan-macos` | the k27 tree (present) |
| the translator at pin `963f0e64f103d2e682cd0cc454a8fe50f52c8cbb` | `steelbrain/metal2vulkan` (public) |
| `configs/arm64e-apple-darwin.json` (the pauthtest ABI target spec, `-Zbuild-std`) | the k27 tree (present) |
| `llvm-as` for the three `.ll` fixtures | LLVM 21 on the author's Linux; on this Mac Apple clang can assemble `.ll` with `-x ir`, and that substitution has to be made explicit before use |
| nightly Rust with `rust-src` | `static.rust-lang.org` answers from the Mac but slowly, so the toolchain is fetched where the network is fast and copied over |
| `SDKROOT` | this Mac's own macOS SDK instead of `toolchains/sdks/MacOSX26.1.sdk` |

Nothing on this list is behind a private repository. The script itself is written to run on Linux (LLVM 21's `clang` for C and
assembly of the dependencies), so building it on macOS is a port, not a run: same target spec and same patches, different host
toolchain. That is the next piece of work, and after it the plugin link (`tooling/metal/build-turnip-a830.sh` continues into the
frontend and plugin), then staging `NativeAdrenoMetal.bundle` into a seed and taking the GPU on this unit - none of which has
been on the tablet yet.
