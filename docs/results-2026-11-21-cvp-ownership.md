# CVP ownership in the packed device tree

The Android v2 section parser extracted the 1,111,663-byte DTB from the DarwinBoot mailbox image. SHA-256 `ed6e1a3c663fda03e833297170542ad6916791999fc685a6597672a2535ce76b` matches its build record. The DTB declares the 7 MiB `cvp_region@9cb00000` as `no-map`. Its phandle `0x36f` is referenced by `qcom,cvp@ab00000`, which names firmware `evass`. The eleven checkpoint pages lie inside this reservation. The firmware dump demonstrated that the bytes are readable from UEFI. The DTB shows a potential consumer during the intervening HyperOS boot, but does not establish when it initializes the region.

The baseline and post-trial dumps each contain more than 1.17 million nonzero bytes and differ at 73,970 byte positions. Every 4 KiB checkpoint page is zero in both. The comparison rules out a wholly zero upload and leaves each checkpoint's prior state unresolved. A current retention control needs a pre-reset sample and a sample before Android can initialize CVP, or an independently qualified range outside that reservation.

The Mac rehashed both retained 26C5035r host images successfully against their recorded digests. ADB reported completed HyperOS OS3.0.309.0.WPYCNXM on A and the original Android persistence marker. No reboot was issued during this audit.
