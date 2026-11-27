// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang. Model observed apps-SMMU semantics, not ideal MMIO.
//   c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -I../../PianoDwc3 smmu_identity_host.cpp -o /tmp/smmu_identity_host && /tmp/smmu_identity_host
#include "SmmuIdentity.h"
#include "UsbDomain.h"
#include "ControllerLoan.h"
#include <cstdio>
#include <map>
#include <vector>
using namespace pianousbhost;
static int failures;
#define CHECK(c) do {if(!(c)){std::printf("FAIL line %d: %s\n",__LINE__,#c);++failures;}}while(0)
struct Model {
  std::map<uint32_t,uint32_t> r;
  std::vector<std::pair<uint32_t,uint32_t>> writes;
  uint32_t reject=~0u,corrupt=0;
  bool illegal=false;
  Model() {r[0x20]=0x4c017e7f;r[0x24]=0x60000053;r[0x28]=0x5111;
    r[0x804]=0x80000060;r[0xc04]=82;r[0x1148]=0x10000;r[0x1948]=1;
    for(unsigned b=0;b<83;++b)r[(128+b)*4096+0x58]=0x400;
  }
  auto rd(){return [this](uint32_t o){return r[o];};}
  auto wr(){return [this](uint32_t o,uint32_t v){
    writes.push_back({o,v});
    if(o>=0x1000&&o<0x1200&&((v>>16)&3)!=1)illegal=true;
    if(o>=0xc00&&o<0xe00&&!(v&0x30000)&&(v&255)>=83)illegal=true;
    if(o==reject)return;
    if(o>=0x80000&&!(o&4095))v&=~0x1000u;
    if(o>=0x80000&&(o&4095)==0x10)v|=0x60;
    if(o>=0x80000&&(o&4095)==0x58){r[o]&=~(v&kFsrFaults);return;}
    r[o]=v^(o==corrupt?1u:0u);
  };}
};
int main(){
  const uint64_t table=0x8e0efb000ull;const auto never=[] {return false;};
  unsigned audits=0;auto audit=[&](uint32_t,uint32_t,uint32_t,uint32_t){++audits;};
  { // A short aperture may not even contain the identification registers.
    Model m;
    unsigned reads = 0;
    auto rd = [&](uint32_t off) { ++reads; return m.r[off]; };
    CHECK(planSmmuIdentity(0x40, 0x2b, rd).error && reads == 0);
    m.r[0x20] = (m.r[0x20] & ~255u) | 129u;
    CHECK(planSmmuIdentity(0x40, 0x100000, rd).error);
    m.r[0x20]--;
    CHECK(!planSmmuIdentity(0x40, 0x100000, rd).error);
    CHECK(planSmmuIdentity(0x8040, 0x100000, rd).error);
  }
  { // Revalidation must cover the SID that was actually planned.
    Model m;
    const auto p = planSmmuIdentity(0x41, 0x100000, m.rd());
    m.r[0x810] = 0x80000041;
    IdentityTxn tx;
    CHECK(!p.error && applySmmuIdentity(p, 0x41, table, m.rd(), m.wr(), never, tx,
                                       [](uint32_t, uint32_t, uint32_t, uint32_t) {}));
    CHECK(m.writes.empty());
  }
  {
    Model m;auto p=planSmmuIdentity(0x40,0x100000,m.rd());IdentityTxn tx;
    CHECK(!p.error&&p.bank==81&&p.asid==81);
    CHECK(!applySmmuIdentity(p,0x40,table,m.rd(),m.wr(),never,tx,audit));
    CHECK(tx.ready&&m.r[p.cb()]==0x27&&m.r[p.cb()+0x10]==0x38061);
    CHECK(m.r[p.cb()+0x24]==0x510008&&m.r[p.cb()+0x2c]==0x510000);
    CHECK(m.r[p.cb()+0x58]==0x400&&audits==14&&!m.illegal);
    CHECK(unwindSmmuIdentity(p,m.rd(),m.wr(),never,tx));
    CHECK(!(m.r[0x800]&0x80000000)&&!m.r[p.cb()]&&m.r[0x1000+4*p.bank]==kCbarStage1&&!m.illegal);
  }
  { // Preserve the original active USB route until the new bank has passed every check.
    Model m;m.r[0x80c]=0x80000040;m.r[0xc0c]=81;m.r[(128+81)*4096]=0x27;
    m.r[0x1144]=kCbarStage1;m.r[0x1944]=1;
    auto p=planSmmuIdentity(0x40,0x100000,m.rd());CHECK(p.bank==80);
    IdentityTxn tx;m.reject=p.cb()+0x38;
    CHECK(applySmmuIdentity(p,0x40,table,m.rd(),m.wr(),never,tx,audit));
    CHECK(!tx.route&&m.r[0xc0c]==81&&m.r[(128+81)*4096]==0x27);
    CHECK(unwindSmmuIdentity(p,m.rd(),m.wr(),never,tx)&&!m.illegal);
  }
  { // Real control errors still reject; all checks are audited after SCTLR mismatch.
    Model m;auto p=planSmmuIdentity(0x40,0x100000,m.rd());m.reject=p.cb();IdentityTxn tx;audits=0;
    CHECK(applySmmuIdentity(p,0x40,table,m.rd(),m.wr(),never,tx,audit));CHECK(audits==12&&!tx.route);
    m.reject=~0u;CHECK(unwindSmmuIdentity(p,m.rd(),m.wr(),never,tx));
  }
  { // ASID collision excludes a bank; ownership changes after plan refuse all writes.
    Model m;m.r[(128+7)*4096]=0x27;m.r[0x101c]=kCbarStage1;m.r[(128+7)*4096+0x24]=81u<<16;
    auto p=planSmmuIdentity(0x40,0x100000,m.rd());CHECK(p.bank==80);
    m.r[0x810]=0x80000123;m.r[0xc10]=80;IdentityTxn tx;
    CHECK(applySmmuIdentity(p,0x40,table,m.rd(),m.wr(),never,tx,audit)&&m.writes.empty());
  }
  { // Global sync failure after route commit: legal original bank restored; invalid reset CBAR never restored.
    Model m;m.r[0x80c]=0x80000040;m.r[0xc0c]=4;m.r[0x1010]=kCbarStage1;m.r[0x1810]=1;
    auto p=planSmmuIdentity(0x40,0x100000,m.rd());IdentityTxn tx;m.r[0x74]=1;
    CHECK(applySmmuIdentity(p,0x40,table,m.rd(),m.wr(),[]{return true;},tx,audit));
    m.r[0x74]=0;CHECK(unwindSmmuIdentity(p,m.rd(),m.wr(),never,tx));
    CHECK(m.r[0x80c]==0x80000040&&m.r[0xc0c]==4&&!m.illegal);
  }
  { // Unconfirmed disconnect/sync retains ownership and memory.
    Model m;auto p=planSmmuIdentity(0x40,0x100000,m.rd());IdentityTxn tx;
    CHECK(!applySmmuIdentity(p,0x40,table,m.rd(),m.wr(),never,tx,audit));m.reject=0x800;
    CHECK(!unwindSmmuIdentity(p,m.rd(),m.wr(),never,tx)&&tx.held);
  }
  for(uint32_t bit:{1u,0x10u,0x100u}) {Model m;m.r[0x28]&=~bit;CHECK(planSmmuIdentity(0x40,0x100000,m.rd()).error);}
  CHECK(identityRange(table,4096)&&!identityRange(1ull<<36,4096)&&!identityRange((1ull<<36)-4096,8192));
  uint64_t entries[64]={};identityTable([&](unsigned i,uint64_t d){entries[i]=d;});CHECK(!entries[1]&&entries[2]==0x80000741ull&&entries[63]==(63ull<<30|0x741));
  { // There is no initialize request on the borrower contract, nor a retry into a live domain.
    pianousb::DomainOwner owner;CHECK(!owner.borrow()&&owner.begin()&&!owner.begin());
    owner.finish(true,false);CHECK(owner.borrow()&&!owner.begin());
    pianousb::DomainOwner held;held.begin();held.finish(false,true);CHECK(!held.borrow()&&!held.begin());
  }
  { // No peripheral/no notifications: deadline still returns exactly once.
    ControllerLoan l;CHECK(!l.begin(100,0,true));CHECK(l.begin(100,60000,true));l.started();
    CHECK(l.end(60099,false,false,false,false)==LoanEnd::none);
    CHECK(l.end(60100,false,false,false,false)==LoanEnd::timeout);
    CHECK(l.returning()&&!l.returning());l.returned(true,true,true);
    CHECK(l.phase==LoanPhase::device&&!l.begin(60101,100,true));l.disconnected();CHECK(l.begin(60102,100,true));
  }
  {ControllerLoan l;l.begin(1,10000,false);l.started();CHECK(l.end(2,false,true,true,false)==LoanEnd::relay);}
  {ControllerLoan l;l.begin(1,10000,false);l.started();CHECK(l.end(2,true,false,false,false)==LoanEnd::unplug);}
  {ControllerLoan l;l.begin(1,10000,false);l.started();CHECK(l.end(2,true,true,false,false)==LoanEnd::role);}
  {
    ControllerLoan l;
    CHECK(l.begin(1, 10000, false));
    l.started();
    CHECK(l.end(2, true, true, true, true, true) == LoanEnd::failure);
    l.disconnected();
    CHECK(!l.canBegin() && l.returning());
    l.returned(true, true, true);
    CHECK(l.canBegin());
    CHECK(l.begin(3, 10000, false) && l.returning());
    l.returned(true, true, false);
    l.disconnected();
    CHECK(!l.canBegin());
  }
  {ControllerLoan l;l.begin(1,10000,true);l.started();CHECK(l.end(2,false,false,false,true)==LoanEnd::none);
    CHECK(l.end(3,false,false,false,false)==LoanEnd::unplug);l.returning();l.returned(false,true,true);CHECK(l.phase==LoanPhase::held);}
  // A required field mismatch in any programmed word refuses routing.
  for(unsigned field=0;field<12;++field) {
    Model m;auto p=planSmmuIdentity(0x40,0x100000,m.rd());IdentityTxn tx;
    const uint32_t bad=identityWord(p,field);
    bool enabled=false;auto wr0=m.wr();
    auto wr=[&](uint32_t o,uint32_t v){wr0(o,v);if(o==p.cb()&&v==kSctlrIdentity)enabled=true;};
    auto rd=[&](uint32_t o){uint32_t v=m.r[o];return enabled&&o==bad?v^1u:v;};
    unsigned comparisons=0;
    auto audit=[&](uint32_t,uint32_t,uint32_t,uint32_t){++comparisons;};
    CHECK(applySmmuIdentity(p,0x40,table,rd,wr,never,tx,audit));
    CHECK(!tx.route&&comparisons==12);
  }
  { // A group has a firmware reset S2CR pointing outside the bank geometry.
    Model m;m.r[0x80c]=0x80000040;m.r[0xc0c]=0x0300006e;
    auto p=planSmmuIdentity(0x40,0x100000,m.rd());IdentityTxn tx;m.r[0x74]=1;
    CHECK(applySmmuIdentity(p,0x40,table,m.rd(),m.wr(),[]{return true;},tx,[](uint32_t,uint32_t,uint32_t,uint32_t){}));
    m.r[0x74]=0;CHECK(unwindSmmuIdentity(p,m.rd(),m.wr(),never,tx));
    CHECK(!m.illegal&&!(m.r[0x80c]&0x80000000));
  }
  { // Only valid W1C bits are cleared, and an old stall is terminated.
    Model m;auto p=planSmmuIdentity(0x40,0x100000,m.rd());IdentityTxn tx;
    m.r[p.cb()+kCbFsr]=0x400|0x40000002;
    CHECK(!applySmmuIdentity(p,0x40,table,m.rd(),m.wr(),never,tx,[](uint32_t,uint32_t,uint32_t,uint32_t){}));
    CHECK(m.r[p.cb()+kCbFsr]==0x400&&m.r[p.cb()+8]==1);
  }
  std::printf("smmu_identity_host: %s\n",failures?"FAILED":"all passed (readback, unwind, ownership, bounded loan)");return failures?1:0;
}
