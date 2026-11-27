# 2026-11-27 - the Metal bridge's inputs, pinned to one coherent line, and built on this Mac

The gate for the real GPU has been `out/metal/bridge/libnative_metal_bridge.a`, which `tooling/metal/build-turnip-a830.sh`
links into `NativeAdrenoMetal.bundle`. It was only ever taken prebuilt from the author's private host (UPSTREAM records its
sha256 81d46fae51eea467eac152cbc7817658986c9d7bb25ae54fa40cba0c3b516d4d). Everything it is built from is public, but the
series has to be assembled from the right revisions, and three different revisions are in play on this machine. This file
records which one is ours.

## The published recipe, and what it reads

The pinned k27 tree's `scripts/build-metal-bridge-macos.py` builds `drivers/NativeMetal/bridge/NativeMetalBridge.rs` (the
metal2vulkan runtime behind a C ABI) as a static library for `arm64e-apple-darwin`, from:

- `sources/UniXNU/.cache/gpu-reference/metal2vulkan`, at the translator commit recorded in
  `sources/UniXNU/scripts/data/gpu-metal-vulkan-inputs.json`;
- the patches in `drivers/NativeMetal/bridge/patches`, then the board's in `sources/UniXNU-turnip/patches/metal2vulkan`
  (a `wip/droidvm` worktree), then this project's `patches/metal2vulkan-macos`;
- the tier-3 target spec `configs/arm64e-apple-darwin.json`, nightly Rust with `-Zbuild-std`, and the pauthtest ABI with
  `-Zpointer-authentication=-init-fini`.

`configs/sources.lock.json` of that tree pins `sources/UniXNU` to `Droid-VM/UniXNU baeb8f0c`, whose record pins translator
`720ef1ce` - the commit UPSTREAM records for the published bridge - with `drivers/NativeMetal 6dcd4cbb` (5 bridge patches, no
`shaders/scanout_convert.ll`). That lock is older than the recipe it is read with: the script needs
`drivers/NativeMetal/shaders/scanout_convert.ll`, which 6dcd4cbb does not have. So the lock line cannot be built as written.

## The line that is ours: UniXNU 527b8b7, AdrenoMetal fb063de, metal2vulkan 624bab39

`tooling/metal/UPSTREAM` already builds Turnip from `Droid-VM/UniXNU 527b8b7a31192df14b96d10a70c339845ed2cd12` (`patches/mesa` 21,
Mesa `cecc0e96`). That same revision pins `drivers/AdrenoMetal` at `fb063de55cbb5429bde12a8bb55538726f8b1a0f` - the
`drivers/AdrenoMetal` UPSTREAM names for the plugin frontend - carries `patches/metal2vulkan` (21), and its
`gpu-metal-vulkan-inputs.json` pins the translator at `624bab3918de5b978e1101517abca2b4fa426565`. The bridge and the frontend are
then the same revision, which is what `build-turnip-a830.sh` wants (`U=$T/drivers/AdrenoMetal`).

Verified on this machine, from GitHub's source archives (no git transport works here for these repositories):

| input | revision | check |
| --- | --- | --- |
| `steelbrain/metal2vulkan` | `624bab3918de5b978e1101517abca2b4fa426565` | README `0df40b72...`, Cargo.toml `e2fc3743...`, `crates/metal2vulkan/src/device/policy.rs` `1e07afdb...`, all as recorded |
| `UniXNU/AdrenoMetal` as `drivers/NativeMetal` | `fb063de55cbb...` (2026-11-06) | 24 `bridge/patches`, `shaders/scanout_convert.ll` present |
| board `patches/metal2vulkan` | `Droid-VM/UniXNU 527b8b7` | 21 patches, fetched at that revision rather than the branch tip (the tip carries 28) |
| this project's `patches/metal2vulkan-macos` | k27 tree | `0001-ycbcr-planes-only.patch` |

With that set the whole series applies cleanly and in order: 24 bridge patches, then 21 board patches, then ours.

## Two things that only show up when you try

`git apply` failures here were a Windows artefact, not a version mismatch: `core.autocrlf=true` gives every checked-out text
file CRLF, and a CRLF `.patch` will not apply to LF sources. The `out/deps-mac/AdrenoMetal` work tree is affected; GitHub's
source archives and `git checkout-index -c core.autocrlf=false` are not. Ship patches as LF.

The published script drops `0020-written-host-page-textures-in-place-at-any-size.patch` because the Mesa at its revision had no
`turnip-render-mode-for-attachment-memory.patch`. At this line the series is coupled: with 0020 out, the board's
`0013-trim-kept-memory.patch` no longer applies. So the whole series goes in, and the Mesa patch it needs has to go into the
Turnip work tree as well - `turnip-render-mode-for-attachment-memory.patch` is one of the two files
`tooling/metal/build-mesa-work.sh` reports as left over against the pinned tree, so it is on this machine already. That Mesa
rebuild is owed before the bundle goes near the tablet.

## Host port

`tooling/metal/build-metal-bridge-macos-host.py` is that script with the host substitutions only - same source pin, patch set,
target spec, `RUSTFLAGS` and release profile: Homebrew LLVM 23.1.6 for `llvm-as`/`clang`/`llvm-ar` instead of
`/usr/lib/llvm-21/bin`, the Xcode macOS 27.2 SDK instead of the tree's `MacOSX26.1.sdk`, and a plain nightly toolchain at
`/Users/zhangqi/rust-nightly` (nightly `2026-11-25`, `6c709eb428d472a3c3d93681c1ba37d44a1c27cf`, with `rust-src` for
`-Zbuild-std`) instead of rustup's `+nightly`. Where the translator source came from an archive rather than a git object,
the recorded file hashes are the pin check, and only with `NMB_ALLOW_HASH_PIN=1`, so the substitution is deliberate and logged.
`tooling/metal/metal2vulkan-pin.json` is this repository's copy of the record, because the k27 checkout here carries `drivers/`
only, not the whole monorepo.