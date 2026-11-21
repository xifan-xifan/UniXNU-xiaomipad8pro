# NativeUfs probe 2 host result (2026-11-21)

Probe 2 now logs the register snapshot with `IOLog` and returns before scratch allocation, worker startup and root initialization. Its register read guards are unchanged. The four NativeUfs host suites passed in plain and ASan/UBSan modes. The Mac build used `NATIVE_UFS_READ_ONLY_ROOT=1`. The arm64e object's SHA-256 is `a89544e6da675af989d11186b0f1a27d665be2ac4bb1ea1933bd69cc92c9101d` on both hosts.

The isolated replay in `/tmp/piano-inspect-relink.BlbwS0` changed the NativeUfs object and output paths in the retained recipe. `readonly-inspect.kc` has SHA-256 `80da9440d3ec0f610dc3721d10f4fc36cad72c88e14a00f09f79cdc73165d18d`. The build logged 26 linked native drivers and zero problem slots in every reported vtable check. Both diagnostic strings are present in the resulting collection. The replay arguments and log reside beside the KC.

The collection awaits mailbox image packaging and tablet verification. Probe 2 returns to the surrounding boot path, which still needs review for logging, storage access and reset behavior before test execution.

The mailbox container was then repacked in the same scratch directory by replacing only the KC in a copy of the previous FAT payload and recomputing the Android header ID. The FAT file inventory and hashes confirmed that the menu, ADT, trust cache and seed matched the prior candidate. The Ubuntu reference verifier reported zero failures and confirmed both XNU entries carry probe 2 and volatile Data. The image is 185733120 bytes. ADB confirmed piano, HyperOS OS3.0.309.0.WPYCNXM, slot _a and boot completed. No XNU device run has taken place.
