# Historical fips4b image inventory (2026-11-21)

The UbuntuPiano WSL directory `/root/piano-native/patched/fips4b` retains a 168,075,264-byte macOS RAM image and an 11,837,440-byte UEFI companion. Direct SHA-256 checks agree with `build-A-bundled.json`. The macOS image is `c0c4e33da0fe96dfe574e0ac9fd784ae6e450a61462d1b09494ab39e06b7b300`; its manifest records KC `afe7646ece5f9ea2580c6a0257a0d977e0d1e334f96457a4816170aaf33cf0bb` and FD `394c5c786f4d114ccb0e709083ce574e4c5964593bd656bdffe8dd0f2e5bd5d2`.

The November 15 historical RAM bring-up report records the same FD in its watchdog candidate, with a different boot image hash (`285f36fc…`). It reports an XNU-stage hang and physical recovery for a separate image (`bbf48ce4…`). Keep those observations attached to their original images. The fips4b manifest marks this image offline, requires native UFS root, has no matched installed seed, and reports no KC store guard. Its data persistence flag alone does not establish write isolation. Compare the embedded KC with the historical source/build inventory before considering it for bring-up.

The Mac responded through SSH alias `mac` and still retains both unshimmed images under `/private/tmp/piano-root-26C5035r.593r2m`. The Windows `mac-lan` lookup failed. No tablet commands were issued in this audit.

Fresh SHA-256 reads on the Mac matched the retained metadata for SystemOS (c970de1b385486b2893c7a03a236ad612fb2f89fc45e48e9a7a80374d247487d) and root (e543c4ed25f77dec07c39b3ba840a0a6887a8a499a1d515eecfb9a0bc6099f2b).
