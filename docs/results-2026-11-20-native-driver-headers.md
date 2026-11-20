# Native driver build with recovered headers (2026-11-20)

Reused the historical storage, HID, graphics and network include trees in the isolated Mac checkpoint. The original k27 pin remains unavailable and the family ABI still needs validation against the target collection. NativeUfs also needed the SM8750 kit extracted with `git archive` at the locked `feaa135df4763e3c19a155402e6b17b124a12e13` commit.

NativeUfs, NativeRamBlock, PianoDwc3, PianoInput, PianoTouch, PianoNullEthernet and PianoWifiEthernet produced objects. The full inventory now covers 22 of 26 configured drivers and 115 arm64e objects with subtype `0xc0000002`. NativeGopDisplay awaits the reference collection and ABI tools, PianoA830 awaits a compatible Python environment and generated inputs, PianoKeyboard awaits its pinned MCU firmware, and PianoVideo awaits FFmpeg for its fixture test. The final KC and tablet qualification remain open.
