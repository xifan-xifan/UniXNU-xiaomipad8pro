# Historical d36b8047 candidate and ESP write

The prior task archive at `E:/codex/2026-11-01/ni/outputs/piano-macos-273-kernel-20261114` is the source of the retained `d36b8047…` KC. Its README says the prelink mapping completed, while `runtime_qualified=false` and `loadable=false` mean it had not been booted on the tablet. The matching `payload.json` records the same KC, ADT, trust cache and scratch Data hashes now found in the retained FAT payload.

The companion `piano-macos-273-esp-test-20261114/install.json` reports `installed_and_verified`, but its scope is explicitly only the first 192 MiB of `sunuefi_esp`; source and readback hashes match after padding. This is an ESP write/readback result, not proof of XNU handoff, desktop startup, or a complete SystemOS/root pairing. No new device write was performed during this audit.
