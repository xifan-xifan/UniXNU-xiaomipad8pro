# Live recovery slot readback (2026-11-21)

Root ADB reported 104,857,600 bytes for each recovery partition. Whole-partition SHA-256 for `recovery_a` was `4c9710a400100e5da89f6657ba9d7081742d03e8bca92579e9e113b334d24e12`, matching the verified OS3.0.309 package `recovery.img`. The `recovery_b` digest was `6a6dfe195bc1bf4bafba28983349b44fa579b397dac3df6b31bd4eea267762b6`.

Recovery entry, shortened userdata formatting, metadata key initialization and HyperOS boot on changed geometry remain to be tested.
