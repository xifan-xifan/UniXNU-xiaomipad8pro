# 2026-11-25: acceptNewMedia probe kernel collection

The ufs171 console stops after `recordMediaParameters`, so the next image runs a
kernel with seven 4-byte branch probes inside `IOBlockStorageDriver::acceptNewMedia`.
The collection is produced by patching `out/inputs/piano-ufs264.kc` in place; no
relink, no `paclr264.macho` and no keep list are needed.

Input: `out/inputs/piano-ufs264.kc`, 147,095,552 B, SHA-256
`529265c7839c51fe4d643af3228f4061297dd5d2ed5cce3e585fbc6bcca14a72`.
Output: `out/k27-acceptprobe/piano-ufs265.kc`, same size, SHA-256
`30f0d67397562a86dc8f9b882ed92b6fe53734c938093a94eac2d1c0e89bcf99`.

| code | address | replaced instruction | position |
|---|---|---|---|
| 0xA0 | `0xfffffff00aeb8268` | `sub sp,sp,#0x140` | `acceptNewMedia` entry |
| 0xA1 | `0xfffffff00aeb859c` | call site | before `instantiateMediaObject` |
| 0xA2 | `0xfffffff00aeb85a4` | `cmp x0,#0` | after instantiate returns |
| 0xA3 | `0xfffffff00aeb865c` | call site | before `m->attach(this)` |
| 0xA4 | `0xfffffff00aeb8664` | `cbz w0` | after `attach` returns |
| 0xA5 | `0xfffffff00aeb8910` | call site | before `attachToParent` |
| 0xA6 | `0xfffffff00aeb8b48` | call site | before `registerService` |

Each stub is 128 B and lives in the zero hole at `0xfffffff00bc07988`
(1,657 B, 8-byte aligned) inside `com.apple.kernel __TEXT_EXEC`. A stub saves
x0-x18, NZCV and x30, calls `piano_dbg_kv` (`0xfffffff00bbc7590`) with the code,
restores state, replays the original instruction and branches back to
`address+4`. x30 must be preserved: the first assembly draft lost it and would
have corrupted the return address.

The generator is `out/patch-acceptprobe.py` (`out/` is ignored, so it is not
committed). It asserts the hole is zero-filled, checks every branch is in range
and re-disassembles the written stubs. Re-running regenerates the same file from
the original KC.

Reading is one 8 KiB dump of `_piano_stage_slots` (`0xfffffff00c912880`) after a
`fastboot boot` of the RAM image that carries this collection; the last code seen
is the stop point: 0xA1 instantiate never returned, 0xA2/0xA3 media built but
attach entry, 0xA4 attach failed or hung, 0xA5 attachToParent, 0xA6
registerService.

Built and structurally checked, to be run on the tablet. No image has been
flashed; the probe RAM image has not been assembled yet, because the KC is
embedded at offset `0x4c1d80` of `out/no-demura-control/boot-piano-ufs264cfg.img`
and the embedded copy diverges from `piano-ufs264.kc` at `0x800`, so the container
layout and its signature have to be checked before swapping the blob.
