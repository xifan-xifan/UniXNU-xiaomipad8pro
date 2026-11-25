// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Bounded passive evidence. No MMIO stores, queue consumption, IRQ clear or waits.
#pragma once
#include "VideoHfi.h"
namespace pianovideo { namespace diagnostic {
struct Packet {
  uint64_t us = 0;
  uint32_t header[8] = {}, words[24] = {}, copied = 0;
};
template<unsigned N> struct Trail {
  Packet entries[N] = {};
  unsigned total = 0;
  void add(const uint32_t *b, unsigned bytes, uint64_t us) {
    if (bytes < 32 || bytes > hfi::kBufferWords*4 || (bytes & 3) || b[0] != bytes) return;
    for (unsigned at = 32, n = 0; n < b[7] && at <= bytes && bytes-at >= 32; ++n) {
      const uint32_t *p = b+at/4;
      if (p[0] < 32 || (p[0]&3) || p[0] > bytes-at) break;
      Packet &e = entries[total++ % N]; e = Packet{}; e.us = us;
      for (unsigned i=0;i<8;++i) e.header[i]=b[i];
      e.copied=p[0]/4 < 24 ? p[0]/4 : 24;
      for (unsigned i=0;i<e.copied;++i) e.words[i]=p[i];
      at+=p[0];
    }
  }
  template<class Log> void print(Log &log, const char *direction) const {
    const unsigned count=total<N?total:N;
    log.note("V6f history %s total %u retained %u",direction,total,count);
    for(unsigned i=total-count;i<total;++i) {
      const auto &e=entries[i%N]; const auto *p=e.words;
      log.note("V6f %s seq %u us %llu sid %08x header %u type %08x flags %08x payload %u port %u packet %u bytes %u copied %u",
        direction,i+1,(unsigned long long)e.us,e.header[1],e.header[2],p[1],p[2],p[3],p[4],p[5],p[0],e.copied);
      for(unsigned j=8;j<e.copied;j+=4)
        log.note("V6f %s seq %u payload-word %u %08x %08x %08x %08x",direction,i+1,j-8,p[j],
          j+1<e.copied?p[j+1]:0,j+2<e.copied?p[j+2]:0,j+3<e.copied?p[j+3]:0);
    }
  }
};
struct History { Trail<40> commands; Trail<16> messages; };
struct Hardware {
  uint64_t startUs=0,endUs=0;
  bool cc=false,controller=false,codec=false,queues=false;
  uint32_t gcc[3]={},clock[12]={},core[12]={},idle[4]={};
  uint32_t queue[3][6]={},head[2][32]={},headWords[2]={};
  // Snapshot each domain only through the existing owner readability contract.
  // Never touch A2HSOFTINTCLR or CPU_IC_SOFTINT (both have side effects).
  template<class Owner> void capture(Owner &o, bool sampleMmio = true) {
    startUs=o.hw.nowUs(); cc=sampleMmio&&o.power.ccReadable();
    controller=sampleMmio&&o.power.controllerReadable();queues=o.mem.uc!=nullptr;
    if(cc) {
      const uint32_t g[]={kGccVideoAhb,kGccVideoAxi0,kGccVideoAxi1};
      const uint32_t c[]={kCcPll0,kCcMvs0Rcg,kCcMvs0Rcg+4,kCcMvs0cGdsc,kCcMvs0cGdsc+4,kCcMvs0Gdsc,kCcMvs0Gdsc+4,
        kCcMvs0cClk,kCcMvs0cFreerun,kCcMvs0Clk,kCcMvs0Freerun,kCcMvs0Div};
      for(unsigned i=0;i<3;++i)gcc[i]=o.ops.rd(kGcc+g[i]);
      for(unsigned i=0;i<12;++i)clock[i]=o.ops.rd(kVideoCc+c[i]);
    }
    if(controller) {
      const uint32_t c[]={kCtrlInit,kCtrlStatus,kCorePowerStatus,kCoreClockConfig,kTzCpuStatus,kWrapperIntrStatus,
        kWrapperIntrMask,kH2xSoftIntEn,kCpuNocLpiCtl,kCpuNocLpiStatus,kAonVideoCtlNocLpiCtl,kAonVideoCtlNocLpiStatus};
      for(unsigned i=0;i<12;++i)core[i]=o.ops.rd(kIris+c[i]);
    }
    // Firmware may collapse MVS0 while the host's provisioned-state flags stay
    // set. Sample its live power bit before touching any codec-domain word.
    codec=controller&&o.power.codecReadable()&&(core[2]&kCorePowered);
    if(codec)for(unsigned i=0;i<4;++i)idle[i]=o.ops.rd(kIris+kVcodecIdleStatus+4*i);
    if(queues) {
      o.hw.barrier();
      const uint32_t offsets[]={hfi::kQRead,hfi::kQWrite,hfi::kQDrop,hfi::kQRxReq,hfi::kQTxReq,hfi::kQSize};
      for(unsigned q=0;q<3;++q)for(unsigned i=0;i<6;++i)queue[q][i]=o.mem.ld(hfi::qhdr(q)+offsets[i]);
      for(unsigned q=0;q<2;++q) {
        const uint32_t r=queue[q][0],w=queue[q][1],size=queue[q][5];
        if(size!=kHfiQueueBytes/4||r>=size||w>=size)continue;
        const uint32_t pending=(w+size-r)%size;
        headWords[q]=pending<32?pending:32;
        for(unsigned i=0;i<headWords[q];++i)head[q][i]=o.mem.ld(hfi::qbase(q)+4*((r+i)%size));
      }
    }
    endUs=o.hw.nowUs();
  }
  template<class Log> void print(Log &l) const {
    l.note("V6f readability cc %u controller %u codec %u queues %u sample-us %llu/%llu sequential 1",
      unsigned(cc),unsigned(controller),unsigned(codec),unsigned(queues),(unsigned long long)startUs,(unsigned long long)endUs);
    if(cc) {
      l.note("V6f clock GCC %08x/%08x/%08x PLL %08x RCG %08x/%08x DIV %08x",gcc[0],gcc[1],gcc[2],clock[0],clock[1],clock[2],clock[11]);
      l.note("V6f power GDSC ctrl %08x/%08x codec %08x/%08x branches %08x/%08x/%08x/%08x HWCG-idle-permitted 1",
        clock[3],clock[4],clock[5],clock[6],clock[7],clock[8],clock[9],clock[10]);
    }
    if(controller) {
      l.note("V6f core init %08x ctrl %08x power %08x clock %08x tz %08x",core[0],core[1],core[2],core[3],core[4]);
      l.note("V6f interrupt status %08x mask %08x h2x-enable %08x cpu-lpi %08x/%08x video-lpi %08x/%08x no-clear 1",
        core[5],core[6],core[7],core[8],core[9],core[10],core[11]);
    }
    if(codec)l.note("V6f codec idle %08x/%08x/%08x/%08x",idle[0],idle[1],idle[2],idle[3]);
    else l.note("V6f codec idle skipped not-powered-or-readable");
    if(queues)for(unsigned q=0;q<3;++q) {
      l.note("V6f queue q%u read %u write %u drop %u rxreq %u txreq %u size %u",q,queue[q][0],queue[q][1],queue[q][2],queue[q][3],queue[q][4],queue[q][5]);
      if(q<2) {
        l.note("V6f pending q%u words %u",q,headWords[q]);
        for(unsigned i=0;i<headWords[q];i+=4)l.note("V6f pending q%u word %u %08x %08x %08x %08x",q,i,head[q][i],
          i+1<headWords[q]?head[q][i+1]:0,i+2<headWords[q]?head[q][i+2]:0,i+3<headWords[q]?head[q][i+3]:0);
      }
    }
  }
};
} }
