# Resumption audit (2026-11-20)

This was a host-side, read-only continuation audit. No tablet command, backup, reboot, Fastboot operation, or write was performed.

## Source and host state

- Direct Git remote checks still report the requested device repository `main` at `2438556c77c90c22c7b76e57eb8b40c44b35f1af`. The Ubuntu switch-tool repository reports `main` at `aa4095a02c8455dfabc3dc42a8bacdb11bacab6b` and tag `v1.6.1` at `6ef7516b940e33b3384a7469783c2d0ed24ba3c8`; this matches the source and release already recorded in the manifest.
- `ssh mac` works for read-only inspection. The `mac-lan` alias currently fails name resolution. The existing Mac root build directory remains at `/tmp/piano-root-26C5035r.593r2m`; `root.unshimmed.img` still hashes to `e543c4ed25f77dec07c39b3ba840a0a6887a8a499a1d515eecfb9a0bc6099f2b`, and `systemos.unshimmed.img` to `c970de1b385486b2893c7a03a236ad612fb2f89fc45e48e9a7a80374d247487d`. No matching build or VM process was found.
- The root recipe requires `overlay/systemos-shims/sbin/mount` and `fsck`. A bounded read-only search of the retained WSL native directories, historical Windows workspaces, and Mac Codex checkout found no such files. The available DroidVM tree documents that its mount shim was removed and it keeps Apple's original `fsck`; it is a different VM layout and does not supply the required native shims.

## Historical deployment evidence

- The preserved `piano-macos-c146` results report a native macOS 27.3 desktop and persistent Data on this tablet. Its source lock differs from the current plan, and its documented successful desktop is not evidence for the current 3.0.309 storage layout.
- Its `persistent-boot.md` documents a computer-side boot-image exchange to Android and back. The available dated result records through 2026-11-14 do not record that exchange being performed or an Android return. The path from that historical installed macOS layout to today's stock Android layout remains unknown.
- `ARTIFACTS.md` records a kernel SHA-256 for the exact historical k27 pin, but the kernel binary is absent from the preserved output tree. The `piano-macos-latest` sibling has RAM images and generated inputs, but no persistent root or ESP; those assets are not a matched deployment set.

## Result and next gate

Historical success confirms that native macOS 27.3 reached a desktop on this hardware, but it does not recover the locked `11ac316b789d81fc9f734e15645456f536c05141` source tree or a reproducible image set. Do not boot the current unshimmed root. Continue host-side work from the frozen 26C5035r inputs; the next image gate is to obtain or recover the native shim source and the matching Data seed/trust inputs. Storage migration and paired Android/macOS recovery remain separate device-write gates.
