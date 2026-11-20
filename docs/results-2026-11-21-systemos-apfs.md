# SystemOS APFS check — 2026-11-21

The Mac attached the retained 6 GiB `systemos.unshimmed.img` read-only without mounting a volume. `diskutil` reported container UUID `03C58B20-7313-41EA-80B8-FF186A64C88C` and a single SystemOS volume with UUID `284548A8-FC87-434E-A0C8-A4038E83704D`, matching the local image metadata. SystemOS had no APFS role and was unsealed.

`fsck_apfs -n` exited successfully after checking the container checkpoint, space manager, object maps, volume metadata and allocation. The Mac detached the image afterwards. This validates APFS structure on the host. Boot shims, the runtime union mount and root authentication remain deployment gates. The SystemOS policy must bind to the local volume UUID and final image bytes. The catalog policy has a different UUID and image hash.
