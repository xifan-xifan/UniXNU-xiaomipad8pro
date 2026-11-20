# Native video build (2026-11-20)

The pinned VPU firmware matched its recorded size and SHA-256. The WSL FFmpeg 6.1.1 test rebuilt the 286-byte H.264 fixture and independently decoded the expected NV12 frame. The Mac ran the firmware loader, protocol, streaming, VideoToolbox contract, and bring-up host suites and compiled 11 objects with Apple clang 17.

LLVM 18 rejected Mach-O `--globalize-symbol`. Ubuntu LLVM 20.1.2 supported both required transforms. The resulting object exports the VideoToolbox initializer and omits its automatic initialization section. Its source and result hashes were checked before updating the object list on the Mac. All 11 objects report arm64e subtype `0xc0000002`. The final import check resolved 408 kernel imports and six expected driver library imports with zero unresolved.

The checkpoint now contains 183 objects across 25 of 26 configured drivers. The separate two-object display probe still needs target ABI verification. Hardware decode, continuous playback, the final KC and tablet qualification remain open.
