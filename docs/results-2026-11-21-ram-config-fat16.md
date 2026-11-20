# RAM configuration volume on hardware (2026-11-21)

Both temporary boots used the same isolated DEBUG FD (`d8a41ebb9f1e54490db012a0d412cfd143cccd8803efa40c9b38043fd9230e96`). The first image (`edb8e54b4844819c25bba85a220272898784bc2960d0be734b6b51e238ba1454`) exposed the expected boot sector with CRC32 `91D530CE` and signature `55AA`. Disk I/O was present, but Simple File System and TOML access returned `Unsupported`. The pinned FAT driver rejects FAT32 below 65,525 data clusters. The original 8 MiB volume had 16,100.

Replacing the ramdisk bytes with an 8 MiB FAT16 volume containing the same menu produced the diagnostic image `9b45743adfdb1b1d79f24ecdff48e8a6def9a2417b1a751ade8da63a93b93778`. The tablet reported CRC32 `958BC497`, Simple File System and TOML read `Success`, and loaded `\EFI\UniXNU\unixnu.toml`. It displayed two entries with `reset` as the default. `ConnectController` still reported `Not Found`, and the filesystem protocol was already installed. The test used the firmware USB reboot command to return to HyperOS. The menu carried no XNU entry.

HyperOS reported boot completed, build `OS3.0.309.0.WPYCNXM`, and slot `_a`. A separate ABL check before and after the FAT16 run gave identical values: userspace `no`, slot `a`, successful `yes`, unbootable `no`, retry `6`, snapshot `none`. No flash command was issued.

The local logs are retained under `D:/Temp/`. The `piano-ram-trace-edb8e54b-ring.bin` SHA-256 is `df25510b5fc5551f5f1f6d3e10d47ed74e2d6a4603ff3999d1274c27fda2c7e0`. The `piano-ram-trace-fat16-ring.bin` SHA-256 is `f54f7133dedc5b7cde8d05aefe0506876d5a5bddd340991469ff412c3a0d0fbb`.

The builder now generates the menu volume with `mkfs.fat` (dosfstools) and `mcopy`. Two independent host runs of the menu payload generator produced identical bytes, `fsck.fat -n` passed, and the verifier rejected the original FAT32 while accepting the generated FAT16. The hardware test used the same FAT16 geometry with a separately prepared volume. The generated image still needs a full packaging replay before a device test.
