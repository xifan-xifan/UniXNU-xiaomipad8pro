# UEFI OS switcher on `lead/v131y12t`

`OWNER-0749` (2026-11-27) approves a selector write to `boot_a` after the recovery and R1 gates pass. Keep ABL unlocked and slot A active. Back up `boot_a`, record and re-read its SHA-256, and read back the whole partition after the write. Preserve `vendor_boot_a`, `init_boot_a`, `userdata`, `super`, `metadata`, `xbl`, `abl`, `tz`, `hyp`, `vbmeta`, `dtbo`, `persist`, and every `_b` partition. Keep the macOS and Android restore sets ready. See [Persistent boot](persistent-boot.md).

`main-repo/switch/sel1` at `48e56066` is its own merge base with `lead/v131y12t` at `3fcf74f4`. The seven switcher commits after `0663a6d2` entered the lead line in `d801f66e` on y12e. There were zero switcher-only commits to replay and no conflicts to resolve.

## Compatibility

- With no request, the selector defaults to macOS. A foreign or unreadable SDAM flag leaves the byte untouched and allows a volume-up request for Android. A failed one-shot consume falls back to macOS. Recovery and off-mode charging retain ABL's stock handoff. ABL's power + volume-down recovery path runs before the selector. The selector also has its own volume-down path back to ABL.
- The packer preserves the stock Android ramdisk and uses BootShim, FD and DTB from a persistent v4 macOS wrapper. The AVB signature covers the packed image with the stock boot properties and rollback index in a 96 MiB partition-sized image. `--check-signed --unsigned` compares the full signed payload with the packed bytes.
- The verified y12q host ESP, `esp-k273-v131y12q.img`, matches `installed.json` and its build record. The FAT image's own ADT requests `0x33fe00000` bytes. The installed v5 FD and rebuilt selector FD both admit up to `0x340000000` bytes. The installed menu defaults to `macos-27.3`, and both macOS entries use `data-persist=2`. The `fastboot` entry uses `data-persist=0`. The menu config contains `reset`, `shutdown` and `tools` and omits the three switcher actions. Those actions need corresponding entries in a future ESP to appear in the menu. The merged y12q arguments keep `piano-bootflag-write` at its default of `0`.
- The available y12u RAM records, including `v131y12u-ram2`, `m5ab1` and `torch-t4`, report the same ADT window size and FD bound as y12q. Their boot DTB hash matches v5 and the rebuilt wrapper. The final y12u persistent ESP image and build record were absent. Recheck the final ESP's actual ADT window, menu configuration, merged boot arguments and FD against its build record before deployment. Keep the established 1024-byte DarwinBoot command-line ABI and k27 ADT tail. See [Boot-argument length](boot-args-length.md).
- The two macOS restore sets contain the same v5 `boot.img`. `macos-set-v5` carries the Ubuntu-era companion images. `macos-set-v5a` carries Android's `vendor_boot` and the Magisk `init_boot`. `android-set` carries Android's `vendor_boot` and its saved `init_boot`. The Android leg of the combined selector needs the existing Android companion pair. The last host snapshot records the Ubuntu-era pair. The lead must confirm the live pair in maintenance. If it is still the Ubuntu-era pair, stop the selector write. Any companion change requires its own scope.

## Host results

The 2026-11-27 baseline was `902430d1d`, the tip of both local `main` remote-tracking refs. The switcher, partition-safety and install-preflight suites passed there. The switch-pair test needed local fixtures and the sandbox denied its loopback transfer. The file-backed test now passes using the same fixture bytes and fake partition map while exercising the production backup, verification and write logic. Set `PIANO_TEST_PERSIST_OUT` to the host reference directory when running it from a checkout without those fixtures.

| Check | Result |
| --- | --- |
| Restore sets | `shasum -a 256 -c SHA256SUMS` passed for all three images in each of `macos-set-v5`, `macos-set-v5a` and `android-set`. The 96 MiB signed v5 image matches `boot-v5.json` and passed AVB verification with its trusted public key. The maintenance RAM image matches its pin. The installed ESP image, embedded ADT and `unixnu.toml` match their records. |
| Selector | `selector.bin` built and passed binary validation, 11,940 bytes, SHA-256 `a93b1fa8034dfa0cf6f74cb80327f3ab66995cc3fc56675cc256b1533316299d`. The switch suite passed 19,869 codec and decision checks and 1,558 SDAM checks, the shell codec, the FD source patch check, and the packer with the saved Android boot image and rebuilt persistent v4 wrapper. |
| FD | The existing `piano-uefi-A-efimap.fd` (`4f81c50b…`) has the EFI-map handoff but lacks all three compiled menu actions. The replacement FD built from `3fcf74f4` and its `unixnu.lock` pins passed `check-fd-patch.py --require-efi-map --fd` against the decompressed firmware volume. It has the three menu actions, RAM payload precedence, the 13 GiB bound, extra-RAM and EFI-map handoffs. It is 3,145,728 bytes, SHA-256 `873339e6a68674cea12af3a554bb17d400c33186266b07ae41ce74c88ed99baa`. The EFI-map host checks passed, including the EBS retry. |
| Pack and signing parameters | The unsigned combined image is 40,984,576 bytes, SHA-256 `e8fc0910655806cc96c8b5d2296c43e1bd7b3ef2ae4133e10336d8e44715f85c`. `SIGN_DRY=1` selected `boot`, 100,663,296 bytes, rollback index `1769904000` and security patch `2026-02-01`. AVB signing and the hardware R1 test remain pending. |
| Maintenance | Switch-pair, partition-safety, install-preflight and slot-success CRC tests passed. The saved LUN 4 GPT headers passed the preflight. The backup entry array starts seven blocks before the end, spans three blocks and matches the primary. The tool checks both copies, changes bit 54 of the `boot_a` entry and the CRCs, and preserves each copy's padding and the protective MBR. |
| Repository | Document links, `git diff --check` and the privacy scan passed with zero privacy hits and zero refused files. |

## Owner gates

Record the private recovery checklist evidence before the first selector write. A0 is a read-only SDAM survey of `0x7100` byte `0x49` across macOS, Android, ABL, off-mode charging and cold boots. Keep `piano-bootflag-write=0` and verify the default-off refusal. Record power + volume-down reaching ABL both from running and from off, the on-unit restore to Android and back to working macOS, and checksum verification of the restore sets on both recovery hosts. The checklist has a prior restore entry with key-path confirmation pending. The host audit establishes one Mac's restore-set hashes. R1 and the other device observations require the lead session.

### Build, sign and register the candidate

Run from the switcher checkout in Bash. Set `STAGE` to a fresh absolute host directory, `PINNED_SOURCE_DIR` to the isolated firmware checkouts at the `unixnu.lock` revisions, `MAINT_RELEASE_DIR` to the pinned maintenance release, `BOOT_DTB` to the validated piano boot DTB, and `PERSIST_OUT` to the host persistence directory containing the live install ledger and `boot-v5.json`. Set `ANDROID_SET`, `MACOS_SET_V5`, and `MACOS_SET_V5A` to the verified restore directories and `TRUSTED_AVB_PUBKEY` to the trusted public key for the existing boot signer. Configure the signing host through the usual local environment.

```bash
set -euo pipefail
export UNIXNU_RESEARCH="$PINNED_SOURCE_DIR"
export PIANO_V110="$MAINT_RELEASE_DIR"
export PIANO_PERSIST_OUT="$PERSIST_OUT"
for set_dir in "$ANDROID_SET" "$MACOS_SET_V5" "$MACOS_SET_V5A"; do
  (cd "$set_dir" && shasum -a 256 -c SHA256SUMS)
done

mkdir -p "$STAGE"
git worktree add --detach "$STAGE/firmware" 3fcf74f4
bash "$STAGE/firmware/tooling/firmware/build-fd.sh"
FD="$STAGE/firmware/out/fd/piano-uefi-A-bundled.fd"
python3 tooling/switch/check-fd-patch.py --fd "$FD" --require-efi-map
make -C firmware/selector test OUT="$STAGE/selector"
python3 tooling/persist/build-boot.py --series 6 --fd "$FD" \
  --dtb "$BOOT_DTB" --out "$STAGE"
UNSIGNED="$STAGE/boot-piano-selector.img"
SIGNED="$STAGE/boot-piano-selector-signed.img"
python3 tooling/switch/pack-selector.py "$ANDROID_SET/boot.img" \
  "$STAGE/selector/selector.bin" "$STAGE/boot-piano-macos-persistent-v6.img" -o "$UNSIGNED"
SIGN_DRY=1 bash tooling/bootimg/sign-on-remote.sh --props-from "$UNSIGNED.avb.json" "$UNSIGNED"
bash tooling/bootimg/sign-on-remote.sh --props-from "$UNSIGNED.avb.json" "$UNSIGNED"
python3 tooling/switch/pack-selector.py --check-signed "$SIGNED" \
  "$ANDROID_SET/boot.img" --unsigned "$UNSIGNED"
AVBTOOL="$MAINT_RELEASE_DIR/upstream/xiaomipad-8pro-mainline-1.1.0/tools/local/aosp-avb/avbtool.py"
verify_dir=$(mktemp -d)
ln -s "$SIGNED" "$verify_dir/boot.img"
python3 "$AVBTOOL" verify_image --image "$verify_dir/boot.img" --key "$TRUSTED_AVB_PUBKEY"
rm "$verify_dir/boot.img"
rmdir "$verify_dir"
```

When using the locally built unsigned image, start at signing with its `boot-v6.json` wrapper record and `.avb.json` sidecar in the same staging directory. Register only the AVB-verified image. These host files are private build output.

```bash
SIGNED_NAME=${SIGNED##*/}
cp -n "$SIGNED" "$PERSIST_OUT/$SIGNED_NAME"
python3 - "$STAGE/boot-v6.json" "$SIGNED" "$PERSIST_OUT" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

wrapper, signed, out = map(Path, sys.argv[1:])
record = json.loads(wrapper.read_text())
with signed.open('rb') as source:
    digest = hashlib.file_digest(source, 'sha256').hexdigest()
with (out / signed.name).open('rb') as staged:
    if hashlib.file_digest(staged, 'sha256').hexdigest() != digest:
        raise SystemExit('staged signed image differs from the verified candidate')
with (out / 'boot-v6-selector.json').open('x') as target:
    json.dump({'fd': record['fd'], 'signed': {signed.name: {'sha256': digest, 'bytes': signed.stat().st_size}}}, target, indent=2)
    target.write('\n')
print('Signed selector SHA-256', digest)
PY
```

### R1 via ABL fastboot, with zero flash writes

On the lead host, set `BACKUP_SESSION` to the session directory for the pre-write backup, export `PIANO_SERIAL` for the expected ABL serial, and set `ESP_IMAGE` to the exact filename from the verified installed ESP record. Have a UEFI input device ready to interrupt the menu countdown and verify the A0 flag baseline before starting. Record the signed image and ESP SHA-256 values with both observations.

```bash
P=tooling/persist/maint-session.py
python3 "$P" --session "$BACKUP_SESSION" --esp "$ESP_IMAGE" \
  --boot-image "$SIGNED_NAME" ram-boot-a
```

`ram-boot-a` checks the signed bytes, the ESP record and ABL, then issues `fastboot boot` with the signed candidate. For the default case, leave volume keys released at selector entry. Observe the macOS ESP menu and its default, then interrupt the timer before launching XNU. In the deployed y12q ESP the daily and diagnostic entries can write to the persistent Data store. If qualification requires a full macOS boot, keep the write on hold until there is a qualified write-free macOS path for those exact signed bytes.

Return to ABL with the power + volume-down recovery gesture. Repeat the same `ram-boot-a` command, holding volume-down across the fastboot boot handoff. Confirm the command transfers the candidate and the expected device disconnects and returns to ABL. A command failure or a device that simply stayed in ABL does not pass. Run `abl-check` and record slot A and `unlocked: yes` after the return. Use `--selector-r1` only once the required hardware observations are recorded.

### Back up, write and verify `boot_a`

Use the same session directory and backup destination as the completed backup. Wait for any separate backup process to exit before continuing. Run from ABL in a terminal that can reach the maintenance USB network. Set `R1_BOOT_SHA` and `R1_ESP_SHA` from the recorded passing R1, and `PIANO_BACKUP_DIR` to the mounted backup destination. The boot writer rechecks the live ESP hash, the selector's window bound, the matching Android companion pair, and compatibility of the previous signed boot image. It requires the existing backup receipt, `manifest.json`, `SHA256SUMS` and a fresh `F_NOCACHE` read of the full backup. It compares the read-back hash of the whole partition to the signed image SHA-256.

```bash
python3 "$P" --session "$BACKUP_SESSION" --esp "$ESP_IMAGE" \
  --boot-image "$SIGNED_NAME" --selector-r1 "$R1_BOOT_SHA" "$R1_ESP_SHA" \
  maint-boot map backup boot-a slot-success-crc check maint-exit
python3 "$P" --session "$BACKUP_SESSION" --esp "$ESP_IMAGE" abl-final
```

`maint-boot` verifies ABL before loading maintenance. `slot-success-crc` skips writes if ABL already reported slot A successful. If it reported `no`, the step backs up and checks both LUN 4 GPT copies, sets only bit 54 in the `boot_a` entry and recomputes the entry-array and header CRCs. It reads each written region back. `check` compares `vendor_boot_a`, `init_boot_a` and `boot_b` with the hashes from `map`, and verifies the expected ESP and LUN 0 GPT. `abl-final` checks that ABL is unlocked and slot A is current, bootable and successful. A refusal or mismatch stops the sequence ahead of reboot. This procedure uses no ESP write step.

### Recover through ABL and roll back `boot_a`

From off, hold power + volume-down. From a hang, hold both for 15–20 seconds. Confirm the expected ABL endpoint. Load the pinned maintenance image and use the original backup session. The rollback reads the full restored partition back and verifies the original SHA-256.

```bash
python3 "$P" --session "$BACKUP_SESSION" --esp "$ESP_IMAGE" maint-boot map
python3 "$P" --session "$BACKUP_SESSION" --esp "$ESP_IMAGE" rollback boot-a
python3 "$P" --session "$BACKUP_SESSION" --esp "$ESP_IMAGE" check maint-exit
python3 "$P" --session "$BACKUP_SESSION" --esp "$ESP_IMAGE" abl-final
```

Keep the verified macOS and Android sets accessible. If maintenance will not load, remain in unlocked ABL and use the owner-approved recovery procedure. Full-set restore changes the companion partitions and requires its own authorization. If a later ESP update raised the required RAM window beyond the backup FD's bound, restore its compatible ESP under that update's rollback procedure before restoring `boot_a`.
