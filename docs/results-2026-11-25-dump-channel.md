# oem dump channel: one command per reader boot, faults surface as Too many links

Date: 2026-11-25. Host and tablet; `fastboot boot` only.

## Result

`oem dump` works again, and the failures seen earlier in the day have a
mechanical explanation.

| Request (first command after `piano-uefi` appeared) | Result |
| --- | --- |
| `oem dump 8c0004000 100` | OKAY, `get_staged` returned 256 bytes, all zero |
| `oem dump 8c000000 100` | `Status read failed (Too many links)`, reader dropped |
| `oem dump 9cb00000 100` after a full reboot into Android | `Too many links`, reader dropped |
| `getvar product` first, then `oem dump 8c0004000 100` | `Too many links` |
| `getvar product` first, then `oem help` | `remote: 'not supported by piano-uefi'` |

Three rules follow:

1. Only the first fastboot command of a reader session reaches the UEFI
   fastboot handler. Anything issued before the dump (`getvar`, `oem help`)
   leaves the session unable to serve it. `get_staged` after a successful
   dump does work.
2. `Too many links` is what a dump of an address outside the reader's current
   stage-2 mapping looks like: the transfer faults, EP0 resets and fastboot
   reports the Windows status error. `0x8c000000` and `0x9cb00000` failed
   this way while `0x8c0004000` returned data. It is not a size limit and the
   channel is not degraded.
3. The reader must be booted in the same warm session as the failed run. After
   `adb reboot bootloader` from a completed Android boot, `0x8c0004000` reads
   back as zeros: the XNU image and its probes are gone. Addresses are in the
   Gunyah IPA space, so the kernel `__TEXT` base is `0x8c0004000`, not
   `0x8c000000`.

## Tooling

`tooling/native/dump-first.ps1 -Addr <hex> -Size <hex> -Out <file>` boots
`reader.img`, makes exactly one `oem dump` request as the first command after
`piano-uefi` enumerates, then pulls the staged file. It exits non-zero without
touching `get_staged` when the dump faults.

## Consequence for the ufs264 probe

`out/no-demura-control/ufs264-slots.bin`, taken earlier from `0x8c5912880`
after a full reboot, is page-table-like data rather than probe values because
the XNU run had already been destroyed. The stage-slot table has to be read in
the same warm session as the ufs264 boot, with the dump as the session's first
command. The static `_piano_stage_slots` address `0xfffffff00c912880` still
holds: `docs/results-2026-11-25-kc-layout.md` shows the ufs256 and ufs264
embedded `__DATA` are byte-identical.

Every run returned unattended to HyperOS OS3.0.309.0.WPYCNXM on `_a` with
`sys.boot_completed=1`; no partition, GPT, boot image or userdata write was
issued.

## The slot table address is right after all

The kernel links at VA `0xfffffff007004000` and DarwinBoot places it at PA
`0x8c0004000`, so a symbol's PA is `VA - 0xfffffff007004000 + 0x8c0004000`.
`_piano_stage_slots` at `0xfffffff00c912880` therefore lands at
`0x8c5912880`, which is the address `run-ufs256-dump2.ps1` used. An earlier
note in this file claimed the value was a transposition of `0x8c0916880`; that
was wrong, `0x8c0916880` is not mapped.

## Then the dump path stopped working altogether

After the first successful 256-byte read, every later `oem dump` failed with the
same status error, whatever the address, size or timing:

| Session | Request (always the first device command) | Result |
| --- | --- | --- |
| cold, from Android | `8c0004000 100` | OKAY, 256 bytes, all zero |
| cold, from Android | `9cb00000 100` | `Too many links` |
| cold, from Android | `8c0004000 100` again | `Too many links` |
| warm, after ufs264 | `8c0916880 1800` | `Too many links` |
| warm, after ufs264 | `8c0004000 100` | `Too many links` |
| warm, after ufs264 | `9cb00000 700000` and `9cb00000 2000` | `Too many links` |
| warm, after ufs264 | `8c5912880 2000`, three retries | `Too many links`, reader dropped |

Two controls separate the variable:

- `oem log` plus `get_staged reader-fw.log` still succeeds in the same reader
  build, so the staging and bulk paths are healthy. Only `oem dump` fails.
- The identical 256-byte request at `0x8c0004000` succeeded earlier in the day
  and failed later, so the failure is not a property of the address.

A host-side re-enumeration was attempted: `pnputil /restart-device
"USB\VID_18D1&PID_4E11\7ccdd855"` is denied with `Access is denied` even when
run escalated. Nothing in the sandbox can reset that interface.

Conclusion: the RAM readback route is blocked at the host USB layer for now.
The stage-slot and console dumps need a working `oem dump`, which needs the
tablet's USB connection re-established (different port or a re-plug) before the
ufs264 crash registers can be read.
