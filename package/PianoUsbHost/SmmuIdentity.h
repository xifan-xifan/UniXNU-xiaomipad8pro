// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang. USB's boot-lifetime WB identity S1 domain.
// Register facts: ARM SMMUv2; UniXNU QcomDwc3 e4efe95; this repository's
// VideoSmmu.h (the apps SMMU's observed ASIDPNE/TBI and legal-release contract).
// One owner configures it with DWC3 halted; borrowers only verify it. Tables stay
// pinned across gadget/host loans. No restoration of unsupported reset CBAR words.
#pragma once
#include <stdint.h>

namespace pianousbhost {
constexpr uint32_t kCbSctlr=0, kCbTcr2=0x10, kCbTtbr0=0x20, kCbTtbr1=0x28, kCbTcr=0x30,
  kCbContextIdr=0x34, kCbMair0=0x38, kCbMair1=0x3c, kCbFsr=0x58,
  kCbTlbiAll=0x618, kCbTlbSync=0x7f0, kCbTlbStatus=0x7f4;
constexpr uint32_t kSctlrIdentity=0x1027, kSctlrRequired=0x27, kCbarStage1=0x1f300,
  kTcr2Identity=0x38001, kTcr2Fields=0x3801f, kTcrIdentity=0x80351c,
  kFsrFaults=0xc00001fe, kS2crFields=0x300ff;
constexpr unsigned kIdentityEntries=64, kIdentityFirstGiB=2;
constexpr uint64_t identityBlock(uint64_t gib) { return gib<<30 | 0x741ull; }
constexpr bool identityRange(uint64_t pa, uint64_t bytes) {
  return bytes && pa >= (2ull<<30) && pa < (1ull<<36) && bytes <= (1ull<<36)-pa;
}
template<class Put> void identityTable(Put put) {
  for(unsigned i=0;i<kIdentityEntries;++i) put(i,i>=kIdentityFirstGiB?identityBlock(i):0);
}
struct IdentityPlan {
  unsigned groups=0,banks=0,pages=0,slot=~0u,bank=~0u,asid=0;
  uint16_t stream=0;
  bool reuse=false;
  uint32_t smr=0,s2cr=0,idr0=0,idr1=0,idr2=0,scr0=0;
  uint32_t baseline[12]={};
  const char *error=nullptr;
  uint32_t cb() const { return (pages+bank)*4096; }
};
constexpr uint32_t kIdentityWords[12]={0,0x10,0x20,0x24,0x28,0x2c,0x30,0x34,0x38,0x3c,0x1000,0x1800};
inline uint32_t identityWord(const IdentityPlan &p,unsigned i) {
  return i<10?p.cb()+kIdentityWords[i]:kIdentityWords[i]+4*p.bank;
}
inline bool identityCovers(uint32_t smr,uint16_t stream) {
  return ((smr^stream)&0x7fff&~((smr>>16)&0x7fff))==0;
}
// Fingerprint/revalidation is a detectable no-competition precondition, not a
// cross-kext atomic lock. SID 0x40 itself has exactly one initializer (PianoDwc3).
template<class Read> const char *identityOwners(const IdentityPlan &p, Read rd, uint64_t &stamp) {
  stamp=1469598103934665603ull;
  auto mix=[&](uint32_t v){stamp=(stamp^v)*1099511628211ull;};
  for(unsigned i=0;i<p.groups;++i) {
    if(i==p.slot) continue;
    const uint32_t smr=rd(0x800+4*i),s2cr=rd(0xc00+4*i);
    mix(smr);mix(s2cr);
    if((smr&0x80000000u) && (identityCovers(smr,p.stream) || (s2cr&kS2crFields)==p.bank))
      return "SID or bank acquired another owner";
  }
  for(unsigned b=0;b<p.banks;++b) {
    if(b==p.bank) continue;
    const uint32_t cb=(p.pages+b)*4096,sctlr=rd(cb),cbar=rd(0x1000+4*b);
    mix(sctlr);mix(cbar);
    if(!(sctlr&1) || !((cbar>>16)&3)) continue;
    const uint32_t a=rd(cb+0x24),z=rd(cb+0x2c);mix(a);mix(z);
    if((a>>16)==p.asid || (z>>16)==p.asid) return "ASID held by active S1 bank";
  }
  return nullptr;
}
template<class Read> IdentityPlan planSmmuIdentity(uint16_t stream,uint64_t bytes,Read rd) {
  IdentityPlan p;
  if(bytes<0x2c || stream>0x7fff) {p.error="invalid SMMU aperture or stream ID";return p;}
  p.stream=stream;
  p.scr0=rd(0);p.idr0=rd(0x20);p.idr1=rd(0x24);p.idr2=rd(0x28);
  if(!(p.idr0&(1u<<30)) || !(p.idr2&(1u<<12)) || (p.scr0&8)) {p.error="unsupported S1/4K/stream layout";return p;}
  // The entire 36-bit identity window must be reachable at input and output.
  if((p.idr2&15)<1 || ((p.idr2>>4)&15)<1 || ((p.idr2>>8)&15)<1) {p.error="identity window needs IAS/OAS/UBS >=36";return p;}
  p.groups=p.idr0&255;
  p.banks=p.idr1&255;p.pages=1u<<(((p.idr1>>28)&7)+1);
  if((p.idr1>>31)||!p.groups||p.groups>128||p.banks<2||p.banks>128||p.pages>128||uint64_t(p.pages+p.banks)*4096>bytes) {
    p.error="unsupported SMMU geometry";return p;
  }
  uint64_t used[2]={};unsigned freeSlot=~0u,matches=0;
  for(unsigned i=0;i<p.groups;++i) {
    const uint32_t smr=rd(0x800+4*i),s2cr=rd(0xc00+4*i);
    if(!(smr&0x80000000u)) {if(freeSlot==~0u)freeSlot=i;continue;}
    if((s2cr&0x30000)==0 && (s2cr&255)<p.banks) used[(s2cr&255)/64]|=1ull<<((s2cr&255)%64);
    if(identityCovers(smr,stream)) {++matches;p.slot=i;p.smr=smr;p.s2cr=s2cr;}
  }
  if(matches>1 || (matches==1 && ((p.smr>>16)&0x7fff))) {p.error="ambiguous or masked SID";return p;}
  p.reuse=matches==1;
  if(!p.reuse) {p.slot=freeSlot;if(p.slot==~0u){p.error="no free stream group";return p;}p.smr=rd(0x800+4*p.slot);p.s2cr=rd(0xc00+4*p.slot);}
  // Never reprogram the old route's bank, even when its M bit is off.
  for(unsigned b=p.banks-1;b-->1;) {
    if((used[b/64]>>(b%64))&1 || (rd((p.pages+b)*4096)&1)) continue;
    p.bank=b;p.asid=b;
    uint64_t stamp=0;
    if(identityOwners(p,rd,stamp)) continue;
    for(unsigned i=0;i<12;++i)p.baseline[i]=rd(identityWord(p,i));
    return p;
  }
  p.error="no unused bank with independent ASID";return p;
}
struct IdentityTxn {bool bank=false,route=false,ready=false,held=false;};
// Audit(offset,want,got,mask) is called for EVERY comparison, including failures.
template<class Read,class Audit> bool verifySmmuIdentity(const IdentityPlan &p,uint64_t table,Read rd,Audit audit) {
  bool ok=true;
  auto check=[&](uint32_t o,uint32_t w,uint32_t m){uint32_t g=rd(o);audit(o,w,g,m);ok&=((w^g)&m)==0;};
  check(p.cb(),kSctlrIdentity,kSctlrRequired);
  check(0x1800+4*p.bank,1,0xffff);check(0x1000+4*p.bank,kCbarStage1,0x3ffff);
  check(p.cb()+0x10,kTcr2Identity,kTcr2Fields);check(p.cb()+0x30,kTcrIdentity,0xffffffff);
  check(p.cb()+0x20,uint32_t(table),0xffffffff);check(p.cb()+0x24,uint32_t(table>>32)|(p.asid<<16),0xffffffff);
  check(p.cb()+0x28,0,0xffffffff);check(p.cb()+0x2c,p.asid<<16,0xffffffff);
  check(p.cb()+0x34,0,0xffffffff);check(p.cb()+0x38,0xff,0xffffffff);check(p.cb()+0x3c,0,0xffffffff);
  return ok;
}
template<class Read,class Write,class Expired> bool identitySync(uint32_t invalidate,uint32_t sync,uint32_t status,Read rd,Write wr,Expired expired) {
  wr(invalidate,0xffffffffu);wr(sync,0xffffffffu);
  while(rd(status)&1)if(expired())return false;
  return true;
}
template<class Read,class Write,class Expired,class Audit>
const char *applySmmuIdentity(const IdentityPlan &p,uint16_t stream,uint64_t table,Read rd,Write wr,Expired expired,IdentityTxn &tx,Audit audit) {
  if(p.error)return p.error;
  if(stream!=p.stream)return "stream ID differs from plan";
  if((table&4095)||!identityRange(table,4096))return "table outside identity DMA window";
  if(rd(0)!=p.scr0||rd(0x20)!=p.idr0||rd(0x24)!=p.idr1||rd(0x28)!=p.idr2||rd(0x800+4*p.slot)!=p.smr||rd(0xc00+4*p.slot)!=p.s2cr)
    return "geometry or route changed since plan";
  for(unsigned i=0;i<12;++i)if(rd(identityWord(p,i))!=p.baseline[i])return "bank changed since plan";
  uint64_t before=0,after=0;const char *owner=identityOwners(p,rd,before);if(owner)return owner;
  tx.bank=true;const uint32_t cb=p.cb();
  wr(cb,0);if(rd(cb)&1)return "bank would not disable";
  wr(0x1800+4*p.bank,1);wr(0x1000+4*p.bank,kCbarStage1);
  wr(cb+0x10,kTcr2Identity);wr(cb+0x30,kTcrIdentity);
  wr(cb+0x20,uint32_t(table));wr(cb+0x24,uint32_t(table>>32)|(p.asid<<16));
  wr(cb+0x28,0);wr(cb+0x2c,p.asid<<16);wr(cb+0x34,0);wr(cb+0x38,0xff);wr(cb+0x3c,0);
  const uint32_t faults=rd(cb+kCbFsr)&kFsrFaults;
  if(faults){wr(cb+kCbFsr,faults);if(faults&(1u<<30))wr(cb+8,1);}
  if(!identitySync(cb+kCbTlbiAll,cb+kCbTlbSync,cb+kCbTlbStatus,rd,wr,expired))return "context sync timeout";
  wr(cb,kSctlrIdentity);
  if(!verifySmmuIdentity(p,table,rd,audit))return "context bank readback";
  owner=identityOwners(p,rd,after);if(owner)return owner;
  if(before!=after||rd(0x800+4*p.slot)!=p.smr||rd(0xc00+4*p.slot)!=p.s2cr)return "SMMU owners changed during apply";
  tx.route=true;wr(0xc00+4*p.slot,p.bank);
  uint32_t got=rd(0xc00+4*p.slot);audit(0xc00+4*p.slot,p.bank,got,kS2crFields);
  if((got&kS2crFields)!=p.bank)return "stream context readback";
  if(!p.reuse)wr(0x800+4*p.slot,0x80000000u|stream);
  got=rd(0x800+4*p.slot);audit(0x800+4*p.slot,0x80000000u|stream,got,0xffffffff);
  if(got!=(0x80000000u|stream))return "stream match readback";
  if(!identitySync(0x68,0x70,0x74,rd,wr,expired))return "global sync timeout";
  if(rd(cb+kCbFsr)&kFsrFaults)return "valid context fault";
  tx.ready=true;return nullptr;
}
// Only with the controller confirmed stopped. Failure pins memory/ownership.
// Successful boot domains are deliberately NEVER released during a role change.
template<class Read,class Write,class Expired>
bool unwindSmmuIdentity(const IdentityPlan &p,Read rd,Write wr,Expired expired,IdentityTxn &tx) {
  if(!tx.bank)return true;
  tx.held=true;const uint32_t cb=p.cb();
  if(tx.route) {
    wr(0x800+4*p.slot,0);if(rd(0x800+4*p.slot)&0x80000000u)return false;
    const unsigned old=p.s2cr&255;
    const bool legal=p.reuse && !(p.s2cr&0x30000) && old<p.banks && old!=p.bank &&
      ((rd(0x1000+4*old)>>16)&3)==1 && (rd(0x1800+4*old)&1);
    if(legal) {
      wr(0xc00+4*p.slot,p.s2cr);if(rd(0xc00+4*p.slot)!=p.s2cr)return false;
      wr(0x800+4*p.slot,p.smr);if(rd(0x800+4*p.slot)!=p.smr)return false;
    } // Otherwise retain an invalid SMR and the last legal S2CR. No FAULT/BYPASS probe.
    if(!identitySync(0x68,0x70,0x74,rd,wr,expired))return false;
  }
  wr(cb,0);if(rd(cb)&1)return false;
  if(!identitySync(cb+kCbTlbiAll,cb+kCbTlbSync,cb+kCbTlbStatus,rd,wr,expired))return false;
  tx={};return true; // CBAR/CBA2R remain legal S1, M=0; unknown reset words are not restored.
}
} // namespace pianousbhost
