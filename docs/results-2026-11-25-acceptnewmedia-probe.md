# 2026-11-25: acceptNewMedia probe kernel collection

The ufs171 console stops after `recordMediaParameters`, so the next image runs a
kernel with seven 4-byte branch probes inside `IOBlockStorageDriver::acceptNewMedia`.
The collection is produced by patching `out/inputs/piano-ufs264.kc` in place; no
relink, no `paclr264.macho` and no keep list are needed.

Input: `out/inputs/piano-ufs264.kc`, 147,095,552 B, SHA-256
`529265c7839c51fe4d643af3228f4061297dd5d2ed5cce3e585fbc6bcca14a72`.
Output: `out/k27-acceptprobe/piano-ufs265.kc`, same size, SHA-256
`05d8ccb53c61c7185f11f025fb8f698eccc342a50a455ccd1d40644077e1c89d`.

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

## RAM image assembly

`out/k27-acceptprobe/boot-piano-ufs265-probe.img`, 185,733,120 B, SHA-256
`86b534f29c528058a85e4ae8660be07f860cfe1ce12d9ab62efac6f683cea5f7`, boot id
`6d766e67785a46c679870df467257b649aa87d8c`.

It is `boot-piano-ufs264cfg.img` with one file replaced inside the v2 ramdisk:
the ramdisk starts at `0x411000` (FAT32, 512 B sectors, 4 sectors per cluster,
32 reserved sectors, 2 x 686 FAT sectors), the path `EFI/UniXNU/Payloads/kernel.macho`
is a 147,095,552 B file in a 71,824-cluster chain starting at cluster 5, and the
new collection has the same length, so only the cluster contents change. The
Android boot id at 0x240 is recomputed over kernel, ramdisk, second, recovery
dtbo and dtb exactly as the archived `vm-mem-repack.py` does. Nothing else in the
image moves: a byte diff against the ufs264 image has 909 differing bytes in 47
runs - the 20-byte id, the 7 four-byte probe branches, the 896-byte stub block and the 4-byte
trampoline - so the header, DTB, trustcache and `scratch-data.img` are untouched.

The embedded copy of `kernel.macho` in `boot-piano-ufs264cfg.img` is byte-identical
to `out/inputs/piano-ufs264.kc` (SHA-256 `529265c7...`), and the readback of the
new image hashes to the probe collection (`05d8ccb5...`). The earlier note about a
divergence at offset 0x800 came from searching for the collection's first bytes at
a raw offset before mapping the FAT; it was not a real mismatch. The image carries
no AVB footer (`avbtool verify_image` rejects both the old and new files the same
way), and `fastboot boot` of a v2 boot image does not need one.

The RAM swap helper is `out/fatswap.py` (`out/` is ignored): it parses the FAT32
ramdisk, asserts the file size, writes through the existing cluster chain and
re-reads the file to hash it. This replaces the `mcopy` call that the archived
scripts made in WSL, which is not usable on this host now.

Built and checked, to be run on the tablet.

## Probe value and readback window

Disassembling `_piano_dbg_kv` (`0xfffffff00bbc7590`) shows the ABI: `w0` is the code, and
the function stores `x1` into all four words of the slot at `_piano_stage_slots + (w0-0x0b)*32`,
then cleans the cache line. The first stub passed `x1 = 0`, so every probe entry would have
read back as zero and the run would have proven nothing. The generator now emits
`mov w1, #code`, and a disassembly of the seven stubs shows `w0, #0xa0` with `w1, #0xa0`
through `w0, #0xa6` with `w1, #0xa6`.

The runner also stopped trusting the fixed address. It now takes one 1 MiB dump at
`0x8c5000000` and runs `tooling/native/decode-slots.py --scan`, which looks for 32-byte
entries whose four words are all a code in 0xa0-0xa6 and prints the implied table base and a
consensus over all hits, so a moved slide no longer hides the answer. The earlier 8 KiB read
at `0x8c5912880` held monotonic `0x001d86...003b0d..` values with spread 4 - not four equal
copies - so it was not the `piano_dbg_kv` table and could not have shown a probe.
- The shipped image was re-checked from the file itself: extracting
`EFI/UniXNU/Payloads/kernel.macho` out of `boot-piano-ufs265-probe.img` hashes to
`05d8ccb5...`, all seven sites hold `b` to their stub in the hole, every stub sets
`w0` and `w1` to its code, calls `_piano_dbg_kv` and branches back to `site+4`.

## Conditional-branch replay fix (0xA4)

Reading back the disassembly of the shipped image exposed a flaw that would have made the run
uninformative at the one place it matters. The 0xA4 site is `cbz w0, 0xfffffff00aeb87d0` - the branch
taken when `attach` *succeeded*. The generator copied that instruction verbatim into the stub, so its
encoded ±1 MiB offset was now measured from the stub hole: capstone decoded the copied word as
`cbz w0, 0xfffffff00bc07d6c`, i.e. a jump into zero padding inside the hole. Any run where `attach`
returned 0 would have died there and never reached 0xA5 or 0xA6, hiding exactly the success path.

The generator now detects conditional branches (`cbz`, `cbnz`, `tbz`, `tbnz`, `b.cond`) and re-targets
the copied word at a 4-byte helper at `0xfffffff00bc07d08` (right after the seven stubs) that holds the
far unconditional `b 0xfffffff00aeb87d0`. The stub tail is now `add sp, sp, #0xb0` / `cbz w0, helper` /
`b site+4`, so the stack is restored before either branch and the success path continues at
`0xfffffff00aeb8668`.

Verified from `boot-piano-ufs265-probe.img` itself: the embedded `kernel.macho` hashes to `d4dfefb4…`,
all seven sites hold `b` into the hole, each stub writes its own code into `w0` and `w1`, calls
`_piano_dbg_kv`, replays the original instruction and branches to `site+4`, 0xA4's replay points at the
helper, and the helper branches to the original destination. The stub block is 7 x 128 B = 896 B and
the helper sits at +896, so nothing else in the hole is touched.

`_piano_dbg_kv` was also disassembled to confirm the probe can write at all: it rejects codes outside
`0x0b..0xca` (`sub w8, w0, #0xcb` / `cmn w8, #0xc0` / `b.lo ret`), stores `x1` into all four words of
`0xfffffff00c912880 + (code-0x0b)*32` and cleans the cache line, so 0xA0-0xA6 land in the table and
survive to the readback.`decode-slots.py --scan` also dumps every non-empty slot of the table it lands on, and when no probe
code is present it reports the longest run of agreeing 32-byte entries with the table base that run
implies. That separates "the window is unmapped or the read landed elsewhere" from "the probe code is
missing", which the bare hit/no-hit answer could not.
## Return-value slots (0xA7, 0xA8)

Knowing only "the last code set" leaves the interesting question open: when `attach` returns, was it a
failure with an IOReturn, or did it never return? The 0xA2 and 0xA4 stubs now store the value live in
`x0` as well: 0xA7 receives `instantiateMediaObject`'s return and 0xA8 receives `attach`'s return, each
written into all four words of its slot like a probe code. A zero 0xA8 with 0xA4 set means success; a
nonzero 0xA8 is the IOReturn to decode.

Stubs grew from 128 B to 192 B to hold the second `_piano_dbg_kv` call (7 x 192 B = 1344 B inside the
1657-byte hole, helper at +1344). The extra call is `ldr x1, [sp]` (the saved original `x0`),
`mov w0, #slot`, `bl _piano_dbg_kv`; the register restores still follow it, so the replayed instruction
sees the original `x0` and the original flags.

Re-verified from the image: embedded `kernel.macho` hashes to `05d8ccb5…`, sites 0xA2 and 0xA4 each hold
two `bl`s into `_piano_dbg_kv`, the other five hold one, 0xA4's conditional replay still reaches the
helper at `0xfffffff00bc07ec8`, and the helper branches to `0xfffffff00aeb87d0`. The byte diff against
the ufs264 image is 1356 bytes in 48 runs, all inside the boot id, the seven branches, the stub block and
the helper.