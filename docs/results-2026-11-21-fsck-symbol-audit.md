# Stock fsck symbol audit (2026-11-21)

A read-only attachment of the 26C5035r OS input was used to inspect the arm64e `/sbin/fsck` symbol and string tables. The binary imports `_getfsent` and `_setfsent` and exposes option text `dfpR:qnNyYl:L`, confirming that the wrapper enumerates fstab entries and supports `-R` and `-L`; it does not prove which fstab class `-R 1` selects on the native composed root. No execution of the attached binary occurred and the image was detached afterward.
