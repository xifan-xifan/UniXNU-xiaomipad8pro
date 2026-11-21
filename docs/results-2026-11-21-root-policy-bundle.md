# Root policy handoff bundle

A host-side `VROOTB01` bundle was assembled from the signed 544-byte OS and SystemOS policy headers already checked into the 26C5035r policy directory. The bundle has two entries, is 1,152 bytes, and hashes to `a6e8283dbe4a696c73757de671f94a059dcd649a009f35bf3eff8af3001d19de`.

The offsets are contiguous and the bundle parser checks the version, count, total length and exact entry bytes. This is the header-only form expected when the block tables are carried as SHNGROOT v2 objects. The corresponding checked-in `SourceRootPolicies.cpp` path still requires firmware to expose an aligned `/chosen/memory-map` `RootPolicies` range and requires the kernel-side crypto provider and compiled owner key. The bundle has not been injected into a device boot image.
