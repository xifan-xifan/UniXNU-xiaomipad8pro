# SHA-512 entry checkpoint

The candidate added stage 190 in the SHA-512 compression routine and stage 191 before its return. The linked marker remained at `0xfffffff00c89be50`. Disassembly placed the stage 190 store at `0xfffffff00bbd8cb8`, after the function prologue and its stack guard load and store. The stage therefore witnesses progress through the prologue.

- Mach-O SHA-256 `8446f824185ad99c817a329c723f62c2b381636f08c1de358f1b7227ffea08f6`
- KC SHA-256 `23cf8c2532b242cf3c8f2e4eb45186a862fda25110f0d7665b5fac162151a7d9`
- Temporary image SHA-256 `f5cc2dff91ccfe865c112c616e8058f957c1984536548c87d1c8323a579a3f93`
- Stage capture SHA-256 `e4fa8ef4ad09b88a5871dd5af7e9749351bbe56d184a8934badc9383088ca842`

The DarwinBoot host parser passed and FAT readback matched the KC byte for byte. ABL preflight verified slot A successful and bootable with six retries and no pending snapshot update. After one temporary boot and watchdog return, the reader recovered stage 186. The remaining interval includes dispatch and the callee prologue. A probe before the stack guard access can separate those paths. HyperOS `OS3.0.309.0.WPYCNXM` returned unattended on slot `_a` with boot completed. The build and live logs are under `out/no-demura-control/sha-entry-*`.
