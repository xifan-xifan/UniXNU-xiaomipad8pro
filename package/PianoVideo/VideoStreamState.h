// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Single-writer ownership model shared by the firmware pump and host tests.
#pragma once
#include <stdint.h>
namespace pianovideo { namespace stream {
constexpr unsigned Credits = 8, Inputs = 2, Raws = 64;
// Reply to at most four dfrm requests ahead of their Demt acknowledgements.
// The next request can reserve a credit but stays Admitting until its reply;
// selector3 must return so the earlier frames' guest replies remain routable.
constexpr unsigned PublishedWindow = 4;
enum class FinishDecision { Schedule, Failed, BadState };
inline FinishDecision finishDecision(bool started, bool invalidating, bool failed, bool running, bool stopping, bool pending) {
  // A decoder error ends native work, but the PV session still owns its error
  // until explicit invalidation. Repeated finish must report that same error.
  if(!started||invalidating)return FinishDecision::BadState;
  if(failed)return FinishDecision::Failed;
  return running&&!stopping&&!pending?FinishDecision::Schedule:FinishDecision::BadState;
}
// Synchronous requests own a deferred original reply. They can enter hardware
// before that reply; asynchronous requests still wait for publication.
enum class Phase : uint8_t { Free, Admitting, SyncQueued, Queued, Submitted, Picture, Delivering, Emitted };
struct Completion {
  static constexpr uint64_t SyncDeadlineMs = 8000; // target PV operation timeout is 10 s
  uint64_t request=0, deadline=0;
  bool synchronous=false, emitAttempted=false, emitSent=false, acknowledged=false, replied=false, failed=false;
  void begin(uint32_t flags,uint64_t id,uint64_t now) {
    *this=Completion{};synchronous=!(flags&1);request=synchronous?id:0;
    deadline=synchronous?now+SyncDeadlineMs:0;
  }
  bool expired(uint64_t now) const {return synchronous&&!replied&&now>=deadline;}
  bool claimEmit() {if(emitAttempted||replied)return false;emitAttempted=true;return true;}
  void sent(bool ok) {emitSent=ok;if(!ok)failed=true;}
  bool needsErrorEmit() const {return !synchronous&&!emitSent;}
  bool ack() {if(!emitSent||acknowledged||replied)return false;acknowledged=true;return true;}
  // An error reply is the sole completion owner if emission never began.
  // After a Demt was sent it only releases the waiting original call; it never
  // authorizes a second emit. Claim under the session lock before transport.
  uint64_t claimReply(bool success,uint64_t now) {
    if(!synchronous||replied||!request||(success&&(!acknowledged||failed||expired(now))))return 0;
    replied=true;if(!success)failed=true;return request;
  }
};
struct Record {
  uint64_t token = 0, serial = 0;
  uint32_t flags = 0;
  Phase phase = Phase::Free;
  Completion completion;
};
struct Ledger {
  Record frames[Credits] = {};
  bool inputOwned[Inputs] = {}, rawHeld[Raws] = {};
  uint64_t inputToken[Inputs] = {}, rawToken[Raws] = {};
  unsigned inputUses[Inputs] = {}, rawUses[Raws] = {};
  unsigned pending = 0, highWater = 0, inputReuse = 0, rawReuse = 0, retired = 0, cancelled = 0;
  uint64_t accepted = 0, barrier = 0;
  bool draining = false, ack = false, last = false, paused = false, stopped = false;
  Record *find(uint64_t t) { for(auto &f:frames) if(f.phase!=Phase::Free && f.token==t)return &f; return nullptr; }
  unsigned published() const {
    unsigned n=0;for(const auto &f:frames)if((f.flags&1)&&f.phase!=Phase::Free&&f.phase!=Phase::Admitting)++n;return n;
  }
  Record *nextAdmission() {
    Record *next=nullptr;
    for(auto &f:frames)if(f.phase==Phase::Admitting&&(!next||f.serial<next->serial))next=&f;
    return next;
  }
  Record *nextQueued() {
    Record *next=nullptr;
    for(auto &f:frames)if((f.phase==Phase::Admitting||f.phase==Phase::Queued||f.phase==Phase::SyncQueued)&&(!next||f.serial<next->serial))next=&f;
    // A later synchronous AU cannot overtake an older held async admission.
    return next&&next->phase!=Phase::Admitting?next:nullptr;
  }
  bool replyReady() const {return !stopped&&published()<PublishedWindow;}
  Record *accept(uint64_t t, uint32_t flags = 1, bool published = true) {
    if(stopped || draining || pending==Credits || find(t))return nullptr;
    for(auto &f:frames)if(f.phase==Phase::Free){f=Record{};f.token=t;f.serial=++accepted;f.flags=flags;f.phase=published?Phase::Queued:((flags&1)?Phase::Admitting:Phase::SyncQueued);++pending;if(pending>highWater)highWater=pending;return &f;}
    return nullptr;
  }
  bool publish(uint64_t t) {auto *f=find(t);if(!f||f->phase!=Phase::Admitting||stopped)return false;f->phase=Phase::Queued;return true;}
  int freeInput() const {
    int chosen=-1;
    for(unsigned i=0;i<Inputs;++i)if(!inputOwned[i]&&(chosen<0||inputUses[i]<inputUses[chosen]))chosen=int(i);
    return chosen;
  }
  bool submit(uint64_t t,unsigned i) {
    Record *f=find(t);if(stopped||!f||(f->phase!=Phase::Queued&&f->phase!=Phase::SyncQueued)||i>=Inputs||inputOwned[i])return false;
    inputOwned[i]=true;inputToken[i]=t;if(inputUses[i]++)++inputReuse;f->phase=Phase::Submitted;return true;
  }
  bool ebd(unsigned i) {if(i>=Inputs||!inputOwned[i])return false;inputOwned[i]=false;return true;}
  bool picture(uint64_t t,unsigned i) {
    Record *f=find(t);if(!f||f->phase!=Phase::Submitted||i>=Raws||rawHeld[i])return false;
    f->phase=Phase::Picture;rawHeld[i]=true;rawToken[i]=t;if(rawUses[i]++)++rawReuse;return true;
  }
  bool release(unsigned i,uint64_t t) {
    if(i>=Raws||!rawHeld[i]||rawToken[i]!=t)return false;
    rawHeld[i]=false;return true;
  }
  bool retire(uint64_t t) {
    Record *f=find(t);if(!f)return false;f->phase=Phase::Free;--pending;++retired;return true;
  }
  bool inputsEmpty() const {for(bool x:inputOwned)if(x)return false;return true;}
  bool loansEmpty() const {for(bool x:rawHeld)if(x)return false;return true;}
  bool beginDrain() {if(stopped)return false;if(draining||paused)return true;draining=true;barrier=accepted;ack=last=false;return true;}
  bool drainReady() const {return draining&&ack&&last&&inputsEmpty();}
  bool drained() {if(!drainReady()||pending||!loansEmpty())return false;draining=false;paused=true;return true;}
  bool resume() {if(stopped||draining||pending||!paused)return false;paused=ack=last=false;return true;}
  void cancel() {stopped=true;for(auto &f:frames)if(f.phase!=Phase::Free){f.phase=Phase::Free;++cancelled;}pending=0;}
};
// One guest request at a time. An epoch belongs to one session UUID; IDs never
// reset within it. No blocking wait is required to keep pumping firmware.
struct Guest {
  uint64_t request=0, deadline=0;
  uint32_t type=0;
  bool replied=false, stopped=false;
  bool begin(uint64_t id,uint32_t t,uint64_t now) {
    if(stopped||!id||!t||(type&&!replied))return false;
    request=id;type=t;deadline=now+2000;replied=false;return true;
  }
  bool matches(uint64_t id,uint32_t t) const {return !stopped&&type&&id==request&&t==type&&!replied;}
  bool reply(uint64_t id,uint32_t t) {if(!matches(id,t))return false;replied=true;return true;}
  bool expired(uint64_t now) const {return type&&!replied&&now>=deadline;}
  void clear(){request=deadline=0;type=0;replied=false;}
  void cancel(){clear();stopped=true;}
};
// Exclusivity is independent of per-picture credits. Quiesce failure is sticky.
struct Owner {
  enum Phase { Idle, Reserved, Running, Closing, Poisoned } phase = Idle;
  uint32_t id=0, serial=0;
  bool owns(uint32_t s) const {return s&&s==id&&(phase==Reserved||phase==Running);}
  int acquire(uint32_t *s) {if(!s)return -22;if(phase!=Idle||serial==0xffff)return -16;*s=id=0x56360000u|++serial;phase=Reserved;return 0;}
  bool start(uint32_t s) {if(!owns(s)||phase!=Reserved)return false;phase=Running;return true;}
  int closing(uint32_t s,bool loans) {if(!owns(s))return -22;if(loans)return -16;phase=Closing;return 0;}
  // DMA release proves memory safety, not a healthy device. A failed session
  // must never bootstrap a new physical core in this boot.
  void closed(bool held,int error=0) {if(phase!=Closing)return;phase=(held||error)?Poisoned:Idle;id=0;}
};
} }
