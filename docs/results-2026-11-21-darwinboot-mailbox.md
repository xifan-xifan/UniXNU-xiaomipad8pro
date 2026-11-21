# DarwinBoot EBS mailbox host build

EDK II completed the DEBUG CLANGPDB build using the retained witness cache and a separate copy of the patched DarwinBoot package. The FD is `UbuntuPiano:/tmp/darwinboot-mailbox.rSIKluGA/mailbox.fd` with SHA-256 `777b2cedd454fc7f65a1ad77c11d7dc473044d18f5ec4cc0405b16958307d102`. `build.log` in the same directory records the compiled input and completed build. The shell reported a trailing carriage return after hashing the copied file.

The RAM container `out/boot-piano-darwinboot-mailbox.img` is 185,733,120 bytes with SHA-256 `978d5a787b1bbcc44ff33d0471fef54947a8f96b8c0a20c8f8df3cba895db42f`. Packaging checked the reference witness container hash and its Android v2 image ID, replaced its unique 3 MiB FD at byte 4288, and recomputed the image ID. The RAM payload and device tree are unchanged.

The loader records stages in eleven pages starting at `0x9D1F5000` inside the CVP carveout. The four little-endian 64-bit words are the magic `0x315342454F4E4150`, packed attempt and stage, EFI status, and the complement of the packed word. Stages 1 and 2 precede GetMemoryMap and ExitBootServices respectively. Stage 3 records the ExitBootServices return and stage 4 records a GetMemoryMap failure. The existing stop on successful exit remains, so this probe cannot branch into XNU.

Before the tablet trial, retention across the watchdog-to-HyperOS route was unqualified. The prior menu-only read proves retrieval of this address range. Capture a baseline and qualify retention across the watchdog-to-HyperOS route before relying on a positive or missing record. Preserve the existing witness FD for comparison.

The offline verifier passed with zero failures using the tablet-tested witness container as the header reference and the cached DTB matching the recorded digest. The verification log is `out/verify-darwinboot-mailbox.log`.

Stock ABL accepted the temporary boot. ADB confirmed unattended return to HyperOS OS3.0.309.0.WPYCNXM on A and the original Android marker. The polling script reported a timeout, but its own final enumeration and subsequent direct ADB checks confirmed recovery.

The previously verified FAT16 reader returned all 7,340,032 bytes of CVP RAM. Capture `out/darwinboot-mailbox-device.bin` has SHA-256 `8ce0df554054f10c9c0f76b45cd1c65b597e1651e61f88ecfdc43a62dca9f17c`. All eleven record locations contained zeroes. Because capture followed a HyperOS boot and a second UEFI boot, this does not distinguish a missing write from later clearing. The reader rebooted normally and ADB again confirmed completed HyperOS and the unchanged marker. Qualify a known pre-EBS record across this reset sequence before interpreting a missing checkpoint.
