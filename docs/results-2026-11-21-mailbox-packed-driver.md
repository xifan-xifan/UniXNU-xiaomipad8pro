# Packed DarwinBoot mailbox driver audit (2026-11-21)

Decompressing the LZMA-guided section of the retained mailbox FD (`777b2cedd454fc7f65a1ad77c11d7dc473044d18f5ec4cc0405b16958307d102`) exposed DarwinBoot FFS `1b64af87-8415-4b57-9b16-680d70913b9f` at decoded offset 5,019,696. Its 27,190 bytes hash to `3dccf3ecb2c6ba053548fae9c0d311366dd5eecc9627e4a48a8e805065b892dd`. The PE32 section payload hashes to `85ffbf1be822f900a4fc01b1f7ec9aeab8abbd0113f9ae23dc29369cc4a3ec0d` and contains the mailbox trial marker. The earlier GUID occurrence at offset 1,372 is a reference rather than an FFS header and its apparent size exceeds the decoded buffer.

This verifies that the trial executable was packaged. The zeroed device readback still requires a known-marker control sampled before and after the same unattended reboot sequence. This audit issued no tablet commands.
