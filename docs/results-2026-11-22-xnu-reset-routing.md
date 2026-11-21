# XNU restart routing trial

Stock ABL accepted temporary image `e49b96f35fc7add5a6e69e3245f73876f1ed56df34adf138e5ec7ecb36196069`. The verified v2 container preserved the kernel field and DTB. The only FAT file change was `unixnu.toml`, which added `piano-reset-to=bootloader` to the default entry. ABL preflight reported slot A successful and bootable with six retries and snapshot state none.

The host sampled piano-uefi from 06:33:16 through 06:33:24 local time on November 22. It did not observe stock ABL after that interface disappeared, so it did not load the CVP reader. Without a host reboot command, HyperOS OS3.0.309.0.WPYCNXM completed boot on slot A with boot ID `fe3349f9-3bc1-4004-9421-c520136291af`, replacing `2700c072-5eb7-4715-b422-8a007735fd51`. The boot reason was `bootloader`. This result leaves XNU entry and execution of the restart hook unresolved. Raw evidence is in `out/reset-to-live/`.
