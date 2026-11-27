# The A830 Metal inputs are public: `UniXNU-turnip` is the public fork `Droid-VM/UniXNU`, not a private repository

`results-2026-11-26-k273-kernel-and-gpu-gap.md` concluded that the native GPU line is blocked because `UniXNU-turnip` is not
public. That conclusion does not hold. `tooling/metal/UPSTREAM` says where the turnip patches come from:

> Mesa (MIT): https://github.com/Droid-VM/mesa cecc0e96d7193c72356a31abb0cc589e4fc0415e (26.3.0-devel), as the VM-era build used it:
> UniXNU-turnip's work tree `.cache/mesa-work` with UniXNU-turnip's `patches/mesa` (21, **Droid-VM/UniXNU 527b8b7a3119**)
> and macOS-DroidVM's `patches/mesa-macos` applied.

`Droid-VM/UniXNU` is a **public fork of `UniXNU/UniXNU`** ("Run iOS and macOS on non-Apple ARM64 hardware through UEFI"), and
the commit it names, `527b8b7a31192df14b96d10a70c339845ed2cd12` (2026-11-05), resolves in both the fork and the upstream
repository (GitHub API: HTTP 200 for each). Its own commit message is the Turnip-darwin weak-import change and names
`scripts/macho-weak-self-binds.py`, the script `tooling/metal/build-turnip-a830.sh` reads from the turnip tree. So
"UniXNU-turnip" is that fork's turnip work, reachable without credentials. Checked at that commit through the contents API:

| What the build reads | Where it is |
| --- | --- |
| `patches/mesa` | 23 files at the pin, including `turnip-darwin-kmd`, `-virtgpu`, `-lazy-bos`, `-bo-cache-*`, `-deadline-queue`, `-va-exhausted-note`, `-queue-priority` and `mesa-darwin-total-memory-sysctl`. UPSTREAM counts 21; the list was not reconciled patch by patch. |
| `scripts/macho-weak-self-binds.py` | present (the post-link pass over the Turnip Mach-O) |
| `scripts/build-turnip-darwin.py` | present; `build-turnip-macos-piano.py` (whose meson options our script copies) is not at this commit, but our script already carries those options |
| `drivers/AdrenoMetal` (the Metal frontend) | submodule `https://github.com/UniXNU/AdrenoMetal.git`, which answers 200; a checkout of it is already on this build host (below) |
| Mesa `cecc0e96d7193c72356a31abb0cc589e4fc0415e` | `Droid-VM/mesa`, that commit answers 200 |
| kernel ABI | `Shad0w23333/Adreno830` `1a13210…` answers 200, and `package/PianoA830/adreno830/NativeAdrenoABI.h` is in this repository |
| Metal bridge | `steelbrain/metal2vulkan` `963f0e64f103d2e682cd0cc454a8fe50f52c8cbb` (`scripts/data/gpu-metal-vulkan-inputs.json` of the UniXNU monorepo), built by `scripts/build-metal-bridge-macos.py` of the published k27 tree |
| the kext | `package/PianoA830/` (Adreno830 copied in, MIT, changes marked `UniXNU-piano`), kext manifest `tooling/k27/kext/PianoA830.json` |

The build host also holds an older checkout of the monorepo with its submodules populated:
`/Users/zhangqi/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research/UniXNU` (`drivers/AdrenoMetal` at `cc0a4e8`,
`patches/mesa` with 17 patches at its own commit) and `unixnu-research/Adreno830`, `AdrenoMetal`, `DarwinBoot`, `RamBlock`,
`XnuPrelink`, `ArmCpu`, `Mu-Silicium-piano` and friends. That is a lead for cross-checking, not the pin: the pin is the commit above.

What is **not** yet shown, in order: that `Droid-VM/mesa` `cecc0e9` plus those patches plus our `tooling/metal/turnip-a830/mesa/*.patch`
configure and compile on this Mac (Xcode 26.6 with `ld-classic` at `/Users/zhangqi/bin/ld-classic`), that the bridge builds here,
that the resulting `NativeAdrenoMetal.bundle` loads against the pinned 27.3 frameworks, and that PianoA830 drives this unit's GPU
(it has never been on the tablet). A shallow `git fetch --filter=blob:none` of the pin from this network stalled
(`curl 28 … Less than 1000 bytes/sec`), so the tree is being taken path by path instead of as a clone.

Consequence for the plan: the desktop does not depend on a repository we cannot reach. `docs/drivers/soft-metal.md` 11 and 12
leave the CPU-composited path at a backdrop assertion it measured and did not clear, and its own conclusion is that Metal
compositing is the way past it; the A830 path's inputs are public, so that becomes the mainline and the CPU path stays what
section 7 says it is - a diagnostic device, now a useful one (first frame, format, and swap accounting on this unit).
