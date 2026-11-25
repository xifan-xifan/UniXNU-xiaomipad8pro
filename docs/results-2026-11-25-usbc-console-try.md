# Trying the USB-C console as the readback route (2026-11-25)

Tablet runs, `fastboot boot` only; no partition, GPT, boot image or userdata write.

## What was built

`out/k27-usbc/boot-piano-ufs267-usbc.img` (`0be526f8…`, 185,733,120 B, boot id `604f94c4…`): the ufs265
probe collection with the `macos-8cpu` boot line changed in the ramdisk's `EFI/UniXNU/unixnu.toml` from
`data-persist=0` to `piano-usb=3` (same byte length, Android boot-image id recomputed; verified by re-parsing
the FAT inside the image). The point was to replace the broken `oem dump` readback with the kernel's own
console: the monitor answers `peek PA [n]`, `ioreg`, `args`, `reset fastboot` (`docs/drivers/usb-device-debug.md`),
which is exactly what this bring-up needs, unattended, with no firmware cooperation.

## What happened

The image booted (sent 181 MiB in 4.7 s, `Booting OKAY`) and the tablet ran the whole 240 s window and came
back to ABL fastboot by itself, as designed. But **no serial device ever appeared on the host**: during and after
the run `serial.tools.list_ports.comports()` returned an empty list, so the gadget never enumerated. The likely
reason is the stop itself: the run ends inside `IOBlockStorageDriver::start` on IOKit's config thread, and
`PianoDwc3` is the twelfth native kext, so its takeover never runs. The console therefore cannot be the readback
route for *this* stop, although the monitor's `peek`/`reset fastboot` remain the right tool once the boot gets
further, and the runner (`tooling/native/run-ufs267-usbc.ps1`) is written for it.

## The cost, measured

After that run ABL stopped accepting *any* RAM image:

| Request | Before | After |
| --- | --- | --- |
| `fastboot boot boot-piano-ufs265-probe.img` (hash verified, booted twice earlier today) | `Sending … OKAY 4.7 s`, `Booting OKAY` | `Booting FAILED (remote: 'Failed to load/authenticate boot image: Load Error')` |
| `fastboot boot boot-piano-ufs268-noufs.img` (new control image) | — | same `Load Error` |
| `fastboot reboot` | tablet returned to HyperOS, `adb devices` showed it | tablet came back to `fastboot` within seconds, `adb devices` empty for 6 min |

`product: piano` still answers, so the ABL fastboot itself is alive; what fails is the *bulk transfer* of the
image. That is the same host/device USB-link symptom as `Status read failed (Too many links)` on `oem dump`
(`docs/results-2026-11-25-dump-channel.md`), and it appeared right after a run that hands the DWC3 controller,
its GDSC and clocks to a driver (`piano-usb=3`): the PHY state that driver leaves behind is not what the next
ABL session expects, and nothing on the host can reset that device (`pnputil /restart-device` now reports
`The device is not connected`, `Get-PnpDevice`/CIM is unavailable from the sandbox).

Conclusion for the plan: the `piano-usb` experiment is not free on this tablet+host pair — it costs one power
cycle to get ABL's download path back. The route is still worth reopening later (the monitor's `peek` replaces
`oem dump` entirely), but only after the boot reaches the console kext, which means after the storage stop.
