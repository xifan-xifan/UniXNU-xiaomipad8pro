# Using native macOS on piano (operator's guide)

Run host commands from the repository root on the Mac connected to the tablet by USB-C. Keep device-specific settings in your
local environment. See the [install design](persistent-boot.md), [driver index](drivers/README.md) and
[bring-up guide](bringup-debugging.md), including [how to set `PIANO_K27_TREE`](bringup-debugging.md#4-rebuild-after-a-change).

## 1. What runs where

| Piece | Where | Changed by |
|---|---|---|
| BootShim + FD (UEFI) | `boot_a` (signed v5, 13 GiB DarwinBoot window) | `maint-session.py boot-a` (rare) |
| UniXNU payload: kernel collection, ADT, trust cache, Data seed, boot arguments | ESP, LUN 0 partition 37 | `tooling/k273/build-k273-esp.py` + `maint-session.py esp-final` |
| macOS root (OS + SystemOS) | windows in `linux_rootfs` | the §13.8 maintenance session |
| Data (user changes) | the 200 GiB store over `linux_data` (LBA 71859642) over the ESP's seed | macOS itself |
| Our userland (agents, the USB root shell, tools, the pmset defaults) | Data: `/usr/local/{bin,libexec}`, `/Library/Launch{Daemons,Agents}` | `configs/userland.toml` via the seed's boot job or a [live update](userland.md) |

On power-on the boot chain is ABL → UEFI → UniXNU → macOS. A `fastboot boot` RAM image takes priority over the installed copy
for that boot. See the [boot-time measurements](boot-time.md).

## 2. The installed profile

The installed daily build is **v13.1y12q** (2026-11-26) on the 27.3-only `macos-27.3` entry built by
`tooling/k273/build-k273-esp.py`. Root is on `linux_rootfs` and the owner's 200 GiB Data store is on `linux_data`.
The build inputs, RAM validation, install session and rollback are documented in §13 and §13.8 of the [install design](persistent-boot.md).

- Data mounts in serial mode with `data-store-concurrent=2`. The daily startup job switches the resumed store to concurrent mode
  after the mount. Write-back and queue modes stay off. [Data concurrency and durability](drivers/data-store-m4-m5.md)
- USB debug network TX flow control is enabled with `piano-usb-net-txflow=1`. The transfer test passed 66/66 with exact data.
  Patch 0076 provides the full boot line through `kern.piano_bootargs` for the USB shell and updated helpers.
  [USB TX flow control](drivers/usb-net-reset.md)
- Display defaults to 60 Hz with Night Shift routing, native panel sleep and refresh switching off. The opt-in fixed 120 Hz
  and 144 Hz profiles (`piano-dpu-modes=120` and `piano-dpu-modes=144`) ran from RAM. App presentation at those rates remains
  in qualification. [Refresh profiles](drivers/dpu-m3.md) · [November 26 evening results](results-2026-11-26.md#evening)

## 3. From the Mac

| Task | Command |
|---|---|
| Live kernel console + monitor | `python3 tooling/native/usb-console.py` (Ctrl-] quits; `help` lists the monitor commands) |
| Record the console | `python3 tooling/native/usb-console.py --capture 600` (logs in `logs/usb-console/`, with Mac timestamps in `.times.txt`) |
| Type the login password | `python3 tooling/native/piano-input.py type --secret --enter` (read without echo; never logged, never in argv) |
| Keys, pointer | `piano-input.py key cmd+space`, `move 200 0`, `click`, `scroll -3` (`--help` for all) |
| To ABL fastboot (no buttons) | `python3 tooling/native/usb-console.py --send 'reset fastboot' --capture 5` |
| Restart into macOS | Monitor command `reset`. In the first 180 s of boot it lands in ABL fastboot under the [boot guard](boot-guard.md). `fastboot reboot` continues to macOS. |
| What is attached | `python3 tooling/native/usb-console.py --list` |

Without the USB console (an older profile, or a hang): hold **power + volume-down for 15–20 s** until the fastboot screen. With the
boot guard (an ESP from a k27 0063 collection) a hang resets by itself: within the first 180 s into ABL fastboot (`fastboot reboot`
goes on to macOS), later into macOS.

## 4. Test a change (RAM boot, nothing written)

```sh
K273_ROOT_PART=linux_rootfs tooling/k273/build-k273-image.sh
tooling/native/native-loop.sh out/ram/boot-piano-macos-ram-A-bundled.img
```

For a 27.3 RAM build on the reference layout, set `K273_ROOT_PART=linux_rootfs`. The builder defaults to `linux_data`.
Set `K273_KERNEL` if you need to select a specific prepared kernel. To test the exact ESP payload, use the
`--root-part linux_rootfs` recipe and its RAM test in §13.8 of the [install design](persistent-boot.md).

Set `PIANO_INSTALLED_SEED` from the current install record. When a RAM image uses a different seed, `build-ram-images.py` gives
it `data-persist=0` by default, so Data is volatile and the installed store stays intact. The current kernel's store guard also
keeps the installed store and uses volatile Data when `PIANO_DATA_PERSIST=2` and the seed differs. With an image built before
the guard, use volatile Data for a different seed to avoid starting over. After the store is flattened, a `data-persist=2`
image resumes it regardless of seed. [Data store and seed binding](data-store.md#3-the-store-guard)

## 5. Update the installed ESP

The Data store is a block overlay over one seed, its **base**. Every ESP that keeps the owner's Data carries that seed byte for byte.
The Metal plugin and our userland form the **system layer**, installed as files on the running system.
[The store's base and system layer](data-store.md#9-the-stores-base-and-the-system-layer-2026-11-12)

1. Build and test the change as a RAM image (§4).
2. Once: pin the installed base, `python3 tooling/persist/base-seed.py pin --esp <persist checkout>/out/persistent/<installed ESP>.img`.
   It checks the identity against `PIANO_INSTALLED_SEED`.
3. If the plugin or the userland changed: `python3 tooling/userland/stage.py --live out/layer --gpu-bundle out/turnip-a830/NativeAdrenoMetal.bundle
   --installed-trust "$(python3 tooling/persist/base-seed.py path trust)"`.
4. Build the 27.3-only ESP with `python3 tooling/k273/build-k273-esp.py --root-part linux_rootfs`, using the current 200 GiB
   base seed and the other inputs listed in [persistent-boot.md](persistent-boot.md) §13.8. Rebuild just before installing.
   - Another `--seed` does not carry the owner's Data over: build-esp.py prints a loud note, and the store guard keeps the store and
     runs Data volatile. `--replace-data` is the explicit "start Data over", the owner's decision.
   - A trust cache that lacks a CDHash of the base's is refused: the resumed Data still has the base's plugin.
5. Tablet to ABL fastboot (§3), then on the install state of the persist worktree (`PIANO_PERSIST_OUT=<persist worktree>/out/persistent`,
   with the image and its record copied there; this checkout's tooling; steps before options):
   `python3 tooling/persist/maint-session.py maint-boot map backup [store-backup] esp-final check maint-exit --esp <esp-k273-...>.img --esp-final <esp-k273-...>.img --new-backup`
   then `abl-final reboot`. The verified backup of the previous ESP is the rollback target for this update. Each write is
   followed by a SHA-256 read-back. `store-backup` adds a verified copy of the Data store itself, restored with
   `rollback store-backup`.
6. If the system layer changed, [install it on the running system](userland.md#74-the-procedure).

## 6. Recover

| Situation | Way out |
|---|---|
| The installed macOS misbehaves | RAM-boot a known-good image (§4); the installed copy is untouched |
| A bad ESP | Use the verified backup in the [ESP rollback procedure](persistent-boot.md#136-roll-back-least-invasive-first). If the previous ESP lacks the live layer's CDHashes, remove the layer first or restore the store backup with the ESP. |
| Undo the install | Follow the [rollback procedure](persistent-boot.md#7-roll-back-least-invasive-first) for the affected parts. |
| Back to Android | See the Android recovery entry in the [rollback procedure](persistent-boot.md#7-roll-back-least-invasive-first). |

## 7. Open checks

Hands-on checks remain for Bluetooth keyboard and mouse pairing, joining Wi-Fi from the menu, microphone and QuickTime
recordings, charge-pump fast charging, closing the cover on battery and using the physical power key. The latest Night Shift
off-state and visible panel-wake checks are tracked with the other [known gaps](STATUS.md#known-gaps).
