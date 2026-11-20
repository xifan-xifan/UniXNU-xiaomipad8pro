# 27.3 offline RAM candidate with read-only native collection (2026-11-20)

The frozen host inputs produced a complete offline RAM candidate with `PIANO_DATA_PERSIST=0` and the 8 GiB ADT window required by the retained menu FD. Inputs were the read-only 26-driver KC, the Apple-only 26C5035r trust bundle, the preserved 24 GiB first-install seed, the diagnostic menu FD, and the DTB extracted from the previously verified menu image. The two pinned firmware files were absent and were explicitly left out; the image records that the kexts would request them from the tablet ESP.

Artifacts on UbuntuPiano:

- `boot-piano-uefi-ram-k273-fixture.img`: 13,766,656 bytes, SHA-256 `ed5f802afee7ddf9703c5a7b41a9afce7ad59f21e97563b27bbb030dd3af6986`
- `boot-piano-macos-ram-k273-fixture.img`: 185,733,120 bytes, SHA-256 `e0ac9c1eafec6bdb2410144f337cd8860308bf9a57e44f3e22b61a7a7e7eb00e`
- `build-k273-fixture.json`: SHA-256 `66f2879585409b19eae61513b1657e615c670a4af43754635a87b0556e49e0a2`

`verify-ram-images.py` passed with the Ubuntu v1.1.0 maintenance image as reference, the retained FD and extracted DTB: `PASS: 0 failure(s)`. This is still a host-only, unsigned, `fastboot boot` candidate. No image was sent to the tablet. It does not establish native XNU handoff, desktop startup, GPU, input, storage, or recovery behavior.

Safety review: the successful RAM candidate's boot arguments include `piano-reset-to=bootloader` and the timed reset path by default. The corresponding k27 patches write the PMIC SDAM restart reason so a reset returns to ABL. This is not a disk/GPT/userdata write, but it is persistent device state and therefore the candidate is not classified as side-effect-free temporary boot. A follow-up candidate must set `PIANO_RESET_AFTER=0` and verify the arguments omit both reset controls before any device trial. One attempted rebuild did not invoke the packer because the outer PowerShell expanded the shell output directory variable; no new image was produced.

A side-effect-reduced candidate was then built with `PIANO_RESET_AFTER=0`, which removes both `piano-reset-after` and `piano-reset-to` from the generated boot arguments. The Ubuntu reference verifier passed with zero failures. Artifacts on UbuntuPiano:

- `boot-piano-macos-ram-k273-manual.img`: SHA-256 `b99eac50488cd8d24938965987f51be16e41af1257c09a294c8ec1fcccd7704a`
- `boot-piano-uefi-ram-k273-manual.img`: SHA-256 `4e8736a9d227e5a476d55bec7a2379532ff6a5d959ecb7c599094801615d1fce`

This removes the planned PMIC SDAM restart-reason write from the image arguments. It is still unsigned and host-only; no device boot was attempted.
