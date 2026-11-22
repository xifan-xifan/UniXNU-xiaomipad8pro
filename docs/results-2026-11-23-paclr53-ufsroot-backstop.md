# paclr53 UFS root trial: the first root-mode image had no reset backstop

Date: 2026-11-23. Device `7ccdd855`. Only `fastboot boot` was used; no partition, GPT or userdata write.

This note corrects the mechanism proposed by the previous root-mode record. It does not change that record's device evidence.

## Verified from source and from the image

The image that hung, `out/no-demura-control/boot-piano-paclr53-ufsroot.img` (`fc9cfa24…`), carries

```
native-ufs-root=1 native-ufs-diagnostics=1 data-persist=0 droidvm-utc=1795179306 cpus=1
```

and nothing else that arms a reset. Reading `/EFI/UniXNU/unixnu.toml` back out of the image's FAT shows no `piano-reset-after`, no `piano-boot-guard`. Its builder, `make-paclr53-ufsroot.py`, only replaces `kernel.macho`.

So the absence of an unattended return is explained before any discussion of PSCI, the XNU timer or the Gunyah watchdog: nothing asked for one. The earlier note's candidates ("the reset never armed, PSCI/hvc reset failed, or the watchdog was not armed") are reduced to the first: no reset was requested.

That leaves the freeze point itself unproven. `NativeUfs::startRoot` logs every stage with `rootStage()` -> `IOLog`, and `pstore-console=0x9cb00000` is in the arguments, so the last stage line is sitting in DRAM. DRAM survives a watchdog bite (the 2026-11-12 guard tests read their console back after one), which is why a backstopped image is the way to recover that line.

## Prepared image

`out/no-demura-control/boot-piano-paclr53-ufsroot3.img` (185,733,120 bytes), SHA-256 `e8d21614f587eba4c488453467d31f35ff9b74027943be63104c426dab0b93e7`.

It is the hung image with two backstops added to both `args` lines:

```
piano-reset-after=180 piano-reset-to=bootloader piano-boot-guard=150 native-ufs-root-part=linux_rootfs
```

The payload is byte-identical to the hung image: `kernel.macho` (`4efc1dea…`, 135,806,976 bytes), trustcache (`002db2e6…`, 451,264) and ADT (`4e904d90…`, 15,736) all hash the same. Only the configuration text changed, so the test isolates the backstop and the progress reporting.

`run-paclr53-ufsroot3.ps1` repeats the same ABL preflight, boots the image, waits up to 400 s for fastboot, dumps `9cb00000 700000` (console) and `8c5945000 20000` (panic) to `paclr53-ufsroot3-console.bin` / `paclr53-ufsroot3-panic.bin`, reboots to Android and requires `boot_completed=1`.

## Run order after the tablet returns

1. `run-paclr53-probe11.ps1` (`723e871a…`, read-only probe 11, `piano-reset-after=240`). It panics on purpose with the bring-up result and each window's GPT, so a clean panic answers the question without needing the watchdog.
2. `run-paclr53-ufsroot3.ps1`. Any hang now ends in ABL fastboot by itself and the console dump names the stage.

Open: whether the hang is before `STARTUP(EARLY_BOOT)` (no backstop can help; would show as another silent hang even with `piano-reset-after`) or inside the root bring-up.

## Host check

`tooling/native/check-boot-image.py` reads the live `args` out of a k27 RAM image and fails any image that asks for a native UFS run without a reset backstop or without `pstore-console`. It walks the ramdisk's FAT directory and cluster chain, because a raw search of the image also finds the orphaned copies of the previous `unixnu.toml` that an in-place `mcopy` leaves in free clusters: the first version of this check reported four `args` lines for an image with two, and two of them were stale text.

Run against the four paclr53 images it separates them correctly:

```
ufsroot.img   native-ufs-root=1   backstop=none                     -> FAIL (the hung image)
ufsroot2.img  native-ufs-root=1   piano-reset-after=420             -> pass
probe11.img   native-ufs-probe=11 piano-reset-after=240             -> pass
ufsroot3.img  native-ufs-root=1   piano-reset-after=180, boot-guard=150 -> pass
```

Images that carry no k27 `args` are reported as skipped (the UEFI FD containers and the maintenance image), so the check can run over a directory without false alarms.
