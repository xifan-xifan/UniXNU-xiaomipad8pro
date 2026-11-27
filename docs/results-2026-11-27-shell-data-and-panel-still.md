# 2026-11-27 21:51 (image dc545522): a root shell on the cable, Data on the medium, and the desktop as the panel shows it

Three things this unit had never done at once: a process of ours running as root on it, its Data written to the medium instead of a
RAM copy, and its own screen read back as a file. All three came from one boot, because all three needed the same missing thing -
a way in - and the way in was blocked by a seed with no boot job.

**The obstacle, stated exactly.** `out/turnip-a830/stage/scratch-data.img` comes from the 27.3 verified-root seed, and mounting the
unpacked seed on the host shows why the whole userland pipeline had never run here: `Library/LaunchDaemons` holds two plists,
`amsdstat.plist` and `com.apple.postfixsetup.plist`, and no `org.droidvm.run.0.plist`. That job is what runs `settime.sh`, and it is
the only root hook a staged seed has (`docs/userland.md` 11: "the boot job is the only root hook on a seed"), so `install.sh`, which
is what copies a unit's files to their dest paths as root, had never run, and `stage.py` refused to build such a seed at all.

**What was built.** `stage.py --seed-install-root UNIT` installs the named unit at its dest paths inside the seed and then, in a
second `hdiutil attach -owners on`, chowns the files and every directory of their path `root:wheel` with sudo - the same re-attach
`camera_seed_ownership` already does for the camera's staging area, and what launchd asks of a job file. A real Data volume owns
`/`, `/Library`, `/usr/local` to `root:wheel` anyway; the seed had them all as uid 501 because it was packed without ownership.
The recipe is `tooling/k273/recipes/a830-shot.sh`: the A830 stage, then `pianousb-shell` and `gop-shot` installed that way, then
`PIANO_DATA_STORE_REPLACE=1` (`data-store-replace=1`) so `NativeRamBlock` builds a store for *this* seed instead of keeping the one
it found and running Data volatile, and `piano-reset-after=900` to leave time to work before the image resets itself.

```
out/a830-shot-stage/scratch-data.img: 63100720 bytes packed, identity bb6dd1c365a6a827 (input 312a2319bbb4d2e4)
root:wheel: usr/local/libexec, usr/local, usr, Library/LaunchDaemons, Library, usr/local/bin, .,
            usr/local/libexec/pianousb-shell, Library/LaunchDaemons/com.unixnu.pianousb-shell.plist, usr/local/bin/gop-shot
```

Boot `logs/native-runs/ram-k273-a830shot-20261127-215044`, console `logs/usb-console/a830shot-20261127-215041-com.log`, image
`dc54552255da0eae50ba5768c5767549d4dbd882ebd44925f2c375b28b29113f`, 999 bytes of command line, self-reset back to ABL after 921 s,
HyperOS back on adb after that. Nothing was written to any partition; the writes below are `NativeRamBlock`'s own store region.

## 1. The shell

```
PianoDwc3: start: piano-usb=2 ... shell=1 ...
PianoDwc3: /dev/pianousb created (major 21): port 1 is the user pipe; select, poll and kqueue
```

port 1 answered with `root@Xiaomi-Pad-8-Pro ~ #` - launchd loaded `com.unixnu.pianousb-shell` from the seed and `/bin/zsh` is on the
pipe as root. Two host-side notes, both from the pipe itself rather than the tablet: the prompt is zle-drawn (bracketed paste,
reverse-video `#`, `[J`), so a prompt-shaped regex is fragile and `piano-shell.ps1` now probes by asking the port to *evaluate*
`echo PIANO_$((6*8))` and looking for `PIANO_48` - which also proves the port is a shell and not the kernel monitor, because the
monitor echoes what it is typed and answers `unknown command:`. And base64 through the pipe loses bytes in big bursts: 48 KiB
requests decoded short by ~8 KB, 12 KiB requests came back byte for byte (`tooling/native/piano-fetch.ps1`, verified by size and
sha256). `tooling/native/piano-console.ps1` is the port-named version for port 0, whose monitor offers `log`, `live on|off`,
`echo on|off`, `usb`, `spmi`, `ioreg`.

The first two post-install checks of the install guide (`skills/install-piano-macos/SKILL.zh-CN.md` 6) are now measured here, not
inherited: `sw_vers` is `macOS 27.3 / 26C5035r` and `sysctl -n hw.model` is `Mac17,11`. 501 processes; `pmset -g batt` says
`Now drawing from AC Power`; `ifconfig -l` is `lo0 en0 utun0 utun1 utun2 utun3`.

## 2. Data on the medium, for the first time on this unit

```
machdep.piano_data_io: mode=single-gate depth=1 inflight_max=1 ... persist=2 state=3 lba=117735999 pages=6291456
                       bound=117735999 generation=3 flags=2 seed=bb6dd1c3 store_seed=bb6dd1c3 errors=0 reset_after=900
                       store_rd=9028/708847us store_wr=87889/11818089us store_sync=2/752us store_state=3
mount: /dev/disk3s1 on / (apfs, local, read-only, union, journaled); /dev/disk1s1 on /System/Volumes/Data (apfs, local, journaled)
```

`state=3` is a store freshly built from this seed and `store_seed == seed`, which is the opposite of every boot up to tonight
(`Data store state 7 (kept) ... Data is volatile this boot`). `store_wr=87889` and 11.8 s of store write time are UFS writes, and
`NativeRamBlock` prints no volatile budget line for this boot. The read-back - write a file, let the image reset, boot the same
seed again without `data-store-replace=1`, find the file - is the next run, with the same seed identity.

Written during this boot and left on Data: `echo piano-20261127-a830shot > /private/var/db/piano-persist.txt` (then `sync`), plus
the still and the diagnostic dump below.

## 3. The panel, as a file

```
/usr/local/bin/gop-shot /private/tmp/shot.png   -> source=boot format=BGRA layout=8-bit, 8714178 bytes
```

The sysctl reported the boot-frameback mapping rather than a latched flip window, which is worth following up (with
`piano-dpu-flip=4` the DPU latches, and `docs/drivers/native-gop-display.md` 9 says a latched flip is read through its IOSurface);
what the file contains is not in doubt. Fetched over the cable and downsampled on the tablet with `sips -Z 320`
(sha256 2f5f062b0736c81718e35b12c1e6aa9b0b5a55d4d7a9ca6ad382edf3c1c69fc8`, 113,893 bytes): the menu bar with the Apple menu
and Finder's menus across the top, the wallpaper full-bleed, the Dock with its tiles along the bottom, and a notification panel in
the top-right whose text is too small at 320 px to read. That is the desktop, on this panel, from this boot - which is what
`results-2026-11-27-dpu-flips-4771-latches.md` could only infer from `flips: 4771 in all` and `underrun none: pass`.

## Still open after this boot

The notification's text and any dialog behind it; input (touch, keyboard, backlight) is untested and nothing in this boot needed it;
the read-back of `/private/var/db/piano-persist.txt` after a reboot; the GPU's `BoAlloc refused ... status=0xe00002bd` at
16 MiB against an 805 MB/1 GiB pool, which appears once the desktop is up and running; cold boots, the two Android<->macOS
rounds, hard-reset recovery and the rest of the daily checks. The seed's identity changes with any restage, so the persistence run
reuses this image with only `data-store-replace=1` dropped.
