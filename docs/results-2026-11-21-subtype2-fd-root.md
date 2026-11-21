# Archived subtype-2 firmware and root selection

Extracting 3,145,728 bytes at kernel-field offset 112 from the corrected-header container yields FD SHA-256 `ed159825a7b6194b0f00226dc3601c30b496e511f7ccf61d0bfef6e28277bb11`, matching the original archive's `boot.json`. The archived container therefore supplies the FD bytes and shim boundary for a reconstructed verification record. This match does not establish equivalence to the tablet-tested witness FD.

The subtype-2 FAT configuration selects `Payloads/kernel.macho` and `Payloads/scratch-data.img`. Its XNU entry has no explicit UFS partition or `data-persist` setting. The ADT matches root IOMedia UUID `84581E99-85BE-4957-AC53-2947E887B3E4`, the pinned 27.3 root identifier. The archived Data image lacks the full-volume firmlink layout required for desktop deployment. Effective storage defaults and a matching authenticated root still need qualification before hardware use.

A direct read of the maintainer repository returned plan baseline `2438556c77c90c22c7b76e57eb8b40c44b35f1af`. ADB reported HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, and `sys.boot_completed=1`. No device writes or reboot were performed.
