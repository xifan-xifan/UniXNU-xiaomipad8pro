# 2026-11-28 ul9: why the GPU sat at 160 MHz, and what one boot argument changed

## The symptom

On every boot of the installed set the Adreno governor reported

    PianoA830: R3b LV ... lvl=1 mhz=160 cap=1 cause=hard ... bind=startup why=startup tab=1
    PianoA830: R3a RES ... ceiling=1 l1=1000 ...
    PianoA830: R3a TEMP ... period=30000 ... age-ms=12189,12188
    PianoA830: R3b THERM ... step=3 board-cap=0 effective=1 ... healthy=0

so the GPU could not leave OPP 1 (160 MHz) no matter what WindowServer asked for.

## The cause

`package/PianoA830/adreno830/A830R3bThermal.h:110` needs three things for `healthy`:

    healthy = gpu && boardLive && (!last || (now >= last && gap <= Lease));

`boardLive` needs `board`, and line 102 demands that the board-temperature snapshot carry

    b.period_ms == 5000 && b.age_ms[0] <= 10000 && b.age_ms[1] <= 10000

`PianoThermal` publishes `period_ms` from the ADC thread, `package/PianoThermal/PianoThermalAdc.cpp:447`:

    uint32_t p = bootArg("piano-thermal-adc-s", periodS);   // clamped to 5..3600

and `PianoThermal.cpp:1184` calls `adc::hookStart(this, adc::hookLevel(), gArgs.logS, gArgs.delay)`, so the default round period is the *report* period: 30 s.
The board thermistors were therefore sampled every 30 s and read about 12 s stale, the `board` test never passed, `healthy` stayed false, `hard` at line 140 is
`!healthy || ...`, and `cap = hard ? 1 : ...` pinned level 1 with `cause=hard`.

## The change

`esp-7ccdd855-k273-ul9.img` (`99d70675776df9fefe1228bec472d9c9c42e7c9969ec466dbd8a710dba39087c`) adds `piano-thermal-adc-s=5` and drops
`piano-keyboard-gestures=1` (26 bytes; the keyboard cover touchpad gestures, nothing on this unit uses them, and they come back once the long boot-args kernel
frees room). The macos entry is 977 bytes. Payload, ADT, trust cache and seed are ul8's byte for byte, the store stays bound to `linux_data` LBA 117735999 with
seed `b2583ea517732505`. Written by `maint-session.py` steps `maint-boot map esp-final check maint-exit`, log `logs\session-flash-ul9-20261128-095708.log`, `done`, rc 0.

## Measured on the tablet, the same boot

    PianoA830: R3a TEMP ... period=5000 ... age-ms=1052,1051
    PianoA830: R3b THERM ... step=14 board-cap=0 effective=7 ... healthy=1
    PianoA830: R3b LV ... lvl=2 mhz=222 cap=13 cause=skinband ... bind=skinband why=demand
    PianoA830: R3a RES ... windows=10896 busy=158 ceiling=13 l2=651 l4=46 l8=288 l10=13
    PianoA830: R3a ACK ... l2=193 l4=187 l8=170 l10=5 l11=1 ... gpu-peak=594
    PianoA830: R3a COUNTERS ... vote=604 failures=0 sent=612 ack=612 timeout=0 errors=0

The cap went from 1 to 13, `cause` from `hard` to the skin band, and the ladder is live: 288 windows at level 8 and 13 at level 10, 612 votes with 612
acknowledgements, `gpu-peak` 594 MHz. Sitting at 222 MHz while idle is now `why=demand`, which is correct behaviour rather than a cap.

Temperature follows the freedom: the SoC peaks moved from about 52 to about 56 C and the ladder settled at `effective=7` with the trend near zero, with the skin
band bounding the top OPP as `docs/drivers/m120-throughput.md` describes on a docked probe.

## What did not change

- `NativeGopDisplay: swap_wait(2160, 0x80000001, 0) = 0xe00002d5 (16000 failures)` is still there. That is the separate GPU and display pool defect, and it survives
  this governor fix.
- Wi-Fi (`level 8 step 311 link up`, 32 networks scanned), the level 8 audio path and the backlight (`percent=45`, 4 writes) read the same on ul9.
- `/Users/piano/Desktop/persist-normal.txt` (`b88c2645...`) and `persist-ul7.txt` (`ed68ce9f...`), written two ESP generations apart, read back unchanged after
  ul7 to ul8 to ul9 with an Android boot in between.

## USB pipes after this boot

COM5 is the PianoDwc3 kernel monitor (`help` lists `log`, `live`, `usb`, `spmi`, `ioreg`, `reset ...`). COM6 is the root shell once the user pipe helper has had
one to two minutes after login; `piano-shell.ps1` can report the kernel monitor's `unknown command` when it probes too early, so `tmp-c6.ps1` (raw COM6 with
DTR/RTS, retried) is the reliable path.
