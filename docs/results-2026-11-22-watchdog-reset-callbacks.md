# Watchdog setup failure reset callbacks

The watchdog gate image audited in [the storage result](results-2026-11-22-watchdog-gate-storage.md) requests an EFI cold reset before starting diagnostic Fastboot when watchdog setup fails. ResetSystemRuntimeDxe runs the boot-time platform filter, standard notification and platform handler lists before calling ResetCold. The Fastboot command channel is therefore unavailable on this failure path.

A source search of the retained piano firmware tree found reset notification registration in the piano block flush hook, AdvancedFileLogger, AdvancedSerialLogger, DebugFileLogger, Tcg2Dxe and NvmExpressDxe. The isolated boot manager omits the piano hook installation. The candidate's decoded FVMAIN lacks the INF file GUIDs of all five standalone drivers. This check covers those open-source registration sites. Qualcomm binaries and actual early reset behavior still require separate evidence.
