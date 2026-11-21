# XNU entry control without Display_Demura probes

The pinned DarwinBoot parser accepted the complete control KC and passed the synthetic parser and packed ramdisk tests. The frozen collection and control contain the same 81 fileset members and seven outer segment ranges. All 364,815 chained fixup sites are unique, and their targets lie in mapped segments. The public root anchor and both signed headers match the frozen collection at the same offsets, once each.

The embedded thread PC still branches from `0xfffffff00bc06488` to `0xfffffff00bc0805c`. The original entry window contains eleven move-wide instructions that form the upper halfword of the Display_Demura addresses used by the removed probes. The first 4 KiB of the control entry contains none of those instructions. The first control instruction is `0xd510109f`.

The Android v2 image retains the original kernel field, DTB, and all FAT files except `EFI/UniXNU/Payloads/kernel.macho`. mtools read back all five files and the expected replacement SHA-256. The recomputed boot ID is `6daa0b9528e795f539c6fd4e7e204afa944090c1`. The manifest records both artifact hashes. ADB reported completed HyperOS OS3.0.309.0.WPYCNXM on slot A. This control has not run on the tablet.

CrossDarwin still advertises `4a671773c2ee289735ab526eda7c9628131250df` as HEAD and `test_27`. Retain it as a source reference for the same release. Keep the current collection and piano recovery assets pinned for the entry comparison.
