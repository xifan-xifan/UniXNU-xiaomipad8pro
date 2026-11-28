# 2026-11-28 ul10: the boot-arg budget stopped being the limit, and everything fits at once

## What this ESP is

`esp-7ccdd855-k273-ul10.img` (`5b357cb923288ff50201b62338c1e5bc29e0b8633f55c38ec2e398791b22e98c`) is ul9's payload, ADT, trust cache and
seed byte for byte, with the macos entry re-cut in the split shape: `tooling/k273/build-k273-esp.py --split --arg piano-long-boot-args=1`.
DarwinBoot's line carries 694 bytes and the rest lives in the base ADT's `/chosen/piano-boot-args-macos` as a 400-byte tail, which the
installed kernel merges at boot (k27 patch 0066, present in this kernel collection; the string `piano-boot-args: /chosen/%s` is in
`out\persistent\esp-7ccdd855-k273-ul9.img` at 236451059, and the same kernel is in ul10's payload).

The tail (`out\persistent\esp-7ccdd855-k273-ul10.json`, `boot_args_split.tail`, 400 bytes, `full_bytes` 1073):

    piano-gpu=2 piano-gpu-delay=20 piano-usb=2 piano-gpu-hang-ms=10000 piano-keyboard=2 piano-backlight=2 piano-backlight-keys=1
    piano-touch=2 piano-power=2 piano-power-policy=1 piano-power-rp=1 piano-thermal=1 piano-thermal-adc=1 piano-usb-shell=1
    piano-usb-net=1 piano-wifi=8 piano-wifi-iova=1 piano-audio=8 piano-thermal-adc-s=5 piano-video=6 piano-video-vt=1
    piano-usb-hid=1 piano-keyboard-gestures=1

`piano-keyboard-gestures=1`, dropped on ul9 to buy room, is back. The default entry is `macos`, seed `b2583ea517732505`, store bound to
`linux_data` LBA 117735999 - untouched by this and every earlier write. Written by `maint-session.py` steps
`maint-boot map esp-final check maint-exit`, log `logs\session-flash-ul10-20261128-101608.log`, `done`.

## The merge, on the tablet's own console

    piano-boot-args: /chosen/piano-boot-args-macos (401 bytes): 400 bytes appended, boot-args 1095 bytes (line 694)

so the limit that forced one driver to be traded for another since ul6 is gone: 1095 bytes in use out of 4095. `sysctl -n kern.bootargs`
displays 1023 bytes of the merged line (the sysctl string is capped), and `ioreg` still carries `piano-video=6`, which the boot log
confirms: `PianoVideo: start: piano-video=6 ...`, `PianoTouch: start: piano-touch=2 ...`, `PianoAudio: plan speaker (level 6 ...)` with
`piano-audio=8` in the tail, Wi-Fi at level 8, backlight, USB shell/net/HID and the five-second ADC round all in the same boot.

## Video, re-proved on ul10 (it had only ever been proved on ul6)

Continuous decode-and-present, `/var/tmp/play-check /var/tmp/piano-1080p-30s.mp4` (avc1 1920x1080 30.000 fps, 30.00 s):

    piano-play-check: RESULT file=/var/tmp/piano-1080p-30s.mp4 seconds=30.0 restarts=0 end=0 samples=29 late=0.76 not-ready=0

exit 0, device `Adreno (TM) 830`. ul6's same binary gave `samples=28 not-ready=0`, so nothing was lost by adding Wi-Fi, audio at level 8,
USB HID and the keyboard gestures back.

Single-frame hardware decode, `/var/tmp/out/native-tools/video-v6 --qualify /var/tmp/package/PianoVideo/tests/fixtures/v6`:

    video-v6: frame phase=single-regression ... sha256=bd230ec4a9cd22c274831d1b526c43d3ceb39a089257c3cb4114fbea0a8102dc latency-us=25388
    video-v6: frame phase=single-regression ... sha256=bd230ec4a9cd22c274831d1b526c43d3ceb39a089257c3cb4114fbea0a8102dc latency-us=9223
    video-v6: PASS single-golden-drain / PASS retained-surface-unchanged / PASS no-callback-after-invalidate
    video-v6: STOP first-failed-phase=single-regression failures=3 later-phases=not-run

The golden frame reproduces exactly (the same SHA-256 as on ul6), twice, in 9-25 ms. The three failures are the same three rejection-path
checks as in ul6's `v6.log` (`rejected-completion`, the third `reject-has-error-disposition`, `original-one-golden-callback`), so this is
unchanged behaviour rather than a regression: the decode path is good, the negative-path checks are not.

## Other things this boot says

- `machdep.piano_data_io`: `state=2 persist=2 lba=117735999 bound=117735999 generation=4 seed=b2583ea5 store_rd=44122/6168629us
  store_wr=12579/1789550us errors=0` - the Data store resumed on the same binding after four ESP generations.
- `machdep.piano_gpu_state`: `lvl=2 mhz=222 cap=13 cap_mhz=1050 why=demand bind=skinband skin_ok=1` - ul9's fix holds on ul10, the cap is
  13 rather than 1 and the idle 222 MHz is demand, not a ceiling.
- `machdep.piano_boot_guard`: `healthy; restart reason normal; health at 180 s ... 95 pings, longest gap 10.0 s`.
- `machdep.piano_audio_spk_state`: `starts 0 ... this PianoAudio has no pins for them (built without
  private/audio/spk-prot.bin and spk-re25.bin)` - the passthrough graph, nothing played yet in this boot.
- Wi-Fi: `piano-wifi-link` `level 8 step 311 link up gen 2`, `piano-wifi-scan-summary` `31 network(s)`, and `piano-wifi-level7`
  `step 701 waiting for the join profile`. Join is still open and is the next entry.

`NativeGopDisplay: swap_wait(2160, 0x80000001, 0) = 0xe00002d5` was recorded 23 times on ul9, with the counter reaching 16000 failures. ul10's capture
(`logs\ul10-boot.log`, 411 KB, boot through login and the video and audio work above) contains no `swap_wait` line at all. That is a
difference worth noting, not yet a verdict: the two captures differ in length, and the later `log` dump from the PianoDwc3 monitor on COM5
carries no NativeGopDisplay lines at all, so it cannot confirm the counter. This claim needs a capture of a UI-active session before it is
called fixed.