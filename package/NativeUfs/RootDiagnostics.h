// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AtomicField.h"

// Passive snapshot formatter; caller supplies integer types and snprintf.
// No hardware access or decision about trust, transport or publication here.
struct UfsRootDiagnostics {
  UfsAtomicField<const char *> stage = "not started";
  UfsAtomicField<uint64_t> dma = 0, list = 0, lba = 0, first = 0, last = 0;
  UfsAtomicField<uint64_t> windowBytes = 0, policyCount = 0, reads = 0, failures = 0;
  UfsAtomicField<uint32_t> cap = 0, version = 0, hcs = 0, hce = 0, doorbell = 0, interrupt = 0;
  UfsAtomicField<uint32_t> smr = 0, s2cr = 0, cbar = 0, sctlr = 0;
  UfsAtomicField<uint32_t> type = 0, opcode = 0, lun = 0, command = 0, ocs = 0xff, scsi = 0xffff;
  UfsAtomicField<uint32_t> response = 0, result = 0, power = 0, manifest = 0xffffffff;
  UfsAtomicField<uint32_t> magic0 = 0, magic1 = 0, windows = 0, prepared = 0, published = 0;
  UfsAtomicField<uint32_t> verify = 0xffffffff;
  UfsAtomicField<uint32_t> protocol = 0, residual = 0, sense = 0;
  UfsAtomicField<uint64_t> far = 0, contiguous = 0;
  UfsAtomicField<uint32_t> runStop = 0, ahit = 0, polls = 0, dbSeen = 0, isSeen = 0, isBefore = 0;
  UfsAtomicField<uint32_t> gfsrBefore = 0, gfsr = 0, gsyn = 0, fsrBefore = 0, fsr = 0, fsyn = 0;
  UfsAtomicField<uint32_t> smmuId2 = 0, cba2r = 0, uic[5] = {};
  UfsAtomicField<uint32_t> smmuId1 = 0, smmuSlot = 0xffffffff;
  UfsAtomicField<uint32_t> publicationPhase = 0, publicationWindow = 0xffffffff;
  UfsAtomicField<uint64_t> waitTicks = 0, elapsedTicks = 0;
  UfsAtomicField<uint32_t> counterHz = 0, counterControl = 0, dmaOcs = 0xff, dmaResponse = 0;
  UfsAtomicField<uint32_t> ie = 0, iacrBefore = 0, iacr = 0, memcfg = 0, tlbStatus = 0xffffffff;
  UfsAtomicField<uint32_t> txBefore = 0xffffffff, txAfter = 0xffffffff, linkCommand = 0;
  UfsAtomicField<uint32_t> linkResult = 0xffffffff, linkStatus = 0, linkAhit = 0;
};

static inline int formatUfsRootDiagnostics(char *buffer, size_t capacity,
                                          const UfsRootDiagnostics &d, const char *reason,
                                          const char *title = "UFS ROOT FAILURE")
{
  return snprintf(buffer, capacity,
    "%s: %s\nstage=%s result=%08x\n"
    "HC cap=%08x ver=%08x hcs=%08x hce=%08x db=%08x is=%08x\n"
    "DMA=%llx UTRL=%llx SMMU smr=%08x s2cr=%08x cbar=%08x sctlr=%08x\n"
    "CMD n=%u type=%02x op=%02x lun=%02x ocs=%02x rsp=%08x scsi=%04x power=%02x\n"
    "PROTOCOL reject=%u residual=%u sense=%06x SMMU slot=%u id1=%x\n"
    "QUEUE rsr=%x ahit=%x polls=%u dbSeen=%x isSeen=%x isBefore=%x contiguous=%llx\n"
    "WAIT ticks=%llx/%llx hz=%x cntkctl=%x DMA ocs=%x rsp=%08x\n"
    "PATH ie=%x iacr=%x->%x memcfg=%x tlb=%x\n"
    "LINK tx=%x->%x cmd=%x rc=%x is=%x ahitBefore=%x\n"
    "FAULT global=%x->%x syn=%x cb=%x->%x syn=%x far=%llx id2=%x cba2r=%x\n"
    "UIC pa=%08x dl=%08x net=%08x tr=%08x dme=%08x\n"
    "GPT/read lba=%llx partition=%llx..%llx magic=%08x %08x manifest=%u\n"
    "WINDOWS found=%u prepared=%u published=%u bytes=%llx policies=%llu verify=%08x\n"
    "IO reads=%llu failures=%llu pubPhase=%u window=%u\n",
    title, reason, d.stage.load (), d.result.load (), d.cap.load (), d.version.load (), d.hcs.load (), d.hce.load (), d.doorbell.load (), d.interrupt.load (),
    (unsigned long long)d.dma.load (), (unsigned long long)d.list.load (), d.smr.load (), d.s2cr.load (), d.cbar.load (), d.sctlr.load (),
    d.command.load (), d.type.load (), d.opcode.load (), d.lun.load (), d.ocs.load (), d.response.load (), d.scsi.load (), d.power.load (),
    d.protocol.load (), d.residual.load (), d.sense.load (), d.smmuSlot.load (), d.smmuId1.load (),
    d.runStop.load (), d.ahit.load (), d.polls.load (), d.dbSeen.load (), d.isSeen.load (), d.isBefore.load (), (unsigned long long)d.contiguous.load (),
    (unsigned long long)d.elapsedTicks.load (), (unsigned long long)d.waitTicks.load (),
    d.counterHz.load (), d.counterControl.load (), d.dmaOcs.load (), d.dmaResponse.load (),
    d.ie.load (), d.iacrBefore.load (), d.iacr.load (), d.memcfg.load (), d.tlbStatus.load (),
    d.txBefore.load (), d.txAfter.load (), d.linkCommand.load (), d.linkResult.load (), d.linkStatus.load (), d.linkAhit.load (),
    d.gfsrBefore.load (), d.gfsr.load (), d.gsyn.load (), d.fsrBefore.load (), d.fsr.load (), d.fsyn.load (), (unsigned long long)d.far.load (),
    d.smmuId2.load (), d.cba2r.load (), d.uic[0].load (), d.uic[1].load (), d.uic[2].load (), d.uic[3].load (), d.uic[4].load (),
    (unsigned long long)d.lba.load (), (unsigned long long)d.first.load (), (unsigned long long)d.last.load (),
    d.magic0.load (), d.magic1.load (), d.manifest.load (), d.windows.load (), d.prepared.load (), d.published.load (),
    (unsigned long long)d.windowBytes.load (), (unsigned long long)d.policyCount.load (), d.verify.load (),
    (unsigned long long)d.reads.load (), (unsigned long long)d.failures.load (), d.publicationPhase.load (), d.publicationWindow.load ());
}
