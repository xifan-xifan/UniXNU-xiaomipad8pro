# Platform Tools Readiness

Audit date: 2026-11-19. Host-only preparation; no tablet command or write was performed.

Windows and `mac-lan` initially had no `adb` or `fastboot`. Homebrew is not installed on `mac-lan`, so the official Google Darwin archive was downloaded to the temporary directory `/tmp/codex-platform-tools-20261119` and verified:

- Android Debug Bridge `1.0.41`, platform-tools `37.0.1-15733141`
- fastboot `37.0.1-15733141`

Running `adb devices -l` and `fastboot devices` through that temporary installation returned no attached devices. The tools are ready for a later user-connected tablet, but this check provides no boot or recovery evidence.
