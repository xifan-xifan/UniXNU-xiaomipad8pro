# Android-reference SDR brightness and CPU ceiling

## Brightness

`piano-backlight-android=1` opts the native adapter into a 600-declared-nit SDR schema. The adapter requires `piano-backlight=2`, `piano-backlight-native=1`, `piano-backlight-keys=0`, and the existing built-in display profile. Both KTZ8866 ICs must report exponential mapping, PWM off, and matching sink and current settings. An absent argument or an unsupported survey uses the v12.1c adapter. The survey reports the two IC configurations and the decision.

The 613 pairs in `PianoAuto/AndroidBrightness.h` are Xiaomi Pad 8 Pro SDR hardware values observed on November 20, 2026. The extraction paired same-PID Android panel DBV requests and KTZ codes within 25 ms. CRC-32 `9673290e` covers the pairs as little-endian unsigned 16-bit words. The lit mapping uses DBV 8–2047 and IC codes through 1952. The floor is the larger of the configured floor and code 178. The native adapter derives its minimum from the same inverse and rounds it up to an advertised millinit. Gaps use monotone interpolation and plateaus use their DBV midpoint. The 600-nit endpoint uses the observed DBV 2047 and code 1952. Display power still uses BL_EN, and the controller publishes a changed current value after both ICs verify it. Luminance here is Android-declared SDR luminance. Physical luminance requires separate measurement.

The root IORegistry property `piano-backlight-reference` starts at 1. Value 0 selects the bounded linear native transfer and the legacy SSC lux curve. Value 1 selects the reference transfer and the reference SSC curve. Code and luminance limits stay fixed across switches. The registry exposes `piano-backlight-map-crc` and `piano-backlight-sdr-revision`.

With `piano-backlight-auto=1`, the reference curve interpolates the device's own Android automatic curve (dumpsys display, adjustment 0): 0 lux 3.5 nits, 1 lux 4.5, 2 lux 6, 4 lux 15, 6 lux 21, 8 lux 27, 10 lux 36, 15 lux 52, 20 lux 71, 25 lux 87, 30 lux 97, 82 lux 101, through 11,165 lux at 600 nits, then converts through the shared table into code permille, never below the effective lit floor. The existing smoothing, hysteresis and sensor guards still govern the target. Both `piano-sensors-light` and `piano-backlight-ambient` carry the numeric curve ID (0 legacy, 1 reference) and revision. Switching shape invalidates the old smoothing session and re-arms its SSC subscription through the same stream lifecycle. A native slider or direct nits request chooses manual mode. Explicitly setting `piano-backlight-auto=1` requests a fresh automatic session. A latched fault still blocks automatic writes.

## Control tool

Build and test with `make -C tooling/native/piano-brightness-control test`. The binary is in `out/piano-brightness-control/`. Package it through the existing signed and trusted runtime path before using it on the tablet.

| Command | Operation |
| --- | --- |
| `piano-brightness-control --probe` | Read the 27.3 DisplayServices capability and normalized readback, IOMobileFramebuffer information and capabilities, and the PianoBacklight registry ranges. |
| `piano-brightness-control --level 0.4` | Send a normalized 0–1 level through DisplayServices in the GUI user's session. |
| `sudo piano-brightness-control --adapter-nits 100` | Submit a direct root 16.16 nits request to the native adapter. The active adapter validates its floor and maximum. |
| `sudo piano-brightness-control --reference 0` | Select the bounded linear native transfer and legacy SSC curve. Use 1 for both reference curves. |
| `sudo piano-brightness-control --auto 1` | Re-enable automatic brightness. Use 0 for manual. |

The native API probe is versioned to macOS 27.3 build `26C5035r`. Exit codes are 0 for success, 64 for invalid CLI syntax, 65 for a rejected property value, 69 for an unavailable service or native API, 74 for another API failure, and 77 for authorization failure. A setter's successful return acknowledges the request. The console and both IC readbacks establish application. The direct root path exercises the adapter under root authority. Use `--level` and the actual GUI to qualify DisplayServices and WindowServer.

## CPU

`piano-cpufreq-android=1` arms root-only `machdep.piano_cpufreq_android` when governor mode and the explicit 2400/2438 MHz cap pair are present. The switch starts at 0. When set to 1, the governor can choose at most 2745.6 MHz for cpu0–5 and 2649.6 MHz for cpu6–7. Both SCMI domains must enumerate the expected base and next levels. The boost must have ended and both clusters must have usable temperature input with fresh CPU samples.

The driver reads the actual `IOPMPowerSource.ExternalConnected` state and the published 32-bit `PianoPolicy.piano-policy-lpm` value at governor ticks. Saver, LPM, unknown power, battery, unavailable policy state, firmware sustained limits and thermal limits can restrict the runtime selection. The thread lowers an active experiment before parking for system sleep. Setting the switch to 0 reinstates the base pair, subject to stricter limits, by the next governor tick. SCMI still receives the firmware level keys and owns clock and voltage coordination.

## Console witnesses

```text
PianoBacklightNativeV1 ready: ...
PianoBacklightAndroidV1 model=reference|linear schema=600 min_millinits=N max_millinits=600000 code_min=N code_max=1952 map_crc=X revision=R
PianoBacklightAndroidV1 source=native|ambient revision=R requested_millinits=N dbv=N target_code=N primary=N secondary=N applied_millinits=N errors=N
PianoBacklightAndroidV1 source=native|ambient revision=R requested_millinits=N dbv=N target_code=N primary=N secondary=N result=pending|failed errors=N
PianoALSAndroidV1 curve=reference|legacy revision=R session=S lux=L target_millinits=N target_permille=P valid=V fault=F
PianoALSAndroidV1 session=S suid=HIGH:LOW name=NAME location=front type=ambient_light rigid_body=N
PianoCpuAndroidV1 enabled=E requested=R source=external|battery|unknown profile=P lpm=L lpm_valid=V boost=B valid=V base_khz=2400000,2438400 selected_khz=N,N sustained_khz=N,N thermal_limit=N,N
PianoCpuAndroidV1 scmi domain=D get_key=K get_known=V get_status=S actual_khz=N request_key=K request_valid=V request_status=S changes=N refusals=N
```

The DBV in a bounded linear transaction is the loaded Android map's equivalent DBV for the declared request. The linear transfer sends a KTZ code directly. The CPU phase report includes the effective ceiling and a separate SCMI GET observation. Use the existing clock and residency reports with those phase lines.
