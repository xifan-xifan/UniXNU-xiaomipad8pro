# Data-store M5 durability qualification

Build the arm64 tablet program with the macOS 27.3 SDK and run the host parser tests before reserving a tablet session.

~~~sh
make -C tooling/native/piano-durability
make -C tooling/native/piano-durability test
~~~

The binary is under `out/piano-durability/macosx27.3/`. Stage it at `/usr/local/bin/piano-durability` in the qualification environment and include its code signature in that environment's trust cache. Verify the binary hash before the test. Run it as one identity with write access to the root of the scratch Data volume. Use the harness's noninteractive root execution path when necessary. The host script accepts a `--transport` executable that takes one remote shell command and preserves stdout and the SSH exit code. It defaults to `/tmp/tssh`. Configure prompt-free execution, no PTY, prompt detection of a lost connection and no retry of a command that has already started.

The live collection needs the scratch qualification fields in `machdep.piano_data_io` from the M5 durability integration. The host parser can also replay saved transcripts from that collection without a live boot.

## Store and boot preflight

Use RAM boots throughout. `data-persist=0` keeps the Data changes in RAM and exercises the tool and capture. `data-persist=1` opens a read-only store probe and also loses changes across boots. Both bypass M5. The mode that can exercise write-back and keep the scratch volume across a RAM reboot is `data-persist=2`.

NativeUfs already supports a second, bound window in the unused tail of `linux_rootfs` on the whole-disk 27.3 layout. The root image occupies the start of that partition and the owner's Data occupies `linux_data`. NativeUfs computes the window from the live GPT, root extent, seed geometry and log reserve. `data-store-lba` requires that exact start before the driver opens the window. The overlay header records the binding and the seed. A different seed or LBA by itself is insufficient protection because two windows can overlap. [Window implementation](../../package/NativeUfs/NativeUfs.cpp) · [Whole-disk layout](../persistent-boot.md#138-the-whole-disk-layout-the-root-on-linux_rootfs-the-owners-data-grown-over-linux_data-2026-11-14)

Before building or entering fastboot for a persistent run, review a fresh maintenance map and the installed boot record. Inventory the GPT, ESP, root image, owner `linux_data` store, Android `userdata`, `super` and `metadata`, logs, and any other retained extents. Approve an unused disposable interval wholly inside `linux_rootfs` after the root plus its 4 MiB guard. Use a pinned, clean 27.3-compatible Data seed that fits this interval and verify the proposed scratch header is blank or is the already-bound test store. Keep the map, seed identity and plan in the run's local evidence directory. Source the plan independently of the proposed image's args.

The host's `--plan` JSON has `linux_rootfs`, `approved` and each entry in `protected` as `[first_lba, block_count]` in 4 KiB units. Include the owner store in `protected`. Set `root_blocks` to the verified size of the root image rounded up to 4 KiB. Set `seed_pages` to the seed's *logical* 4 KiB page count and `reserve_mib` to the retained tail reserve, normally 1024. With `P = seed_pages`, the window has `W = 1 + ceil(P / 32768) + P` blocks and starts at `partition_end_exclusive - 256 * reserve_mib - W`. The script checks that the entire window fits the approved interval and clears every protected extent.

**RAM builder.** Set `PIANO_SCRATCH_PLAN` to the reviewed JSON with `status` set to `CONFIRMED` and the pinned seed identity in `seed.identity_sha256`. The builder checks the actual seed, the approved interval, all protected extents, the installed store record, and the kernel collection's support for the binding and M5 switches. It refuses replace, flatten and Data reset arguments. It rechecks the binding against the merged selector line and payload ADT and records the window, plan hash and seed under `data_store.scratch`. [RAM builder](../../tooling/bootimg/build-ram-images.py) · [Scratch-plan check](../../tooling/persist/common.py)

## Qualification specs and reset sequence

Set `KIND=observe` for the test of each frozen RAM image, including recovery. Use the driver analysis for the implementation build's analysis gate. Keep the tablet lock with the outer harness through each measurement, reset and next boot. Run the normal spec and image verifiers and compare `NEED_ARGS` against the effective arguments, including ADT spillover. [M5 analysis](../drivers/data-store-m4-m5.md#fix-image-gate)

Optional initial smoke spec on a volatile RAM boot:

~~~text
KIND=observe
NEED_ARGS="piano-boot-args=ram piano-durability=1 data-persist=0 data-store-write-back=0 data-store-queue=0"
~~~

For the persistent phases use the same collection, root, seed, approved extent, reserve and background conditions. The switches take effect at publication. Hold `data-store-concurrent=1` in every persistent phase so the queue comparison uses the M4 concurrent write-through baseline.

| Phase | `data-store-write-back` | `data-store-queue` |
| --- | --- | --- |
| `control` | 0 | 0 |
| `writeback` | 1 | 0 |
| `queue` | 0 | 1 |
| `combined` | 1 | 1 |

For each phase, initialize `NEED_ARGS` from `args` and add `piano-boot-args=ram`. Use `PIANO_BOOT_ARGS_SPLIT=1` so the RAM builder supplies this selector. For example, the effective storage/reset arguments for `combined` are:

~~~text
KIND=observe
NEED_ARGS="piano-boot-args=ram piano-durability=1 native-ufs-root=1 native-ufs-root-part=linux_rootfs data-persist=2 data-store-part=linux_rootfs data-store-lba=<computed-first-lba> log-reserve=1024 log=0 data-store-concurrent=1 data-store-write-back=1 data-store-queue=1 piano-reset-after=300 piano-reset-to=bootloader"
~~~

Replace the placeholders and reserve with the approved plan's values. Use `K273_ROOT_PART=linux_rootfs`, `PIANO_DATA_PERSIST=2` and `PIANO_BOOT_ARGS_SPLIT=1`. Replace the corresponding keys in the effective profile and pass the merged profile, before its RAM selector is added, through `PIANO_EXACT_ARGS`. For `data-store-concurrent=2`, enable concurrent mode through its runtime switch before collecting evidence. Preserve the USB console and SSH setup. A boot with `piano-reset-after=N` owns the reset deadline and defers the normal boot guard. Choose `--reset-after` to cover boot, SSH settling, measurement and log anchoring, with time to collect the ACKs before the cut. Confirm the console shows the timer armed and the bootloader restart reason set successfully. [Boot guard](../boot-guard.md)

~~~sh
host=tooling/native/piano-durability/run_durability.py
"$host" --out "$OUT" init
"$host" --plan "$PLAN" args "$PHASE"  # preflight each persistent phase
# Optional, on its own volatile RAM boot:
"$host" --out "$OUT" measure volatile
# For each persistent phase, on its first RAM boot:
"$host" --out "$OUT" --plan "$PLAN" measure "$PHASE"
"$host" --out "$OUT" --plan "$PLAN" --console "$CONSOLE" prepare "$PHASE"
# After the timer reset into ABL, on the next RAM boot of the same phase:
"$host" --out "$OUT" --plan "$PLAN" verify "$PHASE"
# After all four phases:
"$host" --out "$OUT" --plan "$PLAN" summary
~~~

1. Capture the console from boot and confirm SSH and the runtime identity before counting anything. The positive storage lines are the NativeUfs root bring-up and NativeRamBlock's Data store state and Data I/O mode. One fresh host output directory holds the whole campaign. The phase logs include `DUR BEGIN`, the boot UUID and before/after sysctl samples. The tool checks the actual Data APFS mount's IOService ancestry for its single physical `NativeRamBlock` provider. The extended `machdep.piano_data_io` supplies the effective RAM selector and qualification marker, mode, persist state, seed tag, bound LBA, page count, generation, switch state and error count. The tablet accepts a volatile store with state 0 or a healthy scratch store whose live extent, flags and binding match the approved window. First opening the scratch store can report state 3. Keep the tablet's other storage workloads quiet and comparable.
2. Run `measure` and then `prepare` on the same boot. `prepare` anchors a file with 33 initialized 4 KiB slots. Near the deadline it writes slots 1–32 with a run-specific numbered pattern, performs `F_FULLFSYNC` after each and prints each `ACK` to SSH stdout. It writes slot 33 without a sync, emits `READY last=32 tail=33` and waits with the file open. The host fsyncs the transcript before displaying each ACK and READY. Use a fresh console capture for this boot and keep it appending throughout the reset. The collector requires the direct timer marker after READY and a transport disconnect. The outer harness confirms the return to ABL fastboot.
3. Let `piano-reset-after` cut the run via its direct PSCI `SYSTEM_RESET` path. The USB console's `reset fastboot` goes through `PEHaltRestart` and shutdown notifications, which can flush Data. Use that command when leaving a completed recovery boot. Using it during the cut makes the crash result VOID. This qualification covers the direct SoC reset window. Cold removal of UFS power requires a separate hardware test. [Timer reset](../../patches/k27/0056-k27-piano-reset-after-s-PSCI-reset-after-N-seconds-Gunyah-wa.patch) · [Console reset](../../package/PianoDwc3/PianoDwc3.cpp)
4. From ABL, RAM-boot the same frozen image immediately. Collect recovery console evidence and run `verify` before any other workload. It uses a fresh boot UUID, targets the same approved extent and checks that state 2 resumed the same seed and generation. It opens the phase file read-only and checks every 4 KiB record byte for byte against its run and sequence. Save the console and inspect the matching windows for store, UFS and APFS recovery errors.

## Pass criteria and VOID checks

- **Sync rows.** All 32 calls and readbacks per row succeed. The output reports separate counter deltas around each write and each sync interval for plain writes, `fsync`, `F_FULLFSYNC` and `F_BARRIERFSYNC`. `fua` and `cached` count Data write accessors. `flushes` counts write-back flush attempts and `store_sync` counts NativeUfs store sync calls that issue SYNCHRONIZE CACHE. `barriers` and `forced` show provider options seen. Expect first-touch FUA and bitmap activity in the fill, and provider writes in the synced rows. The plain row may have zero provider writes. Write-through phases have zero cached writes and zero write-back flush attempts. Write-back phases require a positive store sync and flush delta in both full-sync and barrier intervals. To establish write-back coverage, sum cached calls in the overwrite rows' write and sync intervals and in the prefilled mixed interval. Zero cached activity in those windows is VOID. APFS can defer provider writes to a sync even with `F_NOCACHE`. These system-wide counters show exercise during the workload. Assigning a write to a specific file extent or checkpoint requires a matching block trace. [M5 APFS evidence](../drivers/m5-apfs.md#what-a-valid-measurement-proves)
- **Mixed p99.** A single 4 KiB random reader and a looping 1 MiB writer use disjoint 32 MiB spans of a prefilled file with the unified buffer cache bypassed. Verify read data and the last writer chunk. Require all 4096 timed reads and writer activity during at least half the samples. Persistent runs need positive UFS read and write deltas. Queued phases need accepted, finished and callback activity, zero queue-full returns and a p99 below `control`. Compare repeated campaigns if the difference is within run-to-run variation.
- **Recovery.** Require a complete, consecutive host ACK stream, matching READY, confirmed direct reset and a distinct recovery boot with the same scratch store. In write-back phases the ACK interval must have cached write activity and at least one observed store sync and flush attempt per ACK in aggregate. Every acknowledged record must match. Missing, torn or corrupt acknowledged data fails. The single unacknowledged slot may survive, retain its placeholder, be torn or disappear. Reinitialization on the matching scratch recovery boot, a positively identified Data mount failure, an I/O error or a degraded store is a valid failure.
- **VOID.** Missing SSH, protocol or console markers, mismatched boot/scratch identity, a clean restart, counter regression, missing known-positive activity or incomplete recovery capture voids the affected measurement. An explicit EOF for an acknowledged record in a complete, valid recovery capture fails. A truncated SSH transcript voids recovery. Preserve failed and voided logs and use fresh run names for retries after fixing the cause.

The host exits 0 for PASS, 1 for a qualification failure or 2 for a VOID without a proven failure. The summary shows each phase's verdict and a separate `VOID CHECK`. Real hardware evidence is required for M5 qualification.
