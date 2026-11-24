# Native k27 build environment audit (2026-11-25)

Question: can a source-level candidate (for example the retained ufs185 publication change) be
compiled anywhere reachable right now?

## This workspace

- `PIANO_K27_TREE` is unset and there is no `~/.config/piano/env`.
- `out/kc/` does not exist, so the KC base `out/kc/kc27.bin` is not present.
- `out/x12377/` does not exist, so the generated storage ABI and IOKit headers are not present.
- What does exist: `out/k27-smprobe/piano-ufs266.kc` (147,095,552 B) and the earlier ufs264
  collections, i.e. finished kernel collections, not build inputs.

## The Mac (`mac-lan`, 192.168.31.183)

- No `~/.config/piano/env`; no `*k27*` tree in `~`, `~/Codex` or `/private/tmp`; no `x12377`
  directory; no `kc27.bin` anywhere within depth 3 of `~`.
- Present: `~/Codex/k273-esp/inputs/piano-ufs264.kc` (the shipped collection) and
  `~/ufs151/NativeUfs.o` (a single kext object from an earlier compile).
- `~/Codex/UniXNU-xiaomipad8pro` is at `2438556` (the plan baseline) with uncommitted local edits
  to `package/NativeUfs/*`, not at the current `main`, so it is not a drop-in build tree either.

The 2026-11-24 ufs184 audit already noted the consequence from the other direction: the object
sets and base KC that linked the 18x series "are no longer the ones on the host".

## What this means

`tooling/k27/build-native-image.sh` and `tooling/k27/build-native-ufs.sh` both stop at
`k27-tree.sh` without `PIANO_K27_TREE`, and even with it the tree must carry
`out/x12377/{storage-abi,xnu-m}/include` and `out/kc/kc27.bin`. So no source change can currently
be turned into an image: the retained ufs185 candidate, and any new production-sequence candidate,
are unbuildable in this workspace and on the Mac.

Images can still be produced the way ufs265 and ufs266 were: patching an existing collection
(`out/k27-smprobe/piano-ufs266.kc`, byte-verified against its boot image) inside zero-filled
padding, with the patcher and the 1,657-byte hole in `docs/results-2026-11-25-startmatching-probe.md`.
That path supports trampolines, stubs and data slots, so a binary fix for a confirmed blocking
point is still reachable; a structural rewrite of the publication sequence is not.

## To restore the build path

Needed, in one place: the xnu-12377 k27 tree at `11ac316b789d81fc9f734e15645456f536c05141` with
the baseline series 0001-0052 applied, the generated headers under `out/x12377`, and the KC base
`out/kc/kc27.bin`. `patches/k27` (0053 onward, with `series`) is already in this repository.
`docs/bringup-debugging.md` section 4 is the procedure. Until those are supplied, only the
existing collections can be tested.

Nothing was booted, built or written for this audit.
