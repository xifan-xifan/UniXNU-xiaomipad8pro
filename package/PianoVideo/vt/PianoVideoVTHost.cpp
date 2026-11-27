// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Native V6 endpoint of the approved UniXNU VT frontend (vt/UPSTREAM).
// One exclusive, bounded AVC/HEVC stream. Selector3 snapshots,
// validates and enqueues. The worker pumps HFI during asynchronous guest waits;
// output lookups/maps are scoped to a copy. Synchronous completion stays on the worker.
#ifndef PIANO_VIDEO_VT_HOST_TEST
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <libkern/crypto/sha2.h>
#endif
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
task_t proc_task(proc_t);
}
namespace {
constexpr int kBadData = -12909, kUnsupported = -12910, kBadState = -12903, kDecodeError = -12911, kNoMemory = -12904;
struct Client;
struct Session {
  Client *client = nullptr;
  uint8_t uuid[16] = {};
  uint32_t native = 0, selectedCodec = 0;
  IOLock *lock = nullptr;
  bool started=false, stopping=false, invalidating=false, workerRunning=false, busy=false, abandoning=false;
  uint64_t invalidateRequest=0, invalidateDeadline=0;
  pianovideo::codec::Config config;
  pianovideo::stream::Ledger frames;
  struct Input { uint8_t *bytes=nullptr; uint32_t size=0; uint64_t reply=0, deadline=0; } inputs[pianovideo::stream::Credits];
  contract::RejectedFrames rejected;
  bool finishWanted=false;
  uint64_t finishRequest=0;
  int firstError=0, closeError=0, completionError=0;
  pianovideo::stream::Guest guest;
  uint32_t replySurfaceID=0;
  bool replyHasSurface=false;
  int replyError=0;
  uint8_t replyUuid[16]={};
  IOUserClient *surfaces=nullptr;
  bool surfacesBorrowed=false;
  OSObject *kernelSurface=nullptr;   // piano-video-vt-borrow=3: the surface object itself (no user-client lookup, no lease)   // piano-video-vt-borrow: the owner task's own IOSurfaceRoot client, never closed by us
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
  // A new session may reuse its predecessor's UUID on this transport.
  uint64_t nextRequest = 100;
  uint64_t takeRequest() { return nextRequest ? nextRequest++ : 0; }
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
// LT-B5 (ANALYSIS.md section 12 of 20261124-browser-gpu): a sandboxed client's surfaces may refuse a lookup by ID through a second
// IOSurfaceRoot client of the same task (27.3 "disallowing global lookups"; WebKit's GPU process: kIOReturnNotPermitted). With boot-arg
// piano-video-vt-borrow=1 the lookup is retried once through that task's own existing IOSurfaceRoot client (IOUserClientCreator
// "pid N, ..."), which already holds the surface. The borrowed client is retained, never closed by us.
void closeSurface(Session *s);
// B1 records survive the kernel ring buffer: the last 8 B1 lines as properties piano-video-vt-b1-<n> on the PV driver service (ioreg).
static void b1Record(const char *line){
  static unsigned seq=0;OSDictionary *m=IOService::nameMatching("AppleVideoToolboxParavirtualizationDriver");
  IOService *pv=m?IOService::copyMatchingService(m):nullptr;OSSafeReleaseNULL(m);if(!pv)return;
  char key[40];snprintf(key,sizeof key,"piano-video-vt-b1-%u",seq++%8);pv->setProperty(key,line);pv->release();
}
// default 3 (the kernel-surface copy) since y12i; piano-video-vt-borrow=0 off, 1/2 the diagnostic lookup walks
static int pianoVtBorrowMode(){static int b=-1;if(b<0){uint32_t v=3;PE_parse_boot_argn("piano-video-vt-borrow",&v,sizeof v);b=int(v);}return b;}
static bool pianoVtBorrow(){return pianoVtBorrowMode()>=1;}
// piano-video-vt-borrow=2 (diagnostic, LT-B5): once per boot, try the refused surface ID through EVERY IOSurfaceRoot client and record
// which creator's client accepts it (find_surface admits the owner task or a client that knows it); each accepted lookup is undone at
// once on that same client (selector 56 invalidate, 1 release: the lease's own pair). Nothing is copied.
static IOReturn clientMethod(IOUserClient *u,uint32_t selector,uint32_t id,void *out,uint32_t *bytes){
  uint64_t scalar=id;IOExternalMethodArguments a={};a.version=kIOExternalMethodArgumentsCurrentVersion;a.selector=selector;
  a.scalarInput=&scalar;a.scalarInputCount=1;a.structureOutput=out;a.structureOutputSize=bytes?*bytes:0;
  IOUserClient2022 *c=OSDynamicCast(IOUserClient2022,u);
  const IOReturn rc=c?c->externalMethod(selector,reinterpret_cast<IOExternalMethodArgumentsOpaque *>(&a)):u->callExternalMethod(selector,&a);
  if(bytes)*bytes=uint32_t(a.structureOutputSize);return rc;
}
static void b1Record(const char *line);
static void diagRecord(const char *line){   // separate keys: the 8-slot B1 ring overwrote the diagnostic in qualvtg6
  static unsigned seq=0;if(seq>=24)return;OSDictionary *m=IOService::nameMatching("AppleVideoToolboxParavirtualizationDriver");
  IOService *pv=m?IOService::copyMatchingService(m):nullptr;OSSafeReleaseNULL(m);if(!pv)return;
  char key[40];snprintf(key,sizeof key,"piano-video-vt-diag-%02u",seq++);pv->setProperty(key,line);pv->release();
}
static void diagOwner(IOUserClient *mine,uint32_t id){
  static bool done=false;if(done||pianoVtBorrowMode()!=2)return;done=true;
  IOService *root=surfaceRoot();if(!root)return;constexpr uint32_t bytes=0xc68;uint8_t *buf=static_cast<uint8_t *>(IOMallocZero(bytes));
  unsigned tried=0,ok=0;OSIterator *it=buf?root->getClientIterator():nullptr;
  if(it){while(OSObject *o=it->getNextObject()){IOUserClient *u=OSDynamicCast(IOUserClient,o);if(!u||u==mine)continue;++tried;
      uint32_t n=bytes;const IOReturn rc=clientMethod(u,4,id,buf,&n);
      if(!rc){++ok;OSObject *c=u->copyProperty("IOUserClientCreator");OSString *cs=OSDynamicCast(OSString,c);
        clientMethod(u,56,id,nullptr,nullptr);clientMethod(u,1,id,nullptr,nullptr);
        char line[160];snprintf(line,sizeof line,"diag id %u ACCEPTED by %s",id,cs?cs->getCStringNoCopy():"?");IOLog("PianoVideoVT B1: %s\n",line);diagRecord(line);
        OSSafeReleaseNULL(c);}}
    it->release();}
  char line[160];snprintf(line,sizeof line,"diag id %u tried %u clients, %u accepted",id,tried,ok);IOLog("PianoVideoVT B1: %s\n",line);diagRecord(line);
  if(buf)IOFree(buf,bytes);OSSafeReleaseNULL(root);
}
// Every IOSurfaceRoot client of the task (a sandboxed process may hold several: CoreVideo's, Metal's), retained, in registry order; the
// diagnostic line lists how many clients each pid has.
// pid > 0: that task's clients ("pid N,"); pid == 0: every client whose creator names a WebKit process ("com.apple.WebKit"), other pids
// included. IOSurface.kext find_surface (27.3) admits a lookup when the surface's owner task (+0x170) is the looking task, or the client
// already knows the surface, or the surface is global. WebKit re-assigns its decode-pool surfaces to the WebContent task
// (IOSurfaceSetOwnershipIdentity, s_set_ownership) and the pool reuses them, so the GPU task's own client is refused.
static unsigned ownerSurfaceClients(IOUserClient *mine,int pid,IOUserClient **out,unsigned max){
  IOService *root=surfaceRoot();if(!root)return 0;
  char want[24];snprintf(want,sizeof want,"pid %d,",pid);const size_t n=strlen(want);
  unsigned count=0,total=0;OSIterator *it=root->getClientIterator();
  if(it){while(OSObject *o=it->getNextObject()){IOUserClient *u=OSDynamicCast(IOUserClient,o);if(!u||u==mine)continue;++total;
      OSObject *c=u->copyProperty("IOUserClientCreator");OSString *cs=OSDynamicCast(OSString,c);
      const bool match=cs&&(pid>0?!strncmp(cs->getCStringNoCopy(),want,n):strnstr(cs->getCStringNoCopy(),", com.apple.WebKit",cs->getLength()+1)!=nullptr);
      if(match&&pid==0){char line[160];snprintf(line,sizeof line,"candidate %s",cs->getCStringNoCopy());IOLog("PianoVideoVT B1: %s\n",line);b1Record(line);}
      OSSafeReleaseNULL(c);
      if(match&&count<max){u->retain();out[count++]=u;}}
    it->release();}
  {char line[160];snprintf(line,sizeof line,"IOSurfaceRoot has %u other clients, %u of pid %d",total,count,pid);
   IOLog("PianoVideoVT B1: %s\n",line);b1Record(line);}
  OSSafeReleaseNULL(root);return count;
}
// LT-B5 qualvtg7: no IOSurfaceRoot client of any of the 24 accepts the surface (diag), so its owner task (WebKit re-owns decode-pool
// surfaces to the WebContent process, which has no IOSurfaceRoot client: its sandbox) is reached by opening a client FOR that task
// (IOSurfaceRoot::newUserClient from the kernel, as for the GPU task): find_surface's rule (1) owner == task then admits it. The
// candidates are the other com.apple.WebKit processes with pids near the GPU process's (WebKit launches its processes together).
static IOUserClient *ownerTaskClient(int gpuPid,uint32_t id,int *ownerPid){
  IOService *root=surfaceRoot();if(!root)return nullptr;constexpr uint32_t bytes=0xc68;uint8_t *buf=static_cast<uint8_t *>(IOMallocZero(bytes));
  IOUserClient *found=nullptr;unsigned tried=0;
  // borrow=2 (diagnostic): every process, pid 0..99999 (qualvtg8: no WebKit task owns it); borrow=1: WebKit tasks near the GPU pid
  const bool all=pianoVtBorrowMode()==2;
  for(int pid=all?0:gpuPid-400;buf&&!found&&pid<=(all?99999:gpuPid+400);++pid){
    if((!all&&pid<=1)||pid==gpuPid)continue;char name[32]={};proc_name(pid,name,sizeof name);
    if(!all&&strncmp(name,"com.apple.WebKit",16))continue;
    if(all&&(!name[0]||pid==0))continue;
    proc_t pr=proc_find(pid);if(!pr)continue;task_t t=proc_task(pr);
    IOUserClient *c=nullptr;const IOReturn orc=t?root->newUserClient(t,t,0,nullptr,&c):kIOReturnNotFound;++tried;
    IOReturn lrc=kIOReturnNotFound;
    if(!orc&&c){uint32_t n=bytes;lrc=clientMethod(c,4,id,buf,&n);
      if(!lrc){clientMethod(c,56,id,nullptr,nullptr);clientMethod(c,1,id,nullptr,nullptr);found=c;*ownerPid=pid;}
      else{c->clientClose();c->release();}}
    proc_rele(pr);
    if(!all||!lrc){char line[160];snprintf(line,sizeof line,"owner-task try pid %d %s: open %08x lookup %08x",pid,name,orc,lrc);IOLog("PianoVideoVT B1: %s\n",line);diagRecord(line);}
  }
  char line[160];snprintf(line,sizeof line,"owner-task scan id %u: %u WebKit tasks tried, %s",id,tried,found?"FOUND":"none");IOLog("PianoVideoVT B1: %s\n",line);diagRecord(line);
  if(buf)IOFree(buf,bytes);OSSafeReleaseNULL(root);return found;
}
// LT-B5 (owner 11:55: carry the output surface itself instead of a by-ID lookup): piano-video-vt-borrow=3 takes the IOSurface object
// straight from IOSurfaceRoot's table when every lookup path refuses it (qualvtg4-9: no client of any of 507 tasks is admitted by
// find_surface). Field offsets of the pinned 27.3 IOSurface.kext (KC kc273-mac17j, ~/scratch/iosurface-re disassembly):
//   IOSurfaceRoot: +0xd0 IOSurface ** table, +0xd8 uint32 count, +0x140 IOLock * (lookupSurface takes it around find_surface)
//   IOSurface: +0x10 id, +0x30 IOMemoryDescriptor *, +0x80 pixel format, +0x88 alloc offset, +0x90 alloc size, +0x98 plane count,
//              +0x180 plane records (56 bytes: +0 width, +8 height, +0x18 offset, +0x20 bytes per row)
// The copy maps [alloc offset, +alloc size) of the surface's own memory descriptor into the kernel. Guarded: the id read back,
// 420v/x420 two planes, sizes in bounds; any mismatch refuses (no guess).
static OSObject *kernelSurfaceFor(uint32_t id,contract::Surface &v){
  IOService *root=surfaceRoot();if(!root)return nullptr;
  const uint8_t *r=reinterpret_cast<const uint8_t *>(root);OSObject *found=nullptr;
  IOLock *lock=*reinterpret_cast<IOLock *const *>(r+0x140);
  if(lock){IOLockLock(lock);
    OSObject *const *table=*reinterpret_cast<OSObject *const *const *>(r+0xd0);const uint32_t count=*reinterpret_cast<const uint32_t *>(r+0xd8);
    if(table&&id<count){OSObject *o=table[id];const uint8_t *b=reinterpret_cast<const uint8_t *>(o);
      if(o&&*reinterpret_cast<const uint32_t *>(b+0x10)==id){o->retain();found=o;}}
    IOLockUnlock(lock);}
  OSSafeReleaseNULL(root);
  if(!found)return nullptr;
  const uint8_t *b=reinterpret_cast<const uint8_t *>(found);
  v=contract::Surface();v.id=id;v.base=1;v.format=*reinterpret_cast<const uint32_t *>(b+0x80);v.size=*reinterpret_cast<const uint64_t *>(b+0x90);
  v.planes=*reinterpret_cast<const uint32_t *>(b+0x98);
  const uint8_t *pl=*reinterpret_cast<const uint8_t *const *>(b+0x180);
  if(pl&&v.planes==2)for(unsigned i=0;i<2;++i){const uint8_t *q=pl+56*i;
    v.plane[i]={*reinterpret_cast<const uint64_t *>(q),*reinterpret_cast<const uint64_t *>(q+8),*reinterpret_cast<const uint64_t *>(q+0x18),
                *reinterpret_cast<const uint64_t *>(q+0x20),0};}
  char line[200];snprintf(line,sizeof line,"kernel surface id %u format %08x planes %u alloc %llu+%llu p0 %llux%llu off %llu bpr %llu p1 off %llu bpr %llu",id,v.format,v.planes,
    *reinterpret_cast<const uint64_t *>(b+0x88),v.size,v.plane[0].width,v.plane[0].height,v.plane[0].offset,v.plane[0].stride,v.plane[1].offset,v.plane[1].stride);
  IOLog("PianoVideoVT B3: %s\n",line);diagRecord(line);
  return found;
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
    if(rc || !s->surfaces){failure(s,40,rc?int(rc):kBadState);return false;}
  }
  uint8_t *result=static_cast<uint8_t *>(IOMallocZero(bytes));if(!result){failure(s,41,kNoMemory);return false;}
  uint32_t resultBytes=bytes;
  IOReturn rc=surfaceMethod(s,4,id,result,&resultBytes);
  if(rc==kIOReturnNotPermitted)diagOwner(s->surfaces,id);
  if(rc==kIOReturnNotPermitted&&pianoVtBorrow()&&pianoVtBorrowMode()<3&&!s->surfacesBorrowed){
    IOUserClient *owners[8]={};unsigned n=ownerSurfaceClients(s->surfaces,s->client->pid,owners,8);
    {const unsigned room=8-n;IOUserClient *more[8]={};const unsigned m=ownerSurfaceClients(s->surfaces,0,more,8);
     for(unsigned j=0;j<m;++j){bool dup=false;for(unsigned k=0;k<n;++k)if(owners[k]==more[j])dup=true;
       if(!dup&&n<8&&j<room+8){owners[n++]=more[j];}else more[j]->release();}}
    {char line[160];snprintf(line,sizeof line,"lookup id %u refused through our client; %u owner-task client(s) (pid %d)",id,n,s->client->pid);
     IOLog("PianoVideoVT B1: %s\n",line);b1Record(line);}
    if(n){closeSurface(s);
      for(unsigned i=0;i<n;++i){
        if(s->surfacesBorrowed){owners[i]->release();continue;}
        s->surfaces=owners[i];s->surfacesBorrowed=true;memset(result,0,bytes);resultBytes=bytes;
        rc=surfaceMethod(s,4,id,result,&resultBytes);
        {char line[160];snprintf(line,sizeof line,"lookup id %u through owner-task client %u of %u status %08x",id,i+1,n,rc);
         IOLog("PianoVideoVT B1: %s\n",line);b1Record(line);}
        if(rc){OSSafeReleaseNULL(s->surfaces);s->surfacesBorrowed=false;}
      }
      if(!s->surfaces){rc=kIOReturnNotPermitted;}
    }
    if(rc==kIOReturnNotPermitted){   // the owner task itself (WebContent): a client of our own for it
      int owner=-1;IOUserClient *c=ownerTaskClient(s->client->pid,id,&owner);
      if(c){if(s->surfaces){closeSurface(s);}s->surfaces=c;s->surfacesBorrowed=false;++s->surfaceOpens;memset(result,0,bytes);resultBytes=bytes;
        rc=surfaceMethod(s,4,id,result,&resultBytes);
        char line[160];snprintf(line,sizeof line,"lookup id %u through a client for owner pid %d status %08x",id,owner,rc);IOLog("PianoVideoVT B1: %s\n",line);diagRecord(line);}
    }
  }
  if(rc==kIOReturnNotPermitted&&pianoVtBorrowMode()==3){
    OSSafeReleaseNULL(s->kernelSurface);contract::Surface k;s->kernelSurface=kernelSurfaceFor(id,k);
    const bool kok=s->kernelSurface&&contract::surface(k,s->config.s.width,s->config.s.height,s->config.pixelFormat());
    if(!kok){OSSafeReleaseNULL(s->kernelSurface);}
    if(kok){v=k;IOFree(result,bytes);IOLog("PianoVideoVT B3: lookup id %u through the kernel surface object (no lease)\n",id);return true;}
  }
  // A successful method owns both references, even if layout validation fails.
  if(!rc){s->surfaceLease.acquire(id);++s->surfaceLookups;}
  bool ok=!rc && resultBytes==bytes;
  if(ok)ok=contract::surfaceResult(result,bytes,id,v,s->config.s.width,s->config.s.height,s->config.pixelFormat());
  IOLog("PianoVideoVT V5a: surface lookup id %u status %08x result-bytes %u format %08x planes %u valid %u\n",id,rc,resultBytes,v.format,v.planes,unsigned(ok));
  IOLog("PianoVideoVT V6p: lookup token %llu request %llu id %u status %08x held %u\n",s->frame.token,s->guest.request,id,rc,unsigned(s->surfaceLease.held()));
  IOFree(result,bytes);if(!ok)failure(s,42,rc?int(rc):kBadData);return ok;
}
bool copyFrameKernel(Session *s,const Frame &f,const contract::Surface &v){
  OSObject *surf=s->kernelSurface;s->kernelSurface=nullptr;if(!surf){failure(s,90,kBadState);return false;}
  const uint8_t *b=reinterpret_cast<const uint8_t *>(surf);
  IOMemoryDescriptor *md=*reinterpret_cast<IOMemoryDescriptor *const *>(b+0x30);const uint64_t off=*reinterpret_cast<const uint64_t *>(b+0x88);
  bool ok=false;
  if(md&&off<=md->getLength()&&v.size<=md->getLength()-off&&!md->prepare()){
    IOMemoryMap *map=md->createMappingInTask(kernel_task,0,kIOMapAnywhere,off,v.size);
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
    ok=dst&&map->getLength()>=v.size&&pianovideo::vtsurface::copyAndPublish(f.layout,f.length,const_cast<const uint8_t *>(f.cpu),v,dst,publisher);
    OSSafeReleaseNULL(map);md->complete();
  }
  {char line[120];snprintf(line,sizeof line,"kernel copy token %llu id %u %s",f.token,v.id,ok?"OK":"FAILED");IOLog("PianoVideoVT B3: %s\n",line);static unsigned n=0;if(n++<4)diagRecord(line);}
  surf->release();
  if(!ok)failure(s,91,kNoMemory);
  return ok;
}
bool copyFrame(Session *s,const Frame &f,const contract::Surface &lookup) {
  if(s->kernelSurface)return copyFrameKernel(s,f,lookup);
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
  // Keep the lookup alive while an unsuccessful unlock still owns the write.
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
  OSSafeReleaseNULL(s->kernelSurface);
  if(!s->surfaces)return;
  if(s->surfacesBorrowed){
    // The foreign client stays open. Retry only the obligations still held.
    const bool unlocked=unlockSurfaceWrite(s);
    const bool released=!s->surfaceLease.held()||releaseSurface(s);
    if(!unlocked||!released)s->completionError=kDecodeError;
  }else{
    s->surfaces->clientClose();++s->surfaceCloses;
  }
  OSSafeReleaseNULL(s->surfaces);s->surfacesBorrowed=false;
  s->surfaceLease.closed();s->writeSurfaceID=0;
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
  uint8_t b[192];const uint64_t id=s->client->takeRequest();Writer w(b,sizeof b);
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
// Called with the session lock held; selector3 must have returned before a
// guest can send this ACK. In 27.3 the dinv reply bypasses the queued Demt
// handler and clears its VTDecoderSession target. Queue order alone is unsafe.
bool waitForTerminalAck(Session *s){
  // PV's default dinv reply timeout is 11/10 s. Leave the same 200 ms
  // margin as admission replies; a timed-out PV target can ACK without emit.
  const auto expired=[&]{const uint64_t now=nowMs();return s->guest.expired(now)||(s->invalidateRequest&&now>=s->invalidateDeadline);};
  while(!s->abandoning&&s->guest.type==cc("Demt")&&!s->guest.replied&&!expired()){
    IOLockUnlock(s->lock);IOSleep(1);IOLockLock(s->lock);
  }
  const bool ok=!s->abandoning&&s->guest.type==cc("Demt")&&s->guest.replied&&!s->replyError&&!expired();
  s->guest.clear();
  if(!ok&&!s->abandoning)s->completionError=kDecodeError;
  return ok;
}
// Cancellation has its own error-only send path. It never carries a surface
// UUID, never retries a delivered Demt, and never reopens the success fence.
bool emitTerminalError(Session *s,uint64_t token,pianovideo::stream::Record *record=nullptr){
  if(s->abandoning)return false;
  uint8_t b[160];const uint64_t id=s->client->takeRequest();Writer w(b,sizeof b);
  w.header(cc("Demt"),FlagHostRequest,s->uuid,id);w.s64(cc(",frm"),int64_t(token));
  w.s32(cc(",err"),kDecodeError);w.u32(cc(",inf"),3);
  s->replyError=kDecodeError;s->replyHasSurface=false;
  const bool sent=s->guest.begin(id,cc("Demt"),nowMs())&&send(s->client,w,b);
  if(record){record->completion.sent(sent);if(sent)record->phase=pianovideo::stream::Phase::Emitted;}
  IOLog("PianoVideoVT V6: terminal Demt token %llu status %d image 0 sent %u\n",token,kDecodeError,unsigned(sent));
  if(!sent){s->guest.clear();s->completionError=kDecodeError;return false;}
  const bool ack=waitForTerminalAck(s);
  if(record&&ack)record->completion.ack();
  return ack;
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
          uint8_t b[128];const uint64_t id=s->client->takeRequest();Writer w(b,sizeof b);
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
  // Fence successes, then release all native/surface ownership before sending
  // null-image terminal dispositions. Keep an already delivered Demt's ACK;
  // replacing that message would complete the same Apple frame twice.
  IOLockLock(s->lock);s->stopping=true;
  if(s->guest.type!=cc("Demt"))s->guest.clear();
  const bool finish=s->finishWanted;const uint64_t finishID=s->finishRequest;s->finishWanted=false;
  IOLockUnlock(s->lock);
  piano_video_native_cancel(s->native);
  closeSurface(s);
  IOLog("PianoVideoVT V6p: surface final lookup %u invalidate %u release %u held %u maps %u open %u close %u\n",
    s->surfaceLookups,s->surfaceInvalidates,s->surfaceReleases,unsigned(s->surfaceLease.held()),s->maps,s->surfaceOpens,s->surfaceCloses);
  if(s->frame.memory)piano_video_native_frame_release(s->native,&s->frame);
  for(auto &in:s->inputs)if(in.bytes){IOFree(in.bytes,contract::kMaxAuBytes);in.bytes=nullptr;in.size=0;}
  s->closeError=piano_video_native_close(s->native);
  IOLockLock(s->lock);
  if(s->guest.type==cc("Demt")){
    const bool ack=waitForTerminalAck(s);
    auto *f=s->currentAccepted?s->frames.find(s->currentToken):nullptr;
    if(f&&ack&&!f->completion.acknowledged)f->completion.ack();
  }
  if(s->current&&!s->currentAccepted&&!s->currentEmitted)emitTerminalError(s,s->currentToken);
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
  while(s->rejected.pop(rejection))emitTerminalError(s,uint64_t(rejection.token));
  for(auto &f:s->frames.frames)if(f.phase!=pianovideo::stream::Phase::Free && f.phase!=pianovideo::stream::Phase::Emitted&&f.completion.needsErrorEmit()){
    emitTerminalError(s,f.token,&f);
  }
  s->frames.cancel();s->guest.cancel();
  if(finish){Header h;h.type=cc("dfin");h.request=finishID;memcpy(h.uuid,s->uuid,16);status(s->client,h,kDecodeError);}
  IOLog("PianoVideoVT V6: worker close session %u status %d pending0 cancelled %u surface-open %u surface-close %u maps %u high-water %u worker-joined-next 1\n",
        s->native,s->closeError,s->frames.cancelled,s->surfaceOpens,s->surfaceCloses,s->maps,s->frames.highWater);
  s->native=0;
  if(s->invalidateRequest&&!s->abandoning){
    Header h;h.type=cc("dinv");h.request=s->invalidateRequest;memcpy(h.uuid,s->uuid,16);
    status(s->client,h,(s->closeError||s->completionError)?kDecodeError:0);s->invalidateRequest=0;
  }
  s->workerRunning=false;IOLockWakeup(s->lock,&s->workerRunning,false);IOLockUnlock(s->lock);
  thread_terminate(current_thread());
}
void teardown(Session *s) {
  IOLockLock(s->lock);s->invalidating=true;s->stopping=true;s->abandoning=true;
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
  // LT-B5 (cryptex1-qual5/6, ctl1): a WebKit client (Safari's GPU process, admitted by the grant) is offered AVC only. With our HEVC
  // entry VT in the GPU process picks it for every HEVC stream and has no in-process fallback when the native envelope (1080p) declines a
  // 4K one (Safari hevc 2160p: code 4; it plays in software through VTDecoderXPCService on the installed y12h). Without the entry, HEVC
  // stays on that path exactly as before the grant; AVC above 1080p already falls back (its VTMaxDecodeLevel 42).
  char who[32]={};proc_name(c->pid,who,sizeof who);const bool webkit=!strncmp(who,"com.apple.WebKit",16);
  unsigned n=0;
  if(h.type==cc("decs")&&ready){entries[n++]=contract::inventoryEntry(*p,false);if(!webkit)entries[n++]=contract::inventoryEntry(*p,true);}
  const int list=p->array(entries,n);
  Writer w(b,sizeof b);w.header(h.type,FlagReply,h.uuid,h.request);w.plist(h.type==cc("decs")?cc(",dec"):cc(",enc"),*p,list);w.u32(cc(",ver"),6);send(c,w,b);delete p;
  IOLog("PianoVideoVT V9: inventory version6 avc1%s ready %u encoders0 rating400 (pid %d)\n",webkit?"":" hvc1",unsigned(ready),c->pid);
}
void capabilities(Client *c,const Header &h,const uint8_t *m,uint32_t n){
  int32_t codec=0,version=0;
  const bool supported=paramS32(m,n,cc(",cod"),codec)&&(uint32_t(codec)==cc("avc1")||uint32_t(codec)==cc("hvc1"))&&
    paramS32(m,n,cc(",ver"),version)&&version==6&&piano_video_native_ready()&&
    !(uint32_t(codec)==cc("hvc1")&&[&]{char who[32]={};proc_name(c->pid,who,sizeof who);return !strncmp(who,"com.apple.WebKit",16);}());
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
    int32_t error=kDecodeError;paramS32(m,n,cc(",err"),error);Param uuid;bool hasSurface=false;
    if(h.type==cc("Dbuf")&&!error){
      hasSurface=surfaces==1&&ids&&ids[0]&&param(m,n,cc(",uui"),uuid)&&uuid.type==cc("=uui")&&uuid.size==16;
      if(!hasSurface)error=kDecodeError;
    }
    IOLockLock(s->lock);
    if(!s->guest.matches(h.request,h.type)||s->guest.expired(nowMs())||s->abandoning||(s->stopping&&h.type!=cc("Demt"))){
      IOLockUnlock(s->lock);return -1;
    }
    s->replyError=error;s->replyHasSurface=hasSurface;
    if(hasSurface){s->replySurfaceID=ids[0];memcpy(s->replyUuid,uuid.value,16);}
    s->guest.reply(h.request,h.type);
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
    case cc("dinv"):{
      IOLockLock(s->lock);
      const bool running=s->workerRunning,duplicate=s->invalidateRequest!=0;
      if(!duplicate){s->invalidating=true;s->stopping=true;if(running){s->invalidateRequest=h.request;s->invalidateDeadline=nowMs()+900;}}
      IOLockUnlock(s->lock);
      // Return selector3 so Apple's send mutex and our user-client lock are
      // free for terminal Demt ACKs. The worker owns the deferred dinv reply.
      if(duplicate)status(c,h,kBadState);
      else if(!running){teardown(s);status(c,h,(s->closeError||s->completionError)?kDecodeError:0);}
      break;
    }
    case cc("-dec"):c->session=nullptr;destroy(s);break;
    case cc("dsup"):properties(c,h,false);break;
    case cc("dcop"):{Param p;char key[96]="";if(param(m,n,cc(",key"),p)&&plistString(p.value,p.size,key,sizeof key)&&!strcmp(key,"UsingHardwareAcceleratedVideoDecoder"))properties(c,h,true);else status(c,h,-12900);break;}
    case cc("dset"):case cc("dmul"):status(c,h,-12900);break;
    default:droidvm_vthost_unknown=droidvm_vthost_unknown+1;droidvm_vthost_last_unknown=h.type;if(h.flags&FlagRequest)status(c,h,-12900);break;
  }
  IOLockUnlock(c->lock);return 0;
}
}
