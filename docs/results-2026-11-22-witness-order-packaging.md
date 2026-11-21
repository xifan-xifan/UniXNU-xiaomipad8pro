# Witness order control packaging (2026-11-22)

The archived witness FD was repacked with DarwinBoot first in APRIORI. The decoded firmware outside that GUID list is byte-identical. The containing FFS header and guided section lengths were recalculated.

- FD SHA-256 `60ada0ee4982f69b737f40ff669301f85e87ae96c3352f7ba400eb145bf55ca4`
- `out/boot-piano-witness-apriori-darwin-first.img` SHA-256 `fd673ee7bc6ca66d73ca5c890d46d79212efa8a9edfa8979626b4807e622f47e`

Android v2 readback matches the archive outside the embedded FD and boot ID. The recomputed boot ID matches the header. The guided section decompresses to the verified control bytes. Hardware dispatch and watchdog coverage remain unverified.
