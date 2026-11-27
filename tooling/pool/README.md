# A2 storage pool tooling

The A2 installer copies the System and SystemOS volumes into the owner's Data APFS
container. The pool ESP adds the `macos-27.3-pool` boot entry and keeps the base
entry available for rollback. [Workflow and safety gates](../../docs/persistent-boot.md#a2-storage-pool-tooling)

## Offline recovery tests

Run from the repository root.

```sh
/usr/bin/python3 tooling/pool/test-pool-recovery-invariants.py
/opt/homebrew/bin/python3 tooling/pool/test-pool-recovery-invariants.py
python3 scripts/ci/self-tests.py --list --match tooling/pool
python3 scripts/ci/self-tests.py --match tooling/pool
```

The Python suite runs the installer preflight, the APFS volume lifecycle against
the mock tablet, and the ESP maintenance apply and rollback subcommands. It uses
temporary files and replaces disk, network, and tablet I/O at the test boundaries.
It checks that bad receipts and protected partition targets refuse writes, failed
read-back preserves the installed pointer and verified backups, and a partial copy
can only proceed after a successful verification or recovery.

An unconfirmed device or maintenance step holds the round for recovery. The
round and backup receipts identify the saved state. An ESP backup with no verified
receipt is kept as evidence. A new backup set can be used once the installed ESP
has been confirmed again.
