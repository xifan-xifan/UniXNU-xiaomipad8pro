/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Leo Zhang. All rights reserved. */
#include "piano_panic_text_core.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static struct ppt_page page;
static unsigned char font[4096];
static void contains(const char *s) { assert(ppt_has(&page.cells[0][0], sizeof page.cells, s)); }
int main(void)
{
    char log[12000];
    strcpy(log,"panic(cpu 2 caller 0xffff): diag-reason1 deliberate panic @sysctl.c:49\n");
    strcat(log,"Debugger message: panic\nKernel slide: 0x123000\nKernel text base: 0xffff0000\n");
    strcat(log,"registers: fp: 0xaaaaaaaaaaaaaaaa lr: 0xbbbbbbbbbbbbbbbb\nPanicked thread: 0xffff, backtrace: 0xffff, tid: 7\n");
    for (unsigned i=0;i<20;++i) {
        char frame[90];snprintf(frame,sizeof frame,"\t\t  lr: 0x%016x  fp: 0x%016x\n",i,i+1);strcat(log,frame);
    }
    ppt_select(&page, log, (unsigned)strlen(log));
    contains("diag-reason1 deliberate panic");contains("Kernel slide: 0x123000");
    contains("lr: 0x000000000000000b");assert(!ppt_has(&page.cells[8][0],PPT_COLS*15,"bbbbbbbbbbbbbbbb"));assert(!ppt_has(&page.cells[0][0],sizeof page.cells,"lr: 0x000000000000000c"));
    size_t pixels=3200u*2136u;
    uint32_t *mem=malloc((pixels+2)*sizeof *mem);assert(mem);mem[0]=mem[pixels+1]=0xdecafbad;
    memset(font,0xff,sizeof font);
    assert(!ppt_render(mem+1,3200,2136,12796,32,font,&page));assert(mem[0]==0xdecafbad);
    assert(ppt_render(mem+1,3200,2136,12800,32,font,&page));
    assert(mem[0]==0xdecafbad && mem[pixels+1]==0xdecafbad);
    assert(mem[1+40u*3200u+40u]==0xffffff);
    assert(mem[1+2135u*3200u+3199u]==0x101010);
    free(mem);
    ppt_select(&page,"",0);contains("PANIC STRING MISSING");contains("BACKTRACE MISSING");
    char *full=malloc(PPT_BYTES);memset(full,'A',PPT_BYTES);
    memcpy(full,"panic(cpu 1): ",14);ppt_select(&page,full,PPT_BYTES);contains("CAPTURE FULL - TRUNCATED");free(full);
    puts("PASS: panic selection, first 12 frames, missing/capacity labels, invalid geometry, full-frame guards (ASan/UBSan)");
}
