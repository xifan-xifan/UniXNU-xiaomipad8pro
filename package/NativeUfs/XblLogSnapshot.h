// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included in NativeUfs's root transport namespace after RootLayout.h.
static constexpr uint32_t kXblBlock = 4096, kXblBlocks = 32;
static constexpr uint32_t kXblBytes = kXblBlock * kXblBlocks;
static uint8_t *gXblSnapshot;
static struct sysctl_oid gXblOid;

static int xblSnapshotSysctl (struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
  if (req->newptr || !kauth_cred_issuser (kauth_cred_get ())) return EPERM;
  return SYSCTL_OUT (req, gXblSnapshot, kXblBytes);
}

// One boot-time scan of the usual six user LUNs. An absent/non-GPT LUN is skipped.
// A GPT with a recognizable signature must pass validation before any payload read.
static const char *xblFindPartition (uint8_t *scratch, uint8_t &foundLun, uint64_t &foundLba)
{
  constexpr char name[] = "xbl_sc_logs";
  bool found = false;
  for (uint8_t lun = 0; lun < 6; ++lun) {
    if (hostRead4k (lun, 1, 1, scratch) != kIOReturnSuccess) {
      if (!gHost.ready || lun == 0) return "GPT-read";
      continue;
    }
    if (memcmp (scratch, "EFI PART", 8)) continue;
    const uint32_t headerBytes = le32 (scratch + 12);
    if (le32 (scratch + 8) != 0x00010000 || headerBytes < 92 || headerBytes > kXblBlock ||
        le32 (scratch + 20) || le64 (scratch + 24) != 1 ||
        rootCrc32 (scratch, headerBytes, true) != le32 (scratch + 16)) return "GPT-header";

    const uint64_t backup = le64 (scratch + 32), first = le64 (scratch + 40), last = le64 (scratch + 48);
    const uint64_t entries = le64 (scratch + 72);
    const uint32_t count = le32 (scratch + 80), entriesCrc = le32 (scratch + 88);
    constexpr uint32_t entrySize = 128;
    if (le32 (scratch + 84) != entrySize || !count || count > 512 || entries < 2 || entries > 64)
      return "GPT-entries-shape";
    const uint32_t blocks = (count * entrySize + kXblBlock - 1) / kXblBlock;
    if (first < entries + blocks || last < first || backup <= last || backup > UINT32_MAX || backup - last <= blocks)
      return "GPT-usable-range";
    // READ CAPACITY(10) establishes the logical block size and physical bound.
    const uint8_t capacityCmd[10] = {0x25};
    uint32_t scsi = 0;
    if (hostScsi (lun, capacityCmd, sizeof capacityCmd, true, 8, 500000, &scsi) != kIOReturnSuccess)
      return "READ-CAPACITY";
    const uint8_t *capacity = ufsSlot (0).data;
    if (getBe32 (capacity + 4) != kXblBlock || getBe32 (capacity) != backup) return "GPT-capacity";

    if (hostRead4k (lun, entries, blocks, scratch) != kIOReturnSuccess ||
        rootCrc32 (scratch, count * entrySize) != entriesCrc) return "GPT-array";
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t *entry = scratch + i * entrySize;
      if (!rootEntryUsed (entry)) continue;
      const uint64_t lo = le64 (entry + 32), hi = le64 (entry + 40);
      if (lo < first || hi < lo || hi > last) return "GPT-entry-bounds";
      bool match = true;
      for (unsigned c = 0; c < sizeof name; ++c) {
        if (entry[56 + 2 * c] != name[c] || entry[57 + 2 * c]) { match = false; break; }
      }
      if (!match) continue;
      if (found || hi - lo + 1 != kXblBlocks) return "GPT-xbl-duplicate-or-size";
      for (uint32_t j = 0; j < count; ++j) {
        const uint8_t *other = scratch + j * entrySize;
        if (i != j && rootEntryUsed (other) && lo <= le64 (other + 40) && le64 (other + 32) <= hi)
          return "GPT-xbl-overlap";
      }
      foundLun = lun;
      foundLba = lo;
      found = true;
    }
  }
  return found ? nullptr : "GPT-xbl-missing";
}

static void captureXblLog ()
{
  uint32_t enabled = 0;
  if (!PE_parse_boot_argn ("native-ufs-xbl-snapshot", &enabled, sizeof enabled) || enabled != 1 || gXblSnapshot) return;
  uint8_t *snapshot = static_cast<uint8_t *> (IOMalloc (kXblBytes));
  uint8_t lun = 0;
  uint64_t lba = 0;
  const char *failure = snapshot ? xblFindPartition (snapshot, lun, lba) : "no-memory";
  // Only READ(10) of the verified, fixed-name extent. The OID serves the immutable RAM copy.
  if (!failure && hostRead4k (lun, lba, kXblBlocks, snapshot) != kIOReturnSuccess) failure = "payload-read";
  if (failure) {
    IOLog ("NativeUfs: xbl-snapshot-v2: failed reason=%s\n", failure);
    if (snapshot) IOFree (snapshot, kXblBytes);
    return;
  }
  gXblSnapshot = snapshot;
  gXblOid.oid_parent = &sysctl__machdep_children;
  gXblOid.oid_number = OID_AUTO;
  gXblOid.oid_kind = CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2;
  gXblOid.oid_name = "native_xbl_sc_logs";
  gXblOid.oid_handler = xblSnapshotSysctl;
  gXblOid.oid_fmt = "S,xbl_sc_logs";
  gXblOid.oid_version = SYSCTL_OID_VERSION;
  sysctl_register_oid (&gXblOid);

  // Log just the identity of the snapshot. The payload stays in the root-only OID.
  uint64_t ns = 0;
  absolutetime_to_nanoseconds (mach_absolute_time (), &ns);
  const uint32_t crc = rootCrc32 (snapshot, kXblBytes);
  IOLog ("NativeUfs: xbl-snapshot-v2: ready uptime_ms=%llu lun=%u bytes=%u crc=%08x\n", ns / 1000000, lun, kXblBytes, crc);
}

static void releaseXblLog ()
{
  if (!gXblSnapshot) return;
  sysctl_unregister_oid (&gXblOid);
  IOFree (gXblSnapshot, kXblBytes);
  gXblSnapshot = nullptr;
}
