# KC data layout, symbol stripping, and the readback channel (2026-11-25)

Host-only analysis plus two unattended `fastboot boot` runs of
`boot-piano-ufs264cfg.img` (`d74fad3a...`). No partition, GPT, boot image or
userdata write was issued; the tablet returned to HyperOS
OS3.0.309.0.WPYCNXM `_a` on its own after every run.

## The stage table address did not move between ufs256 and ufs264

The RAM boot images embed a static `MH_EXECUTE` kernel at file offset
`0x4c2000`, not the collection that the ESP carries. Parsing both headers:

| Image | __DATA vmaddr | fileoff | filesize |
|---|---|---|---|
| ufs256 | `0xfffffff00c848000` | `0x5844000` | `0x4f8000` |
| ufs264 | `0xfffffff00c848000` | `0x5844000` | `0x4f8000` |

Every top-level segment has the same vmaddr and filesize in both images, and
the two `__DATA` blobs are **byte-identical** (5,210,112 bytes; 1,536,632
non-zero bytes each). ufs264 only changed instructions in `__TEXT_EXEC`
(the PPI 27 branch), so it cannot have shifted `_piano_stage_slots`. The
ufs256-derived VA `0xfffffff00c912880` (`__DATA` offset `0xca880`) remains the
correct static address for this family; the earlier "address drifted" premise
is wrong.

## The KC keeps symbol names but not symbol entries

`piano-ufs264.kc` (`529265c7...`, 147,095,552 bytes) has 92 load commands and
81 fileset entries. The name `_piano_stage_slots` is present in two sorted
local-symbol string tables inside `__LINKEDIT` (at `0x7c6fb09` and
`0x8734509`, e.g. between `_piano_split_pages` and `_piano_tr`), but the
`LC_SYMTAB` of every `com.unixnu.*` entry is `(symoff, 0, stroff, 0)`: the
`nlist` array is stripped. Names alone do not give addresses.

`__TEXT_EXEC` is not empty. Its first bytes are `5f2403d5 00008052 c0035fd6`
(`paciasp; mov w0,#0; ret`), and roughly 90% of the first megabytes are
non-zero. An ADRP+ADD sweep of `__TEXT`, `__PRELINK_TEXT` and `__TEXT_EXEC`
finds 8,478 distinct `__DATA` targets, so the earlier "0 references" result
was a parsing bug, not absent code.

## `oem dump` is failing at the transport for small requests too

Three requests were made, each as the *first* dump of a fresh reader boot:

| Request | Result |
|---|---|
| `8c5844000 4f8000` (whole `__DATA`) | `FAILED (Status read failed (Too many links))` |
| `8c0004000 1000` (kernel `__TEXT` base) | `FAILED (Status read failed (Too many links))` |
| `8c5912880 2000` (earlier run) | succeeded, 8,192 bytes |

So the failure is not a plain size limit. Either the transport is flaky after
the 37 s RAM run, or the reader refuses these ranges. Until a 4 KiB dump at a
range already known to read back succeeds, treat `Too many links` as
uncalibrated rather than as evidence about the address.

## Next measurement

Read back 4 KiB at `0x8c5912880` again as a control; if that now fails the
channel itself is degraded. Then dump 64 bytes at `0x8c0004000` and compare
with the static header. A match fixes the slide at zero and makes the stage
table decodable; a mismatch means the runtime image is slid and every PA
derived from the link address must be recomputed.
