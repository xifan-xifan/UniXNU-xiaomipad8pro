# Offline subtype-2 RAM image assembly

Using the retained corrected-header boot container as the kernel/DTB source, I rebuilt a host-only v2 image with the archived subtype-2 FAT payload as its ramdisk. The image remains 171,220,992 bytes, uses the existing kernel field and packaging DTB, moves the 147,436-byte DTB into the v2 `dtb` section, clears `recovery_dtbo_size`, recomputes the mkbootimg section ID, and embeds payload SHA-256 `d12691e0…`. The resulting offline image SHA-256 is `47125fbd…`.

The subtype-2 Mach-O inside the payload passes the DarwinBoot parser replay. This image has not been sent to the tablet, signed, or installed. It is only a host assembly artifact and still lacks a verified SystemOS/root pairing.
