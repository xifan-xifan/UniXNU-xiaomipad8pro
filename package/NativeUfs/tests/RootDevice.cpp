// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano 2026-11-25: RootDevice.h (the ufs-state line and native-ufs-wb) against the fault model (UfsModel.h): the query
// UPIUs it sends, Linux's flag order, a no-op without the switch, refusals and quarantine never fatal and never followed by more
// commands, and the line it logs.
#include "UfsModel.h"
#include "../RootDevice.h"

static std::string line (const UfsDeviceState &s) { char b[512]; hostDeviceStateLine (s, b, sizeof b); return b; }
static bool only (const std::string &prefix) { for (auto &q : m.queries) if (q.rfind (prefix, 0)) return false; return true; }

int main ()
{
  UfsDeviceState s;
  // 1. Without native-ufs-wb: ten reads, no write request at all, the device's flags unchanged.
  reset ();
  m.attrs[3] = 0x0f; m.attrs[0x1d] = 0x0a; m.attrs[0x1f] = 0x3000; m.attrs[0x1e] = 1; m.attrs[0x1c] = 3;
  hostDeviceState (false, &s);
  assert (m.queries.size () == 10 && only ("r"));
  assert (m.queries[0] == "r3:2" && m.queries[2] == "r5:e" && m.queries[3] == "r5:f" && m.queries[4] == "r5:10");
  assert (s.failed == 0 && s.value[0] == 0x11 && s.value[1] == 0x0f && s.value[2] == 0 && s.value[5] == 0x0a && s.value[6] == 0x3000);
  assert (m.flags[0x0e] == 0 && m.flags[0x0f] == 0 && m.flags[0x10] == 0);
  assert (line (s).find ("ufs-state: power 11 icc f wb 0 wb-flush 0 wb-flush-h8 0 wb-avail a wb-size 3000") == 0);
  assert (line (s).find ("native-ufs-wb absent") != std::string::npos);
  assert (!gHost.poisoned && m.syncs == 0 && m.events.empty ());   // no SCSI command, no link command
  // 2. native-ufs-wb=1 on a device with all three off: SET FLAG in Linux's order (0e, 10, 0f), each read back.
  reset ();
  hostDeviceState (true, &s);
  std::vector<std::string> tail (m.queries.begin () + 10, m.queries.end ());
  assert ((tail == std::vector<std::string> { "s6:e", "r5:e", "s6:10", "r5:10", "s6:f", "r5:f" }));
  assert (m.flags[0x0e] == 1 && m.flags[0x10] == 1 && m.flags[0x0f] == 1);
  assert (s.wbSets == 3 && s.wbSetFailures == 0 && s.wbAfter[0] == 1 && s.wbAfter[1] == 1 && s.wbAfter[2] == 1);
  assert (line (s).find ("native-ufs-wb=1: wb 0->1 wb-flush-h8 0->1 wb-flush 0->1, 3 sets, 0 failed") != std::string::npos);
  // 3. Already on (the boot firmware did it): nothing written.
  reset ();
  m.flags[0x0e] = m.flags[0x0f] = m.flags[0x10] = 1;
  hostDeviceState (true, &s);
  assert (m.queries.size () == 10 && only ("r") && s.wbSets == 0 && s.wbSetFailures == 0);
  assert (line (s).find ("wb 1->1 wb-flush-h8 1->1 wb-flush 1->1, 0 sets, 0 failed") != std::string::npos);
  // 4. A device without WriteBooster (the IDNs refused): reads recorded as failed, the sets refused, nothing fatal.
  reset ();
  m.queryRefused = (1ULL << 0x0e) | (1ULL << 0x0f) | (1ULL << 0x10) | (1ULL << 0x1d) | (1ULL << 0x1e) | (1ULL << 0x1f) | (1ULL << 0x1c);
  hostDeviceState (true, &s);
  assert (!gHost.poisoned && s.value[0] == 0x11 && s.value[2] == 0xffffffffU && (s.failed & 0x1fc) == 0x1fc);
  assert (s.wbSets == 3 && s.wbSetFailures == 3);
  assert (hostQueryRead (3, 2, &s.value[0]) == 0);   // the transport still works
  // 5. A flag that does not stick: counted as a failure, the boot goes on.
  reset ();
  m.flagSticky = true;
  hostDeviceState (true, &s);
  assert (!gHost.poisoned && s.wbSets == 3 && s.wbSetFailures == 3 && s.wbAfter[0] == 0);
  // 6. A malformed query answer quarantines the transport (as every command), and nothing more is sent after it.
  reset (Fault::Query);
  hostDeviceState (true, &s);
  assert (gHost.poisoned && s.stoppedPoisoned && m.queries.size () == 1 && s.failed == 0x3ff);
  assert (line (s).find ("STOPPED: transport quarantined") != std::string::npos);
  assertQuarantined ();
  // 7. The read entry point is unchanged: write opcodes refused, index 0.
  reset ();
  uint32_t v = 0;
  assert (hostQueryRead (6, 0x0e, &v) == kIOReturnBadArgument && m.queries.empty ());
  assert (hostQuery (4, 0x03, 0, &v) == kIOReturnBadArgument && m.queries.empty ());   // WRITE ATTRIBUTE is not offered
  printf ("RootDevice: ufs-state reads, native-ufs-wb in Linux's order, already-on no-op, refusals, sticky flag, quarantine stop: ok\n");
  printf ("  sample: NativeUfs: %s\n", line (s).c_str ());
  return 0;
}
