# Where the interrupt-capable kernel came from, and whether this host can rebuild one (2026-11-25)

The remaining wall named by `docs/results-2026-11-24-timer-masked-polling.md` and
`docs/results-2026-11-25-route-progress-comparison.md` is real interrupt delivery: nothing clears `DAIF.I`/`DAIF.F`, so
`thread_block` deadlines never return, `waitForMatchingService` never completes, and the root volume is never matched. That
code is XNU-side (`piano_gicv3_*`, `piano_delay_timer_service`), so the question this note answers is narrow: can this host
build a kernel that contains it. Device action: none. HyperOS was up the whole time (`adb` shows `7ccdd855`).

## 1. The XNU build ran on this host (corrects the environment audit)

`docs/results-2026-11-25-build-environment-audit.md` concluded source-level kernel building was unavailable here because
`PIANO_K27_TREE` is unset and there is no `out/kc/kc27.bin`. That is true of the k27 *tree wrapper* `tooling/k27` expects
(`out/k27/xnu` plus `sources/UniXNU`: nothing like it exists in WSL), but it understates the host. WSL `UbuntuPiano` carries
the author-style build environment and a built 27.3 kernel:

| Path (WSL `UbuntuPiano`) | Bytes / sha-256 | What it is |
| --- | --- | --- |
| `/opt/unixnu-linux` | clang, lld-link, llvm-lib, mig, migcom, iig | the XNU cross toolchain |
| `/root/piano-native/UniXNU` | 2.1 GiB, `profiles/`, `patches/xnu-12377/` | the `unixnu` build system and its patch series |
| `/root/piano-native/xnu-k27-final` | 104 MiB (plain source, no `.git`) | the k27 XNU source the old `build-piano-kernel.sh` points at |
| `.../UniXNU/out/ipsw/26C5035r/kernelcache.decompressed` | `d1003b51...` | byte-identical to the pinned 27.3 mac17j kernelcache in `docs/k273-line.md` |
| `.../UniXNU/out/piano/macos-27.3-26C5035r/piano_macos/kernel/kernel.macho` | 142,393,344 / `afe7646e...` (2026-11-16 04:11) | a kernel built here; `kernel.json` says `format: MH_FILESET`, `inputs.kernelcache_sha256: d1003b51...`, `entry: com.apple.kernel` |
| same directory | `xnu-patches.list` (136 patches), `kernel.map.json`, `kernel.json` | the recorded input set of that build |

So the capability exists; what is missing is a tree shaped the way `tooling/k273/prepare-k27-tree.sh` wants (it clones
`$PIANO_K27_TREE`, applies `patches/k273*` in the clone's `out/k27/xnu` git repo and relinks).

## 2. No source tree or kernel binary here has the interrupt path

- `grep -rln piano_gicv3_init` over `xnu-k27-final`, `xnu-k273-work` and `xnu-k27-patched/osfmk`: zero. The paclr71 GIC patch
  was never committed to any tree on this host.
- The 2026-11-16 `kernel.macho` and `kernel-from-payload.macho` (142,442,496 / `d36b8047...`): zero hits.
- Every `*.macho` in WSL `/tmp` (2,131 entries, no `piano-paclr*` directory left): zero hits.
- Mac `~/Codex` (including `k273-esp`, dated 2026-11-24) and `/private/tmp`: no `kernel.macho` at all, and no
  `piano-paclr*` directory.

The only bytes on these machines carrying the interrupt code are two built collections from the paclr line:
`out/k27-smprobe/piano-ufs266.kc` (147,095,552, sha-256 `9f276dfff4a35ef6ed89ffd2c8a535d3742ed4f843623b744f310c8fa68aca64b`)
and the Mac's `~/Codex/k273-esp/inputs/piano-ufs264.kc` (147,095,552, sha-256 `529265c7839c51fe4d643af3228f4061297dd5d2ed5cce3e585fbc6bcca14a72`).
Scanning `piano-ufs266.kc` byte-wise puts `_piano_gicv3_init`, `_piano_gic_irq_handler`, `_piano_delay_timer_service` and
`_piano_gicv3_timer_selftest` in one sorted symbol run, next to `arm_timer.c` and `pg: enable enter gic %d` in the kernel's
own `__TEXT`/`__LINKEDIT`, and `piano-gic`/`piano-gic-irq` in another string run. Together with `rg` finding no
`piano_gicv3_*` anywhere in `package/`, that confirms the summary in
`docs/results-2026-11-25-ufs269-base-kernel.md`: it is kernel code, and no kext change can supply it.

## 3. What the current 27.3 kernel is, then

`out/no-demura-control/blendfix.macho` (28,034,000) is what the last source-linked collections were built with
(`blendfix.argv.json`), and that link ran in WSL against `/tmp/piano-verified-collection.68bZb6` (still present) with
`/tmp/piano-linkinfo-relink.yab5k88c/k27/kc-build273-piano.py`. It has no GIC or architected-timer service, which is why
every root wait in it blocks forever. Its own build provenance is not a tree on this host, so it cannot be patched at
source in place either.

## 4. Two routes, and what each costs

| Route | Inputs on hand | Cost / risk |
| --- | --- | --- |
| A. Re-author the interrupt patch and build a kernel | `xnu-k273-work`/`xnu-k273-final` + toolchain + the 27.3 base; `docs/results-2026-11-23-paclr71-gicv3.md` (GICv3 init, distributor/redistributor addresses, PPI 27) and `docs/results-2026-11-24-timer-masked-polling.md` (masked polling, the `msr daif` restore trap, Group 0/FIQ as the remaining step) specify the code | full XNU build per iteration; the built kernel must then be relinked at the 27.3 collection's segment addresses (`k27/kc27-layout.py`) and re-pass `check-native-kexts.py`; nothing here proves the tree still builds, so that has to be measured before any device time |
| B. Recover the kernel out of a paclr collection | `piano-ufs266.kc` / Mac's `piano-ufs264.kc` | no extraction path exists in `tooling/`, and a kernelcache's fileset header is not what `kc-build273-piano.py` takes as `KERNEL`; cheaper per try, unknown feasibility |

Route A is the one the goal needs regardless (the patch has to be re-authored at some point to land `DAIF.F` unmasking in the
tree), so route A's first step is a build that changes nothing: build the tree as it is, into a new output directory, and
measure whether it produces a linkable kernel. The 2026-11-16 output tree is the only copy of that build and must not be
overwritten.


## 5. The series does not apply to any XNU source on this host (measured, 2026-11-25)

The probe copied each candidate tree under `/root` and tested `patches/k273/0001` plus `patches/k27/0053-0073` (21 patches):

| tree (WSL) | applies | already in source | fails |
| --- | --- | --- | --- |
| `xnu-k273-work` (as-is, patch text normalized to LF) | 4 | 1 | 16 |
| `xnu-k27-final` | 2 | 6 | 13 |
| `xnu-k27-patched` | 8 | 0 | 13 |

The patches themselves carry CRLF context (a hunk of 0066 quotes `char buf[BOOT_LINE_LENGTH];` with a trailing CR), which says the
tree they were made from lived on a Windows volume; and after normalizing line endings, 13 patches still fail against every
local tree, so none of them is that base. `xnu-k27-final` having six of the series already in its source matches a tree that got
as far as roughly 0052 plus a few of these, not past 0073.

Two consequences, both narrowing route A:

1. Rebuilding the 27.3 kernel means first re-deriving the base the series was written against. That is not a "set
   `PIANO_K27_TREE` and run `prepare-k27-tree.sh`" situation; it is reconstructing a tree, and the mismatch is in the
   tree's own source, not in the patch format.
2. The shipped upstream release images cannot settle the interrupt question either: scanning `out/inputs/v110/release/boot/boot.img`,
   `vendor_boot.img`, `boot-ram-check.img` and `out/persistent/boot-piano-macos-persistent-v4-avb.img` (all ~100 MB) for
   `piano_gicv3_init`, `piano_gic_irq_handler`, `piano_delay_timer_service` and `piano-gic` gives zero, but their FAT payloads
   are compressed, so zero hits here is absence of evidence, not evidence of absence. Reading the payload out with
   `tooling/bootimg` and decompressing it is the cheap way to learn whether the author's shipped kernel already carries the
   interrupt path, and it decides whether route B (extract a kernel from a paclr collection) is worth anything at all.
