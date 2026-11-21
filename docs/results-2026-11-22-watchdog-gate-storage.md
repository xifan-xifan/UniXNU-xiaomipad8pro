# Watchdog gate storage audit

The menu-gate image contains the expected FD at offset 4288. Its decoded FVMAIN contains DiskIoDxe, VariableRuntimeDxe and ResetSystemRuntimeDxe. The USB mass-storage driver GUID is absent. DiskIoDxe alone does not establish a physical storage provider.

The packed VariableRuntimeDxe and ResetSystemRuntimeDxe PE files match their build outputs byte for byte. VariableRuntimeDxe's generated configuration enables emulated NV variables, with no fixed emulation store address and zero flash variable base and size. This supports RAM-backed variables for the linked driver. It does not cover stock ABL, closed firmware modules or reset callbacks.

The candidate remains host-only. Neither the setup-failure cold reset nor the absence of writes across the whole boot chain has been demonstrated.
