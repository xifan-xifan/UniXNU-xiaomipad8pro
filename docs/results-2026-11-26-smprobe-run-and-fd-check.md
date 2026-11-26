# 2026-11-26: the two probe RAM images carry the patched FD, and the unattended startMatching run got no readback

## Which configuration a `fastboot boot` image really reads

A screen photo of the tablet showed the UniXNU menu with the five standard
entries and, for entry 2 (`macOS k27 native, 8 CPUs (PSCI CPU_ON)`),

    'macOS k27 native, 8 CPUs (PSCI CPU_ON)' returned: Unsupported
    drivers 0 loaded / 0 failed   kits 0   acpi 0

The five titles are emitted both by `tooling/bootimg/build-ram-images.py` (the RAM
ramdisk) and by `tooling/persist/build-esp.py` (the installed ESP), so the titles
alone cannot say which volume answered. `docs/persistent-boot.md` section 2 says
why that matters: UniXNULib used the first FAT volume holding
`\EFI\UniXNU\unixnu.toml`, the UFS LUNs are published before
`UniXNURamDiskDxe`, and so any RAM image whose firmware lacks
`firmware/patches/unixnu/0001` boots the ESP configuration instead of its own
payload.

`tooling/native/check-image-fd.py` settles it by locating the firmware device
image inside a boot image's kernel field (BootShim + FD + DTB) and confirming the
copy by SHA-256:

| image | FD found | FD SHA-256 | patched? |
| --- | --- | --- | --- |
| `out/k27-smprobe/boot-piano-ufs266-smprobe.img` | `0x10c0`, one copy | `44531624219cf0a2c9b114ca624c36452c5c5fb9841420f9d0ab90181e4f0afb` | yes |
| `out/upstream-pub-20261125/boot-piano-ufs269.img` | `0x10c0`, one copy | same | yes |

That digest equals `out/fd/piano-uefi-A-bundled.fd.ram-payload-first`, which is
the stamp `tooling/firmware/build-fd.sh` only writes for a firmware containing
patch 0001. Both images therefore boot their own RAM payload and the photo's
`config: \EFI\UniXNU\unixnu.toml` line came from the image the host sent, not
from the installed ESP. Where UniXNU rejects the 8-CPU entry with `Unsupported`
is still open; nothing in `tooling/` produces that string.

## The run, and what the readback did

`tooling/native/run-ufs266-smprobe.ps1` booted
`out/k27-smprobe/boot-piano-ufs266-smprobe.img` (185,733,120 B, `a56f132d...`,
boot id `a79d3a17`), the first run of that image at all. ABL preflight:
`current-slot a`, `slot-unbootable:a no`, `slot-retry-count:a 6`.

- `fastboot boot` was accepted and the tablet came back to ABL fastboot after
  **37.2 s and 37.4 s** on the two runs, not after the `piano-reset-after=240`
  backstop that `tooling/native/check-boot-image.py` confirms is in both arg
  lines. Something ended the run long before 240 s; which stage that was is the
  question the probe was built to answer.
- The readback failed both times. The first run asked for `0x8c5000000`
  `0x1000000` and the second for `0x8c5912880` `0x2000` (the static
  `_piano_stage_slots` page, which has read back before), always as the first
  device command of a warm reader session, and both returned
  `FAILED (Status read failed (Too many links))` and dropped the reader.
  That matches `docs/results-2026-11-25-dump-channel.md`: since 2026-11-25 the
  `oem dump` path faults whatever the address, size or session, while
  `oem log` plus `get_staged` in the same reader build still works.

So no stage code, no console and no interrupt verdict from these two runs. The
documented cure is a host-side re-enumeration of the tablet's USB link, which this
host cannot perform: `pnputil /restart-device "USB\VID_18D1&PID_4E11\7ccdd855"`
returns `Access is denied` in the sandbox and again when escalated. Until the
cable is re-enumerated (unplug/replug or another port) the RAM readback route
stays shut, and any experiment whose only output is a memory dump is uninformative.

## Two collection-level measurements from the same day, kept here for reference

- Linking the 26 native drivers into a finished collection with
  `kc-build.py --kernel-fileset` is not a parameter problem. The base
  `piano-ufs266.kc` links at `0xfffffff007004000` and its top-level `__DATA`
  spans `0x5844000..0x5d3c000`; that window already holds the native drivers
  (from `0x5b04280`) and the original `__DATA` of every kept Apple kext
  (`0x5b20fe0`, `0x5b24aa0`, `0x5b2f3e0`, `0x5cfd504`, ...), leaving a largest gap
  of `0x29f9c`. Replaying a native driver into it fails with
  `no room for a 0x430d0-byte native kext __DATA (region 0x5b04000..0x5d3c000,
  largest gap 0x29f9c)`. A collection is a finished product, not a base.
- The upstream `NativeUfs` is *smaller* than the one inside the collection
  (`TEXT_EXEC 0x10e20` / `__DATA 0x8bf0` against `0x13e70` / `0x8c30`), so the
  in-place entry replacement described in
  `docs/results-2026-11-25-build-environment-audit.md` remains space-feasible.

## Device state and new tooling

Both runs returned the tablet to HyperOS `OS3.0.309.0.WPYCNXM` on slot `_a` with
`sys.boot_completed=1`. `tooling/native/ensure-hyperos.ps1` was added for that
purpose and is now the `finally` of the probe runner: it clears an unbootable slot
and a zero retry count with `--set-active` (which is what turned a merely failed
experiment into an unreachable tablet on 2026-11-25), reboots, and waits for
Android. The runner no longer assumes a TTY, no longer issues a second command
after a faulted dump, and no longer parses `slot-retry-count` by stripping
non-digits (that concatenated the slot name and the value into `60005`).
No partition, GPT, boot image or userdata write was issued in any of these runs.

## Where `returned: Unsupported` comes from, from the upstream source

The UniXNU core checkout in WSL (`/root/piano-native/UniXNU`) explains the
photo's wording in its own device record,
`docs/devices/nx769j-xwsjjctz.md:152`:

> The xnu entry type resolves through `UNIXNU_LOADER_PROTOCOL` and only
> DarwinBoot installs it, so with `loader = firmware` and no DarwinBoot module in
> `DXE.inc` the entry fails with `returned: Unsupported`, an error that names
> neither the protocol nor the missing module.

The menu line under it is upstream too:
`package/UniXNUPkg/Library/UniXNULib/Main.c:114` prints
`drivers %u loaded / %u failed    kits %u    acpi %u` and the entry is run from
the same loop, so nothing in `tooling/` is involved in either string.

Two ways that applies to piano, and they are not yet separated. DarwinBoot may be
absent from FD A, which would make both `xnu` entries fail and would explain why
the probe run ended after 37 s with nothing to read back: no kernel ever started.
Or DarwinBoot is present and the entry's own payload or arguments are refused.
The second possibility is the less likely one to hold on its own, because
`tooling/persist/common.py` (`fd_ram_window`, used by
`tooling/adt/check-ram-window.py`) reads a DarwinBoot memory bound out of FD A,
and `out/fd/piano-uefi-A-bundled.fd.ram-window` records `0x340000000`, which is
patch `firmware/patches/unixnu/0002`. A direct check is cheap and host-only: read
`FILE_GUID` from
`drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.inf` and scan the FD for it
(mixed-endian form), and grep the entry-execution path for the notice to learn
which call returns `EFI_UNSUPPORTED`. Do that before touching the collection or
the payload again: if DarwinBoot is missing from FD A, every image built with it
boots nothing, and no amount of kernel-side work will show.
