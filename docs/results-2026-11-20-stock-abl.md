# Stock ABL inspection (2026-11-20)

Read `images/abl.elf` from the retained HyperOS 3.0.309 archive on the Mac. The 335,672-byte ELF has SHA-256 `0ab1245367d84bd4c16418106cefd8884a3e2fdaeab5dcf6584455a7d50e2148`. Its LZMA-guided section decoded to 1,179,848 bytes. The decoded data contains fastboot, ramdisk and vendor_boot strings. The package hash identifies this offline sample. Ramdisk selection and possible persistent writes during temporary boot remain unverified. String inspection cannot establish the handler control flow or rule out writes through a protocol pointer.

Windows ADB confirmed slot `_a`. A read-only SHA-256 over the first 335,672 bytes of `abl_a` matched the archive ELF exactly. The comparison covers the packaged ELF length. The hash was computed on the tablet and only the digest was returned.
