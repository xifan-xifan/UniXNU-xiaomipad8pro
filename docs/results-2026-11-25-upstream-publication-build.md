# The upstream root-publication driver, compiled and linked into a 27.3 collection (2026-11-25)

Host-side only. Nothing was booted and no byte was written to the tablet; it is still unreachable through ABL
(`docs/results-2026-11-25-usbc-console-try.md`, re-measured again today: `fastboot boot` refuses even the 8 MiB reader).

## What the morning's conclusion missed

`docs/results-2026-11-25-build-environment-audit.md` stopped at `tooling/k27/k27-tree.sh` and reported that no k27 tree
exists, so a source-level root candidate could not be produced. The tree is on **this** machine, in the WSL distribution
`UbuntuPiano` (`/root/piano-native/k273-compat/out/x12377`: the compile recipes and the generated headers), and the two
archives it came from are already in this worktree. `docs/results-2026-11-25-k27-tree-in-wsl.md` has the inventory.

That matters because `3a753d8c` made `package/NativeUfs/NativeUfs.cpp` the **upstream production driver**:
`IOBlockStorageDevice::start` + `registerService` on the window, then the `mediaWithUuidExists` poll, with
`IOService::startMatching` and hand-built `IOMedia` gone. So the target state named by
`docs/results-2026-11-25-upstream-publication-diff.md` is now a source file, not a proposal.

## What was built

`tooling/native/build-ufs269-upstream-pub.sh`, in three steps, all replayable:

1. **Compile** the driver with the retained recipe and this host's clang:

   ```sh
   PIANO_K27_TREE=/root/piano-native/k273-compat UNIXNU_RESEARCH=/root/piano-native/deps \
   python3 tooling/k27/build-native-kext.py NativeUfs package/NativeUfs/NativeUfs.cpp tooling/k27/NativeKextGlue.cpp \
     -I package/NativeUfs -I $INC -I $K/out/x12377/storage-abi/include -I $K/out/x12377/xnu-m/osfmk \
     -D U:TARGET_OS_OSX -D TARGET_OS_OSX=1 -D NATIVE_UFS_SM8750=1 -D "UFS_ROOT_PARTITION=\"$PART\"" \
     -D NATIVE_UFS_POLL_ONLY=1 -D NATIVE_UFS_UNVERIFIED_ROOT=1 \
     --obj k27-native-obj --target arm64e-apple-ios27.0 --probe-warnings --out out/native-kexts/NativeUfs-upstream
   ```

   Two host gaps had to become options in `build-native-kext.py` (both were handled by hand in the 2026-11-21 run): the recipe
   pairs `--target=arm64e-apple-ios17.0.0` with its own `-mios-version-min=27.0` (`-Woverriding-option`, fatal under `-Werror`)
   → `--target`; and two of the appended warning switches are Apple-fork spellings that no clang here knows, which `-Werror`
   turns fatal → `--probe-warnings`, which tests each switch against the compiler and substitutes the upstream spelling of the
   one diagnostic that actually fires (inside XNU's own `kalloc` macros). `--compiler` is there for the Mac.

2. **Link** the 27.3 collection with the retained builder, replaying the recorded command of the last source-built collection
   that booted (`out/no-demura-control/blendfix.argv.json`, 2026-11-22) with only base, kernel, output and the driver
   descriptor changed. The other 25 native drivers are the same bytes; the driver is the only delta. The builder is a wrapper
   around its sibling `scripts/kc-build.py` and reads `k27/segaddrs.json`, so the script copies that tree too and the link no
   longer depends on the WSL tmp.

   `piano-ufs269.kc`: 135,806,976 B, `dafd48f1…`, 364805 chained pointers, 26 native drivers linked, vtable checks clean.

3. **Pack** a v2 RAM image the way the probe images are packed (replace `EFI/UniXNU/Payloads/kernel.macho` inside the ramdisk
   FAT, keep the size, recompute the boot id): `boot-piano-ufs269.img` 185,733,120 B, `c6fc5b7a…`, boot id `a870ecc5…`. Reading
   the collection back out of the packed image hashes to `dafd48f1…`, i.e. the payload in the image is the collection.

## Static evidence that the intended change is in the object

`llvm-nm` of `NativeUfs.o`:

- imports `__ZN9IOService15registerServiceEj` (`IOService::registerService(unsigned)`) — the window is published;
- **no** reference to `startMatching` and **no** reference to `IOMedia` anywhere in the object, which is exactly the
  difference recorded in `docs/results-2026-11-25-upstream-publication-diff.md`, and the structural opposite of the shipped
  collection (whose `publishMedia` allocates an `IOMedia` and whose publisher thread calls `startMatching`);
- keeps the poll (`publishThread` present, `mediaWithUuidExists` in the source).

## What this changes for the next device session

`tooling/native/run-ufs269pub.ps1` runs it unattended (hash-gated image, ABL slot preflight, `fastboot boot`, timed return,
reader-staged console, normal return to HyperOS). **Built, to be run**: the tablet must first get one power cycle to make ABL
accept a RAM image again. If the storage stop moves past `IOBlockStorageDriver::start`, the same link line becomes the way to
iterate for the rest of the bring-up, with no binary patching inside a shipped collection.