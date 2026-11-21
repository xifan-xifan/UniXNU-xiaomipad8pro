# Two-second DarwinBoot probe and reset coverage

Stock ABL accepted one RAM boot of `boot-piano-darwin-lookup-auto2.img`. Before launch, ABL reported `product=piano`, `is-userspace=no`, no snapshot update, and slot A successful and bootable with six retries. The earlier host attempts stopped before reboot. The image digest still matches the artifact manifest.

Windows PnP recorded `PIANO-UEFI` arriving at 14:48:56 and departing at 14:48:57 on November 21, 2026 (host UTC+08:00). The 45-second watcher produced no new capture directory. At 14:54 host time ADB and Fastboot returned empty device lists. Loader dispatch, XNU entry, reset cause and Android return remain unobserved for this run.

The retained Mu BaseCore source implements `WatchdogTimerDxe` through a boot services timer event that calls `ResetSystem`. `CoreExitBootServices` stops the timer before terminating the memory map. This driver can be evaluated for a responsive pre-handoff timeout. Coverage after ExitBootServices and during a stalled CPU requires an independent reset source. The XNU 90-second reset starts during EARLY_BOOT, leaving this interval uncovered by either software timer. Host USB polling only observes that interval.

Qualify any firmware timeout first with an action-only RAM image and record activation, expiry and Android return. Then qualify a reset source that remains armed through the firmware-to-XNU gap before using it to recover intentionally stalled handoffs. Keep the two-second candidate as evidence of the USB capture failure.
