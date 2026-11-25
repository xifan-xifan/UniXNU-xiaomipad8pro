/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Zhang
 * Optional, read-only period evidence for the bounded GPU qualification export.
 */
#ifndef PIANO_GPU_QUALIFICATION_DISPLAY_H
#define PIANO_GPU_QUALIFICATION_DISPLAY_H
#include <stdint.h>
struct PianoGpuQualificationDisplay {
    uint64_t atUs,periodUs,submitted,shown,dropped,copies,meanUs,maxUs,prepareUs,readUs,convertUs,writeUs;
};
extern "C" void adreno830_qualification_display(const PianoGpuQualificationDisplay *);
#endif
