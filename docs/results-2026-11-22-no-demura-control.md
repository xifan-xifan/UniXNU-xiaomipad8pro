# XNU entry control without Display_Demura probes

The pinned DarwinBoot parser accepted the complete control KC and passed the synthetic parser and packed ramdisk tests. The frozen collection and control contain the same 81 fileset members and seven outer segment ranges. All 364,815 chained fixup sites are unique, and their targets lie in mapped segments. The public root anchor and both signed headers match the frozen collection at the same offsets, once each.

The embedded thread PC still branches from `0xfffffff00bc06488` to `0xfffffff00bc0805c`. The original entry window contains eleven move-wide instructions that form the upper halfword of the Display_Demura addresses used by the removed probes. The first 4 KiB of the control entry contains none of those instructions. The first control instruction is `0xd510109f`.

The Android v2 image retains the original kernel field, DTB, and all FAT files except `EFI/UniXNU/Payloads/kernel.macho`. mtools read back all five files and the expected replacement SHA-256. The recomputed boot ID is `6daa0b9528e795f539c6fd4e7e204afa944090c1`. The manifest records both artifact hashes. ADB reported completed HyperOS OS3.0.309.0.WPYCNXM on slot A.

CrossDarwin still advertises `4a671773c2ee289735ab526eda7c9628131250df` as HEAD and `test_27`. Retain it as a source reference for the same release. Keep the current collection and piano recovery assets pinned for the entry comparison.

## Tablet trial

Stock ABL accepted the control as a temporary boot. Preflight confirmed slot A successful and bootable with six retries and no snapshot update. The watchdog returned control to stock ABL, where the frozen reader captured all 7,340,032 CVP bytes before Android started. Ten of eleven EBS records report stage three, attempt zero and EFI_SUCCESS. The XNU console header is still absent. The capture begins with the same bytes as the original control. The reader rebooted into completed HyperOS OS3.0.309.0.WPYCNXM on A. The capture and preflight log are in `out/no-demura-control/`.

The latest proven milestone is ExitBootServices returning success. The next diagnostic should use separate CVP record slots for the assembly entry, the instruction sequence around disabling the MMU, and the first XNU instructions. Preserve the EBS records for comparison.

The CrossDarwin branch `memory/handoff-diagnostics` at `6e43591fb2f775f4203a076eec5cda8babb9d130` reports offered, adopted and refused memory during XNU `EARLY_BOOT`. Its log depends on reaching `ml_adopt_outside_memory`, so it can help verify memory adoption once the entry is observable. The `test_27` debugging guide independently places the loader framebuffer write before the cache sweep, MMU shutdown and branch. The current CVP result stops before any independent evidence of those assembly stages.
