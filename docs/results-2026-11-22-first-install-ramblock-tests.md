# First-install Data seed and NativeRamBlock tests

The current NativeRamBlock host suite passed on the Mac with the frozen first-install Data image. All nine test programs passed, including guard, volatile stress, pool and power tests. The independent C++ seed decoder reported `7e9bd39b72da851d9e4174258b3b2eb2277ef74e75fcb42f0bc07d662448e93b`, matching the deployment manifest. The Linux run was partial because CommonCrypto is unavailable and Clang on that host rejects the mixed unsigned types in the volatile test initializer.

The historical 27.3 RAM desktop recipe uses mode 0 for writable RAM Data. Preserve this mode for the first-install candidate. The pre-init APFS mount can write to NativeRamBlock RAM in this mode. The current host run covers the seed decoder and block code. The remaining device checks include APFS mounting the intended provider, effective boot arguments, the separate UFS logging gate and automatic return after XNU entry.
