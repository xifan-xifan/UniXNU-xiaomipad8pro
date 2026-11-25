# A k27 tree for native kext compiles is on this host; the source-level root candidate is now buildable (2026-11-25)

Host-side only. Nothing was built into an image, nothing was booted, the tablet kept its state (it is still
unreachable through ABL, see the last section).

## What the "no k27 tree" notes missed

The build-environment audit of this morning stopped at `tooling/k27/k27-tree.sh` and concluded that no
`PIANO_K27_TREE` exists. It exists on **this** machine, inside the WSL distribution `UbuntuPiano`:

| Path (inside `wsl -d UbuntuPiano`) | Contents |
| --- | --- |
| `/root/piano-native/k273-compat/out/x12377/build/xnu/k27-native-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json` | the compile recipe every native kext is built with |
| `/root/piano-native/k273-compat/out/x12377/storage-abi/include`, `.../xnu-m/osfmk`, `inc-hid`, `inc-net-piano`, `inc-gfx` | the generated headers the kexts include |
| `/root/piano-native/deps/UniXNU-sm8750-common/kits/Sm8750Platform` | the SM8750 kit `build-native-ufs.sh` links into its include dir |
| `/root/xnu-linux/bin/{clang,clang++,target-clang,target-clang++,ld.lld}` | the clang/ld pair the reconstructed builds used |

The archives that produced it are recorded in this repository: `out/k27-independent-20261120/host-run/` holds
`frozen-inputs-and-recipes.tar.gz` (Apple `xnu-12377.121.6` inputs, 12,084 members) and
`mac-object-checkpoint.tar.gz` (964 kernel objects and 1,093 compile recipes). Both extract cleanly and are the
same bytes the 2026-11-20/21 runs used, so the tree is reproducible, not a one-off leftover.

## What now runs, and what does not

`PIANO_K27_TREE=/root/piano-native/k273-compat tooling/k27/build-native-ufs.sh` reaches the compiler, so the
driver source is really in scope again. Two host gaps showed up, both already solved by hand in the
2026-11-21 run (`out/verified-no-poll-23aca0b8/` is its output) and now expressible as options:

- the recipe pairs `--target=arm64e-apple-ios17.0.0` with its own `-mios-version-min=27.0`, which clang turns into
  a `-Woverriding-option` error; the 2026-11-21 build rewrote the triple to `arm64e-apple-ios27.0`
  (`mac-compile.py`, `target=arm64e-apple-ios27.0`).
- the recipe and `build-native-kext.py` pass Apple-only warning spellings
  (`-Wno-unnecessary-virtual-specifier`, `-Wno-missing-designated-field-initializers`); the Ubuntu clang 18 in the
  distribution warns about them and `-Werror` makes that fatal. The 2026-11-21 objects were built with
  `xcrun -f clang` (Apple clang) on the Mac for the same reason.

`tooling/k27/build-native-kext.py` therefore grows `--target` and `--compiler` (four lines, recipe values stay the
default). Verified on this host: with `--target arm64e-apple-ios27.0` the triple conflict is gone and the only
remaining failure is the Apple warning spellings under clang 18, i.e. the object build is one Apple-clang host away.

```sh
# in wsl -d UbuntuPiano, from the repository
PIANO_K27_TREE=/root/piano-native/k273-compat UNIXNU_RESEARCH=/root/piano-native/deps \
python3 tooling/k27/build-native-kext.py NativeUfs package/NativeUfs/NativeUfs.cpp tooling/k27/NativeKextGlue.cpp \
  -I package/NativeUfs -I "$INC" -I $PIANO_K27_TREE/out/x12377/storage-abi/include -I $PIANO_K27_TREE/out/x12377/xnu-m/osfmk \
  -D U:TARGET_OS_OSX -D TARGET_OS_OSX=1 -D NATIVE_UFS_SM8750=1 -D UFS_ROOT_PARTITION=linux_rootfs \
  -D NATIVE_UFS_POLL_ONLY=1 -D NATIVE_UFS_UNVERIFIED_ROOT=1 \
  --obj k27-native-obj --target arm64e-apple-ios27.0 --compiler <apple-clang> --out out/native-kexts/NativeUfs-27p3
```

Why this matters more than it looks: `3a753d8c` put the **upstream** `package/NativeUfs/NativeUfs.cpp` in this tree
(`registerService` on the window, then the `mediaWithUuidExists` poll, no `IOService::startMatching` anywhere). The
target state named in `docs/results-2026-11-25-upstream-publication-diff.md` is therefore now the source in front of
us; with this tree the production publication sequence becomes a compiled-and-linked kernel collection instead of a
hand-written binary patch inside the shipped collection's zero holes.

## Provenance recheck (the pinned k27 tree is still private)

- `unixnu.lock` records the pin as `local: the k27 tree (PIANO_K27_TREE), branch el1/native-variant`, commit
  `11ac316b789d81fc9f734e15645456f536c05141`, `xnu-12377.121.6 (ac9718f) + patches/xnu-12377 0001-0054`, inner tree
  `de16623d`.
- No public branch named `el1/native-variant` exists in `Droid-VM/macOS-DroidVM`, `Droid-VM/UniXNU-droidvm`,
  `Droid-VM/UniXNU`, `CrossDarwin/UniXNU_macOS`, `lenzhang/UniXNU-xiaomipad8pro`,
  `xiaomi-pad-8-pro-development/android_device_xiaomi_piano-uefi`, or `Yosaki-sonako/UniXNU-droidvm` (404), and a
  GitHub commit search for that hash returns 0 hits.
- The UniXNU core does have `profiles/os/macos-27.3-26C5035r`, and it pins the **same** XNU
  (`xnu_version 12377.121.6`, `xnu_revision ac9718fb1af618d5ce8678d0dc6e8a58f252216f`) and the **same** decompressed
  mac17j kernelcache (`kernelcache_sha256 d1003b51…`, the file already on the Mac as `kc273-mac17j.bin`), which is
  why the reconstructed tree is a credible stand-in for compiles. It is not the pinned source: the core carries no
  KC builder (`kc27*`, `segaddrs.sh`, `kc-keep.txt` are absent; the public `UniXNU/XnuPrelink` component ships
  `fileset_emit/layout/match/names/shape.py` and `native_prelink.py`, not the piano builder).
- Network note for the next session: `raw.githubusercontent.com` is connection-reset from this host, while
  `api.github.com` works. Read repository files through the contents API (base64) or `git` over SSH.

## Device state, re-measured today

`fastboot boot` now refuses every RAM image, including the 8 MiB reader (`Failed to load/authenticate boot image:
Load Error`), and `fastboot reboot` comes back to ABL fastboot within seconds (no HyperOS boot after 150 s);
`getvar product`/`unlocked`/`is-userspace` still answer, `oem log` reports `unknown command`. This is the ABL
download path the `piano-usb=3` run left unusable (`docs/results-2026-11-25-usbc-console-try.md`), and it needs one
power cycle from the owner. The Mac (`192.168.31.183`) has 163 GiB free, so disk space is no longer a constraint.