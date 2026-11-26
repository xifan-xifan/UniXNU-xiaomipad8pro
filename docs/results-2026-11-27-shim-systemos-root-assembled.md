# The shimmed SystemOS root, signed and assembled on two hosts

On November 27, 2026 the root image that answers `mount-phase-1` was built, signed, and readied for `linux_rootfs`. Nothing
here was run on the tablet: the write stopped at the host firewall (last section).

## The gap in the plan was smaller than it looked

`skills/install-piano-macos/SKILL.zh-CN.md` lists the SystemOS boot shims as gap G1b and says to rewrite them in this repository.
That rewrite is not needed. The k27 tree on this Mac (`PIANO_K27_TREE=/Users/zhangqi/Codex/k27-tree`) carries them in its own
history, in the upstream author's commit `abcbc18`:

| file | bytes | body |
|---|---|---|
| `overlay/systemos-shims/sbin/fsck` | 17 | `#!/bin/sh` then `exit 0` |
| `overlay/systemos-shims/sbin/mount` | 358 | `[ "$1" = "-P" ] && exit 0`, everything else `exec /sbin/mount.real "$@"` |

`git status` of that tree is clean under `overlay/`, so these are the tree's files and not something a session wrote.

`tooling/k273/make-systemos.sh` needs only that directory plus the three IPSW images, and its output already existed on this Mac
from 2026-11-26 06:17: `out/inputs/k273/root/systemos.img`, 6,442,450,944 bytes, SHA-256 `1947ac49e4c86186856a6b233c5b690fd0e21c98beb7571825c67b519952dcc3`,
volume UUID `A8D7C405-7BF1-4ACA-94D0-5DE65E3A7D2B` (`systemos.img.json`). Attached read-only and listed, its `/sbin` holds exactly
the three files above, so the union mount the kernel makes over `/` will find a `mount` that answers the `-P` job instead of
falling through to a tool that is not there.

## Signing both windows with one owner key

`out/inputs/k273/26C5035r/` already held the decrypted 27.3 members (`ipsw-members.py` + `decrypt-aea.py`), so no OS disk
download was needed: `141-22233-021.dmg` (14,493,417,472 B, `40d8282e…`, the pinned OS window), `141-22145-021.dmg`
(2,917,138,432 B, `14330ed9…`, the Apple cryptex now on the device) and `141-22106-021.dmg` (54,525,952 B, the App cryptex).

`UniXNU/scripts/make-root-policies.py` (from `UNIXNU_RESEARCH`) signed two windows as one set:

```
--window OS=out/inputs/k273/26C5035r/141-22233-021.dmg:84581E99-85BE-4957-AC53-2947E887B3E4
--window SystemOS=out/inputs/k273/root/systemos.img:A8D7C405-7BF1-4ACA-94D0-5DE65E3A7D2B
```

It needed OpenSSL 3 headers, which this Mac did not
have, so `brew install openssl@3` ran first. The owner key pair is made once under the tree passed as `--tree`
(`out/vr-shim/build/verified-root-host/keys`, never committed); the anchor it produced is
`9cfab48d11b63c91a9c16a1972d31b45bfc657afbbd151a8c81ad7513799f419`, and both `verification.json` records say
`real_c_signature_and_block_verification: true`: OS 3,538,432 blocks, SystemOS 1,572,864 blocks. The earlier anchor
`896b3246…` belongs to the archived upstream-signed stock policies and its private key is not on this machine; nothing outside
our own rebuilt NativeUfs pins an anchor, so a new key for the set is consistent (the kernel's `verified_root_key` is the
`trusted-key.bin` compiled into the kext by `tooling/k27/make-root-policy-source.py`).

The OS table reproduced the known-good bytes: `OS.vroot` is 113,229,824 bytes with SHA-256
`f10b77a0b2f0475c296a66c69ec3c1d26598dbca3e6b09751a0b1cc31af533b0`, the same value recorded for the stock root on 2026-11-21.
`SystemOS.vroot` is 50,331,648 bytes, SHA-256 `3721e1d9298490296aeff14fda252d601de5940a4aae5a0392a68357cb842fb2`.

## The manifest, and two hosts agreeing byte for byte

`findRootLayout` (`package/NativeUfs/RootLayout.h`) reads a `WINDOW.vroot` hash table only from an entry whose block size is 4096
and flags are 3, which makes the manifest version 2, and it indexes objects after windows. `tooling/native/make-root-manifest.py`
could only emit windows, so it now takes `--object NAME=IMAGE` and writes version 2 with the windows first;
`tooling/native/test-root-manifest.py` builds both shapes and reads them back with those rules (15 checks, 0 failures), and
`tooling/k273/make-root-image.sh` copies from `layout['entries']` so objects reach the image.

The image, assembled independently on each host:

| | Mac (`out/root-shim/root.img`) | Windows (`out/root-shim/piano-shim-root-26C5035r.img`) |
|---|---|---|
| OS window | from `26C5035r/141-22233-021.dmg` | sliced out of `out/piano-stock-root-26C5035r.img` at offset `0x100000`, after the whole 17,547,919,360-byte image hashed to the `db3629e5…` the device read back |
| SystemOS window | `out/inputs/k273/root/systemos.img` | that file, fetched (6,442,450,944 B) |
| tables | `out/root-policies-shim/*.vroot` | fetched (113,229,824 + 50,331,648 B) |
| manifest | `c16d365e01e5c9d7bce1a8f42063086f50c215dd2e6b893e44291899f1087aa2` | recomputed here: same |
| image | 21,100,494,848 B `9e4ef50579b3ee5d443431d76434360ee9b0b02e8dbe443d3422e98532a78356` | same |

Windows layout: OS `0x100000`, SystemOS `0x35ff00000`, `OS.vroot` `0x4dff00000`, `SystemOS.vroot` `0x4e6b00000`. Against this
unit's `linux_rootfs` (4 KiB LBAs 102269696..107512575, 5,242,880 blocks) the write is 5,151,488 blocks, ending at 107421183,
with 91,392 blocks of the partition unused. The ESP (`68c28df1…`) and the Data store window (117735999) are outside it.

`out/build-verified-8748-shim.sh` on the Mac built the RAM image against these policies:
`boot-piano-macos-ram-A-bundled.img` 159,686,656 B, SHA-256 `6001bc1c06559e1f1434631c8824dedb744e3c47511da1b7b8b67399a449c70b`,
with the ADT's SystemOS UUID taken from the new `root.json` (`A8D7C405-7BF1-4ACA-94D0-5DE65E3A7D2B`). Its UEFI side is the
unchanged `eb570025859ee90f3b3fb96d3b41414ceda5bf34747a9e94a1ad6dca57e1d128`. Everything else about it matches
`out/build-verified-8748.sh` (the run that opened the owner Data store), so the root is the only difference.

## Three host-side blockers, all in code

The write step is `tooling/persist/maint-session.py`'s `linux-data-root --root-part linux_rootfs`, and three things stood between
this checkout and running it:

1. it imported `fcntl` at module scope, so nothing could even start on the machine holding the tablet (`ca3e3a53`);
2. its `linux_rootfs` gate offered only "cleared after a verified backup" or "block 0 is zero", and `rootfs-clear` itself refuses
   on this unit because it requires that no ESP entry uses `linux_rootfs` - true of the reference machine, where that partition
   held the retired 27.0.1 system, and false here by design (`03bae942`);
3. it launched `maint-shell.py` by the name `python3`, which a Windows checkout does not have (`234255b6`).

The gate now also accepts: the medium, read with its caches dropped, equals a file the host holds over its whole length, and
where the new image is longer the remaining blocks read zero. On this unit that held file is
`out/piano-stock-root-26C5035r.img` (`db3629e5…`), the exact bytes read back from the partition, so every replaced byte is one
the host can stream straight back and nothing else on the partition is displaced.

## The write itself: stopped by the host firewall, not by the tablet

The session reached the tablet and passed `maint-boot` (image `54001a50…` pinned; `piano-maint idle 3600` answered; the image's
`/etc/piano-network` says `host_ip=192.168.88.10`, which is this host's USB-link address, so the fetch check passed). It then
stopped installing `tablet.sh`:

```
tablet# wget -q -O /run/pp/tablet.sh http://192.168.88.10:8000/tablet.sh
wget: can't connect to remote host (192.168.88.10): Connection timed out
```

The USB link is up (the NCM adapter, `以太网 3`, took a lease of 192.168.88.10 from the tablet, which is at 192.168.88.2 and serves
its shell on 2323). `tooling/native/probe-usb-net.py` asks directly and got the same answer with a bare `http.server`: the tablet's
`wget` times out and the server logs no connection at all. A timeout rather than a refusal, on a machine whose Windows Firewall
is on for all three profiles with the USB link classified as Public, and with no application rule for python anywhere in the
ruleset, is the firewall dropping the inbound connection. Nothing was written: the session stopped before any `restore`, and the
partition was not touched.

Clearing it needs one elevated command on this host, which no tool here can grant:

```
netsh advfirewall firewall add rule name="piano-maint-usb-link" dir=in action=allow protocol=TCP localport=8000-9000 remoteip=192.168.88.0/24
```

Ports 8000 (the tablet's `wget`) and 9000 (the export/restore streams) are the session's own, and the scope is the USB link's
subnet only. The same command with `delete rule` instead of `add rule ... enable=yes` undoes it.

## The session to run, once inbound on the link is allowed

Nothing else about it is special: the tablet is driven from ABL, and the session boots the maintenance image itself.

```
set PIANO_SERIAL=7ccdd855
set PIANO_MAINT_HOST=192.168.88.2
set PIANO_V110=E:\project\UniXNU-xiaomipad8pro\out\inputs\maintenance-ubuntu
set PIANO_UNIT_PROFILE=E:\project\UniXNU-xiaomipad8pro\out\unit-profile\piano-7ccdd855.json
set PIANO_BACKUP_DIR=E:\piano-backups
set PATH=E:\project\UniXNU-xiaomipad8pro\out\pt33\platform-tools;%PATH%
python tooling\persist\maint-session.py --session out\persistent\session-shim-root-20261127c `
  maint-boot map linux-data-root check maint-exit `
  --root-part linux_rootfs --root-image out\root-shim\piano-shim-root-26C5035r.img `
  --root-sha256 9e4ef50579b3ee5d443431d76434360ee9b0b02e8dbe443d3422e98532a78356 `
  --root-overwrite-holds out\piano-stock-root-26C5035r.img
```

Before any write it must print, from the medium: the 4,284,160 blocks of `db3629e5…` it replaces and the 867,328 further blocks
reading zero. After the write it re-reads all 5,151,488 blocks and compares them to `9e4ef505…`. The `check` step afterwards may
still complain that the store on the tablet is of seed `8e1a87e1` while the installed ESP resumes `7e9bd39b`; that is the known
deferred item (the ESP has to be rebuilt with this root's SystemOS UUID and this seed), and it does not affect the write.
`PIANO_MAINT_HOST` is what the tablet offers; re-discover it if the link redials (`tooling/native/probe-usb-net.py 192.168.88.10`).

## Not established

No device boot of this root, no `NativeUfs: published SystemOS`, nothing about `mount-phase-1`, a desktop, GPU, input, network,
audio, Data persistence, Android recovery or either direction of the switch. The signing and the two-host assembly are host
results; the shims' effect is inferred from the upstream reference machine using the same volume, and is exactly what the next
RAM run is meant to test.