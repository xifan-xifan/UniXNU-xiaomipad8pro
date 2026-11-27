// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu B1b in the kernel (DpuFlip.cpp).
#pragma once
#include <IOKit/IOLocks.h>
#include <IOKit/IOReturn.h>
#include "DpuSurvey.h"

class IOService;
class OSSymbol;
struct PianoDpuScanoutRequest;
struct PianoDpuScanoutDiagnostics;

namespace pianodpu {

namespace flip { struct Stats; struct CleanQualification; }

// piano-dpu=2: B1b's ladder (piano-dpu-flip=N, docs/drivers/dpu.md section 8) on the calling thread, after the survey s and m
// (smmuLevel: piano-dpu-smmu, 3 lets a bank the boot loader translates be walked and taken over). Step 4 does not return while
// it serves: the thread then watches the surface on the panel (piano-dpu-watch) and logs the summaries.
void flipRun(IOService *owner, const Survey &s, const SmmuSurvey &m, uint32_t smmuLevel);

// PianoDpu::callPlatformFunction: true when name is "PianoDpuFlip" (PianoDpuFlipClient.h), with *result set.
bool flipFunction(const OSSymbol *name, void *request, void *handoff, IOReturn *result);
bool flipStatsSnapshot(flip::Stats &stats, flip::CleanQualification &clean);
IOReturn scanoutRead(PianoDpuScanoutRequest *request, PianoDpuScanoutDiagnostics *diagnostics);

// Serializes timing and PCC transactions with flips.
IOLock *flipTransactionLock();
void flipInvalidateCleanEpochs();   // caller holds flipTransactionLock()
uint32_t flipPendingUnderruns(bool clear);   // underruns acknowledged by the interrupt handler

// The panel worker calls these as a pair after the flip ladder has finished.
void flipPanelBegin();
void flipPanelEnd(bool paused);

}  // namespace pianodpu
