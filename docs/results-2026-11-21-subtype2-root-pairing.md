# Archived subtype-2 root pairing

The ADT selects the Apple OS APFS UUID `84581E99-85BE-4957-AC53-2947E887B3E4`, which matches the retained OS window. SystemOS is a separate window. Its assembled UUID `284548A8-FC87-434E-A0C8-A4038E83704D` differs from the comparison policy's SystemOS UUID `6678E466-2F88-4C56-9408-DC7361BAA0BA`. The SystemOS image sizes and SHA-256 digests also differ.

The retained Mac image header reports SHNGROOT version 1 with two entries. Its layout lists OS and SystemOS windows and contains no verification objects. The root selector match therefore establishes the OS volume identity only. The assembled SystemOS needs a policy for its own bytes and the boot configuration must contain a matching trusted key and verification objects. Native mount and fsck shims remain absent from the assembly.

The Mac was reached through the existing `mac` alias after `mac-lan` failed DNS resolution. Read-only ADB checks found `linux_rootfs`, `linux_data`, `esp` and `userdata` by name, with Android boot complete. These partition names do not establish the contents of the root partition.
