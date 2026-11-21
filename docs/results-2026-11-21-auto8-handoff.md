# Automatic SMP probe on piano — 2026-11-21

Stock ABL reported product piano, slot A successful and bootable, six retries, and no snapshot. `fastboot boot` accepted the reset90 probe from the manifest. RAM UEFI enumerated as `piano-uefi`. The firmware log reports an 8000 ms timeout, automatic selection of `macos-8cpu`, then `xnu: no loader protocol installed; UniXNU does not provide one`. The menu reopened with an indefinite timeout. XNU did not start and the 90-second reset was not exercised.

The screen and firmware log are under `logs/native-runs/20261121-132400-auto8-reset90/`. The firmware dump SHA-256 is `c79b476191431a7d9555fdb8c0ecc23fac093f9d5855bd93bc839a62ead15e4e`.

`fastboot -s piano-uefi reboot` returned to completed HyperOS `OS3.0.309.0.WPYCNXM` on `_a`. The Android persistence marker matched its previous SHA-256. The retained `bfc2417a…` firmware build inventory includes the DarwinBoot driver GUID. That is a candidate for package and dispatch verification with the existing diagnostic payload. The tested `d8a41ebb…` firmware did not publish the loader protocol.
