# DarwinBoot ExitBootServices return on piano

On November 22, stock ABL accepted the temporary mailbox image. Its RAM payload and DTB match the earlier mailbox candidate. Host comparison found changes only in the 3 MiB FD and the Android v2 boot ID. The rebuild reused the existing DarwinBoot mailbox source and added the verified SDAM route to the watchdog gate. The build log is `UbuntuPiano:/tmp/piano-ebs-abl.z03RrXOL/build-replay.log`.

The tablet returned to stock ABL. Before Android started, the reader captured the full CVP range. Eight records agree on attempt 0, stage 3, EFI_STATUS 0. Copies 3, 4 and 7 were invalid. The probe stops before DarwinEnter. The reader then returned to completed HyperOS OS3.0.309.0.WPYCNXM on slot A without operator input. Evidence is under `out/ebs-abl-live/`.
