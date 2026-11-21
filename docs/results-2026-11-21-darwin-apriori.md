# DarwinBoot APRIORI control on piano

The diagnostic build from `d8a41ebb9f1e54490db012a0d412cfd143cccd8803efa40c9b38043fd9230e96` was replayed in a separate workspace with DarwinBoot appended to the DXE APRIORI list. The generated APRIORI GUID payload grew by 16 bytes and ended with the DarwinBoot GUID. The DarwinBoot PE executable section matched the original build. The full rebuild also changed other firmware files, so this is a one-change source comparison.

The rebuilt FD SHA-256 is `96efe3f8d0dbe033a41582709b61489bc5829d77bd0116933d02d160d20917a3`. The action-only RAM image SHA-256 is `b93899cd3ff6d9b3c4eb5e87fea21ecbc6a7ad39665af98c1cd4aa5b71cb3e1a`. Readback confirmed the original shim, FAT payload and DTB alongside the new FD and a valid Android header ID. The menu payload has only restart and shutdown entries.

Stock ABL reported piano, userspace=no, snapshot=none, slot A successful and bootable, and six retries. ABL accepted the RAM image and `piano-uefi` enumerated. `oem log` returned OKAY. The host rebooted before downloading the staged payload, so this run cannot verify loader registration. The UEFI reboot returned to completed HyperOS `OS3.0.309.0.WPYCNXM` on `_a` and the persistence marker SHA-256 matched. The next capture must download the staged payload before reboot. DarwinBoot startup status and XNU handoff remain unverified.
