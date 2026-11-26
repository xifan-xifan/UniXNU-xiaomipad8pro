// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano 2026-11-25 (disk-speed lane): the UFS device's own state after the bring-up, and WriteBooster turned on as Linux
// does it. Runs once in startRoot after the bring-up, single-threaded, before any window is published (the place of
// captureXblLog and the perf link line): its queries use slot 0 with nothing else in flight (RootCommands.h hostQuery).
//
// Reference (behaviour, reimplemented): Linux drivers/ufs/core/ufshcd.c ufshcd_configure_wb(), called by ufshcd_probe_hba() on
// every probe and every reset when the host allows WriteBooster (ufs-qcom sets UFSHCD_CAP_WB_EN): SET FLAG fWriteBoosterEn
// (IDN 0x0E), then fWBBufferFlushDuringHibernate (0x10), then fWriteBoosterBufferFlushEn (0x0F). Index 0: this unit's device
// (KIOXIA THGJFLT2G46BATPB, UFS 4.10) has a shared WriteBooster buffer (Android 20261120 root capture: wb_type 0x01,
// wb_shared_alloc_units 0x3000; there wb_on 1, wb_flush_en 1, wb_flush_during_h8 1, wb_avail_buf 0x0A). NativeUfs never set
// these flags before; whether the boot firmware does is what the state line answers (`ufs-state:`).
//
//   native-ufs-wb=1   set the three flags (each only when it reads 0), read them back. Absent: read only, as before.
//
// Nothing here is fatal for the boot: a query the device answers with a nonzero response code (an IDN it lacks, a flag it
// refuses) is recorded and the next one runs; a malformed answer quarantines the transport exactly as any other command would,
// and then nothing more is asked.
#pragma once

struct UfsDeviceState {
  // the values (0xffffffff: not read); `failed` bit i: read i of kStateReads failed
  uint32_t value[10];
  uint32_t failed = 0;
  bool wbRequested = false;
  uint32_t wbBefore[3] = { 0xffffffffU, 0xffffffffU, 0xffffffffU }, wbAfter[3] = { 0xffffffffU, 0xffffffffU, 0xffffffffU };
  unsigned wbSets = 0, wbSetFailures = 0;
  bool stoppedPoisoned = false;
};

struct UfsStateRead { uint8_t opcode, idn; const char *name; };
// 0x03 READ ATTRIBUTE, 0x05 READ FLAG (JESD220 IDNs, the names Linux's ufs.h gives them)
static constexpr UfsStateRead kStateReads[10] = {
  { 0x03, 0x02, "power" },          // bCurrentPowerMode (0x11 active)
  { 0x03, 0x03, "icc" },            // bActiveICCLevel (Linux sets it on every probe from the power descriptor)
  { 0x05, 0x0e, "wb" },             // fWriteBoosterEn
  { 0x05, 0x0f, "wb-flush" },       // fWriteBoosterBufferFlushEn
  { 0x05, 0x10, "wb-flush-h8" },    // fWBBufferFlushDuringHibernate
  { 0x03, 0x1d, "wb-avail" },       // bAvailableWriteBoosterBufferSize (0x0A = 100 %)
  { 0x03, 0x1f, "wb-size" },        // dCurrentWriteBoosterBufferSize (allocation units)
  { 0x03, 0x1e, "wb-life" },        // bWriteBoosterBufferLifeTimeEst
  { 0x03, 0x1c, "wb-flush-status" },// bWriteBoosterBufferFlushStatus
  { 0x03, 0x05, "bkops" },          // bBackgroundOpStatus
};
// Linux's order: enable, flush during Hibern8, flush. Their places in kStateReads.
static constexpr uint8_t kWbFlags[3] = { 0x0e, 0x10, 0x0f };
static constexpr unsigned kWbRead[3] = { 2, 4, 3 };

static void hostDeviceState (bool enableWb, UfsDeviceState *s)
{
  *s = UfsDeviceState {};
  s->wbRequested = enableWb;
  for (unsigned i = 0; i < 10; ++i) {
    s->value[i] = 0xffffffffU;
    if (gHost.poisoned) { s->stoppedPoisoned = true; s->failed |= 1U << i; continue; }
    uint32_t v = 0;
    if (hostQuery (kStateReads[i].opcode, kStateReads[i].idn, 0, &v) == kIOReturnSuccess) s->value[i] = v;
    else s->failed |= 1U << i;
  }
  for (unsigned k = 0; k < 3; ++k) s->wbBefore[k] = s->wbAfter[k] = s->value[kWbRead[k]];
  if (!enableWb) return;
  for (unsigned k = 0; k < 3; ++k) {
    if (gHost.poisoned) { s->stoppedPoisoned = true; return; }
    if (s->wbBefore[k] == 1) continue;                    // already on: nothing to write
    uint32_t v = 0;
    ++s->wbSets;
    if (hostQuery (0x06, kWbFlags[k], 0, &v) != kIOReturnSuccess) { ++s->wbSetFailures; continue; }
    if (gHost.poisoned) { s->stoppedPoisoned = true; return; }
    uint32_t back = 0xffffffffU;
    if (hostQuery (0x05, kWbFlags[k], 0, &back) == kIOReturnSuccess) s->wbAfter[k] = back;
    if (s->wbAfter[k] != 1) ++s->wbSetFailures;
  }
}

static int hostDeviceStateLine (const UfsDeviceState &s, char *line, size_t size)
{
  int n = snprintf (line, size, "ufs-state: power %x icc %x wb %x wb-flush %x wb-flush-h8 %x wb-avail %x wb-size %x wb-life %x "
                    "wb-flush-status %x bkops %x (ffffffff: not read; failed reads %03x)",
                    s.value[0], s.value[1], s.value[2], s.value[3], s.value[4], s.value[5], s.value[6], s.value[7], s.value[8],
                    s.value[9], s.failed);
  if (n < 0 || size_t (n) >= size) return n;
  if (!s.wbRequested)
    return n + snprintf (line + n, size - n, "; native-ufs-wb absent: WriteBooster flags left as the boot firmware set them%s",
                         s.stoppedPoisoned ? "; STOPPED: transport quarantined" : "");
  return n + snprintf (line + n, size - n, "; native-ufs-wb=1: wb %x->%x wb-flush-h8 %x->%x wb-flush %x->%x, %u set%s, %u failed%s",
                       s.wbBefore[0], s.wbAfter[0], s.wbBefore[1], s.wbAfter[1], s.wbBefore[2], s.wbAfter[2], s.wbSets,
                       s.wbSets == 1 ? "" : "s", s.wbSetFailures, s.stoppedPoisoned ? "; STOPPED: transport quarantined" : "");
}
