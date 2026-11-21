# UEFI debug log export (2026-11-21)

The retained UbuntuPiano firmware source defines `Piano_Log` at `0xD7F00000` with a size of 1 MiB. `PianoLog.h` reserves the last three pages for the SMMU snapshot and Android handoff. SEC initializes a fresh log each boot. `StageDebugLog` stages only the ring's log bytes in chronological order, and the Fastboot command dispatcher selects it with `oem debuglog`. This differs from the `oem log` capture in the APRIORI trial, which stages the Fastboot log.

The source supports collecting the DEBUG ring while UEFI Fastboot responds. The export depends on UEFI boot services and provides no verified post-handoff recovery channel. The next diagnostic candidate needs a direct loader protocol result at BDS and a host capture before selecting the macOS entry. This audit used retained source only.
