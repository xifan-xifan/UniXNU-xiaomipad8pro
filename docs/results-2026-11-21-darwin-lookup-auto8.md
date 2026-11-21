# DarwinBoot lookup and automatic eight-core probe

The action-only diagnostic exported a firmware log containing `DarwinBoot protocol present`. The log at `out/darwin-lookup-staged.log` has SHA-256 `96adbe6d180034538666ff4413f6d0284d117d01ae512e863652b5e2c61e45c2`. This confirms protocol availability at the BDS lookup boundary.

The next RAM candidate, `out/boot-piano-darwin-lookup-auto8.img`, has SHA-256 `de26c50d2a73dd57b1d70fb40b9085450d386a24ef156b36341a133aae1ec0b1`. It preserves the previous diagnostic shim, FAT payload and DTB and replaces the FD. The probe uses `native-ufs-probe=2 data-persist=0 sheng-smp=1 piano-reset-after=90`. The reset timer depends on early XNU initialization.

Before the RAM boot, the stock ABL reported product piano, non-userspace Fastboot, no snapshot update, and slot A successful and bootable with six retries. It accepted the candidate via `fastboot boot`. No ADB or Fastboot interface was visible in subsequent checks and no new log was retrieved. The user-supplied photo shows the UniXNU menu after the eight-core entry returned `Unsupported`, but the photo does not identify its image or run. XNU handoff and return to HyperOS remain unverified for the new candidate.
