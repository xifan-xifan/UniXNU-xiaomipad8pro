# CrossDarwin macOS 27.3 source comparison

At CrossDarwin/UniXNU_macOS commit `4a671773c2ee289735ab526eda7c9628131250df`, `profiles/os/macos-27.3-26C5035r/profile.toml` selects the target Apple build and XNU `12377.121.6` at `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`. This matches the Apple source revision recorded in `unixnu.lock`. Comparing the ordered public patches with the local k27 patch stack can help reconstruct the prepared tree.

The published workflow selects `sheng_macos_defconfig` and separate sheng firmware. The GitHub commit endpoint returned `No commit found for SHA` for local k27 pin `11ac316b789d81fc9f734e15645456f536c05141`. A matching base revision leaves the prepared tree, native kext ABI and recovery behavior to be verified. This review used the pinned profile and workflow via GitHub raw files after the shallow clone ended with an early EOF. No build or tablet test was performed.
