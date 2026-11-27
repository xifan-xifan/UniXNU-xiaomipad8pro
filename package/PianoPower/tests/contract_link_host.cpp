// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Integration: binary GLINK/UCSI replies -> decoded source/contract ->
// ChargeLink snapshot -> production policy -> ACK plus independent readback.
// The source model answers queries; it never assigns contract-valid bits.
#include "ChargeContract.h"
#include "ChargeLink.h"
#include "GaugeAlerts.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace pianopower;

static uint32_t fixedPdo(unsigned mv,unsigned ma) {return mv/50<<10 | ma/10;}
static uint32_t fixedRdo(unsigned object,unsigned ma) {return object<<28 | ma/10<<10 | ma/10;}
static uint32_t ppsPdo() {return 3u<<30 | 110u<<17 | 33u<<8 | 60u;}
static uint32_t ppsRdo(uint32_t word) {return 3u<<28 | 450u<<9 | (word & 0xffff); }

struct Integration {
  PianoChargeContract contract{};
  PianoMca m{};
  ChargeLink link;
  GaugeReading packs[2]{};
  piano_board_temps board{};
  uint64_t now=1000, control=0;
  uint32_t rdo=fixedRdo(1,3000), bus=5000, icl=100, fcc=500, enable=1;
  uint32_t source[3]={fixedPdo(5000,3000),fixedPdo(9000,2000),ppsPdo()};
  std::vector<PianoChargeWrite> writes;
  unsigned voltageRequests=0;
  int32_t iclReadbackOverride=-1;
  bool pd=true, provider=false;   // provider: UCSI says we source VBUS (a UFP partner: a USB device on the port)
  GaugeAlerts alerts;

  Integration() {
    piano_contract_init(&contract); piano_contract_link(&contract,1);
    piano_mca_init(&m,1,1); m.links=1; m.board_allowed=1; m.board_done=7;
    LinkOptions o; o.strictContract=true; o.health=true; o.hv=1; o.kick=false;
    link.configure(m,o);
    alerts.sampled(0x1e);
    board.version=PIANO_BOARD_TEMPS_VERSION; board.valid=15; board.state=PIANO_BOARD_TEMPS_RUNNING;
    for(unsigned i=0;i<4;++i) board.milli_c[i]=30000;
    for(auto &g:packs) {g.ok=true;g.socPct=25;g.tempDeciC=300;g.voltageMv=3900;g.currentMa=300;}
  }
  LinkView view() {
    auto put=[&](int field,int32_t value) {m.valid|=1u<<field;m.value[field]=value;};
    put(McaBusVoltage,int32_t(bus*1000));put(McaBusCurrent,int32_t(icl*1000));
    put(McaType,pd?PIANO_CHG_T_PD:PIANO_CHG_T_SDP);put(McaIcl,iclReadbackOverride<0?int32_t(icl):iclReadbackOverride);put(McaChargeEnable,int32_t(enable));
    put(McaTermVolt,5200);put(McaVerify,0);put(McaTbat,30);put(McaPdState,2);put(McaLpd,0);
    LinkView v;v.nowMs=now;v.chargerPdUp=true;v.chargerPdUpMs=1;
    v.contract=&contract;v.safetyFresh=true;v.cpOff=true;v.dtptValid=alerts.complete();v.dtpt=alerts.active();
    v.packs=packs;v.packsFresh=true;v.packsMs=now;v.board=&board;
    return v;
  }
  void fill() {
    ChargeLink::fill(link.in,m,view(),link.cfg.own_bus != 0);
  }
  void decide() {
    fill();piano_charge_decide(&link.cfg,&link.in,&link.st,&link.plan);
    for(unsigned i=0;i<link.plan.writes;++i) {
      const auto w=link.plan.write[i];
      assert(piano_charge_write_allowed(&link.cfg,w.property,w.value,w.bytes));
      if(w.property==PIANO_CHG_PD_FIXED || (w.property==PIANO_CHG_PPS && link.st.hv==PIANO_CHG_HV_ASKED)) {
        if (!(link.st.icl_set && link.st.icl_set<=link.st.transition_ma)) std::fprintf(stderr,"barrier now=%llu prop=%x value=%u hv=%u set=%u transition=%u read=%u pending=%u valid=%u observing=%u bus=%u rdo=%x\n",(unsigned long long)now,w.property,w.value,link.st.hv,link.st.icl_set,link.st.transition_ma,icl,link.st.pending,contract.valid,contract.notify_pending,bus,rdo);
        assert(link.st.icl_set && link.st.icl_set<=link.st.transition_ma);
        assert(link.st.fcc_set && link.st.fcc_set<=500);
        assert(link.in.icl_ma>=0 && uint32_t(link.in.icl_ma)<=link.st.transition_ma); // independent readback, not the ACK cache
        assert(!(link.st.pending & ((1u<<PIANO_CHG_S_ICL)|(1u<<PIANO_CHG_S_FCC))));
        ++voltageRequests;
      }
      writes.push_back(w);
      switch(w.property) {
      case PIANO_CHG_ICL:icl=w.value;break;
      case PIANO_CHG_FCC:fcc=w.value;break;
      case PIANO_CHG_ENABLE:enable=w.value;for(auto &g:packs)g.currentMa=enable?300:0;break;
      case PIANO_CHG_PD_FIXED:bus=w.value;rdo=fixedRdo(bus==9000?2:1,bus==9000?1600:3000);break;
      case PIANO_CHG_PPS:bus=(w.value>>16)*20;rdo=ppsRdo(w.value);break;
      default:assert(false);
      }
      piano_charge_ack(&link.st,w.property,w.value,0);
    }
    assert(fcc<=1500 && icl<=1600);
  }
  int wire() {
    uint8_t request[PIANO_CONTRACT_REQUEST_MAX];auto n=piano_contract_next(&contract,now,request);
    if(!n) {now+=200;return 0;}
    const auto op=piano_contract_u32(request+8);uint8_t response[544]={};
    piano_contract_put32(response,PIANO_CONTRACT_OWNER);piano_contract_put32(response+4,1);piano_contract_put32(response+8,op);
    uint32_t length=16;
    if(op==PIANO_CONTRACT_WRITE) {
      assert(n==64);control=piano_contract_u32(request+20)|(uint64_t(piano_contract_u32(request+24))<<32);
      assert((control&255)==1 || (control&255)==5 || (control&255)==4 || (control&255)==6 || (control&255)==0x10 || (control&255)==0x12);
    } else {
      assert(n==12 && op==PIANO_CONTRACT_READ);length=64;response[12]=0x10;response[13]=1;
      // The PPM returns no CONTROL echo. CCI and serialized ACKs associate replies.
      uint32_t cci=0;uint8_t *msg=response+28;
      if(contract.stage!=PianoContractProbe) {
        const unsigned command=control&255;
        cci=command==1?1u<<27:command==4?1u<<29:1u<<31;
        if(command==6) {cci|=16u<<8;msg[0]=4;msg[4]=1;msg[5]=16;}
        else if(command==0x12) {cci|=9u<<8;piano_contract_put32(msg,1u<<19|(pd?3u:1u)<<16|(provider?(1u<<20|2u<<29):0u));piano_contract_put32(msg+4,pd?rdo:0);}
        else if(command==0x10) {cci|=12u<<8;for(unsigned i=0;i<3;++i)piano_contract_put32(msg+4*i,source[i]);}
      }
      piano_contract_put32(response+16,cci);
    }
    now+=20;return piano_contract_receive(&contract,now,response,length);
  }
  void observe(bool duringGap=false) {
    auto seq=contract.seq;piano_contract_refresh(&contract);
    for(unsigned i=0;i<100 && contract.seq==seq;++i) {
      assert(wire()>=0);
      if(duringGap && !piano_contract_fresh(&contract,now)) {
        const auto requests=voltageRequests;decide();
        assert(icl<=100 && voltageRequests==requests); // no extra 9V or 5V request during bounded validation
      }
    }
    assert(contract.seq==seq+1 && contract.status_valid && bool(contract.valid)==pd);
  }
  void tick() {now+=5000;observe();decide();}
  bool wrote5(size_t after) const {
    for(size_t i=after;i<writes.size();++i)if(writes[i].property==PIANO_CHG_PD_FIXED&&writes[i].value==5000)return true;
    return false;
  }
  void notify() {
    uint8_t data[24]={};piano_contract_put32(data,PIANO_CONTRACT_OWNER);piano_contract_put32(data+4,2);
    piano_contract_put32(data+8,PIANO_CONTRACT_NOTIFY);assert(piano_contract_receive(&contract,now,data,24)==1);
  }
};

// ownBus (ChargeLink.h, piano-power-own-bus): an Rp always wins; a fresh UCSI status decides; without one, the PMIC's sink Rd.
static void ownBusWitnesses() {
  PortState p; PianoChargeContract c{};
  assert(!ownBus(nullptr,nullptr,0) && !ownBus(&c,nullptr,0));          // nothing known: never ours
  p.ok=true; p.attached=true; p.sinkSeen=true;
  assert(ownBus(nullptr,&p,0) && ownBus(&c,&p,0));                      // the PMIC: a sink's Rd (no fresh UCSI status)
  p.sourceSeen=true; assert(!ownBus(nullptr,&p,0));                     // a source's Rp: a charger, a computer, a dock
  p.sourceSeen=false;
  c.status_valid=1; c.updated_ms=100; c.connected=1; c.sink=1;
  assert(!ownBus(&c,&p,200));                                           // fresh UCSI: we sink
  c.sink=0; assert(ownBus(&c,&p,200) && ownBus(&c,nullptr,200));        // fresh UCSI: we are the provider
  p.sourceSeen=true; assert(!ownBus(&c,&p,200)); p.sourceSeen=false;    // contradicted by an Rp: not ours
  c.connected=0; assert(!ownBus(&c,&p,200) && !ownBus(&c,nullptr,200)); // fresh UCSI: nothing connected
  c.connected=1; c.notify_pending=1; assert(ownBus(&c,&p,200) && !ownBus(&c,nullptr,200)); // a change pending: the PMIC decides
  c.notify_pending=0; assert(!ownBus(&c,nullptr,100+PIANO_CONTRACT_FRESH_MS+1));          // stale: no UCSI witness
}

int main() {
  ownBusWitnesses();
  // UCSI says we are the provider (an OTG device, our boost's 5 V on the bus): the old rule attaches a source (SDP, 500 mA asked);
  // piano-power-own-bus decides nothing attached, writes nothing, raises nothing.
  for(int guard=0;guard<2;++guard) {
    Integration o;o.pd=false;o.provider=true;o.link.cfg.own_bus=uint8_t(guard);o.observe();o.decide();
    assert(o.contract.connected && !o.contract.sink);
    assert(bool(o.link.in.valid & PIANO_CHG_IN_OWN_BUS) == bool(guard));
    assert(!(o.link.in.valid & PIANO_CHG_IN_NONPD));
    for(unsigned i=0;i<6;++i)o.tick();
    if(guard) assert(o.link.plan.own_bus && !o.link.st.attached && o.icl==100 && o.fcc==500 && o.writes.empty() && !o.voltageRequests);
    else assert(!o.link.plan.own_bus && o.link.st.attached && o.icl==500);
  }
  { Integration o;o.link.cfg.own_bus=1;o.observe();o.decide(); assert(!(o.link.in.valid & PIANO_CHG_IN_OWN_BUS)); }
  { Integration o;o.pd=false;o.link.cfg.own_bus=1;o.observe();o.decide();
    assert((o.link.in.valid & PIANO_CHG_IN_NONPD) && !(o.link.in.valid & PIANO_CHG_IN_OWN_BUS)); }
  {
    // An input rise and a PPS refresh can still be waiting in the codec queue at a role change.
    Integration o;
    o.pd=false;
    o.provider=true;
    o.link.cfg.own_bus=1;
    o.link.cfg.hv=2;
    o.link.syncPpsOption(o.m);
    o.observe();
    const uint32_t pps = piano_charge_pps_word(9000, 1600);
    assert(piano_mca_queue(&o.m, PIANO_CHG_ICL, 1500, 4) == 0);
    assert(piano_mca_queue(&o.m, PIANO_CHG_PPS, pps, 4) == 0);
    o.link.st.started = 1;
    o.link.st.generation = o.m.links;
    o.link.st.attached = 1;
    o.link.st.hv = PIANO_CHG_HV_ON;
    o.link.st.hv_pps = o.link.st.pps_acked = 1;
    o.link.st.volt_want_prop = PIANO_CHG_PPS;
    o.link.st.volt_want_value = pps;
    o.link.st.pending = (1u << PIANO_CHG_S_ICL) | (1u << PIANO_CHG_S_VOLT);
    o.link.st.pending_ms[PIANO_CHG_S_ICL] = o.link.st.pending_ms[PIANO_CHG_S_VOLT] = o.now;
    piano_skin_temps skin{};
    skin.version = PIANO_SKIN_TEMPS_VERSION;
    skin.virtual_valid = 1;
    skin.virtual_milli_c = 35000;
    LinkView view = o.view();
    view.skin = &skin;
    PianoChargeInput unguarded{};
    ChargeLink::fill(unguarded, o.m, view);
    assert((unguarded.valid & PIANO_CHG_IN_SKIN) && !(unguarded.valid & PIANO_CHG_IN_OWN_BUS));
    o.link.requestDecision();
    LinkOutcome outcome;
    o.link.step(o.m, view, outcome);
    assert(outcome.decided && o.link.plan.own_bus && !o.m.queued && !o.link.st.pending);
    assert((o.link.in.valid & PIANO_CHG_IN_SKIN) && o.link.in.t_skin_dc == 350);
    assert(!o.link.st.attached && !o.link.st.pps_acked && !o.link.st.volt_want_prop);
  }
  Integration unknown; unknown.alerts.isc[1]=255;unknown.alerts.sampled(0x1e);
  unknown.observe();unknown.decide();
  for(unsigned i=0;i<12;++i)unknown.tick();
  assert(!unknown.voltageRequests && unknown.fcc<=500 && unknown.bus==5000 && unknown.enable==1);
  unknown.alerts.isc[0]=3;unknown.alerts.sampled(2);unknown.tick();
  assert(!unknown.enable && unknown.link.plan.why_off==PIANO_CHG_OFF_FAULT && !unknown.voltageRequests);
  unknown.alerts.isc[0]=255;unknown.alerts.sampled(0x1e);unknown.tick();
  assert(!unknown.enable && unknown.alerts.active() && !unknown.voltageRequests);
  Integration sdp;sdp.pd=false;sdp.observe();sdp.decide();
  assert(sdp.contract.connected && sdp.contract.sink && !sdp.contract.pd && !sdp.contract.last_error);
  assert(sdp.link.in.valid & PIANO_CHG_IN_GAUGES);
  assert(!sdp.voltageRequests && !sdp.link.in.contract_ma && sdp.icl<=500);
  auto sample=sdp.contract.updated_ms; auto seq=sdp.contract.seq;
  sdp.tick();assert(sdp.contract.seq==seq+1 && sdp.contract.updated_ms>sample && !sdp.voltageRequests);
  // A failed next report stays invalid; only a new valid pair restores the input.
  sdp.packs[1].ok=false;sdp.packs[1].voltageMv=65535;sdp.fill();
  assert(!(sdp.link.in.valid & PIANO_CHG_IN_GAUGES));
  sdp.now+=30000;sdp.packs[1].ok=true;sdp.packs[1].voltageMv=3900;sdp.observe();sdp.fill();
  assert(sdp.link.in.valid & PIANO_CHG_IN_GAUGES);
  assert(sdp.packs[0].socPct==25 && sdp.packs[1].socPct==25 && !sdp.voltageRequests);

  Integration s;s.observe();s.fill();
  assert(s.link.in.contract_mv==5000 && s.link.in.contract_ma==3000);
  assert(s.link.in.fixed9_ma==2000 && s.link.in.pps9_ma==3000);
  s.decide();s.now+=20000;s.observe();s.iclReadbackOverride=1500;s.decide();
  assert(s.icl<=500 && s.link.st.icl_set<=500 && s.fcc<=500 && !s.voltageRequests);
  s.now+=100;s.decide(); // both current ACKs arrived, but the independent ICL readback disagrees
  assert(s.link.in.icl_ma==1500 && s.bus==5000 && !s.voltageRequests);
  s.iclReadbackOverride=-1;
  for(unsigned i=0;i<12 && s.bus!=9000;++i)s.tick();
  assert(s.bus==9000 && s.link.st.hv==PIANO_CHG_HV_ASKED);
  s.observe(true); // actual binary RDO changes to object2, with a partial-validation gap
  assert(s.contract.mv==9000 && s.contract.ma==1600 && s.contract.protocol==PIANO_CONTRACT_FIXED);
  for(unsigned i=0;i<6 && s.link.st.hv!=PIANO_CHG_HV_ON;++i)s.tick();
  assert(s.link.st.hv==PIANO_CHG_HV_ON && s.icl==1600 && s.fcc<=1500);

  for(unsigned active=0;active<2;++active) {
    const auto gen=s.contract.generation, seq=s.contract.seq;const auto timestamp=s.contract.updated_ms;
    if(active) {piano_contract_refresh(&s.contract);assert(s.wire()==1);}
    const auto requests=s.voltageRequests;const auto start=s.writes.size();
    // T-14b: the completion of our own GET in flight holds the limits in force (y10 dropped to 100 mA here, then the next fresh
    // snapshot armed the recheck barrier at 500 mA: the 0 / 1.5 A cycle seen live on 2026-11-23)
    s.notify();s.decide();assert(s.icl==1600 && s.link.st.notify_holds==1 && s.voltageRequests==requests && !s.wrote5(start));
    assert(s.wire()==2); // CCI says completion, not a connector event
    assert(!s.contract.notify_pending && s.contract.generation==gen && s.contract.seq==seq && s.contract.updated_ms==timestamp);
    if(active) {for(unsigned i=0;i<100 && s.contract.seq==seq;++i)assert(s.wire()>=0);}
    s.decide();assert(!s.wrote5(start) && s.link.st.hv==PIANO_CHG_HV_ON && s.icl==1600);
  }

  const auto modeStart=s.writes.size();
  assert(!piano_charge_config_transaction(&s.link.cfg,1,80,75,2,0));
  s.decide();
  for(unsigned i=0;i<18 && s.contract.protocol!=PIANO_CONTRACT_PPS;++i)s.tick();
  assert(s.contract.protocol==PIANO_CONTRACT_PPS && s.contract.mv==9000 && s.wrote5(modeStart));
  bool fiveBeforePps=false;
  for(size_t i=modeStart;i<s.writes.size();++i) {
    if(s.writes[i].property==PIANO_CHG_PD_FIXED && s.writes[i].value==5000)fiveBeforePps=true;
    if(s.writes[i].property==PIANO_CHG_PPS)assert(fiveBeforePps);
  }
  assert(fiveBeforePps);
  // The legal actuator test at SOC25 uses the bounded administrator inhibit;
  // threshold validation remains 50/40 minimum and neither gauge is replaced.
  assert(s.packs[0].socPct==25 && s.packs[1].socPct==25);
  assert(piano_charge_config_transaction(&s.link.cfg,1,25,20,1,0)!=0);
  assert(!piano_charge_config_transaction(&s.link.cfg,1,80,75,1,1));
  for(unsigned i=0;i<4 && (s.enable || s.bus!=5000);++i) {s.decide();s.observe();}
  assert(!s.enable && s.bus==5000 && s.packs[0].currentMa==0 && s.packs[1].currentMa==0);
  assert(!piano_charge_config_transaction(&s.link.cfg,1,80,75,1,0));
  for(unsigned i=0;i<5 && !s.enable;++i) {s.decide();s.observe();}
  assert(s.enable && s.packs[0].socPct==25 && s.packs[1].socPct==25);
  assert(s.link.cfg.limit && s.link.cfg.stop_pct==80 && s.link.cfg.resume_pct==75 && !s.link.cfg.admin_stop);
  std::puts("charge integration: binary UCSI -> ChargeLink -> bounded buck, notification stability, mode transition, SOC25 stop/resume PASS");
}
