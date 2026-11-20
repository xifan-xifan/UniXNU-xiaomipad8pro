# Native keyboard build and GPU inputs (2026-11-20)

The `v1.6.1` Ubuntu upstream firmware tree supplied the exact 83,436-byte MCU image pinned by the keyboard builder. The keyboard host test passed 20,124 I2C waits and Apple clang generated three arm64e objects. This brings the object inventory to 118 objects across 23 of the 26 configured drivers.

The recovered 147,436-byte piano boot DT matched the board-input pin. The unmodified GPU board-input generator verified the checked-in header under WSL Python 3.12 and dtc. All four firmware files from the same tagged tree match the GPU builder's byte lengths and SHA-256 pins. The complete GPU wrapper still needs a compatible Python and dtc on the Mac, or a documented split host build that preserves each verification step.

The historical ABI 2 display declarations compiled the display driver and its glue in a separate Mac probe. Count these two objects separately until the historical declarations are checked against the target collection. The complete video wrapper still needs FFmpeg and LLVM objcopy. Final KC integration and tablet qualification remain open.
