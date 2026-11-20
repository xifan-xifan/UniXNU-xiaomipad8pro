# RAM boot firmware audit (2026-11-20)

The pinned piano-uefi revision `31545f9392d5f5c2acae14cec2fa3a0aa1de51d4` lists UFSDxe, EnhancedFatDxe, VariableRuntimeDxe and BdsDxe in `uefi/pianoPkg/Include/DXE.inc` and `APRIORI.inc`. The lists establish which components are declared. They do not identify the variable store backing or prove which block operations the final composed firmware performs.

The device repository's `firmware/patches/unixnu/0001-UniXNULib-config-prefer-the-RAM-payload.patch` tries the launching volume first, then prefers the RAM payload in filesystem enumeration. If neither supplies a configuration and no configuration error stops the search, it can still try other filesystems, including UFS. The preference does not enforce RAM-only boot. The XNU `data-persist` setting cannot constrain earlier firmware execution.

Qualification requires auditing the final FD's active variable services and UFS behavior, and ensuring a diagnostic image stops when the RAM configuration is absent rather than loading a configuration from disk. This review was based on source at the pinned revisions. The final FD and physical device were not exercised.

The retained 3,145,728-byte `piano-uefi-A-bundled.fd` hashes to `394c5c786f4d114ccb0e709083ce574e4c5964593bd656bdffe8dd0f2e5bd5d2`. The historical `ram-native-base-control/build-A-bundled.json` records that digest and classifies the build as `OFFLINE_CANDIDATE_NOT_DEVICE_TESTED`. The digest differs from both FD A entries in the current artifact catalog. Keep the retained FD as an offline comparison sample pending a source-to-artifact provenance match. A check under the Mac checkout and `/tmp` found no standalone `.fd` file.

The pinned board DXE lists select the source `VariableRuntimeDxe`. The same repository carries a separate Qualcomm `VariableDxe` binary. The presence of that binary in the repository does not establish its inclusion in the composed FD. Resolve the includes and inspect the firmware volume before drawing conclusions about persistence.
