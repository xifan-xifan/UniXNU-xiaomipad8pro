# XNU first CPU entry on Piano

The frozen owner link recipe with only `start.o` replaced restored the NativeUfs callback ABI. The KC builder produced a 135,806,976-byte collection with 364,815 chained pointers. DarwinBoot accepted it in host tests. The temporary Android v2 image retained the previous firmware, DTB and every FAT payload except the KC. Its boot ID is `1af4fb4dd6f46ebe0ee2e3100ff824fae81c74ae`.

Stock ABL reported slot A successful and bootable, six retries and no snapshot update. It accepted the temporary image. After the watchdog returned to ABL, the RAM reader captured 7,340,032 bytes. The four previous handoff markers were present. Offset `0x6f5500` held `0x550334844`, written at entry to `start_first_cpu` before MMU setup. The tablet returned unattended to HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, with `sys.boot_completed=1`.

The experiment verifies the first XNU entry instructions. The next diagnostic should sample the MMU transition and first C entry while preserving the same return path. Desktop, storage persistence and dual boot acceptance remain open. The manifest records the image and capture hashes.
