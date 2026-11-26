# 2026-11-26: the 27.3 collection mounts the 27.3 root and panics at the owner cache gate (EAUTH), and why nothing authorizes it

## The run

Same unattended cycle as the 45 s run, one variable: `PIANO_RESET_AFTER=420`.
`out/k273-ram420/boot-piano-macos-ram-A-420.img` (160,735,232 B, `0db5d160f8ebe8797efa0cb67fefc147ad52c9588274be71ee5725d528be7784`,
the same assembly as `50d7b253...` but for the window).

| time | event |
| --- | --- |
| 17:09:44 | `fastboot boot`, entry `macos-8cpu` |
| 17:11:19 | back in ABL fastboot after **100 s**, not 420: something ended the boot at T about 95 s |
| 17:11:29 | one reader boot, `oem dump 9cb00000 700000` `rc=0` cold |
| 17:12:48 | HyperOS up on adb (`sys.boot_completed 1`) |

The console (20,025 bytes, mailbox `logs/native-runs/ram-k273-420-20261126-170924/`) repeats the 45 s boot exactly
(`NativeUfs: SystemOS matched after 1600 ms` ... `apfs_log_op_with_proc: disk5s1 mount-complete volume Macintosh HD`
of volume `84581E99-85BE-4957-AC53-2947E887B3E4` ... `VM Swap Subsystem is ON`) and then:

    panic(cpu 0 caller 0xfffffff000bbfa2c08): offline owner SystemOS mount failed: 80 @kern_exec.c:8675
    MACH Reboot
    piano-psci-halt: PE_halt_restart(1) at 77006 ms: PSCI SYSTEM_RESET (0x84000009) through smc

The line arrives byte-doubled (`ppaanniicc(...`), because the panic path mirrors the string twice; the text is unambiguous.
80 is `EAUTH` (macOS `errno.h`; `python3 -c "import errno"` on the Mac says 80 for `EAUTH`, 81 for `ENEEDAUTH`). So this is not a
mount error from APFS: nothing mounted anything, and the mount never started.

The AMFI question from the older collection stays closed: this boot passed every security-policy registration, mounted the root,
and only then reached `load_init_program`.

## What the gate is, and exactly why it fails here

`patches/xnu-12377/0002` (the published k27 tree's owner chain) calls, before launchd:

    root_authorized = (vfs_flags(root_mount) & MNT_RDONLY) && root_device != NODEV &&
        native_verified_block_root_authorized(root_device);
    if (!root_authorized) goto done;            /* done: if (!root_recheck) error = EAUTH */
    ...
    if (!native_owner_systemos_volume_uuid(volume) ||
        !native_verified_block_volume_device(volume, &selected, device_path, sizeof device_path)) goto done;

and `kern_exec.c` turns any non-zero into the panic above (`XNU_OWNER_CACHE_MOUNT=1` is in the tree's build line, both variants).

`native_verified_block_root_authorized` is compiled into the kernel from the tree's own
`overlay/droidvm/drivers/NativeVirtioBlock/NativeVirtioBlock.cpp` (`return verifiedVolumeQuery(device, nullptr, nullptr)`; the
comment there says the kernel links statically, so it carries a weak default that yields to NativeUfs's definition), and
`verifiedVolumeQuery` asks the block driver through

    __attribute__((weak)) int native_ufs_block_authenticates(IOService *, const char *, uint8_t *) { return -1; }

Our collection has no such provider: `native_ufs_block_authenticates` is defined once, in `package/NativeUfs/NativeUfs.cpp:2444`,
inside `#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK` (the block runs 2281-2452), and
`tooling/k27/build-native-ufs.sh:14` builds the piano kext with `-D NATIVE_UFS_UNVERIFIED_ROOT=1` and no `NATIVE_VERIFIED_BLOCK`
(`package/NativeUfs/README.md`: "Development build: no `NATIVE_VERIFIED_BLOCK` (windows published read-only without owner-signed
block policies)"). The console says so itself: `NativeUfs: UNVERIFIED development root: 2 windows published without owner policies`.
Weak default `-1` means "not a UFS window", authorization fails, `error` stays `EAUTH`, and the kernel panics rather than exec
launchd. The 0054-era collection never reached this point (it had no owner chain at all: its console has no `SystemOS` line), which
is why this is new with the upstream tree rather than a regression from it.

## What upstream did, and what the route needs

`docs/k273-line.md` line 121 records the maintainer's own first 27.3 boot as passing this gate — the checklist line reads
"`Waiting on <dict> IOMedia ... UUID 84581E99...` then the root mounted (`k27: firmlink ...` lines, `owner cache` stage 6)", and
lines 104-111 record what came after it: `sw_vers` 27.3 26C5035r over the USB root shell, launchd boot tasks, WindowServer,
loginwindow, SecurityAgent and MenuBarAgent as GPU clients on the A830 (`piano-gpu=2`), with the desk-dev profile
(`tooling/k27/build-desktop-images.sh desk-dev`, the Turnip `NativeAdrenoMetal` staged, seed from the 27.0.1 line, `data-persist=0`).
Stage 6 is past `root_authorized` and past the volume lookup, so their build did have a working verified-block provider.

The pieces for that exist in this repository's own records, all host-side and all from the 2026-11-21 round:
`results-2026-11-21-verifiedroot-host.md` (the pinned core resolves `kits/VerifiedRoot` to `974f043d0c71ba2e60331483a41c6c6d35274ee3`,
checked out on Mac and UbuntuPiano), `-owner-collection-replay.md` (a saved replay links `ufs-verified.o` and defines `_vr_open`,
`_vr_verify_block`, `_vr_xnu_crypto`), `-stock-policy-object-replay.md` (the policy object rebuilt from the stock-window SHNGROOT v2
inputs), `-verified-ufs-without-poll.md` (Apple ld combined NativeUfs + VerifiedRoot + the XNU crypto adapter + the stock policy
object into one arm64e relocatable, `c5e6e1c163001548321d743cd7a48adac2dcf7d144d99eade563fd0ea165efc9`, defining `_vr_verify_block`
and `_verified_root_policies`), and `-stock-root-v2-assembly.md` (every OS block and SystemOS block of `root.img` verified against
the archived public anchors, and a one-byte change rejected). None of those objects survives on the Mac today, but the kit itself is a
public submodule of `UniXNU/UniXNU` at `kits/VerifiedRoot`, and the builder is
`unixnu-research/UniXNU/scripts/build-verified-root-probe.py`.

Two routes from here, and the cheap one first:

1. **Verified root window (upstream's route).** Initialize `kits/VerifiedRoot` at its pin, build NativeUfs without
   `NATIVE_UFS_UNVERIFIED_ROOT` and with `NATIVE_VERIFIED_BLOCK=1`, and regenerate the policy object from the SHNGROOT manifest of
   the root image that is actually on `linux_rootfs` (its anchors are inside the image; this is signature *verification*, so no
   private key). This is also what a persistent install needs, and it is what the Data write gates were designed against.
2. **Drop the owner cache mount for the native variant** (`XNU_OWNER_CACHE_MOUNT=0`). One build flag, but the shared-cache
   environment (`DYLD_SHARED_CACHE_DIR=/private/preboot/...`) and the SystemOS composition disappear with it, so every binary's
   dyld lookup has to come from the OS volume alone. Untried here, and it changes what a "desktop" means relative to upstream.

Route 1 is the one to take: it is what the maintainer's boot used, and route 2 would leave the desktop unreachable for a reason we
invented.

## Device state

No persistent write this round: one `fastboot boot`, Data volatile (`data-persist=0`), the panic and the guard both returned the
tablet to ABL, the reader then booted the installed system, and HyperOS `OS3.0.309.0.WPYCNXM` came up by itself.