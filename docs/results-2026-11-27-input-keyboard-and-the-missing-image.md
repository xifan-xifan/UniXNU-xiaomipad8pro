# 2026-11-27 (night, fourth boot of this seed): the keyboard serves, the touch screen stops at its panel image

Fourth `fastboot boot` of seed `bb6dd1c3`, the first one with the tablet's own input enabled (`piano-touch=2 piano-keyboard=2`).
To make room for those two words inside the 999-byte command line, the recipe gives up `native-gop-display-shot=1` (26 bytes),
whose question was already answered; `PIANO_BOOT_ARGS_SPLIT=1` was tried first and is refused on this machine (no installed-seed
record, so a reader moved into the ADT tail counts as unreadable; see `tooling/k273/recipes/a830-input.sh`). Nothing was written to
any partition; the image resets itself into ABL and the reader brings HyperOS back.

| Item | Value |
| --- | --- |
| Image | `boot-piano-macos-ram-A-bundled.img`, 205,824,000 bytes, SHA-256 `201b13ec6e750fbcacbf59aa96a48d6018683460741ee3cd2a2a4de6fa9ded9d` |
| Recipe | `tooling/k273/recipes/a830-input.sh` over `a830-shot.sh` with `SHOT_REPLACE=keep` (Mac `out/ram-verified-8748-a830-input`) |
| Run | `logs/native-runs/ram-k273-a830input-20261127-230620` |
| Console | `logs/usb-console/a830input-20261127-230617-com.log` (`capture-com.ps1 -ExcludePorts COM6`, port 1 stayed free for the shell) |
| Args added | `piano-touch=2 piano-keyboard=2`; 983 bytes for the 8-CPU entry, no split |

The store resumed again, third boot in a row: `Data store state 2 ... seed bb6dd1c3, store seed bb6dd1c3, ... generation 3`.

## The keyboard reached its serving loop

```
PianoKeyboard: boot ROM: id 0xc8 (want 0xc8), state 0 (want 0: idle)
PianoKeyboard: step 5: firmware: header, data blocks, verify, boot
PianoKeyboard: firmware: 101 blocks (50668 bytes) in 35686 ms; verify sent, state 3 (want 3)
PianoKeyboard: firmware: booted (35742 ms in all); irq 1, status 1
PianoKeyboard: step 6: serving: gpio97 edges latched and polled, frames read from register 0x4c
```

The bridge image is the stock `MCU_Upgrade.bin` (83,436 bytes, `image checks pass (code 50668 bytes, 101 blocks)`), so this is the
unit's own firmware running. The hall switch is read as well: `hall: gpio80 ... raw 1, cover open at start`. Whether a keypress
turns into a HID event is still untested, because nobody was pressing a key while this ran.

## The touch screen got as far as its firmware and stopped

```
PianoTouch: firmware CSOT: not built in
PianoTouch: firmware BOE: not built in
PianoTouch: identify (GENI): NT36532 after 1 tries, 17 ms: id 0e 00 04 32 65 03, table 0 (E), cascade,
  event buffer 0x11c400, poll info 0x1093d8
PianoTouch: step 5: firmware: lcd-id, host download with the hardware CRC
PianoTouch: firmware: lcd-id gpio100 = 1: BOE panel, image 0 bytes
PianoTouch: FAILED at step 5: firmware: no usable image for this panel (built without it? tooling/k27/build-native-piano-touch.sh)
```

The bus, the clocks, the SMMU stream and the controller itself are fine: GENI SE2 passed its loopback (64/64 bytes) and the
controller identified itself over the real transfers in 17 ms. The panel ID pin says BOE. What is missing is the 245,760-byte
BOE image, and that is a build-host detail rather than a driver one: `build-native-piano-touch.sh` looks for the pinned images in
`../xiaomipad-8pro-mainline/firmware/novatek/piano` and, failing that, the same path ending in `piano` under
`~/Downloads/XiaomiPad8Pro-Ubuntu`, and on this Mac the two files sit one directory higher (`firmware/novatek`, no `piano`), so the
build said so and went on without them:

```
Warning: no .../xiaomipad-8pro-mainline/firmware/novatek/piano/novatek_nt36532_piano_fw_boe.bin:
  PianoTouch built without the boe image (step 5 stops on that panel)
```

The files were already here and are the pinned ones: 245,760 bytes each, SHA-256 `b7999389d49decb14019e24ce68c6fbb0a388e26391b9b0ab913e285c5e44c6b
(boe) and `14b30bb7799d273f6c5d967376694c2cbd3056f3939135ceda70ebec64abfa7a` (csot), which is what `firmware/piano-input.json` pins.
`tooling/k273/recipes/a830-touch.sh` points `PIANO_TOUCH_FW_DIR` at that directory and sets `PIANO_TOUCH_CALIBRATION` empty, so the
private per-unit calibration is not silently read from the build host's `~/.config/piano/env` and the linear map stays. Nothing was
downloaded, and neither the images nor their path go into the repository.

Still open after this boot: whether the download completes and the panel starts reporting, whether contacts reach a HID pointer,
and whether the keyboard's frames reach HID at all.
