# Archived subtype-2 ESP payload and RAM images

The 192 MiB subtype-2 ESP image contains the 160 MiB subtype-2 payload (`d12691e0…`) followed by zero padding. The FAT member `kernel.macho` hashes to `3729e762…`, matching the archived collection that passed the host parser. The historical ESP write/readback hash is `0d902012…`. The rollback record reports restoration to `d33ee195…`.

The retained corrected-header v2 `boot.img` (`be0327b1…`) and `boot-sunuefi-shim.img` (`9323adbc…`) both embed ramdisk `1219349b…`. This is the original payload containing collection `d36b8047…`. Neither RAM image contains the subtype-2 payload. The historical subtype-2 timeline records UEFI detection and a later query. XNU handoff and desktop startup remain unverified.
