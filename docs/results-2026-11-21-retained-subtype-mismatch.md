# Retained Mach-O subtype mismatch

A full byte comparison of the two 142,442,496-byte archived collections `d36b8047…` and `3729e762…` found one changed byte at file offset `0xb`, from `0xc0` to `0x00`. The outer Mach-O CPU subtype changes from `0xc0000002` to `0x00000002`. The `com.apple.kernel` member at `0x198000` retains subtype `0x00000002` in both files.

The working-copy DarwinBoot parser compares the full outer and member subtype words. Therefore the archived original triggers its `EFI_LOAD_ERROR` check and the one-byte variant passes the host parser. The historical firmware build and the stage reached by the subtype-2 tablet trial remain unverified. Preserve the archived subtype-2 collection as the corrected parser input. The next device trial still requires matched root and SystemOS inputs and unattended recovery qualification.
