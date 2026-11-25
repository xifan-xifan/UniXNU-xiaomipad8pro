// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Host-only actual CoreMedia demux/format and bounded AVC/HEVC admission audit.
// Reads compressed samples; never creates a VT decoder or contacts the tablet.
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#include <CoreMedia/CMFormatDescriptionBridge.h>
#include <CommonCrypto/CommonDigest.h>
#include <cstdio>
#include <vector>
#include <string>
#include "vt/PianoVideoVTContract.h"
namespace c = pianovideo::vtcontract;
namespace w = vtwire;
static std::string digest(const std::vector<uint8_t> &b) {
  uint8_t d[32];CC_SHA256(b.data(),CC_LONG(b.size()),d);char text[65];
  for(unsigned i=0;i<32;++i)snprintf(text+2*i,3,"%02x",d[i]);return text;
}
static bool audit(NSString *path, bool allSamples) {
  AVURLAsset *asset=[AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:@{AVURLAssetPreferPreciseDurationAndTimingKey:@YES}];
  NSArray<AVAssetTrack *> *tracks=[asset tracksWithMediaType:AVMediaTypeVideo];if(tracks.count!=1)return false;
  NSError *error=nil;AVAssetReader *reader=[[AVAssetReader alloc] initWithAsset:asset error:&error];
  AVAssetReaderTrackOutput *output=[[AVAssetReaderTrackOutput alloc] initWithTrack:tracks[0] outputSettings:nil];
  output.alwaysCopiesSampleData=YES;if(!reader||![reader canAddOutput:output])return false;
  [reader addOutput:output];if(![reader startReading])return false;
  pianovideo::codec::Config config;unsigned count=0,markers=0;CMTime previous=kCMTimeInvalid;
  while(CMSampleBufferRef sample=[output copyNextSampleBuffer]) {
    CMBlockBufferRef data=CMSampleBufferGetDataBuffer(sample);auto format=CMSampleBufferGetFormatDescription(sample);
    if(!data&&!format&&CMSampleBufferGetNumSamples(sample)==0){CFRelease(sample);if(++markers>8)return false;continue;}
    if(!data||!format||CMSampleBufferGetNumSamples(sample)!=1){CFRelease(sample);return false;}
    CMBlockBufferRef description=nullptr;
    OSStatus rc=CMVideoFormatDescriptionCopyAsBigEndianImageDescriptionBlockBuffer(kCFAllocatorDefault,format,kCFStringEncodingUTF8,nullptr,&description);
    if(rc||!description){CFRelease(sample);return false;}
    const size_t bytes=CMBlockBufferGetDataLength(description);
    std::vector<uint8_t> message(w::HeaderBytes+16+bytes);uint8_t uuid[16]={};w::Writer writer(message.data(),uint32_t(message.size()));
    writer.header(w::cc("dsta"),w::FlagRequest,uuid,1);
    auto *p=message.data()+w::HeaderBytes;w::put32(p,uint32_t(bytes+16));w::put32(p+4,w::cc("para"));w::put32(p+8,w::cc(",fds"));w::put32(p+12,w::cc("=fds"));
    rc=CMBlockBufferCopyDataBytes(description,0,bytes,p+16);CFRelease(description);
    c::FormatReport report;pianovideo::codec::Config incoming;
    if(rc||!c::formatRequest(message.data(),uint32_t(message.size()),report,&incoming)){
      printf("V8_AUDIT_REJECT stage=%s dimensions=%ux%u\n",report.decision,report.width,report.height);CFRelease(sample);return false;
    }
    if(!count){config=incoming;printf("%s_FORMAT active=%ux%u coded=%ux%u crop=%u,%u,%u,%u profile=%u level=%u refs=%u reorder=%u dpb=%u\n",config.isHevc()?"V9":"V8",
      config.s.width,config.s.height,config.s.codedWidth,config.s.codedHeight,config.s.left,config.s.top,config.s.right,config.s.bottom,
      config.s.profile,config.s.level,config.s.refs,config.s.maxReorder,config.s.maxDpb);
      CFShow(CMFormatDescriptionGetExtensions(format));
    }else if(!config.same(incoming)){CFRelease(sample);return false;}
    std::vector<uint8_t> compressed(CMBlockBufferGetDataLength(data)),normalized(c::kMaxAuBytes);
    if(CMBlockBufferCopyDataBytes(data,0,compressed.size(),compressed.data())){CFRelease(sample);return false;}
    const uint32_t n=c::normalize(compressed.data(),uint32_t(compressed.size()),normalized.data(),uint32_t(normalized.size()),config,count==0);
    if(!n){printf("V8_AUDIT_REJECT packet=%u stage=AU\n",count);CFRelease(sample);return false;}normalized.resize(n);
    const CMTime pts=CMSampleBufferGetOutputPresentationTimeStamp(sample),dts=CMSampleBufferGetDecodeTimeStamp(sample);
    if(!CMTIME_IS_NUMERIC(pts)||!CMTIME_IS_NUMERIC(dts)||(CMTIME_IS_VALID(previous)&&CMTimeCompare(previous,dts)>=0)){CFRelease(sample);return false;}
    previous=dts;
    printf("%s_PACKET index=%u bytes=%zu compressed_sha=%s normalized_bytes=%u normalized_sha=%s pts=%lld/90000 dts=%lld/90000\n",config.isHevc()?"V9":"V8",count,compressed.size(),digest(compressed).c_str(),n,digest(normalized).c_str(),
      CMTimeConvertScale(pts,90000,kCMTimeRoundingMethod_Default).value,CMTimeConvertScale(dts,90000,kCMTimeRoundingMethod_Default).value);
    ++count;CFRelease(sample);
  }
  const bool good=reader.status==AVAssetReaderStatusCompleted&&count&&(allSamples||count==60);
  printf("%s_AUDIT_%s samples=%u no_VT_session=1\n",config.isHevc()?"V9":"V8",good?"PASS":"FAIL",count);return good;
}
int main(int argc,char **argv){
  if(argc<2)return 2;setvbuf(stdout,nullptr,_IOLBF,0);
  const bool allSamples=!strcmp(argv[1],"--all");if(allSamples&&argc<3)return 2;
  @autoreleasepool {for(int i=allSamples?2:1;i<argc;++i)if(!audit([NSString stringWithUTF8String:argv[i]],allSamples))return 1;}
  return 0;
}
