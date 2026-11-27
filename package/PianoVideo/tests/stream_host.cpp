// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Scripted firmware peer for the real persistent HFI state machine. These are
// ownership/protocol checks, not evidence that any frame decoded on hardware.
// c++ -std=c++20 -O2 -Wall -Wextra -Werror -I .. -I ../../include stream_host.cpp -o /tmp/stream_host && /tmp/stream_host
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <array>
#include <algorithm>
#include <string>
#include <CommonCrypto/CommonDigest.h>
#include "VideoSession.h"
using namespace pianovideo;
using namespace pianovideo::session;
static unsigned failures;
#define CHECK(x) do { if (!(x)) {std::printf("FAIL line %u: %s\n",__LINE__,#x);++failures;} } while(0)
struct Hardware {
  uint64_t clock=0;
  std::string log;
  void barrier() {}
  void stage(int,const char *) {}
  void sleepMs(unsigned n) {clock+=uint64_t(n)*1000;}
  uint64_t nowUs() const {return clock;}
  void note(const char *fmt,...) {
    char line[1024];va_list ap;va_start(ap,fmt);vsnprintf(line,sizeof line,fmt,ap);va_end(ap);
    log+=line;log+='\n';if(std::getenv("VIDEO_STREAM_ECHO"))std::puts(line);
  }
  void hashActive(const volatile uint8_t *base,char text[65]) {
    CC_SHA256_CTX ctx;CC_SHA256_Init(&ctx);
    activeRows(base,[&](const uint8_t *p,unsigned n){CC_SHA256_Update(&ctx,p,n);});
    uint8_t digest[32];CC_SHA256_Final(digest,&ctx);hexDigest(digest,text);
  }
};
struct Registers {
  bool fault=false,watchdog=false;
  uint32_t rd(uint64_t pa) {return pa==kIris+kWrapperIntrStatus?(watchdog?8:0):(fault&&(pa&4095)==vsmmu::kFsr?2:0);}
};
using Words=std::vector<uint32_t>;
struct Peer {
  Hardware hw;Registers ops;vsmmu::Plan plan;hfi::Mem mem;hfi::Ids ids;
  uint32_t tx[hfi::kBufferWords]={},rx[hfi::kBufferWords]={};
  struct {volatile uint8_t *arena=nullptr;} dma;
  struct {uint32_t fsr=0;} rep;
  unsigned watchdogSamples=0,watchdogSeen=0,rxEvents=0;
  std::vector<uint32_t> uc;
  std::vector<uint8_t> arena;
  std::array<Words,2> inputs;
  std::array<bool,2> inputPending={};
  std::array<Words,64> raws;
  std::array<bool,64> rawPending={};
  std::vector<Words> releasePending;
  std::vector<uint32_t> headers,packets,comvCounts,comvSizes;
  uint32_t sid=0;
  std::vector<Words> wire;
  unsigned n=4,etbs=0,ftbs=0,dpbs=0,opens=0,closes=0,stops=0,drains=0;
  unsigned settingsResume=0,drainInputResume=0,drainRawResume=0;
  uint32_t pscPoc=0,pscProfile=2,pscLevel=4;
  uint32_t pscResolution=0x014000f0,pscDepth=0x00080008,pscCoded=1,pscCrop0=0,pscCrop1=0;
  bool omitOptional=false;
  unsigned decodeOrderProperties=0;
  bool watchdogOnStop=false,watchdogOnClose=false;
  bool autoEbd=true,autoDrainAck=true,autoDrainLast=true,dropStop=false,dropClose=false,lateClose=false;
  bool lastWithPicture=false;
  uint64_t finalToken=0;
  bool pictureBeforeEbd=false;
  Peer():uc(kUcBytes/4),arena(kArenaBytes) {mem.uc=uc.data();dma.arena=arena.data();hfi::initQueues(mem);plan.bank=3;plan.pages=128;}
  static Words packet(uint32_t type,uint32_t flags,uint32_t port,uint32_t payload,const Words &data={}) {
    Words p={uint32_t(32+4*data.size()),type,flags,payload,port,0x1234,0,0};p.insert(p.end(),data.begin(),data.end());return p;
  }
  void reply(const std::vector<Words> &parts,uint32_t session=0) {
    Words b={32,session?session:sid,0xabc,0,0,0,0,uint32_t(parts.size())};
    for(const auto &p:parts)b.insert(b.end(),p.begin(),p.end());b[0]=uint32_t(4*b.size());
    CHECK(hfi::write(mem,hfi::kMsgq,b.data(),b[0],[]{})==0);
  }
  void psc() {
    std::vector<Words> parts={packet(Settings,0,1,0),packet(0x03000103,0,1,1,{pscResolution}),packet(0x03000105,0,1,13,{pscCrop0,pscCrop1}),
      packet(0x0300010f,0,1,1,{pscDepth}),packet(0x03000120,0,1,1,{pscCoded}),packet(0x03000124,0,1,1,{n})};
    if(!omitOptional){parts.push_back(packet(0x03000128,0,1,1,{pscPoc}));parts.push_back(packet(0x03000107,0,1,1,{pscProfile}));
      parts.push_back(packet(0x03000108,0,1,1,{pscLevel}));}
    parts.push_back(packet(0x03000155,0,1,1,{0x14000003}));reply(parts);
  }
  void ebd(unsigned index,bool duplicate=false) {
    CHECK(index<inputs.size()&&!inputs[index].empty());
    if(index>=inputs.size()||inputs[index].empty())return;
    CHECK(inputPending[index]||duplicate);auto b=inputs[index];b[7]=b[8]=b[9]=0;b[10]=0;
    inputPending[index]=false;reply({packet(Buffers,0,1,5,b)});
  }
  unsigned freeRaw() const {for(unsigned i=0;i<n;++i)if(rawPending[i])return i;return 64;}
  void output(uint64_t token,uint32_t picture=2,uint32_t flags=0,unsigned index=64,bool unknown=false) {
    if(index==64)index=freeRaw();
    CHECK(index<raws.size()&&!raws[index].empty());
    if(index>=raws.size()||raws[index].empty())return;
    CHECK(rawPending[index]||unknown);auto b=raws[index];rawPending[index]=false;
    b[7]=kRawBytes;b[8]=uint32_t(token);b[9]=uint32_t(token>>32);b[10]=flags;
    // Distinguish every output's active rows; consumer can prove retained RAW
    // did not change while other slots were pumped and recycled.
    auto *base=arena.data()+b[2]-kArenaBase;
    for(unsigned y=0;y<240;++y)for(unsigned x=0;x<320;++x)base[y*384+x]=uint8_t(token+x+y);
    for(unsigned y=0;y<120;++y)for(unsigned x=0;x<320;++x)base[kUvOffset+y*384+x]=uint8_t((token>>8)+x+y);
    reply({packet(Buffers,0,2,5,b),packet(0x03000162,0,2,1,{picture})});
  }
  void emptyLast() {
    const unsigned index=freeRaw();CHECK(index<raws.size());if(index>=raws.size())return;
    auto b=raws[index];rawPending[index]=false;b[7]=b[8]=b[9]=0;b[10]=0x10000000;
    reply({packet(Buffers,0,2,5,b)});
  }
  void packetTrace(const char *,unsigned,const uint32_t *,unsigned) {}
  void drainDebug() {}
  int send(unsigned bytes,const char *) {
    CHECK(hfi::write(mem,hfi::kCmdq,tx,bytes,[]{})==0);
    uint32_t data[hfi::kBufferWords],got=0;
    CHECK(hfi::read(mem,hfi::kCmdq,data,sizeof data,&got,[]{})==0);
    CHECK(got==bytes&&data[7]==1);
    wire.emplace_back(data,data+got/4);
    const auto *p=data+8;const auto type=p[1],flags=p[2],port=p[4],payload=p[3];
    headers.push_back(data[2]);packets.push_back(p[5]);
    if(type==Open){CHECK(data[1]==0&&flags==3&&payload==1);sid=p[8];++opens;return 0;}
    CHECK(data[1]==sid);
    if(type==Start){CHECK(flags==3&&payload==0&&(port==1||port==2));return 0;}
    if(type==Resume){
      CHECK(flags==3&&payload==1);
      if(p[8]==Settings){CHECK(port==1);++settingsResume;for(auto b:releasePending){b[10]=1;reply({packet(Buffers,0,1,5,b)});}releasePending.clear();}
      else if(p[8]==Drain){if(port==1)++drainInputResume;else if(port==2)++drainRawResume;else CHECK(false);}
      else CHECK(false);
      return 0;
    }
    if(type==Buffers){
      CHECK(payload==5&&p[0]==96);Words b(p+8,p+24);CHECK((b[10]&0x200)&&!b[3]&&!b[4]&&!b[6]);
      if(b[10]&1){CHECK(flags==3);releasePending.push_back(b);return 0;}
      CHECK(flags==1);
      if(b[0]==Bitstream){
        CHECK(b[1]<2&&!inputPending[b[1]]&&b[7]&&b[7]<=(1u<<20));
        inputs[b[1]]=b;inputPending[b[1]]=true;++etbs;
        if(autoEbd)ebd(b[1]);if(etbs==1)psc();
      } else if(b[0]==Raw){CHECK(b[1]<64&&!rawPending[b[1]]&&b[7]==0);raws[b[1]]=b;rawPending[b[1]]=true;++ftbs;}
      else if(b[0]==Dpb){CHECK(b[1]<n);++dpbs;}
      else if(b[0]==Comv)comvSizes.push_back(b[5]);
      return 0;
    }
    if(type==Drain){
      CHECK(flags==7&&port==1&&payload==0);++drains;
      if(autoDrainAck)reply({packet(Drain,1,1,0)});
      if(autoDrainLast){if(lastWithPicture)output(finalToken,4,0x10000000);else emptyLast();}
      return 0;
    }
    if(type==Stop){CHECK(flags==7);++stops;if(watchdogOnStop)ops.watchdog=true;if(!dropStop)reply({packet(Stop,1,port,0)});return 0;}
    if(type==Close){if(watchdogOnClose)ops.watchdog=true;CHECK(flags==7&&port==0);++closes;if(!dropClose){reply({packet(Close,1,0,0)});if(lateClose)reply({packet(0x03000162,0,2,1,{2})});}return 0;}
    if(type==0x03000193)comvCounts.push_back(p[8]);
    if(type==0x0300015b&&port==1){CHECK(p[8]==1&&payload==1&&etbs==0);++decodeOrderProperties;}
    if(type==Subscribe)CHECK(flags==3&&payload==11);
    else CHECK(type>0x03000000&&type<0x03ffffff&&flags==0);
    return 0;
  }
};
struct Fixture {
  Peer peer;Layout layout;SelfTest<Peer> tx;
  uint32_t sid=0x56361234;
  Fixture():tx(peer,layout) {CHECK(layout.init());tx.streaming=true;tx.checkGolden=false;tx.expectedPoc=0;tx.expectedProfile=2;tx.expectedLevel=4;tx.psc.poc=0;tx.psc.profile=2;tx.psc.level=4;}
  void start(uint64_t token=0x100000001ull) {CHECK(tx.nativeInput(sid,kAccessUnit,sizeof kAccessUnit,token));CHECK(tx.transaction()==0);CHECK(tx.r.psc==1&&peer.opens==1&&peer.drains==0&&peer.decodeOrderProperties==1);}
  void pump() {CHECK(tx.pump()==0);}
  void releaseAll() {SelfTest<Peer>::OutputFrame f;while(tx.popOutput(f))CHECK(tx.releaseOutput(f.index,f.token)==0);}
  void drain() {CHECK(tx.beginDrain()==0);pump();CHECK(tx.drainStatus()==1);}
  void close() {CHECK(tx.shutdown()==0);CHECK(peer.stops==2&&peer.closes==1&&tx.r.closed);}
};
static void preparedInputTests() {
  // Session creation may finish well before its first compressed sample. It
  // must not decode the default fixture, start output, or acquire a credit.
  for(unsigned disposition=0;disposition<4;++disposition) {
    Fixture f;f.tx.sessionId=f.sid;f.tx.accessUnit=nullptr;f.tx.accessUnitBytes=0;
    CHECK(f.tx.prepareInput()==0 && f.tx.r.stage==Input);
    CHECK(f.peer.opens==1 && f.tx.openSent && f.tx.inputStarted && !f.tx.rawStarted);
    CHECK(!f.peer.etbs && !f.peer.ftbs && !f.peer.dpbs && !f.peer.drains && !f.tx.ledger.pending);
    CHECK(f.tx.prepareInput()==kErrArgument); // no duplicate OPEN
    f.peer.hw.clock+=10000000;
    CHECK(f.tx.pump()==0 && f.tx.deadline==UINT64_MAX);
    CHECK(!f.tx.nativeInput(f.sid+1,kAccessUnit,sizeof kAccessUnit,19));
    if(disposition==0) {
      CHECK(f.tx.nativeInput(f.sid,kAccessUnit,sizeof kAccessUnit,19));
      CHECK(f.tx.transaction()==0 && f.peer.opens==1 && f.peer.etbs==1);
      f.pump();f.releaseAll();f.close();
      CHECK(f.tx.transaction()==kErrArgument);
    } else if(disposition==1) {
      CHECK(f.tx.shutdown(true)==0 && f.peer.stops==1 && f.peer.closes==1);
      CHECK(!f.tx.r.stopRaw && f.tx.r.stopInput && f.tx.r.closed);
      CHECK(!f.peer.etbs && !f.tx.ledger.accepted);
      for(unsigned i=0;i<f.layout.used;++i)CHECK(!f.layout.buf[i].queued);
      CHECK(!f.tx.nativeInput(f.sid,kAccessUnit,sizeof kAccessUnit,19));
    } else {
      if(disposition==2)f.peer.dropStop=true;
      else f.peer.watchdogOnStop=true;
      const int error=disposition==2?kErrHfiTimeout:kErrWatchdog;
      CHECK(f.tx.shutdown(true)==error && f.peer.stops==1 && !f.peer.closes);
      const auto commands=f.peer.headers.size();
      CHECK(f.tx.shutdown(true)==error && f.peer.headers.size()==commands);
    }
  }
}
static void productionWorkTests() {
  // General native buffers are firmware outputs, not CPU sentinel fixtures.
  // Poison their backing independently of the decoder and inspect every slot
  // before any modeled FBD; this catches hidden writes as well as wrong extents.
  Fixture f;CHECK(f.layout.init(320,240) && f.layout.general);
  std::fill(f.peer.arena.begin(),f.peer.arena.end(),0x3c);
  diagnostic::History history;f.tx.history=&history;
  f.start();
  bool pristine=true;
  for(unsigned i=0;i<f.layout.maxN();++i) {
    const auto &raw=*f.layout.find(Raw,i);
    for(unsigned j=0;j<raw.capacity;++j)pristine&=f.peer.arena[raw.off+j]==0x3c;
  }
  CHECK(pristine);
  CHECK(history.commands.total==f.peer.headers.size());
  CHECK(f.peer.hw.log.find("V4 TX stage 5 ")!=std::string::npos);
  CHECK(f.peer.hw.log.find("V4 TX stage 6 ")!=std::string::npos);
  CHECK(f.peer.hw.log.find("V4 property ")==std::string::npos);
  // FBD ownership, retained TX/RX evidence, publication loan and STOP/CLOSE
  // must remain real after removing the unrelated diagnostic writes/text.
  f.peer.output(0x100000001ull);f.pump();
  SelfTest<Peer>::OutputFrame frame;CHECK(f.tx.popOutput(frame));
  CHECK(frame.token==0x100000001ull && f.peer.arena[f.layout.find(Raw,frame.index)->off]==1);
  CHECK(history.messages.total && f.tx.releaseOutput(frame.index,frame.token)==0);
  f.close();
  // Host-only packet evidence permits a byte-for-byte comparison with the
  // previous implementation; no tablet or proprietary payload is involved.
  if(std::getenv("VIDEO_PACKET_EVIDENCE"))for(const auto &p:f.peer.wire) {
    std::printf("WIRE");for(auto w:p)std::printf(" %08x",w);std::putchar('\n');
  }
}
static void nativeCleanupFailureTests() {
  for(unsigned kind=0;kind<3;++kind) {
    Fixture f;f.start();f.pump();f.releaseAll();
    if(kind==0)f.peer.dropStop=true;
    if(kind==1)f.peer.watchdogOnStop=true;
    if(kind==2)f.peer.watchdogOnClose=true;
    const unsigned samples=f.peer.watchdogSamples;
    const int rc=f.tx.shutdown(true);
    CHECK(rc==(kind==0?kErrHfiTimeout:kErrWatchdog) && f.tx.r.close==rc);
    CHECK(f.peer.stops==(kind==2?2u:1u) && f.peer.closes==(kind==2?1u:0u));
    if(kind==1)CHECK(f.tx.r.stopRaw); // ACK does not erase the same-pump watchdog
    if(kind==2)CHECK(f.tx.r.closed && f.peer.watchdogSamples-samples==3);
    const unsigned stops=f.peer.stops,closes=f.peer.closes;
    CHECK(f.tx.shutdown(true)==rc && f.peer.stops==stops && f.peer.closes==closes);
    CHECK(f.peer.hw.log.find("no-further-HFI 1")!=std::string::npos);
  }
}
static void ledgerTests() {
  stream::Ledger l;
  CHECK(l.freeInput()==0&&!l.find(1));
  CHECK(!l.submit(1,0)&&!l.ebd(0)&&!l.picture(1,0)&&!l.release(0,1)&&!l.retire(1));
  for(unsigned i=0;i<stream::Credits;++i)CHECK(l.accept(i+1));
  CHECK(!l.accept(99)&&!l.accept(1)&&l.pending==8&&l.highWater==8&&l.accepted==8);
  CHECK(l.submit(1,0)&&l.submit(2,1));CHECK(l.freeInput()==-1&&!l.submit(3,0)&&!l.submit(3,2));
  CHECK(l.picture(2,1)&&l.picture(1,0));CHECK(!l.picture(1,2)&&!l.picture(3,2));
  CHECK(l.ebd(1)&&!l.ebd(1));CHECK(l.freeInput()==1&&l.submit(3,1));CHECK(l.inputReuse==1);
  CHECK(!l.release(0,2)&&l.release(0,1)&&!l.release(0,1));
  CHECK(l.retire(1)&&!l.retire(1)&&l.pending==7);
  CHECK(l.ebd(0)&&l.ebd(1));
  CHECK(l.beginDrain()&&l.barrier==8&&!l.accept(10));CHECK(l.beginDrain()&&!l.drainReady());
  l.ack=true;CHECK(!l.drainReady());l.last=true;CHECK(l.drainReady()&&!l.drained());
  CHECK(l.release(1,2));for(unsigned i=2;i<=8;++i)CHECK(l.retire(i));
  CHECK(l.drained()&&l.paused&&!l.draining&&!l.drained());CHECK(l.beginDrain()&&l.paused);
  CHECK(l.resume()&&!l.resume()&&!l.ack&&!l.last);
  CHECK(l.accept(11)&&l.submit(11,0)&&l.picture(11,0)&&l.rawReuse==1);l.cancel();
  CHECK(l.stopped&&l.cancelled==1&&!l.pending&&!l.accept(12)&&!l.submit(11,1)&&!l.beginDrain());
  // Cancellation settles identities while physical loans remain owned until
  // the consumer releases them or final quiesce fences the entire arena.
  CHECK(!l.loansEmpty()&&l.release(0,11)&&!l.release(0,11)&&l.loansEmpty());
}
static void stockReplyWindowTests() {
  // A stock producer waits for each dfrm reply, but never waits for decoded
  // callbacks. Four replies may precede ACK; the fifth must not enter DMA.
  stream::Ledger l;
  uint64_t held=0;unsigned replies=0,acks=0,maxPending=0;
  for(uint64_t token=1;token<=60;++token){
    auto *f=l.accept(token,9,false);CHECK(f&&f->phase==stream::Phase::Admitting);
    if(l.pending>maxPending)maxPending=l.pending;
    if(l.replyReady()) {CHECK(l.publish(token));++replies;}
    else {
      held=token;CHECK(l.published()==4&&l.pending==5&&l.nextAdmission()==f);
      CHECK(!l.submit(token,0)); // unpublished reply cannot race its completion
      // The receiver runs independently of the waiting producer. Retiring the
      // earliest ACK opens the window without requiring the held frame.
      CHECK(l.retire(++acks));CHECK(l.replyReady()&&l.nextAdmission()->token==held);
      CHECK(l.publish(held));++replies;held=0;
    }
    CHECK(l.published()<=stream::PublishedWindow&&l.pending<=5);
  }
  CHECK(replies==60&&acks==56&&maxPending==5&&!held);
  // A finish at the end of preroll does not complete until the outstanding
  // Demt ACK, and must not turn a ready picture into a firmware drain.
  while(l.pending)CHECK(l.retire(++acks));
  CHECK(acks==60&&l.pending==0&&!l.draining&&!l.paused&&l.replyReady());
  CHECK(l.accept(61,9,false)&&l.publish(61)); // same epoch continues normally
  for(uint64_t token=62;token<=65;++token)CHECK(l.accept(token,9,false));
  CHECK(l.nextAdmission()->token==62); // oldest reservation, not array slot
  l.cancel();CHECK(!l.replyReady()&&!l.nextAdmission()&&l.pending==0&&l.cancelled==5);
  CHECK(!l.publish(65)); // no late success after cancellation
}
static void guestTests() {
  stream::Guest guest;
  constexpr uint32_t dbuf=0x44627566,demt=0x44656d74;
  CHECK(!guest.matches(100,dbuf)&&!guest.reply(100,dbuf)&&!guest.expired(999999));
  CHECK(!guest.begin(0,dbuf,10)&&!guest.begin(100,0,10)&&guest.begin(100,dbuf,10));
  CHECK(guest.deadline==2010&&guest.matches(100,dbuf)&&!guest.matches(99,dbuf)&&!guest.matches(100,demt));
  CHECK(!guest.begin(101,demt,20)&&!guest.reply(99,dbuf)&&!guest.reply(100,demt));
  CHECK(!guest.expired(2009)&&guest.expired(2010));
  CHECK(!(guest.matches(100,dbuf)&&!guest.expired(2010))); // actual handler rejects overdue replies
  guest.clear();CHECK(!guest.type&&!guest.request&&!guest.expired(9000));
  CHECK(guest.begin(101,dbuf,9000)&&guest.reply(101,dbuf)&&!guest.reply(101,dbuf)&&!guest.matches(101,dbuf));
  CHECK(!guest.expired(999999));
  // The next stage can replace a replied request, while stale/duplicate Dbuf
  // replies cannot settle the Demt acknowledgement for the same frame.
  CHECK(guest.begin(102,demt,9100)&&!guest.reply(101,dbuf)&&!guest.reply(102,dbuf)&&guest.reply(102,demt));
  guest.clear();CHECK(guest.begin(103,dbuf,9200));guest.cancel();
  CHECK(guest.stopped&&!guest.type&&!guest.reply(103,dbuf)&&!guest.begin(104,demt,9300));
  // New native epoch uses a distinct Guest object. Cancelling its predecessor
  // is irreversible, even if a new session starts its request sequence at100.
  stream::Guest replacement;CHECK(replacement.begin(100,dbuf,9300));CHECK(!guest.reply(100,dbuf));
  CHECK(replacement.reply(100,dbuf));replacement.clear();
  // A send failure leaves an owned pending stage until its deadline/cancel;
  // clearing it prematurely would make a late reply match a newer output.
  CHECK(replacement.begin(101,demt,10000)&&!replacement.begin(102,dbuf,10001));
  CHECK(!replacement.expired(11999)&&replacement.expired(12000));replacement.cancel();
  CHECK(!replacement.reply(101,demt)&&!replacement.expired(12001));
}
static void ownerTests() {
  // A known native failure must block acquisition even after DMA was freed.
  for (int error : {-20,-24,-25}) {
    stream::Owner failed; uint32_t session=0,next=0;
    CHECK(failed.acquire(&session)==0 && failed.start(session));
    CHECK(failed.closing(session,false)==0);
    failed.closed(false,error);
    CHECK(failed.phase==stream::Owner::Poisoned && failed.acquire(&next)==-16 && next==0);
  }
  stream::Owner o;uint32_t a=0,b=0;
  CHECK(o.acquire(nullptr)==-22&&o.acquire(&a)==0&&o.owns(a));
  CHECK(o.acquire(&b)==-16&&!o.owns(a+1)&&!o.start(a+1));CHECK(o.start(a)&&!o.start(a));
  CHECK(o.closing(a,true)==-16&&o.closing(a+1,false)==-22&&o.closing(a,false)==0&&!o.owns(a));
  CHECK(o.acquire(&b)==-16);o.closed(false);CHECK(o.phase==stream::Owner::Idle&&o.acquire(&b)==0&&b!=a);
  CHECK(o.closing(b,false)==0);o.closed(true);CHECK(o.phase==stream::Owner::Poisoned&&o.acquire(&a)==-16);
}
static void persistentTests() {
  Fixture f;f.start();f.pump();CHECK(f.tx.r.ebd==1);
  CHECK(f.peer.comvCounts==std::vector<uint32_t>({4,4})&&f.peer.comvSizes==std::vector<uint32_t>({172544,172544}));
  f.peer.autoEbd=false;
  const uint8_t au[]={0,0,0,1,0x41,0x9a,0x22};
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000002ull)==0);
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000003ull)==0);
  const auto etbs=f.tx.r.etb;
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000004ull)==1&&f.tx.r.etb==etbs&&f.tx.ledger.pending==3);
  f.peer.output(0x100000003ull,4);f.peer.output(0x100000002ull,2);f.peer.output(0x100000001ull,1);f.pump();
  CHECK(f.tx.r.decoded==3&&f.tx.outputCount==3&&f.tx.r.ebd==1);
  SelfTest<Peer>::OutputFrame a,b,c;
  CHECK(f.tx.popOutput(a)&&f.tx.popOutput(b)&&f.tx.popOutput(c)&&!f.tx.popOutput(c));
  CHECK(a.token==0x100000003ull&&b.token==0x100000002ull&&c.token==0x100000001ull);
  char before[65],after[65];auto *held=f.layout.find(Raw,a.index);f.peer.hw.hashActive(f.peer.dma.arena+held->off,before);
  CHECK(f.tx.releaseOutput(b.index,b.token)==0&&f.tx.releaseOutput(c.index,c.token)==0);
  // FBD never frees an input slot. Both DMA slots stay read-only to the host
  // until their independent, timestamp-free EBDs are parsed.
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000004ull)==1);
  f.peer.ebd(1);f.pump();CHECK(f.tx.r.ebd==2&&f.tx.ledger.inputOwned[0]&&!f.tx.ledger.inputOwned[1]);
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000004ull)==0);f.peer.output(0x100000004ull,2);f.pump();
  CHECK(f.tx.ledger.rawHeld[a.index]&&f.tx.ledger.pending==2);
  f.peer.hw.hashActive(f.peer.dma.arena+held->off,after);CHECK(!std::strcmp(before,after));
  f.releaseAll();CHECK(f.tx.releaseOutput(a.index,a.token)==0);
  f.peer.ebd(0);f.peer.ebd(1);f.pump();CHECK(f.tx.r.ebd==4&&f.tx.r.decoded==4);
  f.drain();CHECK(f.tx.r.drainAck&&f.tx.r.last&&f.peer.drains==1);
  CHECK(f.tx.beginDrain()==0&&f.tx.drainStatus()==1&&f.peer.drains==1);
  CHECK(f.tx.streamSubmit(au,sizeof au,0x100000005ull)==0);
  CHECK(f.peer.drainInputResume==1&&f.peer.drainRawResume==1&&!f.tx.r.last&&!f.tx.r.drainAck);
  f.peer.output(0x100000005ull,2);f.peer.ebd(0);f.pump();f.releaseAll();f.drain();
  CHECK(f.peer.opens==1&&f.peer.dpbs==4&&f.peer.settingsResume==1&&f.tx.r.psc==1);
  CHECK(f.tx.ledger.inputReuse>0&&f.tx.ledger.rawReuse>0&&f.tx.ledger.pending==0&&f.tx.ledger.loansEmpty());
  f.close();CHECK(f.peer.closes==1&&f.peer.drains==2);
  for(unsigned i=0;i<f.peer.headers.size();++i){CHECK(f.peer.headers[i]==i&&f.peer.packets[i]==i);}
}
static void firstPscTests() {
  for(unsigned n:{1u,9u,64u}){
    Fixture f;f.peer.n=n;f.start(77);f.peer.output(77,1);f.pump();f.releaseAll();f.drain();f.close();
    CHECK(f.tx.psc.n==n&&f.peer.dpbs==n&&f.peer.comvCounts==std::vector<uint32_t>({4,n}));
  }
  for(unsigned kind=0;kind<9;++kind){
    Fixture f;
    switch(kind){case 0:f.peer.n=0;break;case 1:f.peer.n=65;break;case 2:f.peer.pscPoc=2;break;
      case 3:f.peer.pscProfile=0;break;case 4:f.peer.pscLevel=5;break;case 5:f.peer.pscResolution=0x01400100;break;
      case 6:f.peer.pscDepth=0x000a000a;break;case 7:f.peer.pscCoded=0;break;case 8:f.peer.pscCrop1=1;break;}
    CHECK(f.tx.nativeInput(f.sid,kAccessUnit,sizeof kAccessUnit,78));CHECK(f.tx.transaction()!=0&&f.tx.r.error);
    CHECK(!f.tx.rawStarted&&f.peer.dpbs==0&&f.peer.ftbs==0&&f.peer.drains==0);
    CHECK(f.tx.shutdown()==0&&f.peer.stops==1&&f.peer.closes==1);
  }
}
static void profileAbiTests() {
  // Pinned gen2/sheng ABI, independent of the AVC header's conversion helper.
  Fixture main;CHECK(main.tx.expectedProfile==2&&main.peer.pscProfile==2);
  main.peer.pscProfile=1;CHECK(main.tx.nativeInput(main.sid,kAccessUnit,sizeof kAccessUnit,98));
  CHECK(main.tx.transaction()!=0&&!main.tx.rawStarted);CHECK(main.tx.shutdown()==0);
  for(unsigned profile:{0u,1u}){
    Fixture baseline;baseline.tx.expectedProfile=0;baseline.tx.expectedPoc=2;
    baseline.tx.psc.profile=0;baseline.tx.psc.poc=2;baseline.peer.pscProfile=profile;baseline.peer.pscPoc=2;
    baseline.start(99);baseline.peer.output(99,1);baseline.pump();baseline.releaseAll();baseline.drain();baseline.close();
  }
}
static void omittedPscTests() {
  Fixture f;f.peer.omitOptional=true;
  f.tx.psc.poc=f.tx.expectedPoc;f.tx.psc.profile=f.tx.expectedProfile;f.tx.psc.level=f.tx.expectedLevel;
  f.start(97);CHECK(!(f.tx.psc.returned&((1u<<6)|(1u<<7)|(1u<<8))));
  CHECK(f.tx.psc.poc==0&&f.tx.psc.profile==2&&f.tx.psc.level==4);
  f.peer.output(97,8);f.pump();f.releaseAll();f.drain();f.close();
}
static void ancillaryPictureTests() {
  // vid-v6a2 RX18: RAW0, 147456 bytes, token4, picture0x81, no error.
  // The pinned gen2 and sheng references test known bits, not a closed enum.
  for(uint32_t picture:{0x81u,0x82u,0x84u,0x88u}){
    Fixture f;f.peer.n=9;f.start(4);f.pump();
    CHECK(f.layout.find(Raw,0)->iova()==0x8252c000);
    f.peer.output(4,picture);const int rc=f.tx.pump();
    CHECK(rc==0&&f.tx.r.error==0&&f.tx.r.decoded==1&&f.tx.outputCount==1);
    if(rc){f.tx.shutdown();continue;}
    SelfTest<Peer>::OutputFrame out;CHECK(f.tx.popOutput(out)&&out.token==4&&out.index==0&&out.bytes==147456);
    CHECK(f.tx.releaseOutput(out.index,out.token)==0&&f.tx.ledger.pending==0);
    f.drain();f.close();
  }
  for(uint32_t picture:{0x80u,0x41u,0x82u}){
    Fixture f;f.start(4);f.pump();
    // Ancillary bits never authorize NOSHOW, a missing AVC kind, or a bad token.
    f.peer.output(picture==0x82?5:4,picture);
    CHECK(f.tx.pump()==kErrHfi&&f.tx.r.decoded==0&&f.tx.outputCount==0);
    f.tx.shutdown();
  }
}
static void finishDispositionTests() {
  using D=stream::FinishDecision;
  // Empty/repeated healthy barriers still run on the worker. An in-flight
  // request may not be replaced by another request ID.
  CHECK(stream::finishDecision(true,false,false,true,false,false)==D::Schedule);
  CHECK(stream::finishDecision(true,false,false,true,false,true)==D::BadState);
  CHECK(stream::finishDecision(true,false,false,true,false,false)==D::Schedule);
  // Exact error lifetime: firstError is saved before stopping, remains during
  // cleanup with an outstanding finish, then survives native/worker close.
  CHECK(stream::finishDecision(true,false,true,true,false,false)==D::Failed);
  CHECK(stream::finishDecision(true,false,true,true,true,true)==D::Failed);
  CHECK(stream::finishDecision(true,false,true,false,true,false)==D::Failed);
  CHECK(stream::finishDecision(true,false,true,false,true,false)==D::Failed);
  // Explicit invalidation has precedence; a failed session is never revived.
  CHECK(stream::finishDecision(true,true,true,false,true,false)==D::BadState);
  CHECK(stream::finishDecision(false,false,true,false,true,false)==D::BadState);
  CHECK(stream::finishDecision(true,false,false,false,true,false)==D::BadState);
}
static void delayedGuestTests() {
  Fixture f;f.start(101);f.peer.output(101,2);f.pump();
  SelfTest<Peer>::OutputFrame held;CHECK(f.tx.popOutput(held));
  stream::Guest guest;CHECK(guest.begin(100,0x44627566,0));
  const uint8_t au[]={0,0,0,1,0x41,0x9a,0x22};
  for(unsigned i=102;i<108;++i){CHECK(f.tx.streamSubmit(au,sizeof au,i)==0);if(i<105)f.peer.output(i,4);f.pump();}
  CHECK(f.tx.r.ebd==7&&f.tx.r.decoded==4&&f.tx.ledger.pending==7&&f.tx.ledger.rawHeld[held.index]);
  // Guest has not replied, but HFI input ownership and output progress continue.
  CHECK(guest.matches(100,0x44627566)&&!guest.expired(1999));
  CHECK(guest.reply(100,0x44627566)&&guest.begin(101,0x44656d74,1999));
  CHECK(f.tx.releaseOutput(held.index,held.token)==0);f.releaseAll();
  CHECK(guest.reply(101,0x44656d74));guest.clear();
  for(unsigned i=105;i<108;++i){f.peer.output(i,4);f.pump();f.releaseAll();}
  CHECK(f.tx.r.decoded==7&&f.tx.ledger.pending==0);f.drain();f.close();
}
static void drainPictureTests() {
  Fixture f;f.start(91);f.peer.lastWithPicture=true;f.peer.finalToken=91;
  CHECK(f.tx.beginDrain()==0);f.pump();CHECK(f.tx.r.decoded==1&&f.tx.r.rawReturns==1&&f.tx.outputCount==1);
  CHECK(f.tx.drainStatus()==2&&!f.tx.ledger.paused);
  SelfTest<Peer>::OutputFrame out;CHECK(f.tx.popOutput(out)&&out.token==91);
  const auto ftbs=f.peer.ftbs;CHECK(f.tx.releaseOutput(out.index,out.token)==0&&f.peer.ftbs==ftbs);
  CHECK(f.tx.drainStatus()==1&&f.tx.ledger.paused&&f.tx.r.last&&f.tx.r.drainAck);f.close();
}
static void noOutputTests() {
  Fixture f;f.start(81);CHECK(f.tx.retireNoOutput(81)==-22);
  CHECK(f.tx.beginDrain()==0);f.pump();CHECK(f.tx.r.decoded==0&&f.tx.outputCount==0&&f.tx.drainStatus()==2);
  CHECK(f.tx.retireNoOutput(999)==-22&&f.tx.retireNoOutput(81)==0&&f.tx.retireNoOutput(81)==-22);
  CHECK(f.tx.drainStatus()==1&&f.tx.ledger.pending==0);f.close();
}
static void capacityTests() {
  Fixture f;f.start(1);f.pump();const uint8_t au[]={0,0,0,1,0x41,0x9a,0x22};
  for(unsigned token=2;token<=8;++token){CHECK(f.tx.streamSubmit(au,sizeof au,token)==0);f.pump();}
  CHECK(f.tx.ledger.pending==8&&f.tx.ledger.highWater==8&&f.tx.streamSubmit(au,sizeof au,9)==1&&f.tx.r.etb==8);
  // Credits count pictures, not EBDs. An idle but firmware-pending picture
  // still has a deadline; a completely idle session does not.
  CHECK(f.tx.ledger.inputsEmpty());f.peer.hw.clock+=5000001;CHECK(f.tx.pump()==kErrHfiTimeout);
  CHECK(f.peer.dma.arena==f.peer.arena.data());CHECK(f.tx.shutdown()==0);
}
static void negativeTests(unsigned kind) {
  Fixture f;f.start(0x123456789ull);f.pump();
  switch(kind){
    case 0:f.peer.output(999,2);break; // unknown identity
    case 1:f.peer.output(0x123456789ull,2);f.pump();f.peer.output(0x123456789ull,2);break; // duplicate frame on another RAW
    case 2:f.peer.output(0x123456789ull,2,0x10);break; // READONLY must never be recycled
    case 3:f.peer.psc();break; // in-place DRC deliberately unsupported
    case 4:f.peer.reply({Peer::packet(0x03000162,0,2,1,{2})},f.sid+1);break;
    case 5:f.peer.ebd(0,true);break;
    case 6:{auto *b=f.layout.find(Dpb,0);Words v={Dpb,0,b->iova(),0,0,b->size,0,0,0,0,0,0,0,0,0,0};f.peer.reply({Peer::packet(Buffers,0,2,5,v)});break;}
    case 7:f.peer.reply({Peer::packet(0x04000001,4,0,0)});break;
    case 8:f.peer.reply({Peer::packet(0x05000001,8,0,0)});break;
    case 9:f.peer.ops.watchdog=true;break;
    case 10:f.peer.ops.fault=true;break;
    case 11:f.peer.output(0x123456789ull,2,0x20000000);break; // PSC_LAST without supported DRC
    case 12:f.peer.output(0x123456789ull,0);break;
    case 13:f.peer.output(0x123456789ull,0x40);break; // NOSHOW is not a displayable picture
    case 14:{auto *b=f.layout.find(Raw,0);Words v={Raw,0,b->iova()+4096,0,0,b->size,0,kRawBytes,0x23456789,1,0,0,0,0,0,0};f.peer.reply({Peer::packet(Buffers,0,2,5,v),Peer::packet(0x03000162,0,2,1,{2})});break;}
    case 15:f.peer.output(0x123456789ull,2,0x10000000);break; // unsolicited LAST
  }
  CHECK(f.tx.pump()!=0&&f.tx.r.error!=0&&f.peer.dma.arena==f.peer.arena.data());
  f.tx.shutdown();CHECK(f.peer.stops==2&&f.peer.closes==1);
}
static void staleRetiredTests() {
  Fixture f;f.start(73);f.peer.output(73,2);f.pump();SelfTest<Peer>::OutputFrame out;CHECK(f.tx.popOutput(out));
  CHECK(f.tx.releaseOutput(out.index,out.token)==0&&f.tx.ledger.pending==0);
  // A requeued RAW index does not make a late token from its old generation
  // valid; the frame ledger, not RAW FIFO order, controls identity.
  f.peer.output(73,2,0,out.index);CHECK(f.tx.pump()!=0&&f.tx.r.error);f.tx.shutdown();
}
static void outputCleanupTests() {
  for(bool watchdog : {false,true}) {
    Fixture f;f.start(51);f.peer.output(51,2);f.pump();
    SelfTest<Peer>::OutputFrame out;CHECK(f.tx.popOutput(out));
    CHECK(f.tx.streamSubmit(kAccessUnit,sizeof kAccessUnit,52)==0);
    f.peer.output(52,2);f.pump();
    const unsigned queued=f.peer.ftbs;
    if(watchdog){f.peer.ops.watchdog=true;CHECK(f.tx.pump()==kErrWatchdog);}
    CHECK(f.tx.releaseOutput(out.index,out.token)==0);
    CHECK(f.peer.ftbs==queued+unsigned(!watchdog));
    if(watchdog){
      CHECK(f.tx.shutdown(true)==kErrWatchdog && !f.peer.stops && !f.peer.closes);
    }else{
      CHECK(f.tx.shutdown(true)==0 && f.tx.r.closed);
      CHECK(f.tx.outputCount==0 && f.tx.ledger.loansEmpty() && f.tx.ledger.inputsEmpty());
      CHECK(f.tx.ledger.pending==0 && f.tx.ledger.cancelled==1);
    }
  }
}
static void completionExtentTest() {
  Fixture f;f.start(51);f.pump();
  const unsigned index=f.peer.freeRaw();
  auto b=f.peer.raws[index];
  b[5]=kRawBytes-4096; b[7]=kRawBytes; b[8]=51;
  f.peer.reply({Peer::packet(Buffers,0,2,5,b),Peer::packet(0x03000162,0,2,1,{2})});
  CHECK(f.tx.pump()==kErrHfi && !f.tx.outputCount && f.tx.ledger.loansEmpty());
}
static void timeoutTests(bool ack) {
  Fixture f;f.start(71);f.peer.output(71,1);f.pump();f.releaseAll();
  f.peer.autoDrainAck=!ack;f.peer.autoDrainLast=ack;
  CHECK(f.tx.beginDrain()==0);f.pump();CHECK(f.tx.drainStatus()==0);
  f.peer.hw.clock+=5000001;CHECK(f.tx.pump()==kErrHfiTimeout&&f.tx.r.error==kErrHfiTimeout);
  CHECK(f.peer.dma.arena==f.peer.arena.data());f.tx.shutdown();
}
static void cancelTests() {
  Fixture f;f.start(51);f.peer.output(51,2);f.pump();SelfTest<Peer>::OutputFrame out;
  CHECK(f.tx.popOutput(out));const auto count=f.tx.r.decoded;
  // Entering shutdown is the engine's cancellation fence. Pending RAW/DPB and
  // loan metadata cannot authorize another success after that fence.
  f.tx.cleanup=true;f.tx.ledger.cancel();f.peer.output(999,2);CHECK(f.tx.pump()==0&&f.tx.r.decoded==count);
  CHECK(f.tx.ledger.stopped&&f.tx.ledger.cancelled==1);
  CHECK(f.tx.shutdown()==0&&f.peer.closes==1&&f.peer.dma.arena==f.peer.arena.data());
}
static void idleTests() {
  Fixture f;f.start(61);f.peer.output(61,1);f.pump();f.releaseAll();f.pump();
  CHECK(f.tx.deadline==UINT64_MAX);f.peer.hw.clock+=90000000;f.pump();CHECK(!f.tx.r.error);f.close();
}
static void admissionPublication() {
  stream::Ledger l;
  auto *f=l.accept(0x1234,1,false);
  CHECK(f&&f->phase==stream::Phase::Admitting&&l.pending==1);
  CHECK(!l.submit(0x1234,0)); // worker cannot DMA before the dfrm reply exists
  CHECK(l.publish(0x1234)&&!l.publish(0x1234)&&l.submit(0x1234,0));
  CHECK(!l.accept(0x1234,1,false));
  CHECK(l.ebd(0)&&l.freeInput()==1); // exercise both owned HW inputs on the same stream

  auto *failed=l.accept(0x5678,1,false);CHECK(failed&&failed->phase==stream::Phase::Admitting);
  failed->phase=stream::Phase::Emitted; // transport fallback owns this failed enqueue
  CHECK(!l.publish(0x5678)&&!l.submit(0x5678,1));
  l.cancel();CHECK(l.pending==0&&!l.publish(0x5678));
}
// The stall is an explicit model input, not an explanation of firmware behavior.
struct StalledPeer : Peer {
  bool stalled=false;
  int send(unsigned bytes,const char *what) {
    if(stalled)return hfi::write(mem,hfi::kCmdq,tx,bytes,[]{});
    const bool firstInput=tx[9]==Buffers&&tx[16]==Bitstream;
    const int rc=Peer::send(bytes,what);if(firstInput)stalled=true;return rc;
  }
};
struct TimeoutWitness {
  unsigned calls=0;
  session::Observation saved;
  static void capture(void *context,const SelfTest<StalledPeer> &t) {
    auto &s=*static_cast<TimeoutWitness *>(context);++s.calls;s.saved=t.observation();
    CHECK(!t.cleanup&&!t.closeSent&&t.o.stops==0&&t.o.closes==0);
    CHECK(t.o.hw.clock==1000000&&t.r.error==kErrHfiTimeout);
    CHECK(t.o.mem.ld(hfi::qhdr(0)+hfi::kQRead)==739&&t.o.mem.ld(hfi::qhdr(0)+hfi::kQWrite)==1395);
    CHECK(t.history&&t.history->commands.total==59&&t.history->messages.total>=10);
    t.history->commands.print(t.o.hw,"TX");t.history->messages.print(t.o.hw,"RX");
    s.saved.print(t.o.hw,"current");
  }
};
static void firstTimeoutObservationTests() {
  std::vector<uint32_t> referenceUc;
  for(bool hooked:{false,true}) {
    StalledPeer p;Layout l;CHECK(l.init());p.autoEbd=false;
    p.ids.header=3;p.ids.packet=10;
    p.mem.st(hfi::qhdr(0)+hfi::kQRead,113);p.mem.st(hfi::qhdr(0)+hfi::kQWrite,113);
    SelfTest<StalledPeer> t(p,l);TimeoutWitness witness;diagnostic::History history;
    t.streaming=true;t.checkGolden=false;t.expectedPoc=t.psc.poc=0;t.expectedProfile=t.psc.profile=2;t.expectedLevel=t.psc.level=4;
    if(hooked){t.timeoutObserver=TimeoutWitness::capture;t.observerContext=&witness;t.history=&history;}
    CHECK(t.nativeInput(0x56360002,kAccessUnit,sizeof kAccessUnit,1));
    CHECK(t.transaction()==kErrHfiTimeout&&t.r.failedStage==Output&&p.hw.clock==1000000);
    CHECK(t.r.etb==1&&!t.r.ebd&&t.r.psc==1&&!p.dpbs&&!p.ftbs);
    if(!hooked)referenceUc=p.uc;else CHECK(p.uc==referenceUc); // identical HFI bytes/indices, including the failed wait
    CHECK(witness.calls==unsigned(hooked));
    t.fail(kErrHfiTimeout);CHECK(witness.calls==unsigned(hooked));
    if(hooked) {
      CHECK(witness.saved.sid==0x56360002&&witness.saved.ledger.inputOwned[0]&&witness.saved.ledger.pending==1);
      CHECK(p.hw.log.find("V6f TX seq 59 ")!=std::string::npos);
      CHECK(p.hw.log.find("V6f session current sid 56360002")!=std::string::npos);
      CHECK(p.hw.log.find("V6f input current index 0 owned 1 token 1")!=std::string::npos);
    }
    const auto saved=witness.saved;t.shutdown();CHECK(witness.calls==unsigned(hooked));
    CHECK(witness.saved.sid==saved.sid&&witness.saved.r.stage==saved.r.stage); // survives cleanup
  }
}
static void passiveHardwareTests() {
  struct Model {
    Hardware hw;
    struct Power {
      bool on=false;
      bool ccReadable()const{return on;}bool controllerReadable()const{return on;}bool codecReadable()const{return on;}
    } power;
    struct Reads {
      unsigned count=0;
      bool powered=true;
      uint32_t rd(uint64_t pa){CHECK(pa!=kIris+kCpuCsA2hSoftIntClr&&pa!=kIris+kCpuIcSoftInt);++count;return pa==kIris+kCorePowerStatus?(powered?kCorePowered:0):uint32_t(pa);}
    } ops;
    hfi::Mem mem;
  } m;
  diagnostic::Hardware off;off.capture(m);CHECK(!off.cc&&!off.controller&&!off.codec&&!off.queues&&m.ops.count==0);
  std::vector<uint32_t> uc(kUcBytes/4);m.mem.uc=uc.data();hfi::initQueues(m.mem);
  const auto size=kHfiQueueBytes/4;
  m.mem.st(hfi::qhdr(0)+hfi::kQRead,size-2);m.mem.st(hfi::qhdr(0)+hfi::kQWrite,3);
  for(unsigned i=0;i<5;++i)m.mem.st(hfi::qbase(0)+4*((size-2+i)%size),100+i);
  const auto before=uc;m.power.on=true;
  diagnostic::Hardware s;s.capture(m);CHECK(uc==before&&m.ops.count==31);
  CHECK(s.cc&&s.codec&&s.controller&&s.queues&&s.headWords[0]==5&&!s.headWords[1]);
  CHECK(s.head[0][0]==100&&s.head[0][4]==104&&s.core[5]==kIris+kWrapperIntrStatus);
  s.print(m.hw);CHECK(m.hw.log.find("V6f interrupt status ")!=std::string::npos);
  m.ops.powered=false;const auto reads=m.ops.count;diagnostic::Hardware collapsed;collapsed.capture(m);
  CHECK(!collapsed.codec&&collapsed.controller&&m.ops.count==reads+27);
  collapsed.print(m.hw);CHECK(m.hw.log.find("V6f codec idle skipped not-powered-or-readable")!=std::string::npos);
  // Use the same formatter as the native callback for spec-pattern rehearsal.
  Observation previous,current;previous.sid=0x56360001;previous.r.closed=true;previous.r.decoded=1;
  current.sid=0x56360002;current.r.error=kErrHfiTimeout;current.r.failedStage=Output;
  current.r.etb=current.r.psc=1;diagnostic::History history;
  uint32_t packet[24]={96,0x56360002,61,0,0,0,0,1,64,Start,3,0,2,68};
  for(unsigned i=0;i<41;++i)history.commands.add(packet,sizeof packet,0);
  history.messages.add(packet,sizeof packet,0);
  printTimeoutObservation(m.hw,s,previous,current,history);
  CHECK(m.hw.log.find("V6f snapshot begin version 1 error -20 sid 56360002 before-cleanup 1")!=std::string::npos);
  CHECK(m.hw.log.find("V6f snapshot end sid 56360002 before-cleanup 1")!=std::string::npos);
  CHECK(m.hw.log.find("V6f TX seq 1 ")==std::string::npos&&m.hw.log.find("V6f TX seq 41 ")!=std::string::npos);
  CHECK(m.hw.log.size()<32768);
  // Corrupt indices/size must never authorize an out-of-bounds pending-head read.
  m.mem.st(hfi::qhdr(0)+hfi::kQRead,size);diagnostic::Hardware bad;bad.capture(m);CHECK(!bad.headWords[0]);
}
int main() { preparedInputTests();productionWorkTests();nativeCleanupFailureTests();
  stockReplyWindowTests();
  firstTimeoutObservationTests();passiveHardwareTests();
  ancillaryPictureTests();
  finishDispositionTests();
  admissionPublication();
  profileAbiTests();
  ledgerTests();guestTests();ownerTests();persistentTests();firstPscTests();omittedPscTests();delayedGuestTests();drainPictureTests();noOutputTests();capacityTests();
  for(unsigned i=0;i<16;++i)negativeTests(i);
  staleRetiredTests();outputCleanupTests();completionExtentTest();timeoutTests(true);timeoutTests(false);cancelTests();idleTests();
  std::printf("V6 persistent host: %s (real HFI parser/ledger; scripted peer, never hardware evidence)\n",failures?"FAIL":"PASS");return failures?1:0;
}
