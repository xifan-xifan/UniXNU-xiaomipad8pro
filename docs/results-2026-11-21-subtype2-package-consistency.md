# Subtype-2 boot package consistency

The archived subtype-2 payload is 160 MiB and its 192 MiB ESP image is the exact payload prefix followed by zero padding. The corresponding corrected-header boot image is v2, uses ARM64 header fields `text_offset=0`, `image_size=0x325000`, `flags=0xa`, includes the 147,436-byte packaging DTB (`50636eff…`), and embeds the same 160 MiB payload byte for byte.

The subtype-2 run timeline shows UEFI detection and later query, but no XNU handoff witness or desktop evidence. The installation/readback hash is `0d902012…`; the rollback image was restored and verified as `d33ee195…`. This establishes package consistency and rollback state only.
