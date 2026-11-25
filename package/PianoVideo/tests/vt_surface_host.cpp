// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#include "PianoVideoVTSurface.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace pianovideo::vtsurface;

// The independently decoded 27.3 contract: lookup installs two references;
// invalidate removes the handle, release removes the buffer, stop revokes the
// creator task. This models that contract, not hardware or IOKit qualification.
struct Peer {
  bool creator=true, handle=false, buffer=false;
  uint32_t liveID=0, fail=0;
  std::vector<uint32_t> calls;
  bool lookup(uint32_t id) {
    if (!creator || handle || buffer) return false;
    liveID=id; handle=buffer=true; return true;
  }
  int call(uint32_t selector,uint32_t id) {
    calls.push_back(selector);
    assert(id==liveID);
    if (selector==fail) return -1;
    if (selector==Invalidate) { assert(handle); handle=false; }
    else { assert(selector==Release && buffer); buffer=false; }
    return 0;
  }
  void close() { creator=handle=buffer=false; }
};
static void copyTest(bool ten) {
  using namespace pianovideo;
  native::Layout l;
  if(ten){
    l.codec=vtwire::cc("hvc1");l.width=l.codedWidth=3840;l.height=l.codedHeight=2160;
    l.bitdepth=10;l.format=vtwire::cc("x420");l.stride=7680;l.scanlines=2176;
    l.uvOffset=l.stride*l.scanlines;l.bytes=l.uvOffset+l.stride*((l.scanlines/2+15)&~15u);
  }
  const unsigned rowBytes=l.width*(ten?2:1);
  vtcontract::Surface s;s.base=0x1000;s.id=7;s.format=l.format;s.planes=2;
  const uint64_t dstStride=ten?8000:352;
  s.plane[0]={l.width,l.height,256,dstStride,0};
  s.plane[1]={l.width/2,l.height/2,256+dstStride*l.height+512,dstStride,0};
  s.size=s.plane[1].offset+dstStride*(l.height/2)+512;
  std::vector<uint8_t> src(l.bytes,0xcd),dst(s.size,0xa5),touched(s.size);
  for(unsigned p=0;p<2;++p)for(unsigned y=0;y<(p?l.height/2:l.height);++y){
    uint8_t *r=src.data()+(p?l.uvOffset:l.yOffset)+uint64_t(y)*l.stride;
    for(unsigned x=0;x<l.width;++x){
      const uint16_t value=uint16_t(((x+3*y+211*p)&1023)<<6);
      if(ten)memcpy(r+2*x,&value,2);else r[x]=uint8_t(x+3*y+71*p);
    }
  }
  assert(vtsurface::copy(l,src.size(),src.data(),s,dst.data()));
  for(unsigned p=0;p<2;++p)for(unsigned y=0;y<(p?l.height/2:l.height);++y){
    const auto out=s.plane[p].offset+uint64_t(y)*s.plane[p].stride;
    assert(!memcmp(dst.data()+out,src.data()+(p?l.uvOffset:l.yOffset)+uint64_t(y)*l.stride,rowBytes));
    memset(touched.data()+out,1,rowBytes);
  }
  for(size_t i=0;i<dst.size();++i)if(!touched[i])assert(dst[i]==0xa5);
  auto bad=s;bad.format=vtwire::cc(ten?"420v":"x420");assert(!vtsurface::copy(l,src.size(),src.data(),bad,dst.data()));
  bad=s;bad.plane[1].offset=bad.plane[0].offset;assert(!vtsurface::copy(l,src.size(),src.data(),bad,dst.data()));
  bad=s;bad.plane[1].nonlinear=1;assert(!vtsurface::copy(l,src.size(),src.data(),bad,dst.data()));
  bad=s;bad.plane[1].offset=UINT64_MAX-8;assert(!vtsurface::copy(l,src.size(),src.data(),bad,dst.data()));
  bad=s;bad.plane[0].stride=rowBytes-1;assert(!vtsurface::copy(l,src.size(),src.data(),bad,dst.data()));
  assert(!vtsurface::copy(l,l.bytes-1,src.data(),s,dst.data()));
  auto badLayout=l;badLayout.uvOffset+=2;assert(!vtsurface::copy(badLayout,src.size(),src.data(),s,dst.data()));
  if(ten){bad=s;++bad.plane[0].offset;assert(!vtsurface::copy(l,src.size(),src.data(),bad,dst.data()));}

  // A noncoherent reader retains this backing across two producer rewrites.
  // memcpy/ordering alone leaves its RAM view stale; publication must cover
  // every active byte before the completion fence, without writing padding.
  struct Device {
    const std::vector<uint8_t> &cpu;
    std::vector<uint8_t> ram;
    unsigned cleans=0,completions=0;
    Device(const std::vector<uint8_t> &v):cpu(v),ram(v.size(),0){}
    void clean(const uint8_t *p,uint64_t bytes){
      const size_t offset=size_t(p-cpu.data());
      assert(offset<=cpu.size() && bytes<=cpu.size()-offset);
      memcpy(ram.data()+offset,p,size_t(bytes));++cleans;
    }
    void finish(){assert(cleans==2*(completions+1));++completions;}
  } device(dst);
  for(unsigned epoch=0;epoch<2;++epoch){
    for(unsigned p=0;p<2;++p)for(unsigned y=0;y<(p?l.height/2:l.height);++y){
      auto at=(p?l.uvOffset:l.yOffset)+uint64_t(y)*l.stride;
      for(unsigned x=0;x<rowBytes;++x)src[at+x]^=0xff;
    }
    const auto old=device.ram;
    assert(vtsurface::copy(l,src.size(),src.data(),s,dst.data()));
    bool stale=false;
    for(size_t i=0;i<dst.size();++i)if(touched[i] && old[i]!=dst[i])stale=true;
    assert(stale && device.ram==old); // the old copy+DSB contract cannot publish
    assert(vtsurface::copyAndPublish(l,src.size(),src.data(),s,dst.data(),device));
    for(size_t i=0;i<dst.size();++i)if(touched[i])assert(device.ram[i]==dst[i]);
  }
  const auto cleanCalls=device.cleans;
  assert(!vtsurface::copyAndPublish(l,l.bytes-1,src.data(),s,dst.data(),device));
  assert(device.cleans==cleanCalls && device.completions==2);
}
int main() {
  copyTest(false);copyTest(true);
  Peer old;
  assert(old.lookup(30)); old.close(); assert(!old.lookup(30));
  Peer p; Lease lease;
  assert(!lease.acquire(0));
  for (unsigned frame=0;frame<120;++frame) {
    // Repeated IDs and fresh IDs; the client never stops between pictures.
    const uint32_t id=frame<60?30:frame;
    assert(p.lookup(id) && lease.acquire(id));
    assert(!lease.acquire(id+1)); // at most one output reference pair
    // Identical disposal on success, invalid layout, copy failure or cancel.
    assert(lease.dispose(p) && !lease.held() && !p.handle && !p.buffer);
    assert(p.creator);
    const auto calls=p.calls.size(); assert(lease.dispose(p));
    assert(p.calls.size()==calls); // no duplicate decrement
  }
  assert(p.calls.size()==240);
  for (unsigned i=0;i<240;i+=2)
    assert(p.calls[i]==Invalidate && p.calls[i+1]==Release);
  for (const uint32_t failure : {Invalidate,Release}) {
    Peer bad; Lease pending; bad.fail=failure;
    assert(bad.lookup(30) && pending.acquire(30));
    assert(!pending.dispose(bad));
    assert(bad.calls.size()==2); // second obligation survives first failure
    assert(pending.held() && !pending.acquire(31));
    assert(pending.handle==(failure==Invalidate));
    assert(pending.buffer==(failure==Release));
    bad.close(); pending.closed(); assert(!pending.held());
  }
  p.close(); lease.closed();
  puts("PianoVideo VT surface tests: NV12/P010 gradients arbitrary-stride/offset padding 4K/64MiB bounds reused-noncoherent-publication repeated-ID/ref-pair/error/cancel pass");
}
