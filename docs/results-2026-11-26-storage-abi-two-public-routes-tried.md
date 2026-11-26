# 2026-11-26 - the storage-abi include is a naming puzzle, not a missing package: two public constructions tried, both the wrong flavour

Follows [results-2026-11-26-lun4-head-and-kdk-recheck.md](results-2026-11-26-lun4-head-and-kdk-recheck.md). Host: the desk Mac,
upstream `23f6425f` fetched here. The tablet was not touched (it was already back in HyperOS 3.0.309, slot `_a`).

## One of the three "private directories" is only a name

`-I $PIANO_K27_TREE/out/x12377/xnu-m/osfmk` resolves once `out/x12377/xnu-m` is a symlink to the tree's own xnu
(`out/k27/xnu`), which is where `osfmk/` lives. That include is therefore a naming alias, not a private input; the alias is left in
place (`out/x12377/xnu-m -> out/k27/xnu`) and nothing else about the tree changed. `inc-gfx` (used by eight of the kext scripts)
is still unexplained.

## What `storage-abi` is not

The kext scripts read `<IOKit/storage/IOBlockStorageDevice.h>` from
`$PIANO_K27_TREE/out/x12377/storage-abi/include`. Two constructions from public inputs were tried, and both fail for a reason worth
recording because they look like they should work:

- **The macOS SDK.** `$(xcrun --show-sdk-path)/System/Library/Frameworks/IOKit.framework/Versions/A/Headers/storage/` really does
  hold 31 headers including `IOBlockStorageDevice.h`, and symlinking it into the shim gets the compiler to the real source. It then
  asks for `<TargetConditionals.h>`; symlinking the SDK's 216 top-level `usr/include` headers satisfies that, and the compile
  reaches `package/NativeUfs/NativeUfs.cpp:2853` before failing with 53 errors. The decisive one is
  `package/NativeUfs/RootPublication.h:81: comparison of distinct pointer types ('IORegistryEntry *' and 'NativeUfsBlock *')`:
  the SDK's `IOBlockStorageDevice` is the **user-space** class, whose base is user IOKit's `IOService`, so it is not on the kernel
  `IORegistryEntry` chain the driver assumes. Same file names, wrong flavour - the SDK tree cannot be the shim.
- **The tree's own `iokit/IOKitUser/`.** The 12377 tree spells the headers `iokit/IOKitUser/*.h` (23 files, and
  `installhdrs` exports them under `Kernel.framework/.../IOKitUser/`, which is why both spellings look promising). But
  `iokit/IOKitUser/IOBlockStorageDevice.h` is a single line, `#include <DriverKit/IOBlockStorageDevice.h>`, and no
  `IOBlockStorageDevice.h` under any `DriverKit` path exists in this Xcode's SDKs or in the fully expanded 27.2 KDK
  (`~/Codex/kdk/full`, 5.2 GB). The kernel-side class definition is not on this machine in any spelling.

Both experiments were reverted; the shim directory is gone and the tree is otherwise as it was.

## A build that got past this once, and the input that went with it

`~/Codex/UniXNU-xiaomipad8pro/out/native-kexts/NativeUfs-iobsd/NativeUfs.o` (198,144 B, 2026-11-23) is a real object of this
driver: `xcrun nm -m` shows 46 storage-related entries and undefined `IOBlockStorageDevice::gMetaClass`, `::requestIdle`,
`::doSetPriority(IOBlockStorageDeviceExtent*, unsigned long long)` and `::setProperties(OSObject*)` - kernel C++ symbols of
IOStorageFamily. So some include set made it compile here three days ago. Nothing of that set is left: no script or patch anywhere
in either checkout mentions the variant name, `~/.zsh_history` has no trace (agent runs are non-interactive), and a filesystem
search finds `IOBlockStorageDevice.h` only in the two forwarder locations above.

That is the honest shape of the gap: `storage-abi` is a hand-authored ABI description, not a package. Its own audit is static -
`scripts/audit-reference.py` runs `audit-storage-interface-static.py` from `sources/UniXNU-droidvm`, and those sources are not
published (only two DroidVM GPU patches are, under the k27 tree's `patches/droidvm-gpu/`), together with
`build/storage-abi/interface-audit.json`. Rebuilding it means declaring the class layout ourselves and having no way to check it,
which is exactly the case the goal rules put outside the mainline. The kernel and root image and KC base stay ours and verified;
the native kext object set stays the blocker in front of a desktop made from our own build.

Also unchanged from the previous note: `ld-classic` is settled (the tree carries `out/x12377/ldc`, and
`/Users/zhangqi/bin/ld-classic` is `ld64-957.1` for `$LD_CLASSIC`/`xcrun -f ld-classic`), and `UniXNU-turnip` still gates only the
desktop's Metal plug-in.