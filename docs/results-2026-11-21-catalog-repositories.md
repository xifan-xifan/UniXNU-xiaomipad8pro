# Piano catalog repository audit — 2026-11-21

The [UniXNU piano catalog](https://unixnu.com/zh/devices/models/piano) links this device tree, the UniXNU core, and several related ports. The public references below were inspected without changing the build inputs.

| Reference | Observed revision | Use |
| --- | --- | --- |
| [code002-2/piano-macos-unixnu](https://github.com/code002-2/piano-macos-unixnu) | `e9ff4f0a8dc53fcf0c7e274589ef3ba159106106` | macOS 27.3 comparison tree. Its lock records XNU `d8b80295118ef25ac3a784134bcf95cd8e88109f` and the same SM8750 pin as our feed. Its SystemOS volume retry patch and policy records can guide local mount diagnosis. |
| [code002-2/UniXNU-piano](https://github.com/code002-2/UniXNU-piano) | GitHub API returned 404 | Catalog alias could not be resolved publicly during the audit. |
| [xiaomi-pad-8-pro-development/android_device_xiaomi_piano-unixnu](https://github.com/xiaomi-pad-8-pro-development/android_device_xiaomi_piano-unixnu) | `d4f160c8ee60c11b293b5425af375846fcd438ff` | Related device tree with a SystemOS retry patch at the same path. |
| [donyanchen/UniXNU-piano](https://github.com/donyanchen/UniXNU-piano) | `961371c3613a940a3124928f2422cee858b47b90` | iPadOS/iOS port with board configuration and BootShim patches for comparison. |

The macOS comparison tree reports SystemOS UUID `6678E466-2F88-4C56-9408-DC7361BAA0BA` and SHA-256 `14330ed990fe7022114b10718f4defe3049d49b6e5d52fd3ac71a487541cd1f3`. The hash already appears in the deployment manifest. The author's verification record covers signature and block checks on the host and marks hardware testing and boot root authentication false. The retained local unshimmed image hashes to `c970de1b385486b2893c7a03a236ad612fb2f89fc45e48e9a7a80374d247487d`. Its verification policy therefore needs to be derived from the actual local image. The public tree review did not locate the native `11ac316b789d81fc9f734e15645456f536c05141` source or the two SystemOS shims.

ADB reports HyperOS `OS3.0.309.0.WPYCNXM` on `_a` with `sys.boot_completed=1`. The ordinary ADB shell could not run `snapshotctl`. This is a read-only Android observation. The catalog audit did not exercise macOS boot or switching.

GPT arithmetic correction for the earlier host discussion. With 4 KiB logical sectors, 128 entries of 128 bytes occupy LBA 2 through 5 in the primary table. The original `switch` partition starts at LBA 6. The 64-entry candidate remains the deployment layout under review.
