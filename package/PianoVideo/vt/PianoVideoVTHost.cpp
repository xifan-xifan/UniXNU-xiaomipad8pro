// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Native V6 endpoint of the approved UniXNU VT frontend (vt/UPSTREAM).
// One exclusive, bounded AVC/HEVC stream. Selector3 snapshots,
// validates and enqueues. The worker pumps HFI during asynchronous guest waits;
// output lookups/maps are scoped to a copy. Synchronous completion stays on the worker.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <libkern/crypto/sha2.h>
#include "PianoVideoVTWire.h"
#include "PianoVideoVTContract.h"
#include "PianoVideoVTSurface.h"
#include "../VideoStreamState.h"
using namespace vtwire;
using pianovideo::native::Frame;
namespace contract = pianovideo::vtcontract;
extern "C" {
volatile uint32_t droidvm_vthost_sessions, droidvm_vthost_frames_in, droidvm_vthost_frames_out, droidvm_vthost_errors;
volatile uint32_t droidvm_vthost_error_stage, droidvm_vthost_replies, droidvm_vthost_unknown, droidvm_vthost_last_unknown;
volatile int32_t droidvm_vthost_error_value;
int proc_selfpid(void);
}
namespace {
constexpr int kBadData = -12909, kUnsupported = -12910, kBadState = -12903, kDecodeError = -12911, kNoMemory = -12904;
struct Client;
struct Session {
  Client *client = nullptr;
  uint8_t uuid[16] = {};
  uint32_t native = 0, selectedCodec = 0;
  IOLock *lock = nullptr;
  bool started=false, stopping=false, invalidating=false, workerRunning=false, busy=false;
  pianovideo::codec::Config config;
  pianovideo::stream::Ledger frames;
  struct Input { uint8_t *bytes=nullptr; uint32_t size=0; uint64_t reply=0, deadline=0; } inputs[pianovideo::stream::Credits];
  contract::RejectedFrames rejected;
  bool finishWanted=false;
  uint64_t finishRequest=0;
  int firstError=0, closeError=0;
  uint64_t nextRequest=100;
  pianovideo::stream::Guest guest;
  uint32_t replySurfaceID=0;
  bool replyHasSurface=false;
  int replyError=0;
  uint8_t replyUuid[16]={};
  IOUserClient *surfaces=nullptr;
  unsigned surfaceOpens=0,surfaceCloses=0,maps=0;
  pianovideo::vtsurface::Lease surfaceLease;
  uint32_t writeSurfaceID=0;
  unsigned surfaceLookups=0,surfaceInvalidates=0,surfaceReleases=0;
  Frame frame;
  uint64_t currentToken=0;
  bool current=false, currentAccepted=false, currentEmitted=false, currentImage=false;
  int currentError=0;
  unsigned guestHighWater=0;
};
struct Client {
  void *transport = nullptr;
  bool (*send)(void *, const uint8_t *, uint32_t) = nullptr;
  task_t task = nullptr;
  int pid = 0;
  IOLock *lock = nullptr;
  Session *session = nullptr;
};
void failure(Session *s, uint32_t stage, int error) {
  droidvm_vthost_errors = droidvm_vthost_errors + 1;
  droidvm_vthost_error_stage = stage;
  droidvm_vthost_error_value = error;
  if (s) { IOLockLock(s->lock); if (!s->firstError) s->firstError = error; IOLockUnlock(s->lock); }
  IOLog("PianoVideoVT V5a: first-error stage %u status %d session %u\n", stage, error, s ? s->native : 0);
}
bool send(Client *c, const Writer &w, const uint8_t *b) {
  const bool ok = w.ok() && c->send(c->transport,b,w.size());
  if (ok) droidvm_vthost_replies = droidvm_vthost_replies + 1;
  else failure(nullptr,20,kDecodeError);
  return ok;
}
bool status(Client *c, const Header &h, int error) {
  uint8_t b[128]; Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.s32(cc(",err"),error);
  IOLog("PianoVideoVT V5a: reply %c%c%c%c request %llu status %d\n",int(h.type>>24),int((h.type>>16)&255),int((h.type>>8)&255),int(h.type&255),h.request,error);
  return send(c,w,b);
}
void hash(const uint8_t *b, uint32_t n, char out[65]) {
  SHA256_CTX ctx;SHA256_Init(&ctx);SHA256_Update(&ctx,b,n);uint8_t digest[32];SHA256_Final(digest,&ctx);
  const char hex[]="0123456789abcdef";for(unsigned i=0;i<32;++i){out[2*i]=hex[digest[i]>>4];out[2*i+1]=hex[digest[i]&15];}out[64]=0;
}
IOService *surfaceRoot() {
  OSDictionary *matching=IOService::nameMatching("IOSurfaceRoot");
  IOService *root=matching?IOService::waitForMatchingService(matching,1000000000ULL):nullptr;
  OSSafeReleaseNULL(matching);return root;
}
IOReturn surfaceMethod(Session *s,uint32_t selector,uint32_t id,void *output=nullptr,uint32_t *bytes=nullptr) {
  uint64_t scalar=id;
  IOExternalMethodArguments a={};a.version=kIOExternalMethodArgumentsCurrentVersion;a.selector=selector;
  a.scalarInput=&scalar;a.scalarInputCount=1;a.structureOutput=output;a.structureOutputSize=bytes?*bytes:0;
  IOUserClient2022 *client=OSDynamicCast(IOUserClient2022,s->surfaces);
  const IOReturn rc=client?client->externalMethod(selector,reinterpret_cast<IOExternalMethodArgumentsOpaque *>(&a)):
    s->surfaces->callExternalMethod(selector,&a);
  if(bytes)*bytes=uint32_t(a.structureOutputSize);return rc;
}
IOReturn surfaceWriteMethod(Session *s,uint32_t selector,uint32_t id,void *output,uint32_t &bytes){
  const pianovideo::vtsurface::WriteRequest input={id,0,0}; // ordinary writable access
  IOExternalMethodArguments a={};a.version=kIOExternalMethodArgumentsCurrentVersion;a.selector=selector;
  a.structureInput=&input;a.structureInputSize=sizeof input;a.structureOutput=output;a.structureOutputSize=bytes;
  IOUserClient2022 *client=OSDynamicCast(IOUserClient2022,s->surfaces);
  const IOReturn rc=client?client->externalMethod(selector,reinterpret_cast<IOExternalMethodArgumentsOpaque *>(&a)):
    s->surfaces->callExternalMethod(selector,&a);
  bytes=uint32_t(a.structureOutputSize);return rc;
}
bool unlockSurfaceWrite(Session *s){
  if(!s->writeSurfaceID)return true;
  const uint32_t id=s->writeSurfaceID;uint32_t seed=0,bytes=sizeof seed;
  const IOReturn rc=surfaceWriteMethod(s,pianovideo::vtsurface::WriteUnlock,id,&seed,bytes);
  // A successful operation released the lock even if its output is malformed.
  if(!rc)s->writeSurfaceID=0;
  IOLog("PianoVideoVT V9i: write-unlock token %llu id %u status %08x bytes %u seed %u\n",s->frame.token,id,rc,bytes,seed);
  if(rc||bytes!=sizeof seed){failure(s,82,rc?int(rc):kBadData);return false;}
  return true;
}
bool lookupSurface(Session *s,uint32_t id,contract::Surface &v) {
  constexpr uint32_t bytes=0xc68;
  if(!id || s->surfaceLease.held()){failure(s,45,kBadState);return false;}
  if (!s->surfaces) {
    IOService *root=surfaceRoot();
    const IOReturn rc=root?root->newUserClient(s->client->task,s->client->task,0,nullptr,&s->surfaces):kIOReturnNotFound;
    OSSafeReleaseNULL(root);
    if(s->surfaces)++s->surfaceOpens;
    IOLog("PianoVideoVT V5a: IOSurfaceRoot type0 pid %d status %08x client %u\n",s->client->pid,rc,unsigned(s->surfaces!=nullptr));
    if(rc || !s->surfaces){failure(s,40,int(rc));return false;}
  }
  uint8_t *result=static_cast<uint8_t *>(IOMallocZero(bytes));if(!result){failure(s,41,kNoMemory);return false;}
  uint32_t resultBytes=bytes;
  const IOReturn rc=surfaceMethod(s,4,id,result,&resultBytes);
  // A successful method owns both references, even if layout validation fails.
  if(!rc){s->surfaceLease.acquire(id);++s->surfaceLookups;}
  bool ok=!rc && resultBytes==bytes;
  if(ok)ok=contract::surfaceResult(result,bytes,id,v,s->config.s.width,s->config.s.height,s->config.pixelFormat());
  IOLog("PianoVideoVT V5a: surface lookup id %u status %08x result-bytes %u format %08x planes %u valid %u\n",id,rc,resultBytes,v.format,v.planes,unsigned(ok));
  IOLog("PianoVideoVT V6p: lookup token %llu request %llu id %u status %08x held %u\n",s->frame.token,s->guest.request,id,rc,unsigned(s->surfaceLease.held()));
  IOFree(result,bytes);if(!ok)failure(s,42,rc?int(rc):kBadData);return ok;
}
bool copyFrame(Session *s,const Frame &f,const contract::Surface &lookup) {
  if(s->writeSurfaceID || !f.memory || f.memory->getLength()<f.length || !f.cpu || !contract::layout(f.layout,f.length) || !contract::surface(lookup,f.layout.width,f.layout.height,f.layout.format)){failure(s,60,kBadData);return false;}
  constexpr uint32_t resultSize=0xc68;
  uint8_t *result=static_cast<uint8_t *>(IOMallocZero(resultSize));
  if(!result){failure(s,80,kNoMemory);return false;}
  uint32_t bytes=resultSize;
  const IOReturn locked=surfaceWriteMethod(s,pianovideo::vtsurface::WriteLock,lookup.id,result,bytes);
  if(!locked)s->writeSurfaceID=lookup.id;
  contract::Surface v;
  const bool valid=!locked && bytes==resultSize && contract::surfaceResult(result,bytes,lookup.id,v,f.layout.width,f.layout.height,f.layout.format);
  uint32_t seed=0;if(bytes==resultSize)memcpy(&seed,result+0x68,sizeof seed);
  IOFree(result,resultSize);
  IOLog("PianoVideoVT V9i: write-lock token %llu id %u status %08x bytes %u valid %u seed %u\n",f.token,lookup.id,locked,bytes,unsigned(valid),seed);
  if(!valid){failure(s,81,locked?int(locked):kBadData);unlockSurfaceWrite(s);return false;}
  IOMemoryDescriptor *mem=IOMemoryDescriptor::withAddressRange(v.base,v.size,kIODirectionInOut,s->client->task);
  const IOReturn prepared=mem?mem->prepare():kIOReturnNoMemory;
  if(prepared){OSSafeReleaseNULL(mem);failure(s,61,int(prepared));unlockSurfaceWrite(s);return false;}
  ++s->maps;
  IOMemoryMap *map=mem->createMappingInTask(kernel_task,0,kIOMapAnywhere);
  uint8_t *dst=map?reinterpret_cast<uint8_t *>(map->getVirtualAddress()):nullptr;
  struct Publisher {
    uintptr_t line;
    Publisher(){uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));line=uintptr_t(4)<<((ctr>>16)&15);}
    void clean(const uint8_t *start,uint64_t length){
      const uintptr_t end=uintptr_t(start)+length;
      for(uintptr_t a=uintptr_t(start)&~(line-1);a<end;a+=line)__asm__ volatile("dc cvac, %0"::"r"(a):"memory");
    }
    void finish(){__asm__ volatile("dsb sy":::"memory");}
  } publisher;
  bool ok=dst && map->getLength()>=v.size && pianovideo::vtsurface::copyAndPublish(f.layout,f.length,const_cast<const uint8_t *>(f.cpu),v,dst,publisher);
  OSSafeReleaseNULL(map);
  const IOReturn completed=mem->complete();mem->release();--s->maps;
  if(completed){failure(s,83,int(completed));ok=false;}
  const bool unlocked=unlockSurfaceWrite(s);ok=ok&&unlocked;
  if(ok){
    const auto &l=f.layout;
    IOLog("PianoVideoVT V9i: published token %llu IOSurface %u format %08x depth %u clean-to-PoC 1 unlocked 1 before-Demt 1\n",f.token,v.id,l.format,l.bitdepth);
    if(l.codec==cc("avc1"))IOLog("PianoVideoVT V8: copy success token %llu active %ux%u bytes %u stride %u scanlines %u uv-offset %u IOSurface %u\n",f.token,l.width,l.height,l.width*l.height*3/2,l.stride,l.scanlines,l.uvOffset,v.id);
    else IOLog("PianoVideoVT V9: copy success token %llu active %ux%u bytes %u stride %u scanlines %u uv-offset %u IOSurface %u format %08x depth %u\n",f.token,l.width,l.height,l.width*l.height*3/2*(l.bitdepth==10?2:1),l.stride,l.scanlines,l.uvOffset,v.id,l.format,l.bitdepth);
  }else failure(s,62,kNoMemory);
  return ok;
}
uint64_t nowMs(){uint64_t t;clock_get_uptime(&t);uint64_t ns;absolutetime_to_nanoseconds(t,&ns);return ns/1000000;}
bool releaseSurface(Session *s){
  // An unsuccessful unlock remains owned until session client close. Never
  // invalidate a handle or advertise another picture while its write is live.
  if(s->writeSurfaceID)return false;
  struct Methods {
    Session *s;
    int call(uint32_t selector,uint32_t id){
      const IOReturn rc=surfaceMethod(s,selector,id);
      if(!rc){if(selector==pianovideo::vtsurface::Invalidate)++s->surfaceInvalidates;else ++s->surfaceReleases;}
      IOLog("PianoVideoVT V6p: surface dispose token %llu id %u selector %u status %08x maps %u\n",s->frame.token,id,selector,rc,s->maps);
      if(rc)failure(s,selector==pianovideo::vtsurface::Invalidate?43:44,int(rc));
      return int(rc);
    }
  } methods{s};
  const bool ok=s->surfaceLease.dispose(methods);
  IOLog("PianoVideoVT V6p: surface balance lookup %u invalidate %u release %u held %u maps %u\n",
    s->surfaceLookups,s->surfaceInvalidates,s->surfaceReleases,unsigned(s->surfaceLease.held()),s->maps);
  return ok;
}
void closeSurface(Session *s){
  if(s->surfaces){s->surfaces->clientClose();OSSafeReleaseNULL(s->surfaces);++s->surfaceCloses;s->surfaceLease.closed();s->writeSurfaceID=0;}
}
bool request(Session *s,Writer &w,const uint8_t *b,uint64_t id,uint32_t type){
  // Queue transport while holding the cancellation fence. The send callback
  // only appends to the receive queue; selector3/guest ACK runs independently.
  IOLockLock(s->lock);const bool admitted=!s->stopping&&!s->invalidating&&!s->firstError&&s->guest.begin(id,type,nowMs());
  s->replyHasSurface=false;s->replyError=kDecodeError;
  const bool ok=admitted&&send(s->client,w,b);IOLockUnlock(s->lock);
  if(!ok)failure(s,30,kDecodeError);return ok;
}
// Caller holds the session lock; exactly one original reply may be claimed.
bool syncReply(Session *s,pianovideo::stream::Record &f,int error){
  const bool success=!error&&!s->stopping&&!s->invalidating&&!s->firstError;
  const uint64_t id=f.completion.claimReply(success,nowMs());
  if(!id)return false;
  uint8_t b[128];Writer w(b,sizeof b);w.header(cc("dfrm"),FlagReply,s->uuid,id);
  w.s32(cc(",err"),success?0:(error?error:kDecodeError));w.u32(cc(",inf"),0);
  const bool sent=send(s->client,w,b);
  IOLog("PianoVideoVT V9: sync reply token %llu request %llu status %d after-Demt-ACK %u sent %u\n",f.token,id,success?0:(error?error:kDecodeError),unsigned(f.completion.acknowledged),unsigned(sent));
  return sent;
}
void emitFrame(Session *s,int error,const uint8_t *uuid=nullptr){
  IOLockLock(s->lock);auto *record=s->currentAccepted?s->frames.find(s->currentToken):nullptr;
  const bool allowed=!s->stopping&&!s->invalidating&&!s->firstError&&(!record||(!record->completion.expired(nowMs())&&record->completion.claimEmit()));
  const uint32_t async=record?(record->flags&1):1;IOLockUnlock(s->lock);
  if(!allowed)return;
  uint8_t b[192];const uint64_t id=s->nextRequest++;Writer w(b,sizeof b);
  w.header(cc("Demt"),FlagHostRequest,s->uuid,id);w.s64(cc(",frm"),int64_t(s->currentToken));w.s32(cc(",err"),error);
  // Asynchronous=1, FrameDropped=2. A NULL no-output completion has no Dbuf UUID.
  w.u32(cc(",inf"),contract::completionInfo(async,uuid!=nullptr,error));if(uuid)w.uuid(cc(",uui"),uuid);
  s->currentError=error;s->currentImage=uuid&&!error;
  const bool sent=request(s,w,b,id,cc("Demt"));
  IOLockLock(s->lock);s->currentEmitted=sent;if(s->currentAccepted){auto *f=s->frames.find(s->currentToken);if(f){f->completion.sent(sent);if(sent||f->completion.synchronous)f->phase=pianovideo::stream::Phase::Emitted;}}IOLockUnlock(s->lock);
  IOLog("PianoVideoVT V6: Demt session %u token %llu status %d image %u completed-once 1\n",s->native,s->currentToken,error,unsigned(uuid!=nullptr));
}
void settleCurrent(Session *s){
  IOLockLock(s->lock);
  if(s->currentAccepted && !s->frames.retire(s->currentToken) && !s->stopping){
    IOLockUnlock(s->lock);failure(s,73,kBadState);return;
  }
  s->guest.clear();s->current=false;s->currentAccepted=false;s->currentEmitted=false;
  IOLockUnlock(s->lock);
}
// Caller holds the session lock. A deferred success becomes DMA-eligible only
// after its actual reply is in the receive queue. Never wait inside selector3:
// its caller holds the guest send mutex needed by Dbuf/Demt acknowledgement.
bool answerAdmission(Session *s,pianovideo::stream::Record &f,uint64_t id,bool deferred=false){
  // Cancellation may have happened between the worker's loop check and this
  // lock acquisition. Test it atomically with success enqueue/publication.
  if(s->stopping||s->invalidating||s->firstError||f.phase!=pianovideo::stream::Phase::Admitting)return false;
  uint8_t b[128];Writer w(b,sizeof b);w.header(cc("dfrm"),FlagReply,s->uuid,id);
  w.s32(cc(",err"),0);w.u32(cc(",inf"),1);
  const bool sent=send(s->client,w,b);
  if(sent)s->frames.publish(f.token);
  else {
    // Only an immediate selector return can transfer completion ownership to
    // Apple's transport-error path. A deferred request still needs its reply.
    if(!deferred)f.phase=pianovideo::stream::Phase::Emitted;
    s->stopping=true;
  }
  IOLog("PianoVideoVT V7e: admission reply token %llu request %llu sent %u published %u live %u pending %u\n",
    f.token,id,unsigned(sent),unsigned(sent&&!s->stopping),s->frames.published(),s->frames.pending);
  return sent;
}
void worker(void *arg,wait_result_t){
  Session *s=static_cast<Session *>(arg);thread_set_thread_name(current_thread(),"piano-video-v6");
  for(;;){
    IOLockLock(s->lock);const bool stopping=s->stopping||s->firstError;IOLockUnlock(s->lock);if(stopping)break;
    IOLockLock(s->lock);
    auto *admission=s->frames.nextAdmission();
    bool expired=false;
    for(const auto &f:s->frames.frames)if(f.phase!=pianovideo::stream::Phase::Free&&f.completion.expired(nowMs()))expired=true;
    if(admission&&!s->stopping&&!s->invalidating&&!s->firstError){
      auto &in=s->inputs[unsigned(admission-s->frames.frames)];
      expired=expired||(in.reply&&nowMs()>=in.deadline);
      if(!expired&&in.reply&&s->frames.replyReady()){
        const uint64_t reply=in.reply;
        if(answerAdmission(s,*admission,reply,true))in.reply=in.deadline=0;
      }
    }
    const bool admissionStopped=s->stopping||s->invalidating||s->firstError;
    IOLockUnlock(s->lock);
    // QT's asynchronous PV request has a 1.1 s reply deadline. A held reply
    // must fail explicitly while there is still time to wake that caller.
    if(expired){failure(s,77,kDecodeError);break;}
    if(admissionStopped)break;
    // StartSession already prepared input. Observe idle HFI errors too, before
    // the first AU and while waiting for a guest surface or Demt ACK.
    {const int rc=piano_video_native_poll(s->native,&s->frame);if(rc<0){failure(s,70,rc);break;}}
    IOLockLock(s->lock);const uint32_t stage=s->guest.type;const bool replied=s->guest.replied;
    const int replyError=s->replyError;const bool hasSurface=s->replyHasSurface;
    const uint32_t surfaceID=s->replySurfaceID;uint8_t uuid[16];memcpy(uuid,s->replyUuid,16);
    const bool timedOut=s->guest.expired(nowMs());
    IOLockUnlock(s->lock);
    if(timedOut){failure(s,30,kDecodeError);break;}
    if(stage&&replied){
      if(stage==cc("Dbuf")){
        const uint64_t lookupStart=nowMs();
        contract::Surface v;bool ok=!replyError&&hasSurface&&lookupSurface(s,surfaceID,v);
        const uint64_t lookupEnd=nowMs();
        IOLockLock(s->lock);const bool cancelled=s->stopping;IOLockUnlock(s->lock);
        ok=ok&&!cancelled&&copyFrame(s,s->frame,v);
        const uint64_t copyEnd=nowMs();
        // Unmap first, then drop this lookup's references. Keep the session's
        // client alive: stop() revokes creator authorization for the whole task.
        const bool disposed=releaseSurface(s);ok=ok&&disposed;
        IOLog("PianoVideoVT V6: copy timing token %llu lookup-ms %llu map-copy-unmap-ms %llu close-ms %llu surface-open %u surface-close %u maps %u\n",s->frame.token,
          lookupEnd-lookupStart,copyEnd-lookupEnd,nowMs()-copyEnd,s->surfaceOpens,s->surfaceCloses,s->maps);
        const int released=piano_video_native_frame_release(s->native,&s->frame);
        if(released){failure(s,74,released);break;}
        if(cancelled)break;
        emitFrame(s,ok?0:kDecodeError,hasSurface?uuid:nullptr);
      }else{
        IOLog("PianoVideoVT V6: Demt ACK token %llu wait-ms %llu\n",s->currentToken,nowMs()-(s->guest.deadline-2000));
        if(replyError){failure(s,31,replyError);break;}
        if(s->currentAccepted&&s->currentImage)droidvm_vthost_frames_out=droidvm_vthost_frames_out+1;
        IOLockLock(s->lock);auto *completed=s->currentAccepted?s->frames.find(s->currentToken):nullptr;
        bool syncFailed=false;
        if(completed){
          if(!completed->completion.ack())syncFailed=true;
          else if(completed->completion.synchronous&&!syncReply(s,*completed,s->currentError))syncFailed=true;
        }
        IOLockUnlock(s->lock);
        if(syncFailed){failure(s,78,kDecodeError);break;}
        settleCurrent(s);
      }
    }
    if(!s->current){
      contract::RejectedFrame rejected;
      IOLockLock(s->lock);const bool reject=s->rejected.pop(rejected);IOLockUnlock(s->lock);
      if(reject){s->current=true;s->currentToken=uint64_t(rejected.token);s->currentAccepted=false;emitFrame(s,rejected.error);}
      else if(s->frame.memory){
        IOLockLock(s->lock);auto *f=s->frames.find(s->frame.token);
        const bool valid=f&&f->phase==pianovideo::stream::Phase::Submitted;
        const uint32_t flags=valid?f->flags:0;if(valid)f->phase=pianovideo::stream::Phase::Delivering;IOLockUnlock(s->lock);
        if(!valid){failure(s,75,kBadData);break;}
        s->current=true;s->currentAccepted=true;s->currentToken=s->frame.token;
        if((flags&2)||s->frame.noOutput){
          IOLog("PianoVideoVT V9: no-output token %llu do-not-output %u firmware-NOSHOW %u codec %08x\n",s->currentToken,unsigned((flags&2)!=0),unsigned(s->frame.noOutput),s->config.format());
          const int released=piano_video_native_frame_release(s->native,&s->frame);
          if(released){failure(s,74,released);break;}emitFrame(s,0);
        }else{
          uint8_t b[128];const uint64_t id=s->nextRequest++;Writer w(b,sizeof b);
          w.header(cc("Dbuf"),FlagHostRequest,s->uuid,id);w.s64(cc(",frm"),int64_t(s->currentToken));request(s,w,b,id,cc("Dbuf"));
        }
      }
    }
    // Submit the oldest accepted AU. EBD releases DMA input, never frame credit.
    IOLockLock(s->lock);
    auto *queued=s->frames.nextQueued();const int chosen=queued?int(queued-s->frames.frames):-1;
    IOLockUnlock(s->lock);
    if(chosen>=0){
      auto &in=s->inputs[chosen];auto &record=s->frames.frames[chosen];
      const int rc=piano_video_native_submit(s->native,in.bytes,in.size,record.token,&s->config);
      if(rc<0){failure(s,70,rc);break;}
      if(!rc){
        IOFree(in.bytes,contract::kMaxAuBytes);in={};
        IOLockLock(s->lock);if(!s->stopping)record.phase=pianovideo::stream::Phase::Submitted;IOLockUnlock(s->lock);
      }
    }
    IOLockLock(s->lock);const bool finish=s->finishWanted;const bool rejectedPending=s->rejected.count!=0;IOLockUnlock(s->lock);
    if(finish&&chosen<0&&!s->current&&!s->frame.memory&&!rejectedPending){
      // This endpoint requires decode-order output and one complete picture per
      // AU. Keep pumping those pictures and their guest ACKs; Finish is a join,
      // not EOS. Even preroll calls it with its ready first picture in flight.
      // A stuck native picture still reaches the existing progress timeout.
      IOLockLock(s->lock);const bool done=s->frames.pending==0;
      Header h;h.type=cc("dfin");h.request=s->finishRequest;memcpy(h.uuid,s->uuid,16);
      const bool answered=done&&!s->stopping&&!s->invalidating&&!s->firstError;
      if(answered){s->finishWanted=false;status(s->client,h,0);}
      IOLockUnlock(s->lock);
      if(answered)IOLog("PianoVideoVT V7e: finish barrier session %u pending0 guest0 preserve-reference 1 hardware-drain 0\n",s->native);
    }
    IOSleep(1);
  }
  // Cancellation suppresses success before STOP/CLOSE. Old epoch replies cannot
  // enter a new session: teardown joins this worker before releasing the client.
  IOLockLock(s->lock);const bool invalidated=s->invalidating;s->stopping=true;s->guest.cancel();
  const bool finish=s->finishWanted;const uint64_t finishID=s->finishRequest;s->finishWanted=false;
  // Error dispositions are sent exactly once. Invalidation instead aborts the
  // old epoch and joins; it never sends a successful late frame.
  if(!invalidated && s->current && !s->currentAccepted && !s->currentEmitted){
    uint8_t b[160];Writer w(b,sizeof b);w.header(cc("Demt"),FlagHostRequest,s->uuid,s->nextRequest++);
    w.s64(cc(",frm"),int64_t(s->currentToken));w.s32(cc(",err"),kDecodeError);w.u32(cc(",inf"),3);
    IOLockUnlock(s->lock);send(s->client,w,b);IOLockLock(s->lock);
  }
  // A synchronous original always has one reply owner, including cancel,
  // native timeout, failed Dbuf/Demt send and failed guest ACK. Never emit a
  // second frame after a Demt attempt or authorize a late successful reply.
  for(auto &f:s->frames.frames)if(f.phase!=pianovideo::stream::Phase::Free&&f.completion.synchronous){
    syncReply(s,f,kDecodeError);f.phase=pianovideo::stream::Phase::Emitted;
  }
  // A held dfrm never became eligible. Its nonzero reply lets the Apple PV
  // decoder complete that token itself; do not also send a Demt for it.
  for(unsigned i=0;i<pianovideo::stream::Credits;++i)if(s->inputs[i].reply){
    Header h;h.type=cc("dfrm");h.request=s->inputs[i].reply;memcpy(h.uuid,s->uuid,16);
    s->inputs[i].reply=s->inputs[i].deadline=0;s->frames.frames[i].phase=pianovideo::stream::Phase::Emitted;
    IOLockUnlock(s->lock);status(s->client,h,kDecodeError);IOLockLock(s->lock);
  }
  contract::RejectedFrame rejection;
  while(s->rejected.pop(rejection))if(!invalidated){
    uint8_t b[160];Writer w(b,sizeof b);w.header(cc("Demt"),FlagHostRequest,s->uuid,s->nextRequest++);
    w.s64(cc(",frm"),rejection.token);w.s32(cc(",err"),kDecodeError);w.u32(cc(",inf"),3);
    IOLockUnlock(s->lock);send(s->client,w,b);IOLockLock(s->lock);
  }
  if(!invalidated)for(auto &f:s->frames.frames)if(f.phase!=pianovideo::stream::Phase::Free && f.phase!=pianovideo::stream::Phase::Emitted&&f.completion.needsErrorEmit()){
    uint8_t b[160];Writer w(b,sizeof b);w.header(cc("Demt"),FlagHostRequest,s->uuid,s->nextRequest++);
    w.s64(cc(",frm"),int64_t(f.token));w.s32(cc(",err"),kDecodeError);w.u32(cc(",inf"),3);
    IOLockUnlock(s->lock);send(s->client,w,b);IOLockLock(s->lock);f.phase=pianovideo::stream::Phase::Emitted;
  }
  s->frames.cancel();IOLockUnlock(s->lock);
  piano_video_native_cancel(s->native);
  closeSurface(s);
  IOLog("PianoVideoVT V6p: surface final lookup %u invalidate %u release %u held %u maps %u open %u close %u\n",
    s->surfaceLookups,s->surfaceInvalidates,s->surfaceReleases,unsigned(s->surfaceLease.held()),s->maps,s->surfaceOpens,s->surfaceCloses);
  if(s->frame.memory)piano_video_native_frame_release(s->native,&s->frame);
  for(auto &in:s->inputs)if(in.bytes){IOFree(in.bytes,contract::kMaxAuBytes);in={};}
  if(finish){Header h;h.type=cc("dfin");h.request=finishID;memcpy(h.uuid,s->uuid,16);status(s->client,h,kDecodeError);}
  s->closeError=piano_video_native_close(s->native);
  IOLog("PianoVideoVT V6: worker close session %u status %d pending0 cancelled %u surface-open %u surface-close %u maps %u high-water %u worker-joined-next 1\n",
        s->native,s->closeError,s->frames.cancelled,s->surfaceOpens,s->surfaceCloses,s->maps,s->frames.highWater);
  IOLockLock(s->lock);s->native=0;s->workerRunning=false;IOLockWakeup(s->lock,&s->workerRunning,false);IOLockUnlock(s->lock);
  thread_terminate(current_thread());
}
void teardown(Session *s) {
  IOLockLock(s->lock);s->invalidating=true;s->stopping=true;
  IOLockWakeup(s->lock,&s->busy,false);
  while(s->workerRunning)IOLockSleep(s->lock,&s->workerRunning,THREAD_UNINT);
  IOLockUnlock(s->lock);
  if(s->native){s->closeError=piano_video_native_close(s->native);s->native=0;}
  closeSurface(s);
  IOLog("PianoVideoVT V5a: invalidate safe worker-joined 1 surfaces-released 1 close-status %d\n",s->closeError);
}
void destroy(Session *s){if(!s)return;teardown(s);IOLockFree(s->lock);delete s;}
bool descriptor(Client *c,const Header &h,const uint8_t *m,uint32_t n,contract::FormatReport &r,pianovideo::codec::Config *config) {
  const bool valid=contract::formatRequest(m,n,r,config);
  IOLog("PianoVideoVT V5b: format %c%c%c%c request %llu pid %d fds-type %08x bytes %u declared %u codec %08x size %ux%u extensions %u atoms %u terminal-zero4 %u avcC %u decision %s\n",
    int(h.type>>24),int((h.type>>16)&255),int((h.type>>8)&255),int(h.type&255),h.request,c->pid,
    r.parameterType,r.bytes,r.declared,r.codec,r.width,r.height,r.extensionBytes,r.atomBytes,unsigned(r.terminalZero),r.avcBytes,r.decision);
  if(!valid&&!strcmp(r.decision,"hevc-native-envelope"))
    IOLog("PianoVideoVT V9d: HEVC decline pid %d request %llu size %ux%u native-reserve 0 before-PAS-HFI 1 status %d\n",c->pid,h.request,r.width,r.height,kUnsupported);
  return valid;
}
void attributes(Session *s) {
  Plist *p=new Plist();if(!p){failure(s,10,kNoMemory);return;}
  uint8_t b[640];const int fmt=p->integer(s->config.pixelFormat());
  const int keys[]={p->string("Width"),p->string("Height"),p->string("PixelFormatType"),p->string("BytesPerRowAlignment"),p->string("PlaneAlignment"),p->string("IOSurfaceProperties")};
  const int values[]={p->integer(s->config.s.width),p->integer(s->config.s.height),p->array(&fmt,1),p->integer(64),p->integer(256),p->dict(nullptr,nullptr,0)};
  const int attrs=p->dict(keys,values,6);Writer w(b,sizeof b);w.header(cc("Dpba"),0,s->uuid,0);w.plist(cc(",pba"),*p,attrs);send(s->client,w,b);delete p;
  if(!s->config.isHevc())IOLog("PianoVideoVT V8: Dpba active %ux%u 420v IOSurface pool\n",s->config.s.width,s->config.s.height);
  else IOLog("PianoVideoVT V9: Dpba active %ux%u format %08x depth %u IOSurface pool\n",s->config.s.width,s->config.s.height,s->config.pixelFormat(),s->config.depth);
}
int start(Session *s,const Header &h,const uint8_t *m,uint32_t n) {
  IOLockLock(s->lock);const bool started=s->started||s->stopping;IOLockUnlock(s->lock);if(started)return kBadState;
  contract::FormatReport r;if(!descriptor(s->client,h,m,n,r,&s->config)||s->config.format()!=s->selectedCodec)return kUnsupported;
  const int rc=piano_video_native_open(&s->native);if(rc)return kDecodeError;
  if(piano_video_native_prepare(s->native,&s->config)){
    s->closeError=piano_video_native_close(s->native);s->native=0;return kDecodeError;
  }
  s->started=true;s->workerRunning=true;thread_t t=nullptr;
  if(kernel_thread_start(worker,s,&t)!=KERN_SUCCESS){s->workerRunning=false;s->stopping=true;s->closeError=piano_video_native_close(s->native);s->native=0;return kNoMemory;}
  thread_deallocate(t);attributes(s);
  if(!s->config.isHevc())IOLog("PianoVideoVT V8: dsta native session %u bounded-format %ux%u profile %u level %u progressive8bit420\n",s->native,s->config.s.width,s->config.s.height,s->config.s.profile,s->config.s.level);
  else IOLog("PianoVideoVT V9: dsta native session %u bounded-format %ux%u codec %08x profile %u level %u depth %u\n",s->native,s->config.s.width,s->config.s.height,s->config.format(),s->config.s.profile,s->config.s.level,s->config.depth);
  return 0;
}
int queue(Session *s,const uint8_t *m,uint32_t n,uint64_t request,contract::InputReport &r){
  if(!contract::inputRequest(m,n,r,&s->config))return kBadData;
  IOLockLock(s->lock);
  if(!s->started||s->stopping||s->firstError||s->finishWanted||s->frames.pending==pianovideo::stream::Credits||s->frames.find(uint64_t(r.token))){
    IOLockUnlock(s->lock);r.decision="state-or-credit-or-duplicate";return kBadState;
  }
  // Reserve under the session lock before allocating. Even a rejected ninth
  // request cannot transiently allocate a ninth MiB of SW AU storage.
  uint8_t *input=static_cast<uint8_t *>(IOMalloc(contract::kMaxAuBytes));
  if(!input){IOLockUnlock(s->lock);return kNoMemory;}
  const bool first=s->frames.accepted==0;
  if(first&&!r.picture.idr){IOLockUnlock(s->lock);IOFree(input,contract::kMaxAuBytes);r.decision="first-picture-not-IDR";return kBadData;}
  const uint32_t normalized=contract::normalize(r.data,r.bytes,input,contract::kMaxAuBytes,s->config,first);
  if(!normalized){IOLockUnlock(s->lock);IOFree(input,contract::kMaxAuBytes);return kBadData;}
  auto *record=s->frames.accept(uint64_t(r.token),r.flags,false);
  if(!record){IOLockUnlock(s->lock);IOFree(input,contract::kMaxAuBytes);r.decision="credit-or-duplicate";return kBadState;}
  record->completion.begin(r.flags,request,nowMs());
  const unsigned index=unsigned(record-s->frames.frames);s->inputs[index]={input,normalized};
  const unsigned pending=s->frames.pending;
  char digest[65];hash(input,normalized,digest);
  // Successful admission returns with the session lock held through reply
  // enqueue/publication. Error paths unlock themselves. This also prevents an
  // unrelated worker failure from settling an unpublished frame too early.
  IOLog("PianoVideoVT V6: accepted session %u token %llu flags %u normalized-bytes %u SHA256 %s pending %u SW-cap 8388608\n",s->native,uint64_t(r.token),r.flags,normalized,digest,pending);
  droidvm_vthost_frames_in=droidvm_vthost_frames_in+1;return 0;
}
void inventory(Client *c,const Header &h) {
  Plist *p=new Plist();if(!p){status(c,h,kNoMemory);return;}uint8_t b[4096];int entries[2]={};
  const bool ready=piano_video_native_ready();
  if(h.type==cc("decs")&&ready){entries[0]=contract::inventoryEntry(*p,false);entries[1]=contract::inventoryEntry(*p,true);}
  const int list=p->array(entries,h.type==cc("decs")&&ready?2:0);
  Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.plist(h.type==cc("decs")?cc(",dec"):cc(",enc"),*p,list);w.u32(cc(",ver"),6);send(c,w,b);delete p;
  IOLog("PianoVideoVT V9: inventory version6 avc1 hvc1 ready %u encoders0 rating400\n",unsigned(ready));
}
void capabilities(Client *c,const Header &h,const uint8_t *m,uint32_t n){
  int32_t codec=0,version=0;
  const bool supported=paramS32(m,n,cc(",cod"),codec)&&(uint32_t(codec)==cc("avc1")||uint32_t(codec)==cc("hvc1"))&&
    paramS32(m,n,cc(",ver"),version)&&version==6&&piano_video_native_ready();
  if(!supported){status(c,h,kUnsupported);return;}
  Plist *p=new Plist();if(!p){status(c,h,kNoMemory);return;}uint8_t b[2048];
  const int root=contract::capabilities(*p,uint32_t(codec)==cc("hvc1"));Writer w(b,sizeof b);
  w.header(h.type,FlagReply,h.uuid,h.request);w.s32(cc(",err"),0);w.u32(cc(",ver"),6);w.plist(cc(",dca"),*p,root);send(c,w,b);delete p;
  IOLog("PianoVideoVT V9d: dcap codec %08x version6 metadata-only native-owner-unchanged 1 HEVC-admission-level120 geometry1920x1080 runtime-unqualified 1\n",uint32_t(codec));
}
void properties(Client *c,const Header &h,bool value) {
  Plist *p=new Plist();if(!p){status(c,h,kNoMemory);return;}uint8_t b[512];int root;
  if(value){
    Session *s=c->session;bool hardware=false;
    if(s){IOLockLock(s->lock);hardware=s->started&&!s->stopping;IOLockUnlock(s->lock);}
    root=p->boolean(hardware);
  }
  else{
    const int keys[]={p->string("PropertyType"),p->string("ReadWriteStatus")};const int values[]={p->string("Boolean"),p->string("ReadOnly")};
    const int name=p->string("UsingHardwareAcceleratedVideoDecoder"),type=p->dict(keys,values,2);root=p->dict(&name,&type,1);
  }
  Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.s32(cc(",err"),0);w.plist(value?cc(",val"):cc(",sup"),*p,root);send(c,w,b);delete p;
}
}
extern "C" {
bool droidvm_vthost_available(void){return piano_video_native_ready();}
void *droidvm_vthost_client_open(void *transport,bool (*sendFn)(void *,const uint8_t *,uint32_t),task_t task,int pid) {
  Client *c=new Client();if(!c)return nullptr;c->lock=IOLockAlloc();if(!c->lock){delete c;return nullptr;}
  c->transport=transport;c->send=sendFn;c->task=task;c->pid=pid;return c;
}
void droidvm_vthost_client_close(void *host){Client *c=static_cast<Client *>(host);if(!c)return;destroy(c->session);IOLockFree(c->lock);delete c;}
int droidvm_vthost_message(void *host,const uint8_t *m,uint32_t n,const uint32_t *ids,unsigned surfaces) {
  Client *c=static_cast<Client *>(host);Header h;if(!c||!m||n>contract::kMaxMessageBytes||!header(m,n,h))return -1;
  if(h.type==cc("dcan")){
    IOLog("PianoVideoVT V5b: dcan received request %llu pid %d bytes %u flags %u\n",h.request,c->pid,n,h.flags);
    if(h.flags!=FlagRequest&&h.flags!=0){
      IOLog("PianoVideoVT V5b: dcan request %llu pid %d decision message-flags status -1 transport-rejected 1 no-answer 1\n",h.request,c->pid);
      return -1;
    }
  }
  if(!contract::message(m,n)){
    if(h.type==cc("dcan"))IOLog("PianoVideoVT V5b: dcan request %llu pid %d decision message-layout status -1 transport-rejected 1 no-answer 1\n",h.request,c->pid);
    return -1;
  }
  Session *s=c->session;
  if(h.flags==FlagGuestReply){
    if(!s||memcmp(s->uuid,h.uuid,16))return -1;
    IOLockLock(s->lock);const bool expected=s->guest.matches(h.request,h.type)&&!s->guest.expired(nowMs())&&!s->stopping;IOLockUnlock(s->lock);
    if(!expected)return -1;
    int32_t error=0;paramS32(m,n,cc(",err"),error);Param uuid;bool hasSurface=false;
    if(h.type==cc("Dbuf")&&!error){
      hasSurface=surfaces==1&&param(m,n,cc(",uui"),uuid)&&uuid.type==cc("=uui")&&uuid.size==16;
      if(!hasSurface)error=kDecodeError;
    }
    IOLockLock(s->lock);
    if(s->guest.matches(h.request,h.type)&&!s->guest.expired(nowMs())&&!s->stopping){s->replyError=error;s->replyHasSurface=hasSurface;if(hasSurface){s->replySurfaceID=ids[0];memcpy(s->replyUuid,uuid.value,16);}s->guest.reply(h.request,h.type);}
    IOLockUnlock(s->lock);
    IOLog("PianoVideoVT V5a: guest reply %c%c%c%c request %llu status %d surface %u\n",int(h.type>>24),int((h.type>>16)&255),int((h.type>>8)&255),int(h.type&255),h.request,error,unsigned(hasSurface));return 0;
  }
  if(h.flags!=FlagRequest&&h.flags!=0){
    if(h.type==cc("dfrm")){
      IOLog("PianoVideoVT V5a: input rejected malformed-or-unsupported before-DMA 1\n");
      IOLog("PianoVideoVT V5c: dfrm request %llu flags %u decision unsupported-fragment-or-flags transport-error-emit 1 status %d\n",h.request,h.flags,kBadData);
    }
    return kBadData;
  }
  IOLockLock(c->lock);
  s=c->session;const bool owned=s&&!memcmp(s->uuid,h.uuid,16);
  if(h.type==cc("decs")||h.type==cc("encs"))inventory(c,h);
  else if(h.type==cc("dcap"))capabilities(c,h,m,n);
  else if(h.type==cc("+dec")){
    int32_t codec=0;
    if(c->session||!paramS32(m,n,cc(",cod"),codec)||(uint32_t(codec)!=cc("avc1")&&uint32_t(codec)!=cc("hvc1")))status(c,h,kUnsupported);
    else{
      s=new Session();if(s)s->lock=IOLockAlloc();
      if(!s||!s->lock){delete s;status(c,h,kNoMemory);}
      else{
        s->selectedCodec=uint32_t(codec);s->client=c;memcpy(s->uuid,h.uuid,16);c->session=s;droidvm_vthost_sessions=droidvm_vthost_sessions+1;
        Plist *p=new Plist();if(!p)status(c,h,kNoMemory);else{
          uint8_t b[320];const int key=p->string("VTHostBuildVersion"),value=p->string("PianoVideo native V9 bounded AVC HEVC");const int info=p->dict(&key,&value,1);
          Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.s32(cc(",err"),0);w.u32(cc(",ver"),6);w.plist(cc(",hin"),*p,info);send(c,w,b);delete p;
        }
        IOLog("PianoVideoVT V9: +dec selected com.unixnu.pianovideo.decoder.%s codec %08x pid %d\n",uint32_t(codec)==cc("hvc1")?"hevc":"avc",uint32_t(codec),c->pid);
        IOLog("PianoVideoVT V6p: session-client per-copy invalidate56 release1 no-address-cache\n");
        IOLog("PianoVideoVT V7e: reply-window4 credits8 software-finish no-EOS\n");
      }
    }
  }else if(!owned){
    if(h.type==cc("dfrm")){IOLockUnlock(c->lock);return kBadState;} // guest transport-error path retires its frame
    const bool sent=(h.flags&FlagRequest)?status(c,h,kBadState):false;
    if(h.type==cc("dcan"))IOLog("PianoVideoVT V5b: dcan request %llu pid %d decision uuid-session status %d can-absent 1 transport %u\n",h.request,c->pid,kBadState,unsigned(sent));
  }
  else switch(h.type){
    case cc("dsta"):status(c,h,start(s,h,m,n));break;
    case cc("dcan"):{
      pianovideo::codec::Config *candidate=new pianovideo::codec::Config();
      contract::FormatReport r;const bool valid=candidate&&descriptor(c,h,m,n,r,candidate);
      IOLockLock(s->lock);const bool active=s->started&&!s->stopping;IOLockUnlock(s->lock);
      const bool can=active&&valid&&s->config.same(*candidate);delete candidate;uint8_t b[128];Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.s32(cc(",err"),0);w.s32(cc(",can"),can?1:0);
      const bool sent=send(c,w,b);
      IOLog("PianoVideoVT V5b: dcan request %llu pid %d active %u decision %s status 0 ,can %u transport %u side-effect-free 1\n",h.request,c->pid,unsigned(active),active?r.decision:"inactive-session",unsigned(can),unsigned(sent));
      IOLog("PianoVideoVT V5a: dcan ,can %u side-effect-free 1\n",unsigned(can));break;
    }
    case cc("dsts"):case cc("dtil"):status(c,h,kUnsupported);IOLog("PianoVideoVT V5a: tiles rejected before-DMA 1\n");break;
    case cc("dfrm"):{
      contract::InputReport r;const int error=queue(s,m,n,h.request,r);
      // A rejected request belongs to Apple's immediate error completion path.
      // Returning an error and later sending Demt would complete it twice.
      if(error){IOLockUnlock(c->lock);return error;}
      auto *f=s->frames.find(uint64_t(r.token));
      const bool synchronous=!(r.flags&1);
      const bool deferred=synchronous||!s->frames.replyReady()||s->frames.nextAdmission()!=f;
      bool sent=false;
      if(synchronous){
        IOLog("PianoVideoVT V9: sync admitted token %llu request %llu worker-ready 1 selector-return 1 deadline-ms 8000\n",uint64_t(r.token),h.request);
      }else if(deferred){
        auto &in=s->inputs[unsigned(f-s->frames.frames)];in.reply=h.request;in.deadline=nowMs()+900;
        IOLog("PianoVideoVT V7e: admission held token %llu request %llu live %u pending %u deadline-ms 900 selector-return 1\n",
          uint64_t(r.token),h.request,s->frames.published(),s->frames.pending);
      }else sent=answerAdmission(s,*f,h.request);
      IOLockUnlock(s->lock);
      IOLog("PianoVideoVT V6: dfrm request %llu token %llu decision %s status %d reply-enqueued %u published %u\n",
        h.request,uint64_t(r.token),r.decision,error,unsigned(sent),unsigned(sent&&!s->stopping));
      if(!deferred&&!sent){IOLockUnlock(c->lock);return kDecodeError;}
      break;
    }
    case cc("dfin"):{
      IOLockLock(s->lock);
      const auto decision=pianovideo::stream::finishDecision(s->started,s->invalidating,s->firstError!=0,
        s->workerRunning,s->stopping,s->finishWanted);
      const bool schedule=decision==pianovideo::stream::FinishDecision::Schedule;
      if(schedule){s->finishWanted=true;s->finishRequest=h.request;}
      IOLockUnlock(s->lock);
      if(!schedule)status(c,h,decision==pianovideo::stream::FinishDecision::Failed?kDecodeError:kBadState);break;
    }
    case cc("dinv"):teardown(s);status(c,h,s->closeError?kDecodeError:0);break;
    case cc("-dec"):c->session=nullptr;destroy(s);break;
    case cc("dsup"):properties(c,h,false);break;
    case cc("dcop"):{Param p;char key[96]="";if(param(m,n,cc(",key"),p)&&plistString(p.value,p.size,key,sizeof key)&&!strcmp(key,"UsingHardwareAcceleratedVideoDecoder"))properties(c,h,true);else status(c,h,-12900);break;}
    case cc("dset"):case cc("dmul"):status(c,h,-12900);break;
    default:droidvm_vthost_unknown=droidvm_vthost_unknown+1;droidvm_vthost_last_unknown=h.type;if(h.flags&FlagRequest)status(c,h,-12900);break;
  }
  IOLockUnlock(c->lock);return 0;
}
}
