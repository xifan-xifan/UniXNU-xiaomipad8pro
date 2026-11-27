# Host-side checklist

Use this checklist on the build Mac to check the checkout and existing macOS 27.3 artifacts. Run from the repository
root in a native arm64 shell. The checks use local files or synthetic fixtures. For preparing inputs and building images,
see the [build reference](building.md). The [CI reference](ci.md) describes the hosted checks.

## Ground rules

- Leave the tablet, its live console, and the installed system alone. Use the offline verifiers and fixture tests below.
  Keep line builds, ESP builds, and publishing outside the check session.
- Use `bash` for shell snippets. Select Python 3.11 or newer on `PATH` for build-related tooling, and `unset SDKROOT` for
  the kext wrappers. The Markdown link checker can also run with the macOS system Python. Check the interpreter with
  `python3 --version`. [Build host preflight](../tooling/repro/check-host.sh) · [Markdown link checker](../scripts/ci/test-doc-links.py)
- Keep logs in a per-session directory under the ignored `out/`. Capture stdout and stderr and record each check as PASS,
  FAIL, or SKIPPED. For a skip, record the missing input. Keep raw logs and device-specific values local.

```sh
mkdir -p out
R=$(mktemp -d "$PWD/out/host-checks.XXXXXX")
```

Run object builds one at a time and only when the checkout is idle.

## 1. Checkout and shared policy

| Check | Command | Pass |
| --- | --- | --- |
| Host test inventory | `python3 scripts/ci/self-tests.py --list` | Runner prints the inventory. For the current count and tool requirements, see [CI](ci.md#host-test-inventory-2026-11-27). |
| Repository manifests | `python3 scripts/ci/test-repo-manifests.py` | Exit 0. Covers manifests, native kext descriptors and wrapper wiring, patch hygiene and shell syntax. |
| Shared headers | `bash package/include/tests/run.sh --sanitize` | Exit 0. Covers SPMI and SCM locks, multimedia votes, WCN rails and other shared helpers. |
| Boot arguments and guard | `bash tooling/k27/test-piano-bootargs.sh` and `bash tooling/k27/test-piano-guard.sh` | Both exit 0. |
| Idle and suspend policy | `bash tooling/k27/test-piano-pm.sh` | Exit 0. |
| Native kext rule | `python3 tooling/k27/check-native-kexts.py --self-test` | Exit 0 on synthetic accepted and refused descriptors and link logs. |
| Metal bundle paths | `python3 tooling/metal/check-bundle-paths.py --self-test` | Exit 0. |

## 2. Native kext host tests and object builds

For tests without kernel inputs, use the suite entry points in the [CI inventory](ci.md#host-test-inventory-2026-11-27).
The object wrappers below also need the prepared 27.3 tree, its matching relinked kernel, and its matching collection
base. Set `PIANO_K27_TREE` to the prepared clone, `K27_KERNEL` to the kernel printed by `prepare-k27-tree.sh`, and
`KC_BASE` to the 27.3 `kc273-mac17j.bin`. Set `KC_BUILDER=k27/kc-build273-piano.py` for the trial links in the driver
wrappers. Skip the object builds if those inputs are missing. [Kernel preparation](../tooling/k273/prepare-k27-tree.sh) · [Build reference](building.md#kernel-collection-and-staged-userland)

Run the wrappers in the default manifest's order. PianoDwc3's trial collection needs the PianoA830 objects. The list
below covers all **27 native kexts**, including `PianoIR`. Objects go to `out/native-kexts/`, and the PianoDwc3 trial
link uses scratch files under `out/k27/`. [Native kext manifest](../configs/k27-native-kexts.txt) · [Wrapper mapping](../tooling/k27/build-native-image.sh)

```sh
for driver in standin ufs ram-block gop-display piano-a830 piano-backlight piano-dpu \
    piano-dwc3 piano-input piano-keyboard null-ethernet piano-power piano-torch piano-ir \
    piano-camera piano-thermal piano-touch piano-wifi piano-wifi-ethernet piano-audio \
    piano-usbhost piano-video piano-sensors piano-dp piano-bluetooth piano-policy software-aes; do
  if bash "tooling/k27/build-native-$driver.sh" > "$R/driver-$driver.log" 2>&1; then
    printf '%s PASS\n' "$driver"
  else
    printf '%s FAIL\n' "$driver"
  fi
done > "$R/drivers.log"
```

Check each wrapper's exit status in `drivers.log` and its own output. In the PianoDwc3 log, confirm the extra vtable
check ran and reported `0 problem slots`.

## 3. Userland, provenance tables and persistence

| Check | Command | Pass or skip condition |
| --- | --- | --- |
| Userland staging | `python3 tooling/userland/test-stage.py` | Exit 0 on the staging fixtures. The current manifest defines **14 arm64 Mach-Os** across non-planned units, including the script-built `piano-rotated`. [Userland inventory](building.md#kernel-collection-and-staged-userland) |
| Camera tables | `python3 tooling/camera/sensor-tables.py check data/camera` | Exit 0 on the table format and manifest checks. [Camera provenance](../data/camera/README.md) |
| DisplayPort PHY tables | `python3 tooling/dp/dp-phy-tables.py check data/dp` | Exit 0 on the table format and manifest checks. [DP provenance](../data/dp/README.md) |
| Partition safety | `python3 tooling/persist/test-partition-safety.py` | Exit 0 on the GPT and installed-identity fixtures. |
| Data store | `python3 tooling/persist/test-data-store.py` | Exit 0. Skip if the pinned UniXNU checkout supplying `unixnu.bootimg` is unavailable. |
| Persistence replay | `python3 tooling/persist/test-persist.py` | Exit 0. Skip if the saved boot and partition-table fixtures are unavailable. [Local-input skips](../scripts/ci/self-tests.skip) |

## 4. Existing images and records

Select files from one recorded 27.3 build. Set the variables in the table to the matching input paths in your shell.
Skip a check if its inputs are missing. Keep the verifier's full output and exit status in the session log.

| Check | Command | Pass |
| --- | --- | --- |
| RAM images | `python3 tooling/bootimg/verify-ram-images.py "$RAM_BUILD" --reference "$REFERENCE" --fd "$FD" --dtb "$DTB"` | Exit 0 and `PASS: 0 failure(s)` for all images recorded in the build JSON. The default cap is 250,000,000 bytes per image. [Verification details](building.md#build-and-verify) |
| ESP checksum | `shasum -a 256 "$ESP_IMAGE"` | Digest matches the `sha256` in that image's build JSON. |
| RAM window | `python3 tooling/adt/check-ram-window.py --adt "$ADT" --kc "$KC" --fd "$FD"` | Exit 0 and a window consistent with the selected ADT, collection and FD. |
| Native collection log | `python3 tooling/k27/check-native-kexts.py . --kc-log "$KC_LOG"` | Exit 0. The default profile reports 27 native kexts linked. |
| Backup checksums | Run `shasum -a 256 -c SHA256SUMS` from each selected, already accessible backup set. | Every recorded file is `OK`. |

If a console capture already exists for the same build, compare it with the corresponding [qualification record](STATUS.md).
Review only the subsystems and boot arguments enabled in the selected profile. The [November 27 results](results-2026-11-27.md)
include RAM display, audio and IR checks alongside installed observations.

## 5. Links, privacy and working tree

| Check | Command | Pass |
| --- | --- | --- |
| Relative links and driver index | `/usr/bin/python3 scripts/ci/test-doc-links.py` and `/opt/homebrew/bin/python3 scripts/ci/test-doc-links.py` | Both exit 0 and report `0 error(s)` when both interpreters are available. |
| Tracked-tree privacy | `python3 scripts/ci/privacy-scan.py` | `0 hits, 0 refused files`. |
| Committed diff privacy | `python3 scripts/ci/privacy-scan.py --diff "$BASE"` | `0 hits, 0 refused files`, with `BASE` set to the applicable comparison commit. The scanner reads committed additions and commit messages. |
| Working tree | `git status --short` and `git diff --check` | The checks leave the initial tracked status unchanged and report no new whitespace errors. |

Record outcomes in `"$R/SUMMARY.md"` and include the local session directory when handing the results back.
