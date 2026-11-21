# Watchdog gate storage audit

The menu-gate image contains the expected FD at offset 4288. Its decoded FVMAIN contains DiskIoDxe, VariableRuntimeDxe and ResetSystemRuntimeDxe. The USB mass-storage driver GUID is absent. DiskIoDxe alone does not establish a physical storage provider.

The packed VariableRuntimeDxe and ResetSystemRuntimeDxe PE files match their build outputs byte for byte. VariableRuntimeDxe's generated configuration enables emulated NV variables, with no fixed emulation store address and zero flash variable base and size. This supports RAM-backed variables for the linked driver. It does not cover stock ABL, closed firmware modules or reset callbacks.

The candidate remains host-only. Neither the setup-failure cold reset nor the absence of writes across the whole boot chain has been demonstrated.

The Windows image SHA-256 was verified as `ea520f9ad88b890cb1c5d0e449b17d7d3401cbff6610eab0c48edd1d654b3ecc`. Parsing all 174 file headers in its decompressed FVMAIN found no FFS files matching the INF GUIDs for piano AndroidBoot, either CapsuleRuntimeDxe variant, NvmExpressDxe, Qualcomm UsbMsdDxe or generic UsbMassStorageDxe. This bounds the packaged DXE inventory. The check does not classify proprietary modules or cover stock ABL behavior.
