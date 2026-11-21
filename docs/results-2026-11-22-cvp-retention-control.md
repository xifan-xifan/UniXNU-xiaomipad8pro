# CVP RAM retention before Android starts

On November 22, the frozen RAM maintenance image exposed `/dev/mem` and BusyBox `devmem`. Its mount table listed RAM and kernel virtual filesystems. The checkpoint word at physical address `0x9D1F5000` initially read zero. Maintenance wrote `0x52455431` to that word and read back the exact value before rebooting to stock ABL using `piano-maint fastboot`.

Without starting Android between the two images, stock ABL loaded the previously verified menu-only RAM reader (`708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468`). UEFI uploaded all 7,340,032 bytes of CVP. Offset `0x6f5000` contained `31 54 45 52`, the exact little-endian marker. The capture SHA-256 is `a1207086dae1720f85266152a631f74eadf4059cab33f1b1f517d8d91a3e79ac`. After the reader rebooted, ADB confirmed HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, boot completed.

This qualifies the maintenance-to-ABL reset and the pre-Android reader for this address. It does not establish retention across a Gunyah watchdog bite or show that the XNU restart hook executes. Evidence is under `out/retention-control/` and `out/retention-maint-check/`.
