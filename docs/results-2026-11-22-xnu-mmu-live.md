# XNU MMU transition tablet trial

The paired candidate preserved the prior marker at CVP `0x9d1f56a0` and wrote a pre-SCTLR comparison value to `_piano_stage_slots + 24`. The frozen captured geometry maps that slot to PA `0x8c5941098`. The RAM image passed the DarwinBoot host parser and readback confirmed the replacement KC in its FAT payload. ABL preflight reported successful, bootable slot A with six retries and snapshot status none.

After unattended watchdog return, the low CVP capture still held `0x1000fc14793d`. The 256-byte reader dump at `0x8c5941080` held the existing XNU stage sequence: stage 0x0b across offsets 0 through 0x1f with a few differing bits, 0x0c at offset 0x20, 0x0d at offset 0x40, and 0x0f at offset 0x80. These are the source-defined stores after the SCTLR write and `isb sy`, after the SCTLR comparison, after FP enable, and immediately before returning to `arm_init`, respectively. This demonstrates that the transition completed and execution reached the arm_init return point.

The high-address reader returned 256 bytes, proving the path could read this address on the reset route. The reader returned unattended to HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, with boot complete. Captures and logs are in `out/no-demura-control/sctlr-pair-*`.

The 256-byte readback contains slot 0x10 at offset 0xa0 and slot 0x11 at offset 0xc0. The exact linked kernel calls `_piano_dbg_slot(0x11)` in `_arm_init` immediately before `_arm_vm_init`; its slot 0x12 call follows the return. Slot 0x12 is zero in this capture, so execution reached the pre-call marker but the readback does not establish a return from VM initialization. Root mounting and desktop remain unverified.
