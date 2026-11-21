# DarwinBoot loader witness on piano

The witness RAM image adds a DEBUG message and a five-second capture window before the final memory-map request. It retains the cyan and white stop after successful ExitBootServices. Its SHA-256 is `a74a9e605283aa68c6f5cecf6e1e1fd624e0a1c767774ee302fde868a3f945aa`. The FD SHA-256 is `bf9ff0d490c8aaeecba0f09c4bb359688db4ea08003b7722bf51d08bf419f27f`. The build log is `UbuntuPiano:/tmp/darwinboot-post-ebs.tzsdzOfC/witness.log`.

`out/darwinboot-witness-ring-6.bin` was captured 22.82 seconds after the host started Fastboot. Its SHA-256 is `ff950e8736f7ab3734fcb41cb0dd84b451b2c8fd822bb41ffd1d0fbc71cbd9e5`. It records `SET_TIME=0`, `ENABLE=0`, and then `PIANO: DarwinBoot ready for ExitBootServices`. This proves that the loader reached the final handoff preparation with the watchdog armed. ExitBootServices success still needs a witness from after the call.

The device returned unattended to HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, with `sys.boot_completed=1`. The Android marker SHA-256 remained `306a063b9b6d71bb48419efc7cf512c95c789d493a73d178e481a57dc82575f3`. The initial host command failed before upload because the image had yet to be packaged. The subsequent run checked the image hash before sending it.
