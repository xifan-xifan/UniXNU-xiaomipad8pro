# startMatching probe (2026-11-25)

Second instrumented kernel collection, built offline while the tablet is off USB.
It answers the question the acceptNewMedia probe cannot: whether
`IOService::startMatching(kIOServiceSynchronous)` ever blocks, and if so where the
two candidate stops differ. No relink, no kext sources, no new versions.

## What the disassembly says

`__ZN9IOService13startMatchingEj` at `0xfffffff00c1450ec` in
`out/inputs/piano-ufs264.kc` (`com.apple.kernel`):

| Address | Instruction | Meaning |
| --- | --- | --- |
| `0xfffffff00c1450f0` | `stp x24, x23, [sp, #-0x40]!` | first instruction after `pacibsp` |
| `0xfffffff00c1451f8` | `bl 0xfffffff00c155420` | prepare; its `x0` becomes `w21` at `0xfffffff00c1451fc` |
| `0xfffffff00c145250` | `mov x0, x20` / `bl doServiceMatch` | loop head, `doServiceMatch` at `0xfffffff00c145500` |
| `0xfffffff00c145294` | `ldr w8, [x20, #0x4c]` / `and w9, w8, #0x3ff` | busy count is the low 10 bits of the state word |
| `0xfffffff00c14529c` | `cmp w21, w9` / `b.hs 0xfffffff00c145358` | exit the loop once the entry count `w21` is at or above the current count |
| `0xfffffff00c1452ac` | `orr w8, w8, #0x18000000` | set `kIOServiceSyncPubState | kIOServiceBusyWaiterState` |
| `0xfffffff00c1452ec` | `bl 0xfffffff00ba1f1bc` | `assert_wait(this, THREAD_UNINT)` |
| `0xfffffff00c1452f4` | `bl 0xfffffff00b9fdd84` | release the lock |
| `0xfffffff00c1452fc` | `bl 0xfffffff00ba1fa84` | `thread_block(THREAD_CONTINUE_NULL)` |
| `0xfffffff00c145300` | `ldr w8, [x20, #0x4c]` | first instruction after the block returns |
| `0xfffffff00c145358` | `and w8, w8, #0xf7ffffff` | loop exit, clears `kIOServiceSyncPubState` |
| `0xfffffff00c145398` | `tbnz w19, #4, 0xfffffff00c1453b0` | final path before the epilogue at `0xfffffff00c1453ac retab` |

So the audit's two cases are separable from the table alone: a stop after
`doServiceMatch` was entered but before it returned means the block is inside
`doServiceMatch`; a stop after `thread_block` was entered but before the
instruction behind it ran means the block is in `thread_block`.

## Probe sites

| code | address | replaced instruction | label |
| --- | --- | --- | --- |
| 0xB0 | `0xfffffff00c1450f0` | `stp x24, x23, [sp, #-0x40]!` | startMatching entry |
| 0xB1 | `0xfffffff00c145254` | `bl doServiceMatch` | before the match |
| 0xB2 | `0xfffffff00c145258` | `ldr x16, [x20]` | match returned |
| 0xB3 | `0xfffffff00c1452a0` | `b.hs 0xfffffff00c145358` | busy >= entry: leaving the loop |
| 0xB4 | `0xfffffff00c1452fc` | `bl thread_block` | about to block |
| 0xB5 | `0xfffffff00c145300` | `ldr w8, [x20, #0x4c]` | block returned |
| 0xB6 | `0xfffffff00c145358` | `and w8, w8, #0xf7ffffff` | loop exit path |
| 0xB7 | `0xfffffff00c145398` | `tbnz w19, #4, 0xfffffff00c1453b0` | final return path |

Eight stubs of 192 B take 1,536 B of the 1,657-byte zero hole at
`0xfffffff00bc07988`; the two conditional replays (0xB3, 0xB7) share two 4-byte
helpers at `+1536` and `+1540`, so the block uses 1,544 bytes and leaves 113 B
untouched. `stp` and `pacibsp` are kept apart on purpose: the first instruction
of the function carries the pointer-auth prologue and is not replaced.

## Artifacts

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `out/k27-smprobe/piano-ufs266.kc` | 147,095,552 | `9f276dff4a35ef6ed89ffd2c8a535d3742ed4f843623b744f310c8fa68aca64b` |
| `out/k27-smprobe/boot-piano-ufs266-smprobe.img` | 185,733,120 | `a56f132dc2f3f5e89bbd09f90252c6e630d6f977427a2f9e16267830119e43fe` |

The image is `boot-piano-ufs264cfg.img` with the new collection written through
the existing FAT chain; the ramdisk file is the same length, so nothing moves.
Boot id `a79d3a177e9339a2be3cd88a8a04e725c2bc18b9`. A byte diff against the
ufs264 image is 1,552 bytes in 51 runs: the 20-byte boot id at `0x240`, the
eight 4-byte site branches and the stub/helper block, all inside the embedded
`kernel.macho` at `0x65f2988` onward. Readback of the ramdisk file hashes to the
new collection.

Verified from the image: each site holds `b` into its stub, each stub sets `w0`
and `w1` to its own code, calls `_piano_dbg_kv`, restores `x0`-`x18`, NZCV and
`x30`, replays the original word and branches to `site+4`; 0xB3's `b.hs` goes to
helper `0xfffffff00bc07f88` → `0xfffffff00c145358`; 0xB7's `tbnz` goes to helper
`0xfffffff00bc07f8c` → `0xfffffff00c1453b0`; the rest of the hole is still zero.

Runner: `tooling/native/run-ufs266-smprobe.ps1` (image hash pinned, window
fallback identical to the acceptNewMedia runner, decoder invoked with
`--codes b0 b7`). Without a device it exits 2 with `no adb and no fastboot device`,
which is the current state.

## How to read the answer

- 0xB0 absent: the path never enters `startMatching`; the stall is earlier, in
  service registration or the match caller.
- 0xB0 and 0xB1 present, 0xB2 absent: `doServiceMatch` did not return — case 1.
- 0xB2 present, 0xB4 absent: the loop never waited; the caller stalls after
  this function, so the loop is not the culprit.
- 0xB4 present, 0xB5 absent: `thread_block` never resumed — case 2, which has a
  known `thread_block_reason`/`AST_YIELD` workaround.
- 0xB5 present: the wait resumed; 0xB3 or 0xB6 then shows which exit was taken,
  and 0xB7 shows a normal return.

Built and structurally verified, to be run on the tablet. Nothing was flashed or
booted.
