# DarwinBoot handoff checkpoints

The AArch64 COFF assembly check and the complete DEBUG CLANGPDB build passed on UbuntuPiano. The build reused the successful enter-mailbox package paths with the diagnostic source first. It used `UNIXNU=TRUE`, `FD_BASE=0xA7100000`, `FD_SIZE=0x300000`, `FD_BLOCKS=0x300`, `ENABLE_SECUREBOOT=0` and `DEVICE_MODEL=0`. The log and 3 MiB FD are `build-complete-macros.log` and `handoff-cvp.fd` under `/tmp/piano-handoff.hddbWw4L/`.

The decoded FVMAIN contains 174 entries in the same order as `enter-abl.fd`. DarwinBoot, BdsDxe, PcdDxe and SmBiosTableDxe differ by FFS hash. The extra differences require payload comparison against the frozen no-demura firmware. Tablet testing is pending. The fourth checkpoint is before `br x1`. A separate XNU witness is required to verify execution after the branch.

FD SHA-256 `44531624219cf0a2c9b114ca624c36452c5c5fb9841420f9d0ab90181e4f0afb`.
