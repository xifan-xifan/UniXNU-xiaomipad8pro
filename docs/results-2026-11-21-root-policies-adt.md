# RootPolicies ADT slot

Using the locked UbuntuPiano dependencies and `configs/k273-adt.toml`, the updated ADT generator produced two host artifacts for the same identity and unique chip id:

- without `--root-policies`: 15,736 bytes
- with `--root-policies`: 15,788 bytes

The second artifact contains `/chosen/memory-map/RootPolicies`, a 16-byte zeroed slot. The firmware parser reports no RootPolicies slot in the first artifact and a valid slot in the second. DarwinBoot fills that slot with the address and size of the `.vroot` bundle loaded from the kernel sidecar. This confirms the generator-to-firmware slot contract; it does not prove a device boot.
