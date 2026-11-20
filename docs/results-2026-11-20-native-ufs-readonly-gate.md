# NativeUfs read-only root gate and host tests

The NativeUfs host suite was run from UbuntuPiano using the repository test entrypoint with the working directory set to `package/NativeUfs/tests`. All plain and ASan/UBSan cases passed: RootCommands, RootPower, RootWindowRead and RootDirect.

The new `NATIVE_UFS_READ_ONLY_ROOT` compile definition disables registration of development writable root windows while leaving the unverified root publication path available for diagnostic builds. The build helper accepts `PIANO_UFS_READ_ONLY_ROOT=1` and emits that definition. Data-store and diagnostic-log windows are separate paths and remain subject to their existing write policy; this change does not claim full device write isolation.

No tablet image was built or loaded in this check.

Apple clang 17 built the arm64e NativeUfs object from the frozen independent Mac checkpoint with `PIANO_UFS_READ_ONLY_ROOT=1`. The first attempt failed because the root write registration helper became unused under `-Werror`. Commit `c3078bd` excludes that helper and its state in the read-only build. The rebuilt object's symbol table lacks `gDevWritable` and `gDevWritableCount`, both present in the preserved writable build. SHA-256 is `e4856f94427ea3000eb664bbc5d6bec260a76e76ce94d732e2e383f958996d83`. The object and build log remain under `/tmp/piano-ufs-ro.Jweept` on the Mac. Collection integration and device testing remain pending.
