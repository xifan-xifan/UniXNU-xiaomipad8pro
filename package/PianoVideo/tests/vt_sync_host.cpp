// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Protocol/ownership test, not firmware evidence. The actual worker uses these
// ledger, completion and guest-request primitives under its session lock.
#include "../VideoStreamState.h"
#include "../vt/PianoVideoVTContract.h"
#include <cassert>
#include <cstdio>
#include <mutex>
#include <vector>
using namespace pianovideo;
using namespace vtwire;
struct Peer {
  stream::Ledger ledger;
  stream::Guest guest;
  std::mutex selector;
  std::vector<uint32_t> requests;
  unsigned replies=0, emissions=0, surfaces=0;
  bool cancelled=false, receiveUnlocked=false;
  uint64_t clock=100;
  stream::Record *frame=nullptr;
  void selector3(uint32_t flags) {
    std::lock_guard<std::mutex> sendMutex(selector);
    frame=ledger.accept(19,flags,false);assert(frame);
    frame->completion.begin(flags,71,clock);
    // A sync call returns without publication or a reply. No callback can run
    // here: selector3 owns Apple's guest send mutex until it returns.
    assert(frame->phase==stream::Phase::SyncQueued&&!ledger.published()&&!replies);
  }
  void workerSubmit() {
    assert(selector.try_lock());selector.unlock();
    assert(ledger.submit(19,0)&&ledger.ebd(0));
    assert(!ledger.submit(19,1));
  }
  bool request(uint32_t type,bool sendWorks=true) {
    // Receiver reentry must acquire the same mutex that selector3 released.
    receiveUnlocked=selector.try_lock();assert(receiveUnlocked);selector.unlock();
    assert(guest.begin(100+requests.size(),type,clock));requests.push_back(type);
    if(sendWorks)assert(guest.reply(guest.request,type));return sendWorks;
  }
  bool reply(bool success) {
    const auto id=frame->completion.claimReply(success&&!cancelled,clock);
    if(!id)return false;assert(id==71);++replies;return true;
  }
  bool emit(bool image,bool sendWorks=true) {
    if(cancelled||frame->completion.expired(clock)||!frame->completion.claimEmit())return false;
    ++emissions;
    uint8_t wire[192],uuid[16]={};Writer w(wire,sizeof wire);
    w.header(cc("Demt"),FlagHostRequest,uuid,101);w.u32(cc(",inf"),vtcontract::completionInfo(frame->flags,image,0));
    Param info;assert(param(wire,w.size(),cc(",inf"),info));
    assert(be32(info.value)==(image?0u:2u));
    const bool sent=request(cc("Demt"),sendWorks);frame->completion.sent(sent);return sent;
  }
  void acknowledge() {assert(guest.replied&&guest.type==cc("Demt"));assert(frame->completion.ack());}
  void cancel() {cancelled=true;guest.cancel();assert(reply(false));}
};
static void success(uint32_t flags) {
  Peer p;p.selector3(flags);p.workerSubmit();
  assert(!p.reply(true)); // original dfrm cannot succeed before Demt ACK
  if(!(flags&2)){assert(p.request(cc("Dbuf")));++p.surfaces;}
  assert(p.emit(!(flags&2))&&!p.reply(true));p.acknowledge();
  assert(p.reply(true)&&!p.reply(true)&&!p.reply(false));
  assert(p.emissions==1&&p.replies==1&&p.surfaces==((flags&2)?0u:1u));
  assert(!p.frame->completion.claimEmit()&&p.ledger.retire(19));
}
static void failures() {
  // Error/cancel before RAW: original reply owns completion; no Demt.
  {Peer p;p.selector3(0);p.workerSubmit();p.cancel();assert(!p.emit(true)&&p.emissions==0);}
  // Cancel after Dbuf ACK suppresses all successful emit/reply paths.
  {Peer p;p.selector3(0);p.workerSubmit();assert(p.request(cc("Dbuf")));p.cancel();assert(!p.emit(true)&&!p.reply(true));}
  // Cancel after emission never emits a second time, even on a late ACK.
  {Peer p;p.selector3(0);p.workerSubmit();assert(p.emit(true));p.cancel();assert(!p.frame->completion.ack()&&!p.emit(true)&&!p.reply(true));assert(p.emissions==1);}
  // Send failure is a terminal attempt; cleanup releases the original once.
  {Peer p;p.selector3(2);p.workerSubmit();assert(!p.emit(false,false));assert(p.reply(false)&&!p.reply(false)&&!p.emit(false));assert(p.emissions==1);}
  // Deadline is whole request, not reset by a Dbuf or repeated native polls.
  {Peer p;p.selector3(0);p.workerSubmit();assert(p.request(cc("Dbuf")));p.clock+=stream::Completion::SyncDeadlineMs;assert(p.frame->completion.expired(p.clock));assert(!p.emit(true)&&p.reply(false)&&!p.reply(true));}
  {Peer p;p.selector3(0);p.workerSubmit();assert(p.emit(true));p.clock+=stream::Completion::SyncDeadlineMs;p.acknowledge();assert(!p.reply(true)&&p.reply(false)&&p.emissions==1);}
}
static void asynchronousRegression() {
  stream::Ledger l;
  for(unsigned i=1;i<=stream::PublishedWindow;++i){auto *f=l.accept(i,1,false);assert(f&&l.replyReady()&&l.publish(i));}
  assert(!l.replyReady());auto *held=l.accept(5,1,false);assert(held&&held->phase==stream::Phase::Admitting&&!l.submit(5,0));
  // A sync record neither depends on publication nor consumes the async window.
  auto *sync=l.accept(6,0,false);assert(sync&&sync->phase==stream::Phase::SyncQueued&&l.submit(6,0));
  assert(l.published()==4&&l.retire(1)&&l.replyReady()&&l.publish(5));
  stream::Ledger order;assert(order.accept(20,1,false));auto *later=order.accept(21,0,false);
  assert(later&&!order.nextQueued());assert(order.publish(20)&&order.nextQueued()->token==20);
  assert(order.submit(20,0)&&order.nextQueued()==later);
  assert(vtcontract::completionInfo(1,true,0)==1&&vtcontract::completionInfo(3,false,0)==3);
  // queue->enqueue false guarantees the attempted async Demt was not
  // delivered. Its accepted token still needs the cleanup error emission;
  // sync instead settles through its original reply, never a second Demt.
  stream::Completion async;async.begin(1,70,0);assert(async.claimEmit());async.sent(false);
  unsigned delivered=0;assert(async.needsErrorEmit());if(async.needsErrorEmit()){++delivered;async.sent(true);}
  assert(delivered==1&&!async.needsErrorEmit());
  stream::Completion syncFailure;syncFailure.begin(0,71,0);assert(syncFailure.claimEmit());syncFailure.sent(false);
  assert(!syncFailure.needsErrorEmit()&&syncFailure.claimReply(false,1)==71&&!syncFailure.claimReply(false,2));
  assert(vtcontract::completionInfo(0,true,0)==0&&vtcontract::completionInfo(2,false,0)==2);
}
int main(){success(0);success(2);failures();asynchronousRegression();std::puts("V9 sync host PASS selector-release reentry ACK-before-reply single-owner cancel send-failure deadline async-window4");}
