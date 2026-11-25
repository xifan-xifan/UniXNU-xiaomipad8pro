// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Real host CoreMedia format bridge and, when exported, PV sample serializer.
// No VT session, codec execution, device access or target-binary payload.
#include <CoreMedia/CoreMedia.h>
#include <CoreMedia/CMFormatDescriptionBridge.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <cassert>
#include <cstdio>
#include <vector>
#include "HEVCFixture.h"
#include "../vt/PianoVideoVTContract.h"
using namespace vtwire;
namespace contract=pianovideo::vtcontract;
int main(){
  using Create=OSStatus(*)(uint32_t,uint32_t,const uint8_t*,uint64_t,CMBlockBufferRef*,CFTypeRef*);
  using Append=OSStatus(*)(CFTypeRef,uint32_t,uint32_t,uint32_t,CMSampleBufferRef,void*,CMBlockBufferRef*);
  using Int=OSStatus(*)(CFTypeRef,uint32_t,int64_t);
  using UInt=OSStatus(*)(CFTypeRef,uint32_t,uint32_t);
  auto create=reinterpret_cast<Create>(dlsym(RTLD_DEFAULT,"VTParavirtualizationCreateMessageAndByteStream"));
  auto append=reinterpret_cast<Append>(dlsym(RTLD_DEFAULT,"VTParavirtualizationMessageAppendCMSampleBufferWithLimitAndCopyRemainingData"));
  auto integer=reinterpret_cast<Int>(dlsym(RTLD_DEFAULT,"VTParavirtualizationMessageAppendSInt64"));
  auto flags=reinterpret_cast<UInt>(dlsym(RTLD_DEFAULT,"VTParavirtualizationMessageAppendUInt32"));
  const bool serializer=create&&append&&integer&&flags;
  unsigned formats=0,samples=0,declined=0;
  for(unsigned depth:{8u,10u})for(unsigned envelope=0;envelope<3;++envelope){
    hevcfixture::Fixture f(depth,true,envelope==1?3840:1920,envelope==1?2160:1080,envelope?183:120);
    const uint8_t *sets[]={f.vps.data(),f.sps.data(),f.pps.data()};
    const size_t sizes[]={f.vps.size(),f.sps.size(),f.pps.size()};CMFormatDescriptionRef format=nullptr;
    assert(!CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault,3,sets,sizes,4,nullptr,&format));
    assert(CMFormatDescriptionGetMediaSubType(format)==cc("hvc1"));
    CMBlockBufferRef description=nullptr;
    assert(!CMVideoFormatDescriptionCopyAsBigEndianImageDescriptionBlockBuffer(kCFAllocatorDefault,format,kCFStringEncodingUTF8,nullptr,&description));
    const auto bytes=CMBlockBufferGetDataLength(description);std::vector<uint8_t> message(HeaderBytes+16+bytes);uint8_t uuid[16]={};
    Writer w(message.data(),uint32_t(message.size()));w.header(cc("dsta"),FlagRequest,uuid,41);
    auto *parameter=message.data()+HeaderBytes;put32(parameter,uint32_t(16+bytes));put32(parameter+4,cc("para"));put32(parameter+8,cc(",fds"));put32(parameter+12,cc("=fds"));
    assert(!CMBlockBufferCopyDataBytes(description,0,bytes,parameter+16));CFRelease(description);
    pianovideo::codec::Config config;contract::FormatReport report;
    const bool accepted=contract::formatRequest(message.data(),uint32_t(message.size()),report,&config);
    std::printf("V9_REAL_FORMAT depth=%u accepted=%u codec=%08x bytes=%zu decision=%s\n",depth,unsigned(accepted),report.codec,bytes,report.decision);
    if(envelope){
      assert(!accepted&&!strcmp(report.decision,"hevc-native-envelope"));
      // dcan and dsta must use the same early rejection, including when a
      // caller bypasses inventory capability filtering.
      put32(message.data()+8,cc("dcan"));
      Header header;assert(vtwire::header(message.data(),uint32_t(message.size()),header)&&header.type==cc("dcan"));
      assert(!contract::formatRequest(message.data(),uint32_t(message.size()),report,&config));
      assert(!strcmp(report.decision,"hevc-native-envelope"));
      ++declined;CFRelease(format);continue;
    }
    assert(accepted&&config.depth==depth&&config.s.width==1920&&config.s.height==1080);++formats;
    std::vector<uint8_t> au;hevcfixture::append(au,f.slice());CMBlockBufferRef data=nullptr;
    assert(!CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,nullptr,au.size(),kCFAllocatorDefault,nullptr,0,au.size(),0,&data));
    assert(!CMBlockBufferReplaceDataBytes(au.data(),data,0,au.size()));
    CMSampleTimingInfo timing{CMTimeMake(3000,90000),CMTimeMake(0,90000),CMTimeMake(-3000,90000)};
    const size_t sampleSize=au.size();CMSampleBufferRef sample=nullptr;
    assert(!CMSampleBufferCreateReady(kCFAllocatorDefault,data,format,1,1,&timing,1,&sampleSize,&sample));CFRelease(data);
    if(serializer)for(unsigned mode:{0u,2u,1u,3u}){
      CMBlockBufferRef block=nullptr,remaining=nullptr;CFTypeRef stream=nullptr;
      assert(!create(cc("dfrm"),FlagRequest,uuid,50+mode,&block,&stream));
      assert(!integer(stream,cc(",frm"),100+mode)&&!flags(stream,cc(",dfl"),mode));
      assert(!append(stream,cc(",sbf"),6,2*1024*1024,sample,nullptr,&remaining)&&!remaining);
      std::vector<uint8_t> wire(CMBlockBufferGetDataLength(block));assert(!CMBlockBufferCopyDataBytes(block,0,wire.size(),wire.data()));
      contract::InputReport input;const bool valid=contract::inputRequest(wire.data(),uint32_t(wire.size()),input,&config);
      std::printf("V9_REAL_SAMPLE depth=%u flags=%u accepted=%u bytes=%zu decision=%s\n",depth,mode,unsigned(valid),wire.size(),input.decision);
      assert(valid&&input.flags==mode&&input.picture.idr&&input.token==100+mode);++samples;
      CFRelease(stream);CFRelease(block);
    }
    CFRelease(sample);CFRelease(format);
  }
  assert(formats==2&&declined==4);
  std::printf("V9d_REAL_SERIALIZER formats=%u declined=%u samples=%u private-sample-wire=%s no-VT-session=1\n",formats,declined,samples,serializer?"PASS":"SKIP-unexported");
}
