# USB transition observer on Windows

`tooling/native/watch-piano-usb.py` captured the host-directed Android → stock ABL → Android control on 2026-11-21. The observer saw ADB at 09:37:23.940 UTC, its disappearance at 09:37:26.798, stock Fastboot at 09:37:34.388, its disappearance at 09:37:34.727 and ADB again at 09:37:49.791. Stock Fastboot reported product `piano`. The post-run query returned `sys.boot_completed=1` and `OS3.0.309.0.WPYCNXM`. Log SHA-256: `90a60f50070c6cfd5766cd91c0c8cc440b0f848b89fac0d707e035c622212880`.

Wait for the first complete baseline line before launching a trial. USB states are sampled and brief appearances can be missed. This control did not exercise `piano-uefi`, DarwinBoot or XNU.
