# Live firmware ring at unattended macOS selection

The stock ABL accepted temporary image SHA-256 `1c4feecd143e8c990e42dc022a32100094917a05570efd825429faddba33d90a`. Slot A was current, successful and bootable, with six retries and snapshot status `none`.

Firmware Fastboot delivered eleven 1 MiB `Piano_Log` snapshots in `out/enter-ring-live/`. Rings 0–5 reach the five-entry menu. Rings 6–10 confirm the eight-second timeout selected `macos-8cpu`, the scratch Data image, kernel, ADT and trust cache opened, and DarwinBoot printed the entry address and `PIANO: DarwinBoot ready for ExitBootServices`. The log also reports two FAT cache boundary warnings before handoff preparation; the current evidence does not establish their effect. Watchdog `STATUS`, `SET_TIME` and `ENABLE` returned zero; firmware reported no pet armed.

The next ring upload timed out. ADB subsequently confirmed completed HyperOS `OS3.0.309.0.WPYCNXM` on `_a` without operator input. This run establishes unattended menu selection and the final pre-call marker. It does not establish successful `ExitBootServices`, XNU execution, or a qualified Android/macOS transition. Preserve the frozen set and use independent checkpoints on each side of the call before repeating a hardware run.
