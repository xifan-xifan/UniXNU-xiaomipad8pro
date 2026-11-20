# macOS 27.3 root assembly on Mac (2026-11-20)

The three local 26C5035r inputs were copied to the Mac and passed SHA-256 comparison. `BuildManifest.plist` identifies 141-22233-021 as OS, 141-22145-021 as the SystemOS cryptex and 141-22106-021 as the AppOS cryptex. The retained AppOS member also matches the K114c identity SHA-384.

A copy of `make-systemos.sh` built a host test volume with the native boot shim install step omitted because those files are missing from the pinned tree. It copied the SystemOS and AppOS contents and selected read-only content from the stock OS Data template. The build used 3.4 GiB of the 6 GiB APFS volume. SystemOS UUID `284548A8-FC87-434E-A0C8-A4038E83704D`, SHA-256 `c970de1b385486b2893c7a03a236ad612fb2f89fc45e48e9a7a80374d247487d`.

The host SHNGROOT image is exactly 20,936,916,992 bytes. Both windows passed SHA-256 readback, and the embedded manifest matched the original and passed its internal hash. Full image SHA-256 `e543c4ed25f77dec07c39b3ba840a0a6887a8a499a1d515eecfb9a0bc6099f2b`. Manifest SHA-256 `7369d019d035d5cea211c9cca47121c29b06687841b30dfb3bdb0524fb686e03`. The result confirms the 20 GiB root partition has 537,919,488 bytes of headroom at the selected window sizes. The host image is missing the native mount and fsck shims. The final image still requires those inputs, the matching Data seed and trust bundle, and device qualification.

All three 26C5035r Apple trust-cache inputs matched their manifest digests and parsed as complete version-2 modules. Their entry counts are 18,568, 144 and 87. The Apple-only 451,264-byte bundle passed a module round trip and has SHA-256 `002db2e6ee9000c891bad43247bfa9c55ad6d3b2ee62e6c0b9e1965e78926da6`. The locally staged userland code still needs its matching trust modules.

The locked BootShim source was recovered by Git object hash and built using the pinned piano firmware geometry. The 192-byte binary has SHA-256 `f7ee5e091c70ece86710bcc99256974a66b41134b5b07d77ceea2cfa9f758d16`. Its ARM64 header size covers the shim and 3 MiB FD. Historical WSL FD copies match each other and the separate EFI binaries contain the expected RAM-payload and memory-window markers. Their original build used a different UniXNU checkout state, so the existing FD still needs provenance verification or a pinned rebuild.

The live A-slot recovery partition matched the 3.0.309 archive recovery image SHA-256 during a read-only ADB check. The user-data boundary change and recovery behavior remain untested on the tablet.
