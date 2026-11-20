# Native GPU build (2026-11-20)

The build reused the pinned board DT and firmware. WSL Python 3.12 and dtc verified the board input header. The Mac then used the same input bytes, isolated Python 3.11 and Apple clang 17. The AdrenoFirmware test kit was extracted at `20ad3d7aca14bd46aecab6335c780f5413fa054b`, the gitlink recorded by the pinned UniXNU core.

Each Adreno host configuration passed 46 tests with zero unexpected failures. Four known limitations per configuration concern unpublished register inputs and the upstream 13T firmware expectations. Separate staging verified all four Piano firmware pins. Both GPU configurations and the board checks completed. Apple clang produced 54 GPU objects, and the import check resolved 440 kernel symbols and four IOGraphics library symbols. All 54 object headers report subtype `0xc0000002`.

The checkpoint now covers 24 of 26 drivers and 172 objects. The separately compiled display probe awaits target ABI validation. The video build still needs the FFmpeg fixture check and LLVM objcopy. Final collection linking and tablet validation remain open.
