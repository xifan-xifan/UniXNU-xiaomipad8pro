# 2026-11-27 (night, fifth boot of this seed): the touch screen serves frames; and a password cannot be changed here

Fifth `fastboot boot` of seed `bb6dd1c3`: the input boot again, but with the two pinned panel images actually compiled in
(`tooling/k273/recipes/a830-touch.sh`). Nothing was written to any partition; the image resets itself into ABL and the reader
brings HyperOS back.

| Item | Value |
| --- | --- |
| Image | `boot-piano-macos-ram-A-bundled.img`, 205,824,000 bytes, SHA-256 `c0c1e2afc1659557fcb496bb6a5e933d50feabd786eb00bb76237b3dc839798c` |
| Recipe | `a830-touch.sh` (images staged from `/Users/zhangqi/Downloads/XiaomiPad8Pro-Ubuntu/xiaomipad-8pro-mainline/firmware/novatek`, calibration forced none) |
| Run | `logs/native-runs/ram-k273-a830touch-20261127-234031` |
| Console | `logs/usb-console/a830touch-live-com.log` - started at about +140 s of this boot, so step 5's own line is not in it |
| Store | resumed again (`Data store state 2`, `seed bb6dd1c3`, `store seed bb6dd1c3`, generation 3) |

## The panel is talking

```
PianoTouch: frames at 145 s: 20 reads/s, 20 edges/s; edges 1272, reads 1271 (data 1271: checksum ok 809, bad 460, inconsistent 2; handshake ...
PianoTouch: gestures at 145 s: 0 taps, 0 right clicks, 0 drags, 0 scrolls, 0 pinches, 0 cancelled; contacts 1 (2 palms, 18 palm points, 0 ...)
PianoTouch: idle: 1119 wakeups in 60 s, irq 0 (1119 frame reads; 1119 idle sleeps; 0 reads on an interrupt's edge, ...)
```

The host download of step 5 (the thing the previous boot failed) is behind us: the kext is reading matrices from the controller at
20 Hz and decoding them, and the owner watched the pointer move on the login window. Two things to keep working on, both visible
above: 460 of 1271 frames came back with a bad checksum, and one phantom contact is being tracked as a palm while nothing touches
the glass.

**Orientation.** Left/right correct, up/down reversed, reported by the owner with a finger on the glass. That is this BOE unit's
documented Y reversal (`docs/drivers/touch.md` section 5 and 7: `PIANO_TOUCH_INVERT_Y=1` at build, `piano-touch-flip=auto` taking it),
which this host had never stamped because the images - and so the whole orientation path - had never been in the build. The recipe
now exports `PIANO_TOUCH_INVERT_Y=1`; that is a compiled constant, so it costs none of the 999 command-line bytes.

## The login window, and why this build cannot change a password

What the owner saw was the idle lock, not a failed auto-login: the seed sets `autoLoginUser => piano` plus
`/private/etc/kcpassword`, and the boot does reach the desktop by itself. From the USB root shell the display and lock policy was
set to stop asking, and it is on the persisted Data volume:

```
pmset -a displaysleep 0; pmset -a sleep 0        -> pmset -g custom: displaysleep 0, sleep 0
defaults write /Users/piano/Library/Preferences/com.apple.screensaver askForPassword -int 0
defaults write /Users/piano/Library/Preferences/com.apple.screensaver idleTime -int 0
```

The account password itself could not be changed, and the reason is a userland gap rather than a permission:

```
dscl . -passwd /Users/piano <new>        -> passwd: DS error: eDSOperationFailed (-14483)
passwd piano                             -> completed with New password / Retype prompts, but the hash did not change
dscl . -authonly piano <new>             -> eDSAuthFailed; the old password still authenticates (RC=0)
launchctl print system/com.apple.localauthenticationd
                                         -> Could not find service "com.apple.localauthenticationd" in domain for system
                                         (and /usr/libexec/localauthenticationd is not in the image)
```

The same absence is what the console reports on its own: `authorizationhos ... _aks_verify_password:1956: aks connection failed`.
Authentication through the directory works (`-authonly` answers), only the change path, which goes through that daemon, does not.
`/private/etc/kcpassword` was therefore left alone: auto-login only works while it matches the account, and writing the account
hash by hand behind a directory service that cannot validate it would risk locking the unit out for no gain.

The password for `piano` is recoverable by decoding that `kcpassword` blob with the public key in the seed tooling
(`make-macos-data-seed.py`), which is how it was read here. The value itself is unit data and stays out of the repository.
