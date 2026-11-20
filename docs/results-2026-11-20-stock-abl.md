# Stock ABL inspection (2026-11-20)

Read `images/abl.elf` from the retained HyperOS 3.0.309 archive on the Mac. The 335,672-byte ELF has SHA-256 `0ab1245367d84bd4c16418106cefd8884a3e2fdaeab5dcf6584455a7d50e2148`. Its LZMA-guided section decoded to 1,179,848 bytes. The decoded data contains fastboot, ramdisk and vendor_boot strings. The package hash identifies this offline sample. Ramdisk selection and possible persistent writes during temporary boot remain unverified. String inspection cannot establish the handler control flow or rule out writes through a protocol pointer.

Windows ADB confirmed slot `_a`. A read-only SHA-256 over the first 335,672 bytes of `abl_a` matched the archive ELF exactly. The comparison covers the packaged ELF length. The hash was computed on the tablet and only the digest was returned.
## Fastboot boot source review

Read-only source review was performed on a public Qualcomm ABL reference checkout's `FastbootCmds.c`, `PartitionTableUpdate.c`, and `BootLinux.c` on the Mac. This source set is not yet proven to be the exact source revision used to build the device ABL; conclusions below are reference-source findings, not binary control-flow proof.

In that source, unlocked `CmdBoot` sets the boot-device-image flag, calls `ClearUnbootable()` for multi-slot boot, passes the downloaded boot image to `LoadImageAndAuth()`, then clears the flag before calling `BootLinux()`. `ClearUnbootable()` clears the active boot partition's unbootable attribute in the in-memory GPT. `UpdatePartitionAttributes()` compares that in-memory table with its saved copy and returns without block writes when unchanged. If changed, it updates the primary and backup GPT through `BlockIo->WriteBlocks()` and flushes. The previously captured A-slot state was successful=1 and unbootable=0, so this specific operation would not alter GPT if that state remains unchanged and this code matches the binary. This does not establish that other boot paths have no writes.

`FindBootableSlot()` in the reviewed source does not decrement retries for a successful slot. Its unsuccessful-slot retry decrement is gated by `!IsBootDevImage()`, but `CmdBoot` clears that flag before entering `BootLinux()`. The call path and the device binary's exact implementation therefore still require confirmation; no general claim that temporary boot cannot change A/B state is made.

The reviewed `BootLinux.c` selects an `init_boot` ramdisk only when `Info->HasBootInitRamdisk` is true and the boot header is version 4 or later. The planned Android boot v2 container does not meet that source condition and appears to use the downloaded image's ramdisk. The exact `LoadImageAndAuth()` implementation and the device binary's populated `BootInfo` remain unverified.

The reference source conditionally writes the non-volatile UEFI variable `DestinationCore` when `TARGET_LINUX_BOOT_CPU_ID` enables CPU selection. The target build configuration has not been matched to this ABL. Boot statistics, UEFI/blackbox log backends, error handling, and hard-reset persistence paths also remain unaudited. The decompressed ABL strings alone cannot rule these out.

Result: no temporary boot was attempted. The exact ABL source-to-binary mapping and non-GPT persistent side effects are open, so the read-only review does not yet satisfy the authorized temporary-boot gate.
