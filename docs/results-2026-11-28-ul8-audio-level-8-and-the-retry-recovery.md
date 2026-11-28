# 2026-11-28 ul8: audio level 8 streams to the ADSP, and the retry-count recovery run end to end

## The pair that is installed now

`boot_a` = `boot-piano-macos-persistent-v6-sealed.img` (`df6d5a18f27054e6…`), ESP = `esp-7ccdd855-k273-ul8.img`
(`2cb111ea817c96263c4314c297bdea3d29ce0b0d30d86b45b3ef56060c902705`), which is ul7 with one value edited: `piano-audio=2` to `piano-audio=8` (same 14
bytes, so the 981-byte line is unchanged). Written by `maint-session.py` with steps `maint-boot map boot-a esp-final check maint-exit`
System.Object[]

System.Object[]

After ul6 and ul7 `slot-retry-count:a` read **0**, and ABL refused the maintenance image:
`Booting FAILED (remote: 'Failed to load/authenticate boot image: Load Error')` with `slot-unbootable:a = no`. The recovery is a normal
System.Object[]

    slot-successful:a: yes    slot-retry-count:a: 6    slot-unbootable:a: no

That Android boot is evidence in its own right: `ro.build.version.incremental = OS3.0.309.0.WPYCNXM`, `/data` on `dm-56`, and
`/data/local/tmp/piano-rt1.txt` from the earlier round trip read back verbatim. Writing the ESP afterwards needed boot_a first
System.Object[]

System.Object[]

The kext reached the ADSP on the persistent boot: "piano-audio-dsp = "APM ready (status 1) after 1 queries from port 0x1 ….
System.Object[]
System.Object[]

    level 8: speaker down (3 starts, 1983 periods, 723 with sound, 2 underruns, 59 slips), mic down (0 starts, 0 periods); frames offered 839760, taken 676096, dropped 163664
System.Object[]

System.Object[]
System.Object[]

System.Object[]
System.Object[]
System.Object[]

System.Object[]

System.Object[]

With `piano-usb-hid=1` gone the ports re-numbered: COM5 is the kernel console and its shell, and the user pipe is on COM6 as a plain
root shell (`root@Xiaomi-Pad-8-Pro ~ #`). `tooling/native/piano-shell.ps1` probes with a userland command and can report the kernel shell's
`unknown command` instead, so `tmp-c6.ps1` (raw COM6 with DTR/RTS set) is the reliable path when in doubt. `sysctl -a` and `sysctl -A` hang
System.Object[]

