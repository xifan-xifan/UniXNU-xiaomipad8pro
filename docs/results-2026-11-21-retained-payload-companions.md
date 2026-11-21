# Retained boot payload companions

Read the FAT members directly with mtools. The ADT, trust cache and scratch Data SHA-256 values match `payload.json` (`d9c6da93…`, `002db2e6…` and `f87d2ec6…`). The trust cache matches the Apple-only 26C5035r bundle verified in the root host assembly report. It provides no evidence of matching trust modules for locally staged userland.

The scratch image has a DBRDISK1 version 1 header, 4 GiB logical size, six aligned extents and an APFS `NXSB` superblock at logical offset zero. The payload directory contains the KC, ADT, trust cache and scratch image. It contains no SystemOS or root image. These checks establish the bundled inputs for KC `d36b8047…`. The identity of any external SystemOS and root paired with that KC remains unverified.

ADB reported the tablet online with HyperOS OS3.0.309.0.WPYCNXM during the host audit.
