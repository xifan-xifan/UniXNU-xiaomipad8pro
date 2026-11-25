# 2026-11-26 - the kext stage stops at two generated header trees from the private bootstrap

Follows [results-2026-11-26-k273-kernel-and-gpu-gap.md](results-2026-11-26-k273-kernel-and-gpu-gap.md). Host: the desk Mac,
upstream `d70c16ce`. The tablet was untouched.

## What ran

`tooling/k27/build-native-image.sh` with `PIANO_STOP_AFTER=kexts` and `PIANO_K27_TREE` = our locally prepared 27.3 clone
(`out/k273/k27-tree`, the tree that produced `kernel-k273-0075-s0001-native.macho`). Two host-side path facts came out first and
are worth recording, because both failures look like missing inputs and are not:

- the kext log lines are written relative to **this repository's** root (`> out/k27/build-native-<name>.log` after `cd $D`), so
  `out/k27` must exist in the checkout, not in the k27 tree;
- `ARMPlatformStandIn`, the first kext in `configs/k27-native-kexts.txt`, builds fine here: 5 objects in `out/native-kexts/ARMPlatformStandIn`.

The stage then stops on the second kext:

```
package/NativeUfs/NativeUfs.cpp:7:10: fatal error: 'IOKit/storage/IOBlockStorageDevice.h' file not found
```

## Why: two include trees the published sources never create

`tooling/k27/build-native-ufs.sh` (and seven other kext scripts) pass

```
-I $PIANO_K27_TREE/out/x12377/storage-abi/include   -I $PIANO_K27_TREE/out/x12377/xnu-m/osfmk
```

and eight of them also pass `-I $PIANO_K27_TREE/out/x12377/inc-gfx`. Neither directory exists in the public k27 tree, and nothing
in the public tree, in this repository or in `Droid-VM/UniXNU` creates them:

| Path | Present here | Anywhere in the published sources |
|---|---|---|
| `out/x12377/storage-abi/include` (a `IOKit/storage/...` tree) | no | no - only referenced by the build scripts and read as `build/storage-abi/interface-audit.json` by `$PIANO_K27_TREE/scripts/audit-reference.py` |
| `out/x12377/xnu-m/osfmk` | no | no |
| `out/x12377/inc-gfx` | no | no |

The xnu sources are not a substitute: `out/k27/xnu/iokit/IOKit` has **no `storage` subdirectory** (0 entries), and the installed
kernel headers (`installhdrs` into the fakeroot) put those files under `Kernel.framework/Versions/A/Headers/IOKitUser/`
(`IOBlockStorageDevice.h` and 23 siblings), which is a different spelling from the `<IOKit/storage/...>` include NativeUfs and
NativeRamBlock use. `installhdrs` also produced no `IOKit/storage` directory under `Headers` or `PrivateHeaders`. The KDK of
`Kernel_Debug_Kit_27.2_build_26C5035r.dmg` (5.2 GB expanded) contains 76 headers in total and no IOKit tree, so the KDK is not
where those headers come from either.

So `storage-abi` is a header shim the owner's bootstrap environment holds privately, in the same family as gap G1's other local
pieces (the DroidVM bootstrap of the 12377 environment). It is not listed in `skills/install-piano-macos` "Known gaps", which is
worth correcting on our side of the fence: the skill's section-3 blocker list says the kernel/KC/RAM images are "blocked without the
k27 tree, the base trust bundle, a Data seed, the Turnip tree", and we have closed the tree, the trust bundle and the seed - the
remaining blockers of that stage are `storage-abi`/`xnu-m`/`inc-gfx` for the kexts, and `UniXNU-turnip` for the Metal plugin.

## What this changes in the plan, precisely

- The kernel is built (0066 and the 27.3 line's 0075), the KC base is ours, the root image is ours, the trust bundle and Data seed
  are ours. The next missing input is the **native kext object set**, which the kernel collection links; without it there is no
  bootable collection, hence no new RAM boot of our own build. This is now the top of the build queue, ahead of the desktop.
- The set is not all-or-nothing: `ARMPlatformStandIn` compiles from published inputs; the eight scripts that need `storage-abi` or
  `inc-gfx` do not. Reconstructing `IOKit/storage/*.h` is possible from the IOKitUser headers plus the public IOKit sources, but the
  ABI of a storage-ABI shim has to match what `NativeUfs`/`NativeRamBlock` expect (`audit-reference.py` compares against a recorded
  `interface-audit.json`, which we do not have), so a reconstruction of ours would be unverified. The honest move is to ask upstream
  for these three directories - they are headers, not secrets, and G1's own text already treats the bootstrap as something that
  could be published as a hash-manifest - and to keep our reconstruction as a labelled fallback rather than a silent substitute.
- This does not stop the parts of the goal that do not need a new build: the selector/switch path already validated on the tablet
  runs against existing images, and LUN 4's GPT head can still be collected read-only from the maintenance RAM image.

Nothing was written to the tablet.
