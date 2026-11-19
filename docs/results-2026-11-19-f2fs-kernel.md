# F2FS Kernel Evidence Audit

Audit date: 2026-11-19. This is a static audit of an existing verified boot-image capture; it is not a new tablet capture, device write, or userdata backup.

The input was `E:\piano-verified-readonly-20261119-073450\boot_a.img`, whose 100,663,296-byte length and SHA-256 `7c444a2d6aa930cd79e2d48c569f6c891fbaff054c63fc85c3a57fb61c660ac2` are recorded in `outputs/manifests/native-macos-27.3.json`. The Android boot v4 header reports a 36,866,560-byte kernel. Its SHA-256 is `3ea8e88cc67b206de0c1753b2abd7c8a42eb68fee0e4d9bd5985abdd4b9ed87a`.

The embedded Linux release string is:

```text
6.6.118-android15-8-gc44b714366cc-abogki519650608-4k
```

The kernel build timestamp is `Thu Jun 4 03:48:35 UTC 2026`. Its embedded configuration has `CONFIG_IKCONFIG=y`, `CONFIG_IKCONFIG_PROC=y`, `CONFIG_F2FS_FS=y`, and F2FS compression enabled. A byte search of the extracted kernel found no `resizable_tail_secno`, `pinned_area_max_secno`, `/sys/fs/f2fs/features/resizable_tail`, or `resizable_tail` strings.

The kernel timestamp predates Android common's `android15-6.6` `resizable_tail` sysfs probe commit [`39e1749b1b88a1d9d0253e96b1d15ffed6f3fb67`](https://android.googlesource.com/kernel/common/+/39e1749b1b88a1d9d0253e96b1d15ffed6f3fb67), dated 2026-11-05. The stock vendor may carry independent changes, so the timestamp and string search do not prove the full source history. Combined with the previously recorded read-only mount/sysfs probes, they provide no evidence that this installed kernel supports the F2FS reserved-tail shrink workflow.

Conclusion: F2FS is enabled, but userdata shrinkability is still **unverified**. Do not use online F2FS shrink as a storage-layout assumption. No userdata contents were read for this audit, and no device access or write was performed.
