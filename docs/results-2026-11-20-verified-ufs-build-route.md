# NativeUfs build route verification (2026-11-20)

The normal 27.3 image entry point delegates to `build-native-image.sh`, which builds NativeUfs through `build-native-ufs.sh`. That script explicitly selects `NATIVE_UFS_UNVERIFIED_ROOT=1`. It can open aligned root windows writable and does not link the verified policy inputs.

The retained `owner-cryptex-bindfs.kc` matched the manifest digest. Its link symbol inventory contains `native_ufs_block_authenticates`, `vr_open`, and `verified_root_policies`. The registered NativeUfs descriptor resolves to the retained verified fixture object, registration entry, and host polling stub. The verified fixture object matched its previously recorded SHA-256 (`df7d1cf7da92b2983ac275ebbdbc9edc61d2ba951023ae29f00e9dd1dfd41f43`). These findings establish the host fixture link composition. The signing policy is still a fixture and the normal image builder does not produce that composition.

Correction to the description of ac38c09: verified window configuration already took an exclusive preprocessor branch and returned before development write setup. The new compiler rejection catches a contradictory build configuration. It did not close a reachable verified-root write path.

A production build needs pinned VerifiedRoot source and headers, the kernel crypto adapter, signed policies bound to the final OS and SystemOS images, and the authenticator registration entry wired into the normal collection build. The current host fixture cannot be used as the production policy input.
