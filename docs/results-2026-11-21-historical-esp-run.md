# Historical ESP test run outcome

The archived 2026-11-14 ESP test did reach the UniXNU menu. The saved console then reports `DARWINBOOT: boot refused: Load Error; firmware retained`; the device remained in ABL fastboot. The run timeline records only UEFI detection and capture, with no XNU handoff witness. The subtype-2 payload was also installed/read back, but its install record only proves the ESP payload window, not startup. The earlier documentation records the same ABL `Load Error` behavior and the need to reset the ABL session before another boot attempt.

This is the strongest historical device evidence for the retained candidate: UEFI and menu execution succeeded, DarwinBoot refused before XNU. No desktop or persistent macOS boot was reached.
