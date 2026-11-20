# Probe mailbox and EFI variable audit (2026-11-21)

The exact tracing FD (`d8a41ebb9f1e54490db012a0d412cfd143cccd8803efa40c9b38043fd9230e96`) was decompressed from its LZMA section. Its packed volume contains the generated 5,111,808-byte FVMAIN and the 46,596-byte VariableRuntimeDxe PE32 section byte for byte. The matching generated settings enable `PcdEmuVariableNvModeEnable=1` and set `PcdEmuVariableNvStoreReserved=0`, selecting runtime pool memory for emulated nonvolatile variables. This resolves the backend for EFI variable calls through that driver. Direct platform panic callbacks and the reset path require separate evidence.

The retained 27.3 KC matches its manifest SHA-256 and contains the mailbox console implementation symbols and boot argument strings. The tracing firmware's dump handler copies the requested range into a staging buffer and accepts up to 64 MiB, sufficient for the 7 MiB mailbox. This check covers software support and transfer limits. The actual range mapping and survival across reset still need device evidence.

The Linux maintenance image disables EFI pstore with `efi_pstore.pstore_disable=1`. Its `panic=0` controls Linux reboot behavior. Neither result establishes an XNU panic control, so the current probe image retains the audited boot arguments. No probe ran on the tablet for this audit.
