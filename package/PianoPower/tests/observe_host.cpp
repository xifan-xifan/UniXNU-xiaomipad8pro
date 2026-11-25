/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include "../AdspObserve.h"
#include "../../PianoSensors/SensorsFaultObserve.h"
static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static std::string decode(QcomGlink &g,const uint8_t *p,unsigned n,unsigned tail=0,unsigned length=0x4000) {
    char buffer[65536]={};pianoobserve::Text t={buffer,sizeof buffer};adspobserve::frames(t,g,p,n,tail,length);assert(!t.truncated);return buffer;
}
int main() {
    QcomGlink g={};auto &c=g.channel[0];c.used=1;c.lcid=3;c.rcid=7;c.up=1;c.remote[0].valid=1;c.remote[0].id=9;
    strcpy(c.name,"fastrpcglink-apps-dsp");
    uint8_t packet[128]={};put(packet,9|(3u<<16));put(packet+4,9);put(packet+8,40);
    put(packet+16,1);put(packet+20,2);put(packet+24,0x1234);put(packet+32,4);put(packet+36,0x506);
    put(packet+40,0x9d200000);put(packet+44,3);put(packet+48,4096);
    for(unsigned i=56;i<88;i+=8){put(packet+i,14|(3u<<16));put(packet+i+4,1);}
    auto s=decode(g,packet,88,1672);assert(s.find("padded_end=1728 complete=1")!=std::string::npos);
    assert(s.find("frames=5 walked=88")!=std::string::npos);
    assert(s.find("fastrpc pid=1 tid=2 ctx=4660 handle=4 sc=0x506 addr=0x39d200000 size=4096")!=std::string::npos);
    assert(s.find("iid_valid=1")!=std::string::npos);
    // adsp_apps is audio GPR, not FastRPC: a 72B GPR packet occupies the historical 88B GLINK interval.
    uint8_t audio[88]={};strcpy(c.name,"adsp_apps");
    put(audio,9|(3u<<16));put(audio+4,9);put(audio+8,72);
    put(audio+16,(72u<<8)|(6u<<4));put(audio+20,2|(3u<<8));put(audio+24,2);put(audio+28,2);
    put(audio+32,123);put(audio+36,0x0100100f);memcpy(audio+40,"PRIVATE-AUDIO-PAYLOAD",21);
    s=decode(g,audio,sizeof audio,1672);assert(s.find("padded_end=1760 complete=1")!=std::string::npos);
    assert(s.find("gpr version=0 header_bytes=24 size=72")!=std::string::npos && s.find("opcode=0x100100f valid=1")!=std::string::npos);
    assert(s.find("fastrpc")==std::string::npos && s.find("PRIVATE")==std::string::npos);
    put(audio+16,(100u<<8)|(6u<<4));s=decode(g,audio,sizeof audio);assert(s.find("valid=0")!=std::string::npos);
    strcpy(c.name,"fastrpcglink-apps-dsp");
    // Wrapped pending bytes copy is decoded with original circular offsets, not linear indices.
    uint8_t ring[128]={},linear[88];for(unsigned i=0;i<88;++i)ring[(112+i)%128]=packet[i];
    for(unsigned i=0;i<88;++i)linear[i]=ring[(112+i)%128];
    s=decode(g,linear,88,112,128);assert(s.find("padded_end=40 complete=1")!=std::string::npos);
    // Every truncation and a malicious chunk/count must stop without interpreting bytes outside the captured head.
    for(unsigned n=0;n<=88;++n)decode(g,packet,n);
    put(packet+8,0xffffffff);s=decode(g,packet,88);assert(s.find("complete=0")!=std::string::npos);
    put(packet,5|(3u<<16));put(packet+4,0xffffffff);decode(g,packet,88);
    put(packet,10);s=decode(g,packet,88);assert(s.find("unknown-opcode")!=std::string::npos);
    // Protocol header disclosure stops before private application data.
    strcpy(c.name,"PMIC_RTR_ADSP_APPS");memset(packet,0,sizeof packet);put(packet,9|(3u<<16));put(packet+8,48);
    put(packet+16,0x800b);put(packet+20,1);put(packet+24,0x11);memcpy(packet+28,"PRIVATE-CALIBRATION",19);
    s=decode(g,packet,64);assert(s.find("PRIVATE")==std::string::npos);assert(s.find("raw_protocol_header=0b8000000100000011000000\n")!=std::string::npos);
    strcpy(c.name,"IPCRTR");put(packet+8,48);put(packet+16,1);put(packet+20,1);put(packet+36,7);
    s=decode(g,packet,64);assert(s.find("PRIVATE")==std::string::npos);
    // All six channels and every local/remote slot fit without silently truncating the owner snapshot.
    for(auto &ch:g.channel){memset(ch.name,'x',sizeof ch.name);ch.intent_count=24;}
    char output[65536]={};pianoobserve::Text t={output,sizeof output};adspobserve::owner(t,g);assert(!t.truncated);
    assert(std::string(output).find("remote channel=5 index=47")!=std::string::npos);
    char tiny[8]={};pianoobserve::Text shortText={tiny,sizeof tiny};shortText.add("012345678901234");assert(shortText.truncated&&tiny[7]==0);
    // SMEM: only the requested partition, distinguish item absence/short/corrupt and bound every metadata load.
    std::vector<uint8_t> smem(0x4000);auto rd=[&](uint32_t at){assert(!(at&3)&&uint64_t(at)+4<=smem.size());return atr32(smem.data()+at);};
    auto w=[&](uint32_t at,uint32_t value){put(smem.data()+at,value);};
    w(0x3000,0x434f5424);w(0x3004,1);w(0x3008,1);w(0x3020,0x1000);w(0x3024,0x1000);w(0x302c,2u<<16);w(0x3030,8);
    w(0x1000,0x54525024);w(0x1004,2u<<16);w(0x1008,0x1000);w(0x100c,32+16+40);w(0x1010,0x1000);
    w(0x1020,(443u<<16)|0xa5a5);w(0x1024,40);
    w(0x1030,0x504d5324);w(0x1034,0x101);w(0x1038,2);w(0x103c,(1u<<16)|1);w(0x1040,1);
    memcpy(smem.data()+0x1044,"slave-kernel",13);w(0x1054,3);
    auto item=adspobserve::findItem(rd,0x4000,0,2,443);assert(!item.status&&item.offset==0x1030&&item.size==40);
    memset(output,0,sizeof output);pianoobserve::Text st={output,sizeof output};adspobserve::smp(st,rd,item,443);
    assert(std::string(output).find("value=0x3 fatal=1 ready=1")!=std::string::npos);
    assert(adspobserve::findItem(rd,0x4000,0,2,429).status==-7);
    const auto before=smem;adspobserve::findItem(rd,0x4000,0,2,443);assert(smem==before);
    w(0x100c,0xffffffff);assert(adspobserve::findItem(rd,0x4000,0,2,443).status==-4);
    // Fuzz malformed table/packet metadata under ASan/UBSan: no out-of-range reads or unbounded traversal.
    uint32_t random=27;auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned i=0;i<4000;++i){for(auto &b:packet)b=uint8_t(next());decode(g,packet,next()%129);}
    for(unsigned i=0;i<1000;++i){for(unsigned at=0;at<smem.size();at+=4)w(at,next());adspobserve::findItem(rd,0x4000,0,2,443);}
    // Real register geometry and non-fixed slot resolution, with a read allowlist. No write callback exists.
    std::array<uint32_t,0x100000/4> regs={};regs[0x20/4]=128;regs[0x24/4]=(6u<<28)|80;
    uint16_t sids[4]={0x1003,0x1083,0x1043,0x1063};
    for(unsigned i=0;i<4;++i){regs[(0x800+4*(21+i))/4]=0x80000000|sids[i];regs[(0xc00+4*(21+i))/4]=75-i;regs[((128+75-i)*4096+0x58)/4]=i+1;}
    auto regRead=[&](uint32_t at){assert(!(at&3)&&at<0x100000);assert(at!=0x68&&at!=0x70);return regs[at/4];};
    const auto oldRegs=regs;memset(output,0,sizeof output);pianoobserve::Text ft={output,sizeof output};sensorsobserve::faults(ft,regRead,0x100000,sids);
    assert(regs==oldRegs&&!ft.truncated);s=output;assert(s.find("sid=0x1083 slot=22")!=std::string::npos);assert(s.find("bank=72")!=std::string::npos);
    regs[0x24/4]=0xffffffff;memset(output,0,sizeof output);pianoobserve::Text bad={output,sizeof output};sensorsobserve::faults(bad,regRead,0x100000,sids);
    assert(std::string(output).find("unsupported-geometry")!=std::string::npos);
    puts("observe_host: PASS (bounded frames, private payload omission, fresh SMEM bounds, dynamic routes/faults, no mutation)");
}
