# 2026-11-26 - a RAM boot of our persistent v5 ended with the tablet off the bus; and the two things it settled on the way

Follows [results-2026-11-26-storage-abi-two-public-routes-tried.md](results-2026-11-26-storage-abi-two-public-routes-tried.md).
Host: this Windows machine. Upstream unchanged (`23f6425f`); the 8 commits since `d70c16ce` are all display/GPU qualification
(PianoDpu M3/M5, GOP FourCC decoding, a presented-cadence probe, the media showcase) - nothing about the switcher, persistence or
the published gaps, so nothing to take from them for the current blocker.

## What ran

`adb reboot bootloader` -> `fastboot -s 7ccdd855 boot out/persistent/boot-piano-macos-persistent-v5-ufs.img` (100,663,296 B,
sha256 `5de858b8157e9572f9f44ad9648c87d669d6a53e5aecb50630df34d0b080f19c`), with `tooling/native/usbcap.py` capturing the
PianoDwc3 CDC-ACM ports (VID:PID `1209:0001`) for 210 s. `Sending OKAY`, `Booting OKAY`. The console file stayed at **0 bytes**,
the device came back into ABL fastboot (so XNU was reached or refused and reset back to the loader), `fastboot oem log` never
produced a response, and within a couple of minutes the tablet stopped appearing on the USB bus entirely: no fastboot, no adb, no
serial port, for ~9 minutes of polling. That is a state no host command can leave; it needs power at the device.

## What it settled: the AVB identity of our persistent images

Parsing the AVB footer of both persistent images:

| image | bytes | footer | authentication block sha256 |
|---|---|---|---|
| `boot-piano-macos-persistent-v5-ufs.img` | 100,663,296 | `AVB0` at 100,593,664 | `0744ca237cc3be29e5418f7a...` |
| `boot-piano-macos-persistent-v4-avb.img` | 100,663,296 | `AVB0` at 100,593,664 | `0744ca237cc3be29e5418f7a...` (identical) |

`v4-avb` is the image that was actually flashed to `boot_a` and booted from disk on this tablet (its sha256 `0d52dd5e...` is the
device's own `boot_a` read back on 2026-11-25, [results-2026-11-25-slot-a-bootset-revert.md](results-2026-11-25-slot-a-bootset-revert.md)).
v5 carries the same authentication block, so **v5 is as flashable as v4** and the shorthand recorded earlier - "host AVB key is not
the device key, so host-built images only work with `fastboot boot`" - does not apply to these persistent images: they were built
signed for this device's chain, which is exactly why flashing v4 to `boot_a` produced a disk boot on 2026-11-25. The claim stays
true only for the unsigned/offline RAM images.

## What it settled: the real gap in the installed state

`out/persistent/installed.json` (written 2026-11-24 23:23) records the ESP on this tablet: `esp-device.img` 1,073,741,824 B
sha256 `68c28df15bcf55a589441df37e9c5fd94614e2771c437e955f190e3eb0aab898`, partition index 37 at `first_lba 124289792` - our own
geometry, produced by `tooling/persist/build-esp.py` with `profile: daily`, whose inputs are all ours: KC
`529265c7839c51fe...` (`piano-ufs264.kc`), ADT `4e904d90e92e880c...` (`paclr53.adt`), trust `9d225ea325b3b10e...` (our 3-module
bundle), Data seed `48dbc08d5955a202...` (ours, not the reference pin). The `daily` profile carries `native-ufs-root`
(`build-esp.py` lists it among the arguments the fastboot-profile drops, i.e. the daily one keeps it), so the installed ESP asks
for a **UFS root** - and the root image was never written to this tablet's disk. That is the honest shape of the milestone: the
boot chain and the ESP are ours and in place, the payload they point at is not on the medium. It also matches why the RAM boot did
nothing visible: the ESP's root has no manifest to find.

Two consequences recorded here rather than left implicit:

- The root image exists and is verified (`root.img` 20,936,916,992 B `e682bd1f9b9c...`), and the write plan for this unit is
  settled by the unit profile made from our own GPT heads: `linux_data` first `107512576`, 5,111,552 blocks, last `112624127`,
  `11,665,664` blocks left free, Data reserve from `124027648`. Writing it is a persistent write and goes through the write plan,
  not through a raw `dd` by hand.
- The stale reference-geometry question is now testable rather than arguable: a root image written by the reference plan would
  have landed at LBA `71789568`, inside our `userdata` (3,632,128..102,269,695). Whether any such write ever happened on this
  tablet is answerable by reading the first bytes of that offset for the root manifest - worth doing in the next maintenance
  session before the real write, so we know whether a stale manifest exists to confuse root selection.

## Reopen condition

The tablet has to be given power at the device (hold power until it restarts, or charge for a while and then power on) - nothing
on the host can reach it in this state. Once it enumerates again: `adb`/`fastboot` see it, and the read-only checks above are the
next step. This is the first time in this goal that a physical action was required after a RAM-only boot, and it is recorded as a
risk of the RAM boot loop rather than as a normal step.