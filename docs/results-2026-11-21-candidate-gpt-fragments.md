# Candidate GPT fragment verification (2026-11-21)

The offline layout generator reproduced the candidate entry-array SHA-256 `ba67d6aa7863835bb019231cfe97054138f649e58e7e06d08abe858da3f9751e` and extents recorded in the storage study. It generated explicit candidate write fragments and byte diffs from the retained stock captures under ignored `out/rollback-3.0.309-gpt/candidate/`.

The 24,576-byte primary fragment SHA-256 is `12710135635beee2a5de1dd910924a3d2d26e09a6f368c191b879fd6b41a499b`; its diff contains 153 changed bytes. The 20,480-byte backup fragment SHA-256 is `9acf222558a6766c4f1fcd09509a41cfd5be10d53f34a2a0f84a8cfe0a18c2f5`; its diff also contains 153 changed bytes. Both fragments were applied only to a fresh sparse host fixture. `sgdisk -v` on a read-only 4096-byte-sector loop device found no CRC/table errors and reported the four expected extents and 251 free LBAs. It retained the stock-format warning that its 64-entry GPT array is below the GPT specification's minimum size. No tablet storage write was made.

This host result qualifies the candidate bytes and parsing, not maintenance writer behavior, device GPT selection during a partial update, Recovery entry, or Android recreation on the shortened userdata geometry.
