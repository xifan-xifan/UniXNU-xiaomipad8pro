# 2026-11-27 (night, second boot): the Data store resumes, and the file written last boot is still there

Second `fastboot boot` of this seed, with `data-store-replace` left out so the store that the previous boot created had to be
found again. Nothing was written to any partition; the image reset itself into ABL and the reader brought HyperOS back.

| Item | Value |
| --- | --- |
| Image | `boot-piano-macos-ram-A-bundled.img`, 205,824,000 bytes, SHA-256 `0e3447ed3fa6c0f35f11b2d0aa3edd4bcea1050ee5b8d556ac7398ff4865af2f` |
| Recipe | `tooling/k273/recipes/a830-shot.sh` with `SHOT_REPLACE=keep` (Mac `out/build-a830-shot3-8748.log`) |
| Boot | `fastboot boot` at 22:26:28, `piano-reset-after=900`, `piano-reset-to=bootloader` |
| Run | `logs/native-runs/ram-k273-a830read-20261127-222607` |
| Console | `logs/usb-console/a830read-20261127-222603-com.log` (`capture-com.ps1 -ExcludePorts COM6`, so port 1 stayed free for the shell) |
| Previous boot | `dc54552255da0eae50ba5768c5767549d4dbd882ebd44925f2c375b28b29113f`, which wrote the files being read back here |

## The store resumed

```
NativeRamBlock: data-store-lba=117735999: window at LBA 117735999, store bound to it
NativeRamBlock: Data store state 2 (1 blank, 2 resumed, 3 fresh, 4 failed, 7 kept), header 0, first LBA 117735999,
  6291456 pages, direct 1, seed bb6dd1c3, store seed bb6dd1c3, flags 2, generation 3, present 429589
```

The previous boot reported `state 3` (it built the store); this one reports `state 2` with the same seed `bb6dd1c3` and the
same `store seed`. From the kernel side that is a resume, not a rebuild.

`machdep.piano_data_io` at the end of the boot, with the store doing real work:

```
persist=2 state=2 lba=117735999 pages=6291456 bound=117735999 generation=3 flags=2
seed=bb6dd1c3 store_seed=bb6dd1c3 errors=0 store_state=2
store_rd=21946/2111174us store_wr=9403/1110864us store_sync=0/0us store_max=14331us
write_back=0 dirty=0 fua=0 cached=0 flushes=0 barriers=0
```

Data mounts as a real APFS volume (`/dev/disk1s1 on /System/Volumes/Data (apfs, local, journaled)`), root is
`/dev/disk3s1 on / (apfs, local, read-only, union)`. `sw_vers` is 27.3.

## The read-back

Written during the previous boot, read here:

```
# cat /private/var/db/piano-persist.txt
piano-20261127-a830shot
# ls -l /private/var/db/piano-persist.txt
-rw-r--r--  1 root  wheel  24 Nov 24  2026 /private/var/db/piano-persist.txt
```

**A file written through the mounted Data volume during one boot is present after the machine reset itself and booted
again.** That is the write-and-read-back gate, first time.

`/private/tmp` came back empty (`total 0`), so the PNGs from the previous boot are gone: `/private/tmp` is cleared per boot
and is not a persistence target. Everything meant to be kept goes under `/private/var`.

Left on the store for the next boot to read back:

| Path | Content |
| --- | --- |
| `/private/var/db/piano-persist2.txt` | `boot2-20261127-222644` |
| `/private/var/db/piano-persist.d/nested.txt` | `nested-boot2` |
| `/private/var/db/piano-persist.d/blob.bin` | 1 MiB from `/dev/urandom`, SHA-256 `b3bfdacf44bb15ea8d223f9d93e9acc847c78c35c2dd30e357582492dafd2e21` |
| `/private/var/db/piano-persist.d/blob.sha` | the line above, so the next boot can `shasum -c` it |

## What the panel is showing, settled

`gop-shot` this boot reports `source=flip format=#BGA layout=8-bit`. The previous boot reported `source=boot`. Under
`piano-dpu=2 piano-dpu-flip=4` the capture now reads the surface that was flipped to the DPU rather than the GOP mapping,
which is what native-gop-display.md section 9 expects and closes the question left open by the first still.

## Two ways the USB shell was lying, both fixed

`tooling/native/piano-shell.ps1` sent its sentinel as a literal backtick before the question mark, so zsh stayed inside a
command substitution and ate the next command: the session looked alive and answered nothing, and the answer only appeared
once another line happened to close the quote. It now sends Ctrl-C and a newline first and drops the backtick.

The echoed command line itself contains the sentinel, so a reply that arrived a chunk later than the echo was reported as
the whole answer, which is what the empty `ls -lR` output had been. The helper now waits for the sentinel twice.

`capture-com.ps1` opened every serial port it could find, including the user pipe, so the shell could not be reached while a
capture was running; `-ExcludePorts` keeps COM5 and leaves COM6 alone. This is the combination that made this boot workable.

## Not established by this boot

No input device (this recipe carries no `piano-touch`, `piano-keyboard` or `piano-backlight`), no cold start, no persistent
boot path, no hard-reset recovery, no Wi-Fi, audio or brightness checks. The user pipe stopped answering at about nine
minutes in, during a `diskutil list`; the console kept printing to the reset. `diskutil list` is off the list of things to
run over this pipe.