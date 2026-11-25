# 2026-11-26: the pre-UEFI selector booted on the unit in both directions (RAM only)

`firmware/selector` and the combined `boot_a` came from upstream (T-16) and had never been put on a tablet. Two
`fastboot boot` runs settle that. Nothing was flashed, no slot attribute was touched, and both runs ended in HyperOS by
themselves; the evidence is `logs/selector-20261126-02*.log`.

Images, both signed with this host's AVB key from the stock 3.0.309 props (`artifacts/ARTIFACTS.md`):

| image | SHA-256 | selector |
| --- | --- | --- |
| `out/switch/boot-a-probe-android-20261126-signed.img` | `8e8669ba44df25460d5f9222ddc758bc2aa3abd72792a7afe7934a7d95fbf859` | forced to hand off to the stock kernel |
| `out/switch/boot-a-combined-20261126-signed.img` | `375478f6c64110a952d44ca57b7c166f3c391729aafef351351a148390d8bed5` | `b4130d11...`, the selector of record |

## Run 1: the selector ran, and Android came up (`02:24:52`)

ABL preflight `product piano`, `current-slot a`, `slot-successful:a yes`, `slot-unbootable:a no`, `slot-retry-count:a 6`.
`fastboot boot` finished in 3.3 s; 11 s later adb answered with `ro.build.version.incremental OS3.0.309.0.WPYCNXM` and
`uptime` reporting **0 min**, so this is a fresh boot of the image we sent, not the tablet sitting in Android. The forced
handoff therefore proves the chain `ABL -> stock kernel -> code1 redirected to the selector -> stock primary_entry`.

## Run 2: with no boot flag the boot goes to the UEFI payload (`02:40:00`)

Same preflight, `fastboot boot` in 3.0 s, and **6.1 s later a fastboot device named `piano-uefi` appeared**, answering
`getvar product` with `piano-uefi`. That is Mu-Silicium's firmware running: the selector chose macOS, which is the
documented fail-safe (`piano_select` case 3: the SDAM flag is unreadable or not ours, nothing written; the flag in the
PM8550 SDAM is still factory-empty). The firmware stayed in its own fastboot, so no root and no data were mounted, and
`tooling/native/ensure-hyperos.ps1` rebooted the unit: `hyperos=up build=OS3.0.309.0.WPYCNXM`, rc 0.

So both handoffs the switch needs are demonstrated on this unit: to Android (run 1) and to macOS (run 2), each with the
tablet returning to HyperOS afterwards and `slot-retry-count:a` still 6.

## Why the UEFI payload of a combined `boot_a` has to be the persistent image

`tooling/switch/pack-selector.py` starts the payload the way ABL starts a Linux Image: it passes only the FDT in `x0`.
A RAM image's macOS payload lives in that image's Android ramdisk, which the selector neither sees nor points the FDT at,
so packing a RAM image would leave the firmware with no payload (the `drivers 0 loaded` symptom of
[the smprobe run](results-2026-11-26-smprobe-run-and-fd-check.md)). Only `out/persistent/boot-piano-macos-persistent-v4-avb.img`
carries what the firmware needs by itself, and it does that through the ESP:

| image | `unixnu.toml` | ramdisk references | consequence |
| --- | --- | --- | --- |
| `boot-piano-macos-persistent-v4-avb.img` | 0 | 0 | config and payload come from partition 37 |
| `boot-piano-ufs269.img` | 0 | 36 | carries its own config, unusable as a packed payload |

`tooling/native/check-image-fd.py` confirms the packed payload carries `piano-uefi-A-bundled.fd` `44531624...` at
`0x23d10c0`, the `ram-payload-first` build, so a RAM boot of the combined image does not silently prefer the installed ESP
configuration. A packed `boot_a` is nonetheless a macOS-installed-device object: the switch design assumes the installed
system, and the first persistent write of it is still gated on the G6 unit profile.

## Two host faults fixed on the way

- `tooling/native/ensure-hyperos.ps1` treated a tablet that is on adb with Android still booting as absent, fell through to
  the fastboot branch and hung `fastboot.exe` on `getvar` forever. It now gives that boot `WaitBoot` seconds to finish
  before it issues any fastboot command. Its `Line` also wrote with `Add-Content -Message`, which is not a parameter, so
  every run that passed `-Log` silently logged nothing; it is `-Value`.
- `fastboot boot` of a combined image does not always let `fastboot.exe` exit after the transfer (first run: the tablet
  booted, the host process hung for 6 min). `tooling/switch/run-selector-boot.ps1` caps the transfer at
  `FastbootExitTimeout` and then waits for the tablet rather than the process.

A forced-macOS probe (`out/selector-macos-probe-src`) was prepared and then dropped: run 2 already shows the default is
macOS, so the probe would prove nothing the run did not.
