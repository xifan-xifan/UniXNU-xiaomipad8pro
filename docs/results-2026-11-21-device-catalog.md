# Piano device catalog audit (2026-11-21)

The [device catalog](https://unixnu.com/zh/devices/models/piano) links five device repositories and UniXNU core. GitHub API results and source review follow. No tablet operations were performed.

| Source | Revision or API result | Relevance |
| --- | --- | --- |
| lenzhang/UniXNU-xiaomipad8pro | `2438556c77c90c22c7b76e57eb8b40c44b35f1af` | API returned the deployment baseline for main. |
| donyanchen/UniXNU-piano | `961371c3613a940a3124928f2422cee858b47b90` | iPadOS staged payload and early boot diagnostics. See the alternate UEFI payload audit. |
| code002-2/UniXNU-piano | API returned 404 | The catalog link could not be verified. The previously documented Unixnu-SM8750-Piano repository has a CI overlay tree. |
| code002-2/piano-macos-unixnu | `e9ff4f0a8dc53fcf0c7e274589ef3ba159106106` | The SystemOS retry patch waits for the verified block volume. Its prerequisites include the existing verified-volume resolver and root authorization, so the patch alone cannot restore that path in the independent build. |
| xiaomi-pad-8-pro-development/android_device_xiaomi_piano-unixnu | `d4f160c8ee60c11b293b5425af375846fcd438ff` | Separate macOS profile and lock. Its tree also lists a SystemOS retry patch. See the resumption audit for its build status. |

The related [piano UEFI repository](https://github.com/xiaomi-pad-8-pro-development/android_device_xiaomi_piano-uefi) at `5b0ddb72cd9eb4ba1f9f6955baf0f789746d5e7e` contains the selector and AndroidBoot application for studying device-side switching. Its README reports HyperOS 4.0.0.44. `tools/restore.sh` writes the active slot's boot image and sets the slot active. It does not verify slot success or retries, restore a complete boot triplet, or restore GPT. Applicability to HyperOS 3.0.309 remains unverified.

The reviewed sources did not supply the exact native k27 tree or the two SystemOS shims. Dependency pins were not changed.
