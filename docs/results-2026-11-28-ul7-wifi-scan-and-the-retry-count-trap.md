# 2026-11-28 ul7: Wi-Fi up to level 8 with a real scan, and the retry-count trap that stopped the next flash

## What changed

`esp-7ccdd855-k273-ul7.img` (`a106ed514a77f5b9d6a67feea67facdc85d81a2b9aae446c5bba503bc29ca87d`, 1 GiB, built from the installed
ul6 record with `tooling/k273/build-k273-esp.py --entry macos --root-part linux_rootfs`, `PIANO_UNIT_PROFILE` set to this unit's
profile, in WSL `UbuntuPiano` because that is where mtools lives). Payload, ADT, trust cache and Data seed are byte for byte ul6's;
only `EFI/UniXNU/unixnu.toml` differs, and the store stays bound to `linux_data` LBA 117735999 with seed `b2583ea517732505`.

The macos entry is 981 bytes (the budget is 999), so the same collection cannot carry everything at once. Dropped for this round:
`piano-video=6`, `piano-video-vt=1`, `piano-usb-hid=1` (48 bytes; video's own acceptance had already passed on ul6 and is recorded in
`results-2026-11-28-usb-net-ssh-and-the-arg-budget.md`). Added: `piano-wifi=8`, `piano-wifi-iova=1`, `piano-audio=2` (46 bytes).

Written to LUN 0 partition 37 by `maint-session.py` (steps `maint-boot map backup esp-final check maint-exit`, log
`logs\session-flash-ul7-20261128-090630.log`, `done`, rc 0). The step `check` re-read the store header and confirmed
`the store at LBA 117735999 on linux_data (seed b2583ea517732505, generation 4, bound to LBA 117735999) IS the one this ESP resumes`.

## Wi-Fi: the radio scans on the installed system

From the root shell over USB (COM6, and `en1` at 169.254.80.2 for SSH), `ioreg -r -c PianoWifi -d 1`:

    "piano-wifi" = 8
    "piano-wifi-link" = "level 8 step 311 link up gen 2 bar 2048 KiB at 0x40400000"
    "piano-wifi-mac" = <025049414e01>
    "piano-wifi-regulatory-country" = "CN"
    "piano-wifi-scan-summary" = "32 network(s), 84 frame(s) (65 beacon, 0 probe), 19 ignored, 8 dropped"

`en0` exists and is `UP,RUNNING` with `ether 02:50:49:41:4e:01`, which is `PianoWifiEthernet`'s documented shape.
`machdep.piano_wifi_join: not set`: the join needs the root-only profile
(`/private/var/db/piano-wifi/profile`, `ssid=`, `security=wpa2-psk`, `psk=`), which needs the owner's network name and passphrase.
So this round proves level 8's link, the regulatory domain and a 32-network scan on the persistent boot; it does not yet prove an
association, a DHCP lease or traffic.

## Audio level 2 only, and level 2 is not sound

`"piano-audio" = 2`, `"piano-audio-dsp" = "APM ready (status 1) after 1 queries from port 0x1 ..."`. Level 2 is the GPR/APM bring-up;
`docs/drivers/audio.md` puts the macOS Speaker at level 8 (`piano-audio=8`, the same 14 bytes, so it costs nothing to ask for), so
`piano-audio=2` was the wrong value and no CoreAudio device appeared. `esp-7ccdd855-k273-ul8.img`
(`2cb111ea817c96263c4314c297bdea3d29ce0b0d30d86b45b3ef56060c902705`) is ul7 with that one value edited, to be run.

## Brightness and battery, read on ul6 the same morning

`ioreg -r -c PianoBacklight -d 1`: `piano-backlight=2`, `piano-backlight-percent=45`, `piano-backlight-code=921`,
`piano-backlight-floor=128`, `piano-backlight-exponential=Yes`, `piano-backlight-writes=6`,
`piano-backlight-primary-enable=63`, and `primary`/`secondary` `readbacks=1` with `errors=0` on both — two write paths, both read back.

`PianoCharge` inside `PianoPower`: `Pack0SOC=94`, `Pack1SOC=95`, `VBUS=4978` mV, `InputMW=2548`, `AppliedFCC=1500`,
`AppliedICL=3000`, `Pack0TempDC=391`, `Pack1TempDC=372`, `ChargePathName="USB 5 V"`, `Connected=1`;
`TemperaturesDeciC = {nsp=517, gpu=525, cpu0=560, cpu1=521}`. `pmset -g batt` says `Now drawing from AC Power`.
Both gauges are live and updating (the `seq` counter advances), so brightness and battery need no boot arguments and no rebuild.

## The trap: `slot-retry-count:a` reached 0 and ABL then refused the maintenance image

Before ul7 the retry count read 6 for four consecutive boots; after ul6 and ul7 it read 0 (HANDOFF's rule: only Android writes the
success bit back, so every macOS boot spends one). The next `maint-session.py` run stopped at `maint-boot`:

    fastboot boot boot-ram-maintenance.img ... Booting FAILED (remote: 'Failed to load/authenticate boot image: Load Error')

with `slot-successful:a = no`, `slot-unbootable:a = no`, `slot-retry-count:a: 0`. This is the failure HANDOFF warns about
("别等归零被标 unbootable（那之后连 RAM 镜像都被拒）"), reached without the slot being marked unbootable.

Recovery, in progress: `fastboot flash boot_a` with the stock Android `boot_a` (`7c444a2d6aa930cd…` from
`E:\piano-backup\persistent-boot-20261128-010938-fe5vsrzx`), boot Android once so it writes the success bit and a fresh retry count,
then return to ABL. That restores the maintenance image and is a third Android round trip at the same time.
