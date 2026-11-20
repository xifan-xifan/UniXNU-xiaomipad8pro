# NativeUfs read-only root gate and host tests

The NativeUfs host suite was run from UbuntuPiano using the repository test entrypoint with the working directory set to `package/NativeUfs/tests`. All plain and ASan/UBSan cases passed: RootCommands, RootPower, RootWindowRead and RootDirect.

The new `NATIVE_UFS_READ_ONLY_ROOT` compile definition disables registration of development writable root windows while leaving the unverified root publication path available for diagnostic builds. The build helper accepts `PIANO_UFS_READ_ONLY_ROOT=1` and emits that definition. Data-store and diagnostic-log windows are separate paths and remain subject to their existing write policy; this change does not claim full device write isolation.

No tablet image was built or loaded in this check.

Apple clang 17 built the arm64e NativeUfs object from the frozen independent Mac checkpoint with `PIANO_UFS_READ_ONLY_ROOT=1`. The first attempt failed because the root write registration helper became unused under `-Werror`. Commit `c3078bd` excludes that helper and its state in the read-only build. The rebuilt object's symbol table lacks `gDevWritable` and `gDevWritableCount`, both present in the preserved writable build. SHA-256 is `e4856f94427ea3000eb664bbc5d6bec260a76e76ce94d732e2e383f958996d83`. The object and build log remain under `/tmp/piano-ufs-ro.Jweept` on the Mac. Collection integration and device testing remain pending.
# Read-only NativeUfs full collection relink

The preserved 26-driver object set was relinked with the arm64e NativeUfs object built using `PIANO_UFS_READ_ONLY_ROOT=1`. The relink used the frozen Apple-normalized KC input and positioned kernel from the independent Mac checkpoint; all other driver objects were unchanged.

The builder wrote `/tmp/piano-ro-relink.Rf99b9/readonly.kc` (135,806,976 bytes, 364,799 chained pointers), SHA-256 `00f0ba553e16a42021623d9805f6d77ae95ae09d968c761ae1cae4cb6bb06831`. NativeUfsBlock, NativeRamBlock, NativeGopDisplay, AGXAccelerator, PianoUsbHID, PianoHIDDevice, PianoKeyboardHID, PianoNullEthernet, PianoTouchHID, PianoWifiEthernet and NativeAES vtable checks all reported zero problem slots.

The pinned DarwinBoot parser passed its arm64/arm64e capability, malformed-range, fileset-membership and entry checks. `tooling/k27/check-native-kexts.py` passed its self-test. Its separate KC log check reported 26 native kexts and no VM-era entries. The production descriptor path check was not run on these staged descriptors. The output remains a host-only artifact. It has not been packed into a device image or loaded on the tablet.
