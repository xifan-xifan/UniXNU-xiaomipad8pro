# Stock Recovery UI extension trace

Host inspection traced the UI pointer used by the wipe dispatch to the Device virtual call at `0x129a4c`. The returned pointer is saved at `[sp,#168]` at `0x129d1c`. The default `ScreenRecoveryUI` address point at `0x31eb48` maps both offsets `0x68` and `0x78` to `0x15bbe0`, which returns zero immediately.

The stock ramdisk contains `system/lib64/librecovery_ui_ext.so` (SHA-256 `a2ba69959befd5a0b8c21d4b4b583bb6a98a60b3d21664481217b3f69b9c36d8`). It exports `make_device`, the `MiScreenRecoveryUI` vtable, separate data and storage handlers, and `WipeData(Device*, bool, bool)`. At those vtable offsets the file contains zero placeholders. Resolve the dynamic relocations and confirm factory selection before assigning real format targets to the command-line path. This analysis ran on the host.

The Android packed relocations resolve offset 0x68 at address point 0x2a0018 to SubMenuWipeCmdWipeData(bool), which calls internal_erase_data_cache at 0xfa2a8 and then WipeData at 0x112aa0. Offset 0x78 resolves to SubMenuWipeCmdFormatStorage(), which calls internal_erase_storage at 0xfa624. The runtime factory and the effective partition targets still need confirmation.
