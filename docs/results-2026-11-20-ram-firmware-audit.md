# RAM boot firmware audit (2026-11-20)

The pinned piano-uefi revision `31545f9392d5f5c2acae14cec2fa3a0aa1de51d4` lists UFSDxe, EnhancedFatDxe, VariableRuntimeDxe and BdsDxe in `uefi/pianoPkg/Include/DXE.inc` and `APRIORI.inc`. The lists establish which components are declared. They do not identify the variable store backing or prove which block operations the final composed firmware performs.

The device repository's `firmware/patches/unixnu/0001-UniXNULib-config-prefer-the-RAM-payload.patch` tries the launching volume first, then prefers the RAM payload in filesystem enumeration. If neither supplies a configuration and no configuration error stops the search, it can still try other filesystems, including UFS. The preference does not enforce RAM-only boot. The XNU `data-persist` setting cannot constrain earlier firmware execution.

Qualification requires auditing the final FD's active variable services and UFS behavior, and ensuring a diagnostic image stops when the RAM configuration is absent rather than loading a configuration from disk. This review was based on source at the pinned revisions. The final FD and physical device were not exercised.
