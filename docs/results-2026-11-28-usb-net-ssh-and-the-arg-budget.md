# 2026-11-28 - the USB network on the installed set, root ssh from Windows, and why video, audio and Wi-Fi are off

Two boot arguments turned out to be the interesting part of this session, so this file records the install first and
the argument budget second.

## The installed set

`esp-7ccdd855-k273-ul4.img` 1,073,741,824 bytes `fe3d428ab0ed46eb919742eddf073552ccdbc6e863059321f55308821a698d18`,
`macos` entry 978 bytes: ul3 plus `piano-usb-net=1` (the CDC-NCM function after the serial ports) and
`native-gop-display-shot=1` (the root-only panel capture the video checks need). Same store seed `b2583ea5`, same
`data-persist=2 data-store-lba=117735999 native-ufs-root-part=linux_rootfs`, still no `piano-reset-to`.
Written to UFS LUN 0 partition 37 by `tooling/persist/maint-session.py` in session
`out/persistent/session-k273-ul1-20261128`, log `logs/session-flash-ul4-20261128-073241.log`, `done`. `boot_a` was not
touched, so the Android side of the switch (`7c444a2d` in `E:/piano-backup/persistent-boot-20261128-010938-fe5vsrzx`)
still rolls the tablet back.

## Verified on the tablet

- The Data store survived the ESP change: `cold1.txt` `0d50e3f85c7322392216e11a1f77f500`, `cold2.txt`
  `046b1cb31f82c26641ad0000f7410026`, `cold3.txt` `010e6b2f1f71e9e56b98a97723d8de7c`, `persist-normal.txt`
  `b88c264588d85dc3f76957506b07ccdb`, `persist-normal2.txt` `8bd0e7a8811c747617ce8a54441a81fa` - all five read back
  with the MD5 they had before the flash.
- The network function came up as designed (`logs/usb-console/ul4-boot1-com.log`): `PianoDwc3: net: CDC-NCM network
  function requested (piano-usb-net=1)`, then `macOS named the interface en1`, then `net: link up: the Mac runs the
  NCM data interface (480 Mb/s)`. Windows 11 named its end `UniXNU piano network`, MAC `02-50-49-41-4E-03`, 480 Mbps,
  and self-assigned `169.254.142.124/16`. The serial ports are unchanged: the console still reports
  `piano-usb=2 ... shell=1 hid=1 net=1`.

## Root shell over the cable, without the console pipe

The kernel names `169.254.80.2/16` the debug address, but `piano-usbnet-address` only keeps an address that `desk-dev`
already set, and the installed image never set one. So set it once per boot and start sshd:

```sh
ifconfig en1 inet 169.254.80.2 netmask 255.255.0.0 up
/usr/sbin/ssh-keygen -A            # the image ships no host keys
/usr/sbin/sshd -p 2222 -o UsePAM=no -o PermitRootLogin=yes
```

`launchctl bootstrap system /System/Library/LaunchDaemons/ssh.plist` is refused (`Bootstrap failed: 5: Input/output
error`) because Remote Login is not switched on, and on port 22 the pubkey is accepted and the session then dies in
`pam_restriction`; `-o UsePAM=no` is what makes port 2222 usable. The authorized key is in
`/var/root/.ssh/authorized_keys` (root reads its home as `/var/root`). From Windows:

```powershell
ssh -p 2222 -i E:\piano-backup\usb-dev-key\dev2 -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL root@169.254.80.2
```

Two traps: a key generated with a passphrase fails here as `Permission denied` after the server has accepted the key
(the client cannot prompt, and the server log ends at `Postponed publickey` when the client hangs up), and `ioreg -l`
is far too much traffic for this link - keep command output small.

## Known limit: the link goes deaf without `piano-usb-net-txflow=1`

After a few hundred kilobytes from the tablet, ping and ssh both went silent while COM5/COM6 kept answering and
`ifconfig en1` still said `status: active`. `ifconfig en1 down` plus the address again restored it in seconds. That is
the case `piano-usb-net-txflow=1` exists for (`package/PianoDwc3/PianoUsbNet.cpp` holds the frame when the TX ring
fills and retries through the family output queue), and the daily profile has carried it since v13.1y12q. The setting
is `immutable after init`, so there is no runtime switch, and its 24 bytes do not fit on this line.

## Why `video-v6 --qualify` failed here, and what that says about audio and Wi-Fi

On the installed set the tool passes its host-side phases and stops at
`FAIL create-required-hardware-pv status=-12906` (`first-failed-phase=single-regression`). Neither the kernel nor the
fixtures are at fault: `package/PianoVideo/PianoVideo.cpp` takes its level with `gArgs.level = bootArg("piano-video",
0)` and `piano_video_native_ready()` is only true at `gArgs.level == 6`. The installed boot line carried no
`piano-video=` at all, so no Iris session is offered and VideoToolbox has no hardware decoder to create. The same
holds for `piano-audio` (absent) and for `piano-wifi` with `piano-wifi-iova` (absent, and that one defaults to 0, so
level 8 needs both words). The B10 and Wi-Fi rows in `docs/BACKLOG-hardware.md` describe the boots they came from,
not this line.

`esp-7ccdd855-k273-ul5.img` `ca139fc640c63255e25353672bbba4143e451bbdecc34c0b5f0781245ac9fad5`, line 992 bytes, adds
`piano-video=6` and nothing else. That is the whole remaining headroom.

## The budget, stated exactly

`tooling/adt/boot_args.py` gives the tools 999 bytes (`LINE_MAX - 1`); DarwinBoot passes at most 1023. The costs are
`piano-video=6` 14, `piano-audio=2` 15, `piano-wifi=8` 13, `piano-wifi-iova=1` 18 and `piano-usb-net-txflow=1` 24
bytes each including the leading space, and the line already carries 978 before them. About 130 of those bytes are
debug conveniences (`pstore-console` and its size, `piano-usb-shell`, `piano-usb-hid`, `piano-usb-net`,
`native-gop-display-shot`). `docs/boot-args-length.md` already gives the designed answer: k27 0066 merges an ADT tail
of up to 4095 bytes and 0076 shows userland the same merged text, which is what `piano-long-boot-args=1` needs. This
collection ships the plain kernel (`kernel.macho` `d4892e26`), and the builder says so when asked to split a line.

The sibling build is on the Mac: `~/Codex/k27-tree/out/k27/kernel-k27-0066-native.macho` 27,557,496 bytes
`12d3088acce0b2c375066d94dd60264766c9d6cb978f5cd03e062a1c81dcc3be`, built `Nov 26 05:52` - the same minute as the
installed kernel reads in `uname -v`, so from the same tree and run. Moving the installed set onto another kernel
collection is a bigger step than adding an argument (kernel, ADT, trust cache and every kext ABI at once), so it is
the next thing to qualify rather than something to slip in beside a video test.

## Next

1. Boot ul5, read `machdep.native_gop_display_shot`, and take `video-v6 --qualify` plus a panel capture as the
   single-frame decode evidence on the installed set.
2. Continuous playback on the same capture path.
3. Brightness and battery need no new arguments; audio does (`piano-audio=2`).
4. Wi-Fi needs 31 bytes. Either the 0066 collection lands, or one round runs on a Wi-Fi ESP that trades the video
   words for the radio ones. Wi-Fi also needs the owner SSID and passphrase in
   `/private/var/db/piano-wifi/profile` (root-only: `ssid=`, `security=wpa2-psk`, `psk=`), which is not in this
   checkout.

