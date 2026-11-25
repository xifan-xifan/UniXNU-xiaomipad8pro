// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Bounded AVC streaming qualification, never a default decoder registration.
// --self-test audits CoreMedia and, when exported, the private PV serializer.
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#include <CoreMedia/CMFormatDescriptionBridge.h>
#include <VideoToolbox/VideoToolbox.h>
#include <IOSurface/IOSurface.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "VideoFixture.h"
#include "vt/PianoVideoVTContract.h"
namespace {
namespace contract = pianovideo::vtcontract;
std::atomic<unsigned> failures{0};
unsigned serializerAudits=0,serializerSkips=0;
bool check(const char *stage, bool good, int status = 0) {
  std::printf("video-v6: %s %s status=%d\n",good?"PASS":"FAIL",stage,status);
  if (!good) ++failures;
  return good;
}
uint64_t nowUs() { static mach_timebase_info_data_t t = []{mach_timebase_info_data_t v;mach_timebase_info(&v);return v;}();return mach_absolute_time()*t.numer/t.denom/1000; }
void timeout(int) { const char m[]="video-v6: FAIL process-timeout status=124\n";(void)!write(1,m,sizeof m-1);_exit(124); }
std::string hash(const void *p,size_t n) { uint8_t d[32];CC_SHA256(p,CC_LONG(n),d);char h[65];for(unsigned i=0;i<32;++i)std::snprintf(h+2*i,3,"%02x",d[i]);return h; }
std::string imageHash(CVImageBufferRef image) {
  if (!image || CVPixelBufferGetPixelFormatType(image)!=kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
      CVPixelBufferGetWidth(image)!=320 || CVPixelBufferGetHeight(image)!=240 ||
      CVPixelBufferGetPlaneCount(image)!=2 || !CVPixelBufferGetIOSurface(image) ||
      CVPixelBufferGetWidthOfPlane(image,0)!=320 || CVPixelBufferGetHeightOfPlane(image,0)!=240 ||
      CVPixelBufferGetWidthOfPlane(image,1)!=160 || CVPixelBufferGetHeightOfPlane(image,1)!=120) return {};
  if(CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly))return {};
  CC_SHA256_CTX ctx;CC_SHA256_Init(&ctx);bool good=true;
  for(unsigned p=0;p<2;++p) {
    auto *base=static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(image,p));
    const size_t stride=CVPixelBufferGetBytesPerRowOfPlane(image,p);
    if(!base||stride<320){good=false;break;}
    for(unsigned y=0;y<(p?120:240);++y)CC_SHA256_Update(&ctx,base+y*stride,320);
  }
  uint8_t d[32];CC_SHA256_Final(d,&ctx);if(CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly))good=false;
  char h[65];for(unsigned i=0;i<32;++i)std::snprintf(h+2*i,3,"%02x",d[i]);return good?std::string(h):std::string();
}
struct Clip {
  const char *name = nullptr;
  std::vector<CMSampleBufferRef> samples;
  std::vector<std::string> gold;
  std::vector<int64_t> pts,dts;
  std::vector<bool> idr,reference;
  CMVideoFormatDescriptionRef format = nullptr;
  ~Clip(){for(auto s:samples)CFRelease(s);if(format)CFRelease(format);}
};
std::vector<uint8_t> compressed(CMSampleBufferRef sample) {
  CMBlockBufferRef b=CMSampleBufferGetDataBuffer(sample);if(!b)return {};
  std::vector<uint8_t> v(CMBlockBufferGetDataLength(b));
  if(CMBlockBufferCopyDataBytes(b,0,v.size(),v.data()))v.clear();return v;
}
bool nals(const std::vector<uint8_t>& v,bool &idr,bool &reference) {
  idr=reference=false;size_t at=0;bool picture=false;
  while(at<v.size()) {
    if(v.size()-at<4)return false;const uint32_t n=vtwire::be32(v.data()+at);at+=4;
    if(!n||n>v.size()-at)return false;const uint8_t h=v[at],type=h&31;
    if(type==1||type==5){picture=true;idr|=type==5;reference|=(h&0x60)!=0;}at+=n;
  }
  return picture;
}
NSDictionary *manifest(NSString *folder) {
  NSData *data=[NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"manifest.json"]];NSError *error=nil;
  id value=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:&error]:nil;
  if(!check("manifest-json",[value isKindOfClass:[NSDictionary class]]))return nil;return value;
}
bool load(NSString *folder,NSDictionary *m,const char *name,Clip &clip) {
  clip.name=name;NSString *key=[NSString stringWithUTF8String:name];NSDictionary *ref=m[@"fixtures"][key];
  NSString *file=[folder stringByAppendingPathComponent:[key stringByAppendingString:@".mp4"]];NSData *data=[NSData dataWithContentsOfFile:file];
  if(!check("fixture-file-sha",data && hash(data.bytes,data.length)==[ref[@"file_sha256"] UTF8String]))return false;
  AVURLAsset *asset=[AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:file] options:@{AVURLAssetPreferPreciseDurationAndTimingKey:@YES}];
  NSArray<AVAssetTrack *> *tracks=[asset tracksWithMediaType:AVMediaTypeVideo];if(!check("one-video-track",tracks.count==1))return false;
  NSError *error=nil;AVAssetReader *reader=[[AVAssetReader alloc] initWithAsset:asset error:&error];
  AVAssetReaderTrackOutput *output=[[AVAssetReaderTrackOutput alloc] initWithTrack:tracks[0] outputSettings:nil];
  output.alwaysCopiesSampleData=YES;if(!check("compressed-reader",reader&&[reader canAddOutput:output]))return false;
  [reader addOutput:output];if(!check("reader-start",[reader startReading]))return false;
  NSArray *packets=ref[@"packets"],*gold=ref[@"output"];
  bool ok=true;int64_t mediaOffset=0;unsigned i=0,editMarkers=0;CMTime previous=kCMTimeInvalid;
  while(CMSampleBufferRef sample=[output copyNextSampleBuffer]) {
    // AVAssetReader emits zero-sample edit-boundary notifications before
    // media. They are container events, never AVC AUs sent to the decoder.
    if(CMSampleBufferGetNumSamples(sample)==0 && !CMSampleBufferGetDataBuffer(sample) &&
       !CMSampleBufferGetFormatDescription(sample) &&
       (CFEqual(CMGetAttachment(sample,CFSTR("EditBoundary"),nullptr) ?: kCFBooleanFalse,kCFBooleanTrue) ||
        CFEqual(CMGetAttachment(sample,CFSTR("DrainAfterDecoding"),nullptr) ?: kCFBooleanFalse,kCFBooleanTrue) ||
        CMGetAttachment(sample,CFSTR("PostNotificationWhenConsumed"),nullptr) ||
        CFEqual(CMGetAttachment(sample,CFSTR("EmptyMedia"),nullptr) ?: kCFBooleanFalse,kCFBooleanTrue))) {
      ++editMarkers;CFRelease(sample);if(editMarkers>8){ok=false;break;}continue;
    }
    const auto bytes=compressed(sample);CMTime p=CMSampleBufferGetPresentationTimeStamp(sample),d=CMSampleBufferGetDecodeTimeStamp(sample);
    const CMTime outputPts=CMSampleBufferGetOutputPresentationTimeStamp(sample);
    const int64_t rawPts=CMTimeConvertScale(p,90000,kCMTimeRoundingMethod_Default).value;
    const int64_t pts=CMTimeConvertScale(outputPts,90000,kCMTimeRoundingMethod_Default).value;
    if(!i)mediaOffset=rawPts-pts;
    const int64_t dts=CMTimeConvertScale(CMTIME_IS_VALID(d)?d:p,90000,kCMTimeRoundingMethod_Default).value;
    bool idr=false,isref=false;
    ok &= CMSampleBufferGetNumSamples(sample)==1 && i<packets.count && nals(bytes,idr,isref);
    if(i<packets.count) {
      NSDictionary *packet=packets[i];const std::string expected=[[packet[@"data_hash"] substringFromIndex:7] UTF8String];
      ok &= hash(bytes.data(),bytes.size())==expected && pts==[packet[@"pts"] longLongValue] && rawPts==pts+mediaOffset && dts==[packet[@"dts"] longLongValue]+mediaOffset &&
        bytes.size()==uint64_t([packet[@"size"] longLongValue]) && idr==[packet[@"flags"] hasPrefix:@"K"];
    }
    ok &= !CMTIME_IS_VALID(previous)||CMTimeCompare(CMTIME_IS_VALID(d)?d:p,previous)>0;previous=CMTIME_IS_VALID(d)?d:p;
    const unsigned display=unsigned(pts/3000);std::string sha;
    if(pts>=0 && pts%3000==0 && display<gold.count && [gold[display][@"pts"] longLongValue]==pts)sha=[gold[display][@"sha256"] UTF8String];else ok=false;
    CMVideoFormatDescriptionRef description=CMSampleBufferGetFormatDescription(sample);
    if(!description){CFRelease(sample);ok=false;break;}
    if(!clip.format){clip.format=(CMVideoFormatDescriptionRef)CFRetain(description);}
    else ok &= CFEqual(clip.format,CMSampleBufferGetFormatDescription(sample));
    clip.samples.push_back(sample);clip.gold.push_back(sha);clip.pts.push_back(pts);clip.dts.push_back(dts);clip.idr.push_back(idr);clip.reference.push_back(isref);++i;
  }
  std::printf("video-v6: demux-state ok=%u markers=%u status=%ld count=%u packets=%lu gold=%lu idr0=%u idr30=%u\n",unsigned(ok),editMarkers,long(reader.status),i,(unsigned long)packets.count,(unsigned long)gold.count,unsigned(clip.idr[0]),unsigned(clip.idr[30]));
  ok &= reader.status==AVAssetReaderStatusCompleted && editMarkers<=8 && i==60 && packets.count==60 && gold.count==60 && clip.idr[0] && clip.idr[30];
  std::printf("video-v6: fixture=%s samples=%u closed-gop-boundary=30 dts-order=1 media-offset=%lld/90000 output-pts-audited=1 software-truth=1\n",name,i,(long long)mediaOffset);
  return check("demux-every-packet-pts-dts-sha",ok);
}
std::vector<uint8_t> formatWire(CMVideoFormatDescriptionRef format) {
  CMBlockBufferRef b=nullptr;OSStatus rc=CMVideoFormatDescriptionCopyAsBigEndianImageDescriptionBlockBuffer(kCFAllocatorDefault,format,kCFStringEncodingUTF8,nullptr,&b);
  if(rc||!b)return {};std::vector<uint8_t> v(CMBlockBufferGetDataLength(b));rc=CMBlockBufferCopyDataBytes(b,0,v.size(),v.data());CFRelease(b);if(rc)v.clear();return v;
}
std::vector<uint8_t> wire(uint32_t type,uint32_t key,uint32_t kind,const std::vector<uint8_t>& value) {
  using namespace vtwire;std::vector<uint8_t> m(HeaderBytes+16+value.size());uint8_t uuid[16]={};Writer w(m.data(),uint32_t(m.size()));w.header(type,FlagRequest,uuid,103);
  uint8_t *p=m.data()+HeaderBytes;put32(p,uint32_t(16+value.size()));put32(p+4,cc("para"));put32(p+8,key);put32(p+12,kind);memcpy(p+16,value.data(),value.size());return m;
}
bool audit(Clip &clip,bool publicOnly) {
  using namespace vtwire;
  auto fmt=formatWire(clip.format),m=wire(cc("dsta"),cc(",fds"),cc("=fds"),fmt);contract::FormatReport report;pianovideo::codec::Config config;
  if(!check("real-coremedia-format-contract",contract::formatRequest(m.data(),uint32_t(m.size()),report,&config)))return false;
  // Some hosts export these private helpers; 27.3 keeps them local. Missing
  // exports only prevent this offline audit. Public VT decode still calls the
  // real serializer internally, and qualification must check its actual output.
  using Create=OSStatus(*)(uint32_t,uint32_t,const uint8_t*,uint64_t,CMBlockBufferRef*,CFTypeRef*);
  using Append=OSStatus(*)(CFTypeRef,uint32_t,uint32_t,uint32_t,CMSampleBufferRef,void*,CMBlockBufferRef*);
  using Int=OSStatus(*)(CFTypeRef,uint32_t,int64_t);
  using UInt=OSStatus(*)(CFTypeRef,uint32_t,uint32_t);
  auto symbol=[&](const char *name) {
    void *p=publicOnly?nullptr:dlsym(RTLD_DEFAULT,name);
    std::printf("video-v6: serializer-symbol clip=%s name=%s available=%u forced-public-only=%u\n",clip.name,name,unsigned(p!=nullptr),unsigned(publicOnly));
    return p;
  };
  auto create=reinterpret_cast<Create>(symbol("VTParavirtualizationCreateMessageAndByteStream"));
  auto append=reinterpret_cast<Append>(symbol("VTParavirtualizationMessageAppendCMSampleBufferWithLimitAndCopyRemainingData"));
  auto integer=reinterpret_cast<Int>(symbol("VTParavirtualizationMessageAppendSInt64"));
  auto flags=reinterpret_cast<UInt>(symbol("VTParavirtualizationMessageAppendUInt32"));
  if(!create||!append||!integer||!flags) {
    ++serializerSkips;
    std::printf("video-v6: SKIP real-sample-serializer clip=%s reason=private-exports-unavailable actual-wire-audited=0 qualification-path=VTDecompressionSessionDecodeFrame\n",clip.name);
    return true;
  }
  ++serializerAudits;
  check("real-sample-serializer",true);
  bool ok=true;
  for(unsigned i=0;i<clip.samples.size();++i) {
    CMBlockBufferRef b=nullptr,remaining=nullptr;CFTypeRef stream=nullptr;uint8_t uuid[16]={};
    OSStatus rc=create(cc("dfrm"),FlagRequest,uuid,1000+i,&b,&stream);
    if(!rc)rc=integer(stream,cc(",frm"),int64_t(0x56360000+i));
    if(!rc)rc=flags(stream,cc(",dfl"),1);
    if(!rc)rc=append(stream,cc(",sbf"),6,2*1024*1024,clip.samples[i],nullptr,&remaining);
    std::vector<uint8_t> request;
    if(!rc&&b&&!remaining){request.resize(CMBlockBufferGetDataLength(b));rc=CMBlockBufferCopyDataBytes(b,0,request.size(),request.data());}
    if(remaining)CFRelease(remaining);if(stream)CFRelease(stream);if(b)CFRelease(b);
    if(rc||request.empty()){
      std::printf("video-v6: serializer-call-failed clip=%s sample=%u status=%d bytes=%zu\n",clip.name,i,int(rc),request.size());
      ok=false;continue;
    }
    contract::InputReport input;const bool accepted=contract::inputRequest(request.data(),uint32_t(request.size()),input,&config);
    if(!accepted)std::printf("video-v6: serializer rejection clip=%s sample=%u decision=%s bytes=%zu\n",clip.name,i,input.decision,request.size());
    ok &= accepted && input.hasToken && input.token==int64_t(0x56360000+i);
    std::vector<uint8_t> normalized(contract::kMaxAuBytes);
    ok &= accepted && contract::normalize(input.data,input.bytes,normalized.data(),uint32_t(normalized.size()),config,i==0)>0;
  }
  return check("real-coremedia-all-60-samples-contract",ok);
}
struct Frame {
  uint64_t token=0,submitUs=0;CMTime pts=kCMTimeInvalid,duration=kCMTimeInvalid;std::string gold;
  OSStatus submissionStatus=0;unsigned calls=0,images=0,errors=0;bool noOutput=false,reject=false,submitted=false;
};
bool rejectedDisposed(const Frame &f) { return f.images==0 && (f.submissionStatus!=0 || f.errors!=0); }
void rejectionContractTest() {
  Frame f;f.reject=true;
  check("async-rejection-still-pending",!rejectedDisposed(f));
  f.errors=1;check("async-rejection-callback-disposes",rejectedDisposed(f));
  f.images=1;check("rejected-image-never-accepted",!rejectedDisposed(f));
  f.images=f.errors=0;f.submissionStatus=-12909;check("sync-rejection-status-disposes",rejectedDisposed(f));
}
struct Held { CVPixelBufferRef image=nullptr;std::string sha; };
struct Context {
  pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
  Frame frames[64];unsigned count=0,inflight=0,high=0,callbacks=0,images=0,nulls=0,errors=0,beforeFinish=0;
  uint64_t epoch=0,startUs=0,lastUs=0,longestUs=0,latencySum=0,maxLatency=0;bool finished=false,cancelling=false,invalidated=false;
  const char *phase=nullptr;std::vector<Held> held;
  ~Context(){for(auto &h:held)if(h.image)CVPixelBufferRelease(h.image);pthread_cond_destroy(&changed);pthread_mutex_destroy(&lock);}
};
void releaseHeld(Context &ctx) {
  for(auto &h:ctx.held){check("retained-surface-unchanged",imageHash(h.image)==h.sha);CVPixelBufferRelease(h.image);}ctx.held.clear();
}
void output(void *arg,void *source,OSStatus status,VTDecodeInfoFlags flags,CVImageBufferRef image,CMTime pts,CMTime duration) {
  auto &ctx=*static_cast<Context *>(arg);pthread_mutex_lock(&ctx.lock);
  Frame *frame=nullptr;for(unsigned i=0;i<ctx.count;++i)if(source==&ctx.frames[i])frame=&ctx.frames[i];
  const bool known=frame&&frame->submitted&&!ctx.invalidated;const uint64_t token=known?frame->token:0;
  check("known-live-token",known);if(!known){pthread_mutex_unlock(&ctx.lock);return;}
  ++frame->calls;++ctx.callbacks;check("token-exactly-once",frame->calls==1);if(frame->calls==1&&ctx.inflight)--ctx.inflight;
  const uint64_t when=nowUs(),latency=when-frame->submitUs;ctx.latencySum+=latency;ctx.maxLatency=std::max(ctx.maxLatency,latency);
  if(ctx.lastUs)ctx.longestUs=std::max(ctx.longestUs,when-ctx.lastUs);ctx.lastUs=when;
  std::string sha;if(image)sha=imageHash(image);
  const bool timing=CMTimeCompare(frame->pts,pts)==0&&CMTimeCompare(frame->duration,duration)==0;
  const bool good=!status&&image&&!frame->noOutput&&!frame->reject&&timing&&sha==frame->gold;
  if(image){++frame->images;++ctx.images;if(!ctx.finished)++ctx.beforeFinish;}
  else if(!status&&frame->noOutput){++ctx.nulls;}
  else {++frame->errors;++ctx.errors;}
  if(!ctx.cancelling)check(frame->reject?"rejected-completion":frame->noOutput?"no-output-disposition":"token-pts-software-sha",
    frame->reject?(status!=0&&!image):frame->noOutput?(!status&&!image&&timing):good,status);
  // Retain at most two delivered surfaces across subsequent pool allocations.
  // Rehash before release so RAW recycling cannot silently alter client pixels.
  if(good&&ctx.held.size()<2){CVPixelBufferRetain(image);ctx.held.push_back({image,sha});}
  else if(good&&ctx.callbacks%8==0)releaseHeld(ctx);
  std::printf("video-v6: frame phase=%s epoch=%llu token=%llu callback=%u status=%d flags=%u pts=%lld/%d duration=%lld/%d image=%u sha256=%s latency-us=%llu\n",
    ctx.phase,(unsigned long long)ctx.epoch,(unsigned long long)token,frame->calls,int(status),unsigned(flags),(long long)pts.value,pts.timescale,
    (long long)duration.value,duration.timescale,image!=nullptr,sha.empty()?"none":sha.c_str(),(unsigned long long)latency);
  pthread_cond_broadcast(&ctx.changed);pthread_mutex_unlock(&ctx.lock);
}
CFStringRef key(const char *name){void *p=dlsym(RTLD_DEFAULT,name);return p?*static_cast<const CFStringRef *>(p):nullptr;}
CFDictionaryRef specification() {
  CFStringRef require=key("kVTVideoDecoderSpecification_RequireParavirtualizedDecoder"),allow=key("kVTVideoDecoderSpecification_AllowParavirtualizedDecoder");
  if(!check("require-pv-keys",require&&allow))return nullptr;
  const void *keys[]={kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,require,allow},*values[]={kCFBooleanTrue,kCFBooleanTrue,kCFBooleanTrue};
  return CFDictionaryCreate(kCFAllocatorDefault,keys,values,3,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
}
VTDecompressionSessionRef create(CMVideoFormatDescriptionRef format,CFDictionaryRef spec,Context &ctx,bool wanted=true) {
  NSDictionary *attrs=@{(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey:@{}};
  VTDecompressionOutputCallbackRecord cb={output,&ctx};VTDecompressionSessionRef session=nullptr;const uint64_t start=nowUs();
  const OSStatus rc=VTDecompressionSessionCreate(kCFAllocatorDefault,format,spec,(__bridge CFDictionaryRef)attrs,&cb,&session);
  check(wanted?"create-required-hardware-pv":"exclusive-second-rejected",wanted?!rc&&session:rc&&!session,rc);
  std::printf("video-v6: create phase=%s epoch=%llu elapsed-us=%llu status=%d\n",ctx.phase,(unsigned long long)ctx.epoch,(unsigned long long)(nowUs()-start),int(rc));
  if(session&&wanted){CFTypeRef hardware=nullptr;OSStatus query=VTSessionCopyProperty(session,kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,kCFAllocatorDefault,&hardware);
    check("using-hardware",!query&&hardware&&CFEqual(hardware,kCFBooleanTrue),query);if(hardware)CFRelease(hardware);}
  return session;
}
bool credit(Context &ctx) {
  pthread_mutex_lock(&ctx.lock);bool ok=true;
  while(ctx.inflight>=7){timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=5;
    if(pthread_cond_timedwait(&ctx.changed,&ctx.lock,&until)){ok=false;break;}}
  pthread_mutex_unlock(&ctx.lock);return check("callback-credit-progress",ok);
}
bool submit(VTDecompressionSessionRef session,CMSampleBufferRef sample,Context &ctx,unsigned index,uint32_t flags,const std::string& gold,bool suppress=false,bool reject=false) {
  Frame &f=ctx.frames[index];f.token=(ctx.epoch<<32)|uint64_t(index+1);f.pts=CMSampleBufferGetOutputPresentationTimeStamp(sample);
  f.duration=CMSampleBufferGetOutputDuration(sample);f.gold=gold;f.noOutput=suppress;f.reject=reject;
  pthread_mutex_lock(&ctx.lock);f.submitted=true;f.submitUs=nowUs();++ctx.inflight;ctx.high=std::max(ctx.high,ctx.inflight);pthread_mutex_unlock(&ctx.lock);
  VTDecodeInfoFlags info=0;const OSStatus rc=VTDecompressionSessionDecodeFrame(session,sample,VTDecodeFrameFlags(flags),&f,&info);f.submissionStatus=rc;
  auto bytes=compressed(sample);const auto digest=hash(bytes.data(),bytes.size());
  std::printf("video-v6: submit phase=%s epoch=%llu index=%u token=%llu flags=%u pts=%lld/%d dts=%lld/%d bytes=%zu sha256=%s status=%d info=%u\n",
    ctx.phase,(unsigned long long)ctx.epoch,index,(unsigned long long)f.token,flags,(long long)f.pts.value,f.pts.timescale,
    (long long)CMSampleBufferGetDecodeTimeStamp(sample).value,CMSampleBufferGetDecodeTimeStamp(sample).timescale,bytes.size(),digest.c_str(),int(rc),unsigned(info));
  // Synchronous transport failures can retire in Apple's synchronous error path;
  // count this rejection once if no callback occurred. Accepted work may not.
  if(rc){pthread_mutex_lock(&ctx.lock);if(!f.calls&&ctx.inflight)--ctx.inflight;pthread_mutex_unlock(&ctx.lock);}
  return reject ? true : check("decode-submit",!rc,rc);
}
bool drain(VTDecompressionSessionRef session,Context &ctx,const char *stage) {
  pthread_mutex_lock(&ctx.lock);ctx.finished=true;const unsigned early=ctx.beforeFinish;pthread_mutex_unlock(&ctx.lock);
  OSStatus rc=VTDecompressionSessionFinishDelayedFrames(session);check(stage,!rc,rc);
  OSStatus wait=VTDecompressionSessionWaitForAsynchronousFrames(session);check("barrier-wait",!wait,wait);
  OSStatus repeat=VTDecompressionSessionFinishDelayedFrames(session);check("barrier-repeat-idempotent",!repeat,repeat);
  pthread_mutex_lock(&ctx.lock);const bool zero=ctx.inflight==0;const auto pending=ctx.inflight;releaseHeld(ctx);pthread_mutex_unlock(&ctx.lock);
  std::printf("video-v6: barrier phase=%s epoch=%llu before-first-finish=%u pending=%u\n",ctx.phase,(unsigned long long)ctx.epoch,early,pending);
  return check("barrier-token-zero",zero)&&!rc&&!wait&&!repeat;
}
void close(VTDecompressionSessionRef &session,Context &ctx) {
  if(!session)return;VTDecompressionSessionInvalidate(session);pthread_mutex_lock(&ctx.lock);ctx.invalidated=true;const unsigned calls=ctx.callbacks;releaseHeld(ctx);pthread_mutex_unlock(&ctx.lock);
  CFRelease(session);session=nullptr;usleep(20000);pthread_mutex_lock(&ctx.lock);const bool unchanged=ctx.callbacks==calls;pthread_mutex_unlock(&ctx.lock);check("no-callback-after-invalidate",unchanged);
}
CMVideoFormatDescriptionRef metadata(CMVideoFormatDescriptionRef format) {
  CFMutableDictionaryRef ext=CFDictionaryCreateMutableCopy(kCFAllocatorDefault,0,CMFormatDescriptionGetExtensions(format));
  CFDictionarySetValue(ext,kCMFormatDescriptionExtension_FormatName,CFSTR("Piano V6 metadata only"));CMVideoFormatDescriptionRef out=nullptr;
  OSStatus rc=CMVideoFormatDescriptionCreate(kCFAllocatorDefault,kCMVideoCodecType_H264,320,240,ext,&out);CFRelease(ext);check("metadata-format",!rc&&out&&!CFEqual(format,out),rc);return out;
}
bool stream(Clip &clip,Clip &other,CFDictionaryRef spec,const char *phase,uint64_t epoch,uint32_t flags,int suppressed=-1,unsigned begin=0,unsigned end=60,bool split=true) {
  Context ctx;ctx.phase=phase;ctx.epoch=epoch;ctx.startUs=nowUs();ctx.count=end-begin;
  VTDecompressionSessionRef session=create(clip.format,spec,ctx);if(!session)return false;
  check("dcan-identical",VTDecompressionSessionCanAcceptFormatDescription(session,clip.format));CMVideoFormatDescriptionRef meta=metadata(clip.format);
  if(meta){check("dcan-wire-metadata",VTDecompressionSessionCanAcceptFormatDescription(session,meta));CFRelease(meta);}
  check("dcan-different-sps-rejected",!VTDecompressionSessionCanAcceptFormatDescription(session,other.format));
  if(epoch==1){Context duplicate;duplicate.phase="exclusive-control";duplicate.epoch=99;auto second=create(clip.format,spec,duplicate,false);close(second,duplicate);}
  check("empty-finish",VTDecompressionSessionFinishDelayedFrames(session)==0);
  bool ok=true;unsigned submitted=0;
  for(unsigned i=begin;i<end;++i) {
    if(split&&i==30&&i>begin){ok &= drain(session,ctx,"mid-gop-drain");check("callbacks-before-first-finish",ctx.beforeFinish>0);}
    if(!credit(ctx)){ok=false;break;}
    const bool suppress=int(i)==suppressed;
    if(suppress)check("suppressed-picture-is-reference",clip.reference[i]&&!clip.idr[i]&&i+1<end);
    if(!submit(session,clip.samples[i],ctx,i-begin,flags|(suppress?2u:0u),clip.gold[i],suppress)){ok=false;break;}++submitted;
  }
  ok &= drain(session,ctx,"eof-drain");
  pthread_mutex_lock(&ctx.lock);bool complete=ctx.callbacks==submitted&&ctx.errors==0&&ctx.images==submitted-(suppressed>=int(begin)&&suppressed<int(end)?1:0)&&ctx.nulls==(suppressed>=int(begin)&&suppressed<int(end)?1u:0u);
  for(unsigned i=0;i<submitted;++i)complete &= ctx.frames[i].calls==1;
  const uint64_t elapsed=nowUs()-ctx.startUs;std::printf("video-v6: summary phase=%s epoch=%llu submitted=%u callbacks=%u images=%u nulls=%u errors=%u high-water=%u early=%u elapsed-us=%llu fps=%.3f max-callback-us=%llu longest-output-gap-us=%llu\n",
    phase,(unsigned long long)epoch,submitted,ctx.callbacks,ctx.images,ctx.nulls,ctx.errors,ctx.high,ctx.beforeFinish,(unsigned long long)elapsed,
    elapsed?ctx.images*1000000.0/elapsed:0.0,(unsigned long long)ctx.maxLatency,(unsigned long long)ctx.longestUs);pthread_mutex_unlock(&ctx.lock);
  ok &= check("all-token-pts-hashes",complete)&&check("bounded-seven-plus-ack-credit",ctx.high<=7)&&check("stream-output-before-eof",ctx.beforeFinish>0);
  close(session,ctx);return ok;
}
CMSampleBufferRef make(CMVideoFormatDescriptionRef format,const std::vector<uint8_t>& bytes,CMTime pts=CMTimeMake(6001,60000)) {
  CMBlockBufferRef block=nullptr;CMSampleBufferRef sample=nullptr;OSStatus rc=CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,nullptr,bytes.size(),kCFAllocatorDefault,nullptr,0,bytes.size(),0,&block);
  if(!rc)rc=CMBlockBufferReplaceDataBytes(bytes.data(),block,0,bytes.size());size_t n=bytes.size();CMSampleTimingInfo t={CMTimeMake(1,30),pts,kCMTimeInvalid};
  if(!rc)rc=CMSampleBufferCreateReady(kCFAllocatorDefault,block,format,1,1,&t,1,&n,&sample);if(block)CFRelease(block);check("make-single-sample",!rc&&sample,rc);return sample;
}
void single(CFDictionaryRef spec) {
  using namespace pianovideo::session;const uint8_t *sets[]={kAccessUnit+4,kAccessUnit+30};const size_t sizes[]={22,5};CMVideoFormatDescriptionRef format=nullptr;
  if(!check("original-single-format",!CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,2,sets,sizes,4,&format)&&format))return;
  Context ctx;ctx.phase="single-regression";ctx.epoch=10;ctx.count=4;auto session=create(format,spec,ctx);if(!session){CFRelease(format);return;}
  check("single-empty-finish",VTDecompressionSessionFinishDelayedFrames(session)==0);
  const uint8_t bad[]={0,0,2,0,0x65,0x88};std::vector<uint8_t> malformed(bad,bad+sizeof bad),large(2*1024*1024,0x88),bytes(252);
  vtwire::put32(large.data(),uint32_t(large.size()-4));large[4]=0x65;vtwire::put32(bytes.data(),248);memcpy(bytes.data()+4,kAccessUnit+38,248);
  auto sb=make(format,malformed);if(sb){submit(session,sb,ctx,0,1,"",false,true);CFRelease(sb);drain(session,ctx,"malformed-empty-finish");}
  sb=make(format,large);if(sb){submit(session,sb,ctx,1,1,"",false,true);CFRelease(sb);drain(session,ctx,"oversize-empty-finish");}
  sb=make(format,bytes);if(sb){submit(session,sb,ctx,2,0,"",false,true);CFRelease(sb);drain(session,ctx,"flags-zero-empty-finish");}
  sb=make(format,bytes);if(sb){submit(session,sb,ctx,3,1,kGolden);CFRelease(sb);drain(session,ctx,"single-golden-drain");}
  for(unsigned i=0;i<3;++i)check("reject-has-error-disposition",rejectedDisposed(ctx.frames[i]));
  check("original-one-golden-callback",ctx.frames[3].calls==1&&ctx.frames[3].images==1&&ctx.frames[3].errors==0&&ctx.images==1);
  close(session,ctx);CFRelease(format);
}
void cancel(Clip &clip,Clip &other,CFDictionaryRef spec) {
  Context ctx;ctx.phase="cancel-pending";ctx.epoch=20;ctx.count=8;auto session=create(clip.format,spec,ctx);if(!session)return;
  unsigned submitted=0;for(unsigned i=0;i<8;++i)if(submit(session,clip.samples[i],ctx,i,9,clip.gold[i]))++submitted;
  pthread_mutex_lock(&ctx.lock);ctx.cancelling=true;const unsigned pending=ctx.inflight;pthread_mutex_unlock(&ctx.lock);
  std::printf("video-v6: cancel-begin epoch=20 accepted=%u pending=%u no-wait-before-invalidate=1\n",submitted,pending);
  check("cancel-had-accepted-work",submitted==8);check("cancel-had-pending",pending>0);close(session,ctx);
  std::printf("video-v6: cancel-joined epoch=20 callbacks=%u remaining-public-callbacks-not-required=%u\n",ctx.callbacks,ctx.inflight);
  stream(clip,other,spec,"recreate-at-idr",21,9,-1,30,60,false);
}
template<class F> bool phase(const char *name,F action) {
  std::printf("video-v6: phase-begin name=%s\n",name);
  const unsigned before=failures.load();
  if(!action()||failures.load()!=before) {
    if(failures.load()==before)check("phase-result",false);
    std::printf("video-v6: STOP first-failed-phase=%s failures=%u later-phases=not-run\n",name,failures.load());
    return false;
  }
  std::printf("video-v6: phase-pass name=%s\n",name);return true;
}
int run(NSString *folder,bool qualify,bool publicOnly) {
  std::printf("video-v6: begin pid=%d stamp-us=%llu hardware-required=%u pv-required=%u\n",getpid(),(unsigned long long)nowUs(),unsigned(qualify),unsigned(qualify));
  if(!phase("rejection-contract",[]{rejectionContractTest();return true;}))return 1;
  NSDictionary *m=nil;
  if(!phase("manifest",[&]{m=manifest(folder);return m!=nil;}))return 1;
  Clip ip,ibp;
  if(!phase("demux-ip",[&]{return load(folder,m,"ip",ip);})||!phase("demux-ibp",[&]{return load(folder,m,"ibp",ibp);})||
     !phase("audit-ip",[&]{return audit(ip,publicOnly);})||!phase("audit-ibp",[&]{return audit(ibp,publicOnly);}))return 1;
  std::printf("video-v6: preflight-pass actual-wire-audits=%u unavailable-wire-audits=%u mandatory-public-checks=passed\n",serializerAudits,serializerSkips);
  if(!qualify){std::printf("video-v6: PASS self-test failures=0 no-vt-session=1 actual-wire-audits=%u unavailable-wire-audits=%u\n",serializerAudits,serializerSkips);return 0;}
  CFDictionaryRef spec=nullptr;
  if(!phase("decoder-spec",[&]{spec=specification();return spec!=nullptr;}))return 1;
  const bool ok=phase("single-regression",[&]{single(spec);return true;})&&
    phase("ibp-async",[&]{return stream(ibp,ip,spec,"ibp-async",1,1);})&&
    phase("ip-temporal",[&]{return stream(ip,ibp,spec,"ip-temporal",2,9);})&&
    phase("ibp-temporal",[&]{return stream(ibp,ip,spec,"ibp-temporal",3,9);})&&
    phase("ibp-no-output",[&]{return stream(ibp,ip,spec,"ibp-no-output",4,9,1);})&&
    phase("cancel-recreate",[&]{cancel(ibp,ip,spec);return true;});
  CFRelease(spec);if(!ok)return 1;
  std::printf("video-v6: %s qualification failures=%u bounded-stream-correctness-only=1\n",failures?"FAIL":"PASS",failures.load());return failures?1:0;
}
}
int main(int argc,char **argv) {
  setvbuf(stdout,nullptr,_IOLBF,0);signal(SIGALRM,timeout);alarm(300);
  if(argc!=3||(strcmp(argv[1],"--self-test")&&strcmp(argv[1],"--self-test-public-only")&&strcmp(argv[1],"--qualify"))){std::fprintf(stderr,"usage: video-v6 --self-test|--self-test-public-only|--qualify FIXTURE_DIR\n");return 2;}
  @autoreleasepool { return run([NSString stringWithUTF8String:argv[2]],!strcmp(argv[1],"--qualify"),!strcmp(argv[1],"--self-test-public-only")); }
}
