# Retained SystemOS input matches the reference policy

On November 21, 2026, a read-only check on the Mac measured `/private/tmp/piano-root-26C5035r.593r2m/141-22145-021.dmg` at 2,917,138,432 bytes. SHA-256 `14330ed990fe7022114b10718f4defe3049d49b6e5d52fd3ac71a487541cd1f3` matches both the native manifest and the archived 26C5035r SystemOS policy metadata. This identifies the retained input for reproducing the reference policy pairing. The modified 6 GiB SystemOS in the existing SHNGROOT assemblies has a different hash and volume UUID.

The archived OS and SystemOS public anchors both hash to `896b32469a0214eae0b6384e100a6f9734503dce6816371988d02f2690dfe4fd`. The archive contains policy headers. The storage builder also checks profile UUIDs and digests and expects complete policy inputs. Before a device trial, verify the full policy and block table against the stock input and establish whether the stock SystemOS can supply the required native mount and fsck behavior. Any modified SystemOS needs a policy generated for its resulting bytes.

ADB confirmed completed HyperOS `OS3.0.309.0.WPYCNXM` on slot `_a`. The tablet was inspected without reboot or writes.
