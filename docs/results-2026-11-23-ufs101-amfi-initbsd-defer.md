# ufs101-103: AMFI initbsd defer and the first full 240 s window, 2026-11-23

ufs100 identified the first remaining stop after `IOKitBSDInit`: `mac_policy_initbsd()`
called AMFI's `mpo_policy_initbsd` inline and that call never returned.  The string
`AKSKernelServices` exists only in AMFI's `__TEXT`; no code in the collection
references it, so the wait is a pure runtime wait for a SEP service this device
does not provide.

ufs101 (`tooling/native/mkpaclr101.py`) moves AMFI's initbsd off the serial
`mac_policy_initbsd` loop.  Every policy except AMFI still runs inline.  AMFI is
recorded, the exclusive lock is released, and then `kernel_thread_start` spawns
one thread per deferred policy.  Kernel sha256
`779d044e0c029319d8e20ca8e49ae9ec42bc5934ec14d2ff22c011e2f098f570` (28055320 B);
`piano-ufs101.kc` rebuilt with `--keep-all` at 0x8c48000 bytes and 364797 chained
pointers; image `boot-piano-ufs101cfg.img` sha256
`e4f1aee3d369927f40913175e790c106c4ad632c6c69083c5a279a111d983758`.
Its console shows `bsd_autoconf: IOKitBSDInit rc=0`, the AMFI thread spawn
returning 0, and then `NativeRamBlock: RAM window 0x8c0004000..0xac0004000`.
Both deadlocks were gone.

ufs102 only moved `piano-reset-after` from 90 to 240 while reusing the ufs101
KC (image sha256 `dd351955f78c83abe808c13eb920730738b0c84516bbcf3cb0b25a0d05776d35`).
It still returned to fastboot after 81.7 s: the reset tick and the watchdog pets
are both `thread_call_enter_delayed` work and no arm timer callout fires, so the
Gunyah watchdog bit 32 s after the last pet regardless of the requested window.

ufs103 (`tooling/native/mkpaclr103.py`) replaces the re-armed thread call with a
kernel thread that spins on `mach_absolute_time()`, pets the watchdog every 10 s
and resets at the requested limit.  `osfmk/arm64/machine_routines.c` is compiled
with `-DXNU_VM_GUEST=1` so the piano block is present; kernel sha256
`e6c5cd7c0460638f60a8c7d9058c90235b4f3165abe90d9aa4d7164ee8eda155`,
`piano-ufs103.kc` 0x8c48000 bytes / 364797 pointers, image
`boot-piano-ufs103cfg.img` sha256
`2053ac54c831a8ad4b042bd09e11ab7df8ce01af83ef9e59b6b0afc2839d212c`.
The run reset itself after 277.8 s (`piano-reset: spin` beats at 27/57/.../237 s,
then `240 s reached, PSCI SYSTEM_RESET`), the reader dumped the zone and the
tablet returned to completed HyperOS `OS3.0.309.0.WPYCNXM`.

The decoded console (`out/no-demura-control/ufs103-console.txt`, 13119 bytes;
`xnu-log.py decode`) shows `bsd_init` completing `IOKitBSDInit`, `loopattach`,
`ether_family_init` and `setconf`, then the root publisher bypassing the IOBSD
resource wait and walking the registry (`visited=6 emitted=3`).  The expected
root UUID `84581E99-85BE-4957-AC53-2947E887B3E4` is not present, and
`IOService::waitForMatchingService(matching, 30 s)` never returns because its
deadline wait also depends on the dead timer.  The AMFI worker never printed
`thread run` in this window: the non-yielding ticker held the only runnable CPU
after `bsd_init` blocked.  ufs104 keeps the 240 s ticker but calls
`thread_block(THREAD_CONTINUE_NULL)` in its spin loop so the deferred AMFI
worker and the boot thread can run.

Every run in this round used `fastboot boot` only.  No partition, GPT, boot
image or userdata write was issued; each run returned unattended to completed
HyperOS A.
