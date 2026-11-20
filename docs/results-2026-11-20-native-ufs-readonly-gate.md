# NativeUfs read-only root gate and host tests

The NativeUfs host suite was run from UbuntuPiano using the repository test entrypoint with the working directory set to `package/NativeUfs/tests`. All plain and ASan/UBSan cases passed: RootCommands, RootPower, RootWindowRead and RootDirect.

The new `NATIVE_UFS_READ_ONLY_ROOT` compile definition disables registration of development writable root windows while leaving the unverified root publication path available for diagnostic builds. The build helper accepts `PIANO_UFS_READ_ONLY_ROOT=1` and emits that definition. Data-store and diagnostic-log windows are separate paths and remain subject to their existing write policy; this change does not claim full device write isolation.

No tablet image was built or loaded in this check.
