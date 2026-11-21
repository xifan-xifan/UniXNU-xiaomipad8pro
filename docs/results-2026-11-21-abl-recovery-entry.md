# ABL to stock Recovery on piano (2026-11-21)

From completed HyperOS on slot A, `adb reboot bootloader` entered Fastboot. `fastboot reboot recovery` returned OKAY, and ADB enumerated the same piano tablet in `recovery` mode.

The GPT was stock. No partition write or wipe was issued. This verifies the direct Recovery entry before a normal Android boot; the Recovery menu return and initialization on the shortened layout remain separate checks.
