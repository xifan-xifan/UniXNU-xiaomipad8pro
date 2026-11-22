# XNU bootstrap table trial

The frozen owner kernel redirects five existing pre-MMU stores from Display_Demura to the CVP capture window. The KC changed by 41 bytes, covering the UUID and five address loads. The DarwinBoot parser accepted the collection. The Android v2 image retained the firmware, DTB and other FAT payloads. Boot ID is `93be973f3a7b86b6ed5553c985153991be0862d3`.

Stock ABL reported A successful and bootable with six retries and no snapshot update. After temporary boot the reader captured 7,340,032 bytes. The record at `0x6f5620` contained 8 GiB, 4096 L2 entries, virtual base `0xfffffff007004000` and physical base `0x8c0004000`. The table-exhaustion slot at `0x6f5640` was zero. Both completion markers at `0x6f5660` and `0x6f5680` were present, confirming execution to the branch to `common_start`. MMU activation remains unverified.

The reader returned unattended to HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, with `sys.boot_completed=1`.
