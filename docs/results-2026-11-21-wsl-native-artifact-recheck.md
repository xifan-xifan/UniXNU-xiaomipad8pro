# WSL native artifact recheck (2026-11-21)

UbuntuPiano retains a macOS 27.3 output tree under `/root/piano-native/UniXNU/out/piano/macos-27.3-26C5035r/piano_macos`, including `kernel.macho` (SHA-256 `afe7646ece5f9ea2580c6a0257a0d977e0d1e334f96457a4816170aaf33cf0bb`), `boot.img`, and `payload.img`. The checkout is heavily modified and has generated backup files; its HEAD is a documentation commit, not the locked native k27 source. The retained FIPS4b RAM candidates are explicitly marked `OFFLINE_CANDIDATE_NOT_DEVICE_TESTED`, and their manifests do not establish a matched SystemOS/Data/authentication set.

The separate WSL XNU directories (`xnu-k27-final`, `xnu-k27-patched`, and `xnu-k273-work`) still cannot resolve locked commits `11ac316b789d81fc9f734e15645456f536c05141` or `de16623d957cccff6dfb78d8eca4a79842af9702`. These artifacts are inventory leads only; no image was loaded and no file was modified.
