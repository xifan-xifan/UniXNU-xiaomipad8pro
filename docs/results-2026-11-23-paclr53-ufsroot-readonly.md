# paclr53 UFS root trial: first root-mode boot hangs before a return

Date: 2026-11-23. Device `7ccdd855`. Only `fastboot boot` was used; no partition, GPT or userdata write. The tablet was in the maintenance RAM image (`out/maintenance-gpt/base.img`) before this trial.

## Root already on linux_rootfs

A 4096-byte read at LBA 818157568 (byte offset 102269696 x 4096) returned a complete SHNGROOT header: magic `SHNGROOT`, version 2, four entries, image length 17,547,919,360. That is the stock 26C5035r version 2 root assembled on 2026-11-21 (`db3629e5…`), not an Android root. The earlier interpretation that 11-22 wrote an Android root to `linux_rootfs` was wrong.

`linux_data` (LBA 860100608) and `esp` (LBA 994318336) still read as zero at block 0.

## Candidate

`out/no-demura-control/boot-piano-paclr53-ufsroot.img`, SHA-256 `fc9cfa24f1eaa1959629928114c63a397136d2b2fd5886afe493a677d8096008`. It is the frozen manual RAM image (`native-ufs-root=1 native-ufs-diagnostics=1 data-persist=0`, root partition default `linux_rootfs`) with `kernel.macho` replaced by `paclr53.kc` (`4efc1dea…`). The manual image and the paclr53 image carry identical trustcache (`002db2e6…`), ADT (`4e904d90…`) and seed (`48dbc08d…`).

The earlier paclr53 image ran `native-ufs-probe=2`, so it stopped at `NativeUfs: start … root=0` by design. This image runs the root path (`root=1`).

## Device result

The image passed the ABL preflight (product piano, slot a successful, retry 6, snapshot none) and `fastboot boot` returned OKAY. The tablet then produced no fastboot or ADB device for more than six minutes: the run script's 180 s fastboot wait and 180 s Android wait both expired. The USB Ethernet interface that the maintenance image exposes was also absent. The tablet therefore did not panic-reset and did not return to Android on its own.

No console, pstore or panic buffer could be captured because those live in DRAM and the reader image can only dump them after the device returns to fastboot. The freeze point is inside the root path after `NativeUfs::start`; it is not the previously known pthread stop.

## Next candidates

1. `out/no-demura-control/boot-piano-paclr53-probe11.img` (`723e871a…`) runs the read-only `native-ufs-probe=11` path: bring-up, manifest parse, first block of each root window, then a panic that names the result. It adds `piano-reset-after=240 piano-reset-to=bootloader`.
2. `out/no-demura-control/boot-piano-paclr53-ufsroot2.img` (`50a3fb45…`) repeats the root path with `piano-reset-after=420` and an explicit `native-ufs-root-part=linux_rootfs`.

Both need a physical power cycle before the next run because the current candidate left the tablet without USB.
