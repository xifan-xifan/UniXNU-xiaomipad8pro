# PianoSensors S6 persist reader

B13's S6 host deliverables are [PersistExt4Ro.h](PersistExt4Ro.h) and
[PersistExt4Verify.h](PersistExt4Verify.h). PianoSensors gets its daily file
tree from the RAM addon's pack or the ESP's `EFI/UniXNU/piano-sensors.pack`.
The file service serves the validated pack from memory. S6 runs in the host
tests. Any future runtime integration needs a separate opt-in design.

## Reader contract

Use a `persist_ext4::Reader` with a stable, immutable ext4 partition image
starting at byte zero. Supply the image length and a
`bool readBlocks(const void *ctx, uint64_t offset, size_t len, void *out)`
callback. The callback copies exactly `len` bytes into `out` and reports
failure on a short read. Offsets and lengths are in bytes and can be
unaligned. The reader has one 4 KiB block cache, uses fixed-size local
storage, and performs no allocation, mounting, journal reads or replay,
or filesystem writes. Serialize operations on a reader and obtain fresh
inode handles and directory cursors after reopening it.

`lookup` resolves a canonical absolute path with an explicit byte length.
`read` extracts an exact range of a regular file, including empty ranges
and reads across block boundaries. On error the caller discards the output,
which can contain a partial read. `next` enumerates a directory using a
cursor initialized to zero. Results distinguish missing paths, end of
directory, malformed data, unsupported formats, I/O failures, and bounds
on work. Symlinks and special files are refused by inode mode.

## On-disk subset and bounds

- The primary superblock starts at byte 1024. The reader checks the magic,
  revision, clean state, block and inode geometry, group counts, and
  descriptor layout. It accepts 1, 2 and 4 KiB blocks and power-of-two
  inode sizes from 128 bytes through one block. The contiguous primary
  group descriptor table uses 32-byte descriptors or 64-byte descriptors
  with the 64-bit feature. Inode tables can reside outside their own
  block group on flexible block group filesystems.
- Images marked as needing recovery, with a pending orphan cleanup, or
  with unsupported incompatibility or read-only compatibility flags are
  refused. This includes alternate descriptor layouts (`meta_bg`),
  bigalloc, inline data, encryption, and casefolding. Clean images may
  carry a journal. Extended attributes and allocation bitmaps are
  outside the read path.
- Extent trees use the inode root and up to four external levels. Each
  visited node is checked for magic, depth, capacity, ordering, logical
  ranges inherited from its parent, and physical bounds before binary
  search. No run in a visited node may overlap its ancestor blocks. Sparse holes and
  unwritten extents yield zeros. Without extent mapping, twelve direct
  pointers and the single-, double-, and triple-indirect maps are
  supported. A zero pointer means a hole. Repeated blocks on the selected
  indirection path are refused.
- Directories are scanned linearly using ext4 directory records. Indexed
  directories are traversed through their ordinary leaf records, skipping
  index records and checksum tails as the on-disk format permits. Record
  lengths are aligned and contained within their blocks. Invalid live
  names, out-of-range inode numbers, directory holes, ambiguous matches,
  and ancestor cycles encountered during lookup are errors. Lookup limits
  are 255 bytes per component, 4096 bytes per path, 32 components, and
  4 MiB scanned per directory.
- Little-endian fields are decoded from bytes. The declared filesystem
  fits the image before block numbers are scaled to byte offsets. The
  primary descriptor span, full inode table, extent runs, and pointer
  blocks fit before use. Every callback span is checked against the
  filesystem or, for the initial superblock, the image. File ranges are
  checked against EOF using subtraction to avoid overflow.
- With `metadata_csum`, the reader verifies CRC32C for the primary
  superblock and the group descriptors, Linux inodes, external extents,
  directory leaves, and htree nodes it reads. It supports UUID-derived and
  stored checksum seeds. With legacy `gdt_csum`, it verifies group descriptors
  using CRC16. Allocation ownership, bitmaps, and unvisited subtrees are
  outside the validation scope. [Linux ext4 checksums](https://www.kernel.org/doc/html/latest/filesystems/ext4/checksums.html),
  [superblock](https://www.kernel.org/doc/html/latest/filesystems/ext4/super.html),
  [group descriptors](https://www.kernel.org/doc/html/latest/filesystems/ext4/group_descr.html),
  [block maps](https://www.kernel.org/doc/html/latest/filesystems/ext4/ifork.html),
  and [directory records](https://www.kernel.org/doc/html/latest/filesystems/ext4/directory.html)
  describe the on-disk layouts.

## Registry equivalence

Call `verifyRegistry(reader, packBytes, packSize)` with an open image and an
in-memory pack. Link the host caller with [SensorsPack.c](SensorsPack.c).
The verifier uses the existing `SSCPACK1` parser and PianoSensors' 4 MiB
pack limit. It walks `/sensors/registry` in the image and maps its descendants
to `/mnt/vendor/persist/sensors/registry` in the pack, including the inner
`registry` tree and outer files such as `sns_reg_version`. Both sides must
contain the same nonempty set of regular-file paths, with identical sizes
and bytes. An empty file is a valid entry. An empty directory contributes
no files, matching how the pack producer derives directories from file
paths. Other pack content, such as vendor configuration and SoC metadata,
is outside the comparison.

The verifier checks for malformed packs, missing or extra files, repeated
matched paths, directory cycles and read errors. It compares in 4 KiB chunks
and bounds traversal to 8192 live entries and 16 MiB of directory data, with
the reader's depth limit and the pack's 255-byte path limit. Its result
contains only a status code.

The [host suite](tests/persist_ext4_ro_host.cpp) builds synthetic images in
memory and encodes expected pack entries independently of reader output. It
exercises block sizes, descriptor widths, high block addresses, extent depths,
legacy block maps, sparse content, indexed directories, byte equivalence,
checksum corruption, and malformed or truncated inputs under ASan and UBSan.
The PianoSensors host runner includes it and the CI runner also discovers
its inline build command.

```sh
python3 scripts/ci/self-tests.py --only persist_ext4_ro_host
python3 package/PianoSensors/tests/run-host-tests.py --only persist
python3 scripts/ci/self-tests.py --only PianoSensors
```
