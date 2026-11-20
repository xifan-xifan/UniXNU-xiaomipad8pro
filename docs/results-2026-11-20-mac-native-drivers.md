# Mac native kext object build (2026-11-20)

Using the frozen Mac XNU recipe archive and Apple clang 17, three native kexts from device baseline `2438556c77c90c22c7b76e57eb8b40c44b35f1af` compiled for arm64e subtype `0xc0000002`: PianoBacklight (2 objects), PianoDpu (4), and SoftwareAES (2). All eight object headers and SHA-256 values are recorded in `out/k27-independent-20261120/host-run/native-driver-result.json`. The build log is in the adjacent `native-driver-build.log`.

PianoBacklight's model test passed 5,644 I2C waits. PianoDpu's five host suites passed: core probe, B1b flip, refresh, PCC and panel. SoftwareAES has no build wrapper host suite. The object builds were performed on `mac-lan`; no tablet command or write occurred.

The standalone Mac checkout had no configured `PIANO_K27_TREE`, so the build used the frozen `xnu-source-119dc3f35273` archive and recipe inputs from the preceding kernel build in a temporary isolated checkout. The build helper was adjusted to suppress only Clang's diagnostic for an unsupported warning *option* (`-Wno-unnecessary-virtual-specifier`); `-Wall -Wextra -Werror` and the remaining checks stay enabled. Recipe source paths were mapped and checked to match their file records before compilation. The helper succeeds with Apple clang 17 after that compatibility adjustment.

These are host-built kext objects, not an ABI-qualified collection. Only 3 of the 26 configured native descriptors have objects. The remaining descriptors, family ABI/import checks, final KC serialization and DarwinBoot validation remain open; nothing was signed or booted.

The owner reports that HyperOS 3.0.309 was factory-reset before work began and has no personal data. This was not independently re-read from the device, and no tablet data backup was made. The empty-userdata report does not establish free GPT space: the existing read-only inventory shows userdata occupying the remaining LUN 0 range. Any repartition or flash still requires a concrete WRITE-PLAN and recovery sequence before a separate explicit confirmation.
