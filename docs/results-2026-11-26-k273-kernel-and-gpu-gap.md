# 2026-11-26 - the 27.3 kernel and the host verdict; what really gates the desktop

Follows [results-2026-11-26-root-image-built-and-write-plan-hazard.md](results-2026-11-26-root-image-built-and-write-plan-hazard.md).
Built on the desk Mac from upstream `d70c16ce`; the tablet was untouched (no write, no reboot).

## Upstream's own host check passes on this machine

`bash tooling/repro/check-host.sh` -> **`summary: 0 FAIL, 1 WARN, 1 GAP`**. The WARN is `fastboot` (the tablet steps, not the build -
this host reaches the tablet from Windows). The GAP was the Data seed check, which passes when `K273_SEED` names our own seed. It
also reports `ok ld-classic in the k27 tree (out/x12377/ldc/ld)` - the `stage_ld` symlink of our own ld64-957.1 - and
`ok UNIXNU_RESEARCH checkouts`, so the FD inputs are present here too.

## The 27.3 line's kernel, built here

`tooling/k273/prepare-k27-tree.sh 0066` (with `PIANO_K27_TREE` = our public k27 tree, `K27_TREE_TOP=0066`, `LD_CLASSIC` as above):

```
out/k273/k27-tree/out/k27/kernel-k273-0075-s0001-native.macho
27,669,352 bytes  sha256 308a6dbf0101937d81ccbc9b66fea9eb3999bbc5d75fb4630347f2fd95abe516
0 x LC_FILESET_ENTRY (a plain kernel Mach-O)
```

The clone's xnu history carries the k27 patches past 0066 as its own commits (0067-0075, including
`0075 k27 user VA 47-bit`, which is what Chrome's V8 sandbox needs), and the KC base of 27.3 (`kc/kc273-mac17j.bin`, from Apple's
own kernelcache member) decided the segment addresses (`k27/segaddrs.sh` rewritten, no layout CONFLICT). One detail to keep in
mind when reading artifacts: `XNU_BUILD_STRING` is stamped into an object, so a relinked kernel still prints the timestamp of the
compile that produced that object - the file mtime, the object timestamps and the link log are the evidence of when a link happened,
not that string.

## Gap G4 in exact terms: one input is not published, and it gates only the desktop

`tooling/metal/build-turnip-a830.sh` reads its Mesa tree from `$TURNIP_TREE/.cache/mesa-work` and its frontend from
`$TURNIP_TREE/drivers/AdrenoMetal`, i.e. from **`UniXNU-turnip`**. Checked from this host against GitHub (distinguishing "no such
repository" from network failure by retrying until an error class repeats):

| Repository | Reachable |
|---|---|
| `Droid-VM/mesa` | yes - 14 heads, and the pinned commit `cecc0e96d7193c72356a31abb0cc589e4fc0415e` fetches (113 MB, verified as a commit object) |
| `Droid-VM/UniXNU` | yes - and it carries `patches/mesa/` (8 patches, the `turnip-darwin-*` ones) and `scripts/build-turnip-darwin.py` |
| `UniXNU/metal2vulkan` | yes (the bridge's source, pinned by the k27 tree's `scripts/data/gpu-metal-vulkan-inputs.json`) |
| `Shad0w23333/Adreno830` | yes (`NativeAdrenoABI.h`) |
| `Droid-VM/UniXNU-turnip`, `UniXNU/UniXNU-turnip` | **no such repository** (`git` answers with the credentials prompt GitHub gives for a nonexistent path, on every retry) |

So my earlier note ("G4 might already be public") was half right and is corrected here: the *patch series* this repository applies
(`tooling/metal/turnip-a830/{mesa,frontend,plugin}/*.patch`) is published, and Mesa itself is published, but the tree they apply to -
UniXNU-turnip, with its `drivers/AdrenoMetal` frontend and its 21 `patches/mesa` - is not. The k27 tree also expects a prebuilt
`out/metal/bridge/libnative_metal_bridge.a`, which is absent from the public tree; its builder
(`$PIANO_K27_TREE/scripts/build-metal-bridge-macos.py`) is present and reads `UniXNU/metal2vulkan` at its pin, so the bridge is
buildable here (it needs a Rust nightly with `arm64e-apple-darwin`, tier 3).

Consequence for the plan, stated precisely: **without UniXNU-turnip there is no A830 Metal plugin, hence no WindowServer desktop**;
everything below the desktop - kernel, KC, kexts, RAM image boot, the boot-flag switcher, Data persistence, recovery - does not
depend on it. The switch acceptance this goal must run therefore does not wait on G4, while any "reach the desktop" milestone does.
Reconstructing Mesa from `Droid-VM/mesa` + `Droid-VM/UniXNU patches/mesa` + the published patch series is plausible but is not the
same tree the owner builds (21 patches vs 8 published there), so it would be our own untested variant; it is not a shortcut to take
silently.

## Unit profile: our own write plan, recorded

`out/unit-profile/piano-7ccdd855-write-plan.json` (not committed; `out/` is ignored) records this unit's geometry and the corrected
plan: `linux_data` first LBA `107512576`, root image `5,111,552` blocks -> last `112,624,127`, `11,665,664` blocks free in the
partition. The reference plan (`71789568..76901119`) is marked in the file as never usable on this unit, for the reason given in the
previous result file. `esp` (37) sits *after* `linux_data` here (`124289792`), where the reference unit's tooling carves the ESP from
the end of `linux_rootfs` - so the ESP placement is another per-unit difference for the write tooling to carry.
