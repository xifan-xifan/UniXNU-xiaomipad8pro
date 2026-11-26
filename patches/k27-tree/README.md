# The k27 tree: our delta on top of macOS-DroidVM

The k27 tree is the kernel and KC-builder tree the native build reads (`PIANO_K27_TREE`, `tooling/k27/k27-tree.sh`). It is
[Droid-VM/macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) at a pinned commit plus the two patches here. Only the
patches are in this repository; the upstream tree is fetched by its commit.

```sh
tooling/k27/fetch-k27-tree.sh DEST      # clone upstream at the pin, git am patches/k27-tree, check the result's tree id
```

| | |
| --- | --- |
| Upstream | `https://github.com/Droid-VM/macOS-DroidVM.git`, commit `abcbc18f4791b9cd4a3b2a52c92ef8382435376d` (2026-11-07, "patches/xnu-12377 0045-0049: third-party arm64 apps ...", on its `main`) |
| Result | git tree `544f9468b660aabbc73ee24638adabd14c274671` (`unixnu.lock` `[pin."k27"]` `tree`; `fetch-k27-tree.sh` refuses another) |
| 0001 | build entry points (`build-k27-piano.sh`, `build-metal-plugin-piano.sh`, `scripts/piano-frontend-patch.py`), `k27/kc-build27-piano.py`, `k27/build-native-display-piano.sh`, the KC keep list, overlay edits (DroidVMDisplay, NativeAdrenoMetalPlugin), `overlay-piano/`, droidvm-audio 0004, droidvm-video 0013-0017, `patches/xnu-12377` 0050-0052 |
| 0002 | the native variant (`K27_VARIANT=native`, `XNU_VM_GUEST=0`), `patches/xnu-12377` 0053-0054 (the same diffs as `patches/k27/0053-0054`) |

## Provenance

Both patches come from the maintainer's local checkout of the tree (branch `el1/native-variant`): 0001 is its commit `a1e3f4e`,
0002 its commit `11ac316` (`[pin."k27"]` `local`; several `package/*/UPSTREAM` files name `macOS-DroidVM 11ac316`: that is this
local commit, i.e. upstream `abcbc18` + these two patches, not a commit on GitHub). Changes made for publishing, nothing else:

- left out of 0001: `boot-disk-piano.sh`, `iterate-k27-piano.sh`, `restart-vm-piano.sh` and a `.bak` copy of the last: they
  drove the retired Gunyah VM on a host of the local network (addresses, accounts) and no native build step reads them;
- the author of both commits and the `From:` header of the kernel patches 0050-0054 inside them: the repository's author
  identity (the local commits carried two older addresses of the same person).
- in 0001: a comment in `build-metal-plugin-piano.sh` named a host of the local network; it says "Linux 构建机" (a Linux build host)

Upstream's own kernel patches (`patches/xnu-12377` 0001-0049, among them six authored `t <t@t>` upstream) come with the fetch and
are not republished here.

## What is not here

Nothing under the tree's `out/`: the xnu checkout with the series applied (`out/k27/xnu`), the kernel objects and their compile
recipes, the generated headers (`out/x12377/inc-*`, `storage-abi`), the fakeroot, the KDK copy, the KC base (`out/kc/kc27.bin`,
Apple's payload) and every kernel and collection built. `k27/segaddrs.json` / `segaddrs.sh` are the values committed upstream
and in 0001; the build regenerates them for the collection it links against (`k27/kc27-layout.py --write`,
`tooling/k273/prepare-k27-tree.sh`). The KC-builder patches of `patches/k27-kc/` and `patches/k273-kc/` stay separate and are
applied as before.

## Updating

Make the change in a fetched tree, commit it there (author: the repository's identity), then
`git format-patch --no-signature --zero-commit -o patches/k27-tree abcbc18f4791..HEAD` and put the new `git rev-parse HEAD^{tree}`
into `unixnu.lock`. A new upstream base is a new `commit` in the lock and a rebase of these patches.
