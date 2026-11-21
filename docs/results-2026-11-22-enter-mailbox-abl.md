# DarwinEnter trial with ExitBootServices checkpoints

The temporary image preserves the no-poll candidate payload and adds the DarwinBoot mailbox checkpoints while calling DarwinEnter after successful ExitBootServices. The host verified the embedded FD and recalculated boot ID. The rebuild log is `UbuntuPiano:/tmp/piano-ebs-abl.z03RrXOL/build-enter-mailbox.log`.

The tablet returned to stock ABL without operator input. The reader captured all 7,340,032 CVP bytes before Android started. Nine copies report attempt zero, stage three and EFI_SUCCESS. Copies 4 and 7 are invalid. The XNU log signature was absent (`0x1106` observed). HyperOS OS3.0.309.0.WPYCNXM completed startup on slot A after the reader reboot. Evidence is in `out/enter-mailbox-live/`.

The last verified execution point is the successful ExitBootServices return. DarwinEnter masks interrupts, writes to the framebuffer, cleans the owned range, disables the MMU and branches to the computed kernel entry. The capture cannot distinguish those steps from execution before XNU initializes its log. An independent entry checkpoint is needed.
