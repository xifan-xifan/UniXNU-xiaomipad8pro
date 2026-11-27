PianoBacklight: the panel backlight of the Xiaomi Pad 8 Pro (two Kinetic KTZ8866 at I2C 0x11, on the I2C hub's SE3 pins gpio70/71
and QUP2 SE0's pins gpio0/1, driven in software) for native macOS 27. Inert without the boot argument piano-backlight (1: read-only
survey; 2: control: a moderate boot level, backlight off on display sleep and on again on wake, brightness steps, optionally off
while the cover is closed). The design and boot arguments are at the top of PianoBacklight.cpp, the register facts and the state
machine in Ktz8866.h, the interface for other drivers in PianoBacklightClient.h; the page is docs/drivers/backlight.md; provenance
in UPSTREAM. Build: tooling/k27/build-native-piano-backlight.sh (host tests, then the objects); it is in configs/k27-native-kexts.txt.

| File | What |
|---|---|
| `PianoBacklight.cpp` | the IOService on IOPlatformExpertDevice: pins, the two software buses, the thread, the callouts, the properties |
| `Ktz8866.h` | portable: registers, survey decoding, brightness steps, the log limit, the controller (what is written when) |
| `BitI2c.h` | portable: the software I2C master (PianoKeyboard's, copied) |
| `PianoBacklightClient.h` | header only: the function names other drivers call, the brightness key watch (`piano-backlight-keys=1`) |
| `tests/backlight_host.cpp` | host tests (bit-level KTZ8866 model, register sequences, sleep/wake, lid, failures, keys) |
| `tests/backlight_curve_sync_host.cpp` | ASan/UBSan coverage of the Android ALS/SDR curve, native brightness input, and paired KTZ8866 power and fault sequences |

Status (2026-11-11): built and host-tested, not run on the tablet.

## Goal 5 offline qualification

Run `python3 scripts/ci/self-tests.py --only package/PianoBacklight/tests/` from the repository root. The curve suite exercises
the live portable policy and controller over two simulated bit-banged I²C buses. The forward curve is nondecreasing with
quantization plateaus, and inverse luminance rises strictly across SDR codes 921–1952. The suite checks invalid raw samples,
strict SDR decoding, and ambient snapshot bounds. The live swap queue clamps positive out-of-range nits, and the ambient callout
clamps positive targets above 1000 permille.

The modeled IOKit handoff permits touch park and M5 only after both BL_EN readbacks confirm dark. A chip that keeps NACKing can
retain BL_EN after the bounded blank retries, which causes the handoff to fail closed. Wake checks cover the register sequence and
rejection of targets from the previous ambient revision. The physical dark → bright → dark lux sweep and hands-on Control Center
slider verification remain **HOLD**.
