# Retained Android boot image and embedded KC

The Android v2 header places the 167,772,160-byte ramdisk at byte 4,263,936. A byte comparison against the retained FAT payload succeeded, with SHA-256 `1219349b77bdfde2f91f80e28197a11863347a84a1d617179417c50236a8f76b`. The Android header's kernel region hashes to `7b5160f0e7a56c8135a0711bb27170a45f09d19e8043a66e6e71a66ae45d79d8`, matching `boot.json`.

Extracting `EFI/UniXNU/Payloads/kernel.macho` from the FAT payload with mtools yielded `d36b8047dcf9cf7dc5acc077b8661a20341a0546573c72279a2038846121bdf9`, matching `payload.json`. The loose `kernel/kernel.macho` hashes to `afe7646ece5f9ea2580c6a0257a0d977e0d1e334f96457a4816170aaf33cf0bb` and belongs to a separate artifact chain. Use `d36b8047…` when tracing the retained boot image to its build inputs. SystemOS and Data pairing and on-device qualification remain open. This was a host-only check.
