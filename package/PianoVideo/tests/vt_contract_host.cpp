// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#include "PianoVideoVTContract.h"
#include "HEVCFixture.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace pianovideo;
static int failures;
#define CHECK(x) do { if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x); ++failures;} } while(0)
// Our synthesized headers isolate each admission boundary; actual Apple wire
// serialization and every compressed packet are also checked by video-v6 --self-test.
struct BitWriter {
  std::vector<uint8_t> raw; unsigned bit = 0;
  void put(uint32_t value, unsigned n) {
    for (unsigned i = n; i; --i) { if (!bit) raw.push_back(0); raw.back() |= ((value >> (i - 1)) & 1) << (7 - bit); bit = (bit + 1) % 8; }
  }
  void ue(uint32_t n) { ++n; unsigned bits = 0; for (auto v = n; v; v >>= 1) ++bits; put(0,bits-1); put(n,bits); }
  void se(int n) { ue(n <= 0 ? uint32_t(-2*n) : uint32_t(2*n-1)); }
  std::vector<uint8_t> nal(uint8_t header) {
    put(1,1); while (bit) put(0,1);
    std::vector<uint8_t> out{header}; unsigned zeros = 0;
    for (auto b:raw) { if (zeros >= 2 && b <= 3) { out.push_back(3); zeros=0; } out.push_back(b); zeros=b?0:zeros+1; }
    return out;
  }
};
static std::vector<uint8_t> makeSps(unsigned profile=77,unsigned poc=0,unsigned refs=2,unsigned width=19,bool fields=false,bool crop=false,unsigned reorder=1) {
  BitWriter b; b.put(profile,8);b.put(profile==66?0xc0:0x40,8);b.put(13,8);b.ue(0);b.ue(0);b.ue(poc);if(!poc)b.ue(1);
  b.ue(refs);b.put(0,1);b.ue(width);b.ue(14);b.put(!fields,1);b.put(1,1);b.put(crop,1);
  b.put(1,1);b.put(0,1);b.put(0,1);b.put(0,1);b.put(0,1);b.put(0,1);b.put(0,1);b.put(0,1);b.put(0,1);
  b.put(1,1);b.put(1,1);b.ue(0);b.ue(0);b.ue(9);b.ue(9);b.ue(reorder);b.ue(refs);
  return b.nal(0x67);
}
static std::vector<uint8_t> makePps(bool entropy=true,unsigned groups=0,bool redundant=false,unsigned sps=0) {
  BitWriter b;b.ue(0);b.ue(sps);b.put(entropy,1);b.put(0,1);b.ue(groups);b.ue(1);b.ue(0);b.put(1,1);b.put(2,2);
  b.se(-2);b.se(0);b.se(-2);b.put(1,1);b.put(0,1);b.put(redundant,1);return b.nal(0x68);
}
static std::vector<uint8_t> avcc(const std::vector<uint8_t> &s,const std::vector<uint8_t> &p) {
  std::vector<uint8_t> out{1,s[1],s[2],s[3],0xff,0xe1,uint8_t(s.size()>>8),uint8_t(s.size())};
  out.insert(out.end(),s.begin(),s.end());out.push_back(1);out.push_back(uint8_t(p.size()>>8));out.push_back(uint8_t(p.size()));out.insert(out.end(),p.begin(),p.end());return out;
}
static std::vector<uint8_t> picture(const avc::Config &c,unsigned type,unsigned frame,unsigned poc,unsigned firstMb=0,bool idr=false,bool reference=true,unsigned pps=0) {
  BitWriter b;b.ue(firstMb);b.ue(type);b.ue(pps);b.put(frame,c.s.frameBits);if(idr)b.ue(0);if(!c.s.pocType)b.put(poc,c.s.pocBits);
  return b.nal(uint8_t((reference?0x60:0)|(idr?5:1)));
}
static void appendNal(std::vector<uint8_t> &out,const std::vector<uint8_t> &nal) {
  const auto at=out.size();out.resize(at+4);vtwire::put32(out.data()+at,uint32_t(nal.size()));out.insert(out.end(),nal.begin(),nal.end());
}
static void appendAtom(std::vector<uint8_t> &out,uint32_t kind,const void *p,uint32_t n) {
  const auto at=out.size();out.resize(at+8+n);vtwire::put32(out.data()+at,8+n);vtwire::put32(out.data()+at+4,kind);if(n)memcpy(out.data()+at+8,p,n);
}
static std::vector<uint8_t> frameRequest(const std::vector<uint8_t> &sample,uint32_t flags=1,unsigned count=1,unsigned sizeDelta=0,unsigned timingBytes=72,const std::vector<uint8_t> *format=nullptr) {
  using namespace vtwire;
  uint8_t uuid[16]={},head[256]={};Writer w(head,sizeof head);w.header(cc("dfrm"),FlagRequest,uuid,23);w.s64(cc(",frm"),124);w.u32(cc(",dfl"),flags);
  std::vector<uint8_t> body;uint32_t bytes=uint32_t(sample.size())+sizeDelta;uint8_t timing[80]={};
  uint32_t scale=90000,valid=1;memcpy(timing+24+8,&scale,4);memcpy(timing+24+12,&valid,4);
  appendAtom(body,cc("nsmp"),&count,4);appendAtom(body,cc("ssiz"),&bytes,4);appendAtom(body,cc("stia"),timing,timingBytes);if(format)appendAtom(body,cc("fdsc"),format->data(),uint32_t(format->size()));appendAtom(body,cc("sdat"),sample.data(),uint32_t(sample.size()));
  std::vector<uint8_t> sbuf;appendAtom(sbuf,cc("sbuf"),body.data(),uint32_t(body.size()));
  std::vector<uint8_t> request(head,head+w.size());const auto at=request.size();request.resize(at+16+sbuf.size());
  put32(request.data()+at,uint32_t(16+sbuf.size()));put32(request.data()+at+4,cc("para"));put32(request.data()+at+8,cc(",sbf"));put32(request.data()+at+12,cc("=sbf"));memcpy(request.data()+at+16,sbuf.data(),sbuf.size());return request;
}
static std::vector<uint8_t> entry(unsigned key,uint32_t type,const std::vector<uint8_t> &value) {
  std::vector<uint8_t> body;uint8_t idx[2]={uint8_t(key),uint8_t(key>>8)};
  appendAtom(body,vtwire::cc("idxk"),idx,2);appendAtom(body,type,value.data(),uint32_t(value.size()));
  std::vector<uint8_t> out;appendAtom(out,vtwire::cc("keyv"),body.data(),uint32_t(body.size()));return out;
}
static std::vector<uint8_t> sampleFormatBody(const std::vector<uint8_t> &conf,bool protection=false,bool duplicate=false,uint32_t codecType=vtwire::cc("avc1"),uint32_t width=320,uint32_t height=240,bool stringKey=false) {
  using namespace vtwire;uint8_t plist[1024];Plist pl;int top=pl.data(conf.data(),uint32_t(conf.size()));unsigned n=pl.write(top,plist,sizeof plist);
  auto e=entry(codecType==cc("hvc1")?112:105,cc("datv"),{plist,plist+n});
  if(stringKey){
    Plist key;uint8_t name[128];const unsigned bytes=key.write(key.string(codecType==cc("hvc1")?"hvcC":"avcC"),name,sizeof name);
    std::vector<uint8_t> body;appendAtom(body,cc("strk"),name,bytes);appendAtom(body,cc("datv"),plist,n);
    e.clear();appendAtom(e,cc("keyv"),body.data(),uint32_t(body.size()));
  }
  auto descriptions=e;if(duplicate)descriptions.insert(descriptions.end(),e.begin(),e.end());
  if(protection){auto sinf=entry(109,cc("datv"),{plist,plist+n});descriptions.insert(descriptions.end(),sinf.begin(),sinf.end());}
  auto dict=entry(49,cc("dict"),descriptions);std::vector<uint8_t> extn;appendAtom(extn,cc("dict"),dict.data(),uint32_t(dict.size()));
  std::vector<uint8_t> fdsc;uint32_t media=cc("vide"),codec=codecType,dims[2]={width,height};
  appendAtom(fdsc,cc("mdia"),&media,4);appendAtom(fdsc,cc("codc"),&codec,4);appendAtom(fdsc,cc("vdim"),dims,8);appendAtom(fdsc,cc("extn"),extn.data(),uint32_t(extn.size()));return fdsc;
}
static void streamTests() {
  const auto s=makeSps(), p=makePps();auto conf=avcc(s,p);avc::Config c;
  CHECK(avc::config(conf.data(),uint32_t(conf.size()),c));CHECK(c.valid()&&c.pocType==0&&c.hfiProfile()==2&&c.hfiLevel()==4&&c.s.maxReorder==1&&c.s.maxDpb==2);
  codec::Config wrapped;CHECK(codec::parseAvc(conf.data(),uint32_t(conf.size()),wrapped));
  auto formatBody=sampleFormatBody(conf);vtwire::Atom fdsc;fdsc.type=vtwire::cc("fdsc");fdsc.body=formatBody.data();fdsc.size=uint32_t(formatBody.size());
  CHECK(vtcontract::sampleFormat(fdsc,wrapped));
  for(auto bad:{sampleFormatBody(conf,true),sampleFormatBody(conf,false,true),sampleFormatBody(avcc(makeSps(77,2),p))}){
    fdsc.body=bad.data();fdsc.size=uint32_t(bad.size());CHECK(!vtcontract::sampleFormat(fdsc,wrapped));
  }
  auto invalidFormat=formatBody;invalidFormat[8]^=1;fdsc.body=invalidFormat.data();fdsc.size=uint32_t(invalidFormat.size());CHECK(!vtcontract::sampleFormat(fdsc,wrapped));
  fdsc.body=formatBody.data();for(unsigned n=0;n<formatBody.size();++n){fdsc.size=n;CHECK(!vtcontract::sampleFormat(fdsc,wrapped));}
  CHECK(c.same(c));avc::Config different=c;different.pps[2]^=1;CHECK(!c.same(different));
  CHECK(!avc::config(nullptr,0,different));
  for(auto invalid:{makeSps(100),makeSps(77,1),makeSps(77,0,5),makeSps(77,0,2,120),makeSps(77,0,2,19,true),makeSps(77,0,2,19,false,true),makeSps(77,0,2,19,false,false,3)}){
    auto bad=avcc(invalid,p);CHECK(!avc::config(bad.data(),uint32_t(bad.size()),different));
  }
  for(auto invalid:{makePps(true,1),makePps(true,0,true),makePps(true,0,false,1)}){
    auto bad=avcc(s,invalid);CHECK(!avc::config(bad.data(),uint32_t(bad.size()),different));
  }
  for(unsigned i:{0u,1u,2u,3u,4u,5u}){auto bad=conf;bad[i]^=1;CHECK(!avc::config(bad.data(),uint32_t(bad.size()),different));}
  for(unsigned n=0;n<conf.size();++n)CHECK(!avc::config(conf.data(),n,different));
  std::vector<uint8_t> samples[4];
  appendNal(samples[0],{0x09,0x10});appendNal(samples[0],{0x06,0x80});appendNal(samples[0],picture(c,2,0,0,0,true));
  appendNal(samples[1],picture(c,0,1,6));appendNal(samples[2],picture(c,1,2,2,0,false,false));appendNal(samples[3],picture(c,1,2,4,0,false,false));
  for(auto &sample:samples){
    avc::Picture pic;CHECK(avc::accessUnit(sample.data(),uint32_t(sample.size()),c,&pic));
    std::vector<uint8_t> normalized(sample.size()+c.parameterBytes());
    auto bytes=vtcontract::normalize(sample.data(),uint32_t(sample.size()),normalized.data(),uint32_t(normalized.size()),wrapped,true);
    CHECK(bytes==normalized.size()&&avc::normalized(normalized.data(),bytes,c));
    CHECK(!vtcontract::normalize(sample.data(),uint32_t(sample.size()),normalized.data(),bytes-1,wrapped,true));
    normalized[5]^=1;CHECK(!avc::normalized(normalized.data(),bytes,c));
    vtcontract::InputReport report;
    for(unsigned flags:{0u,1u,2u,3u,4u,5u,6u,8u,9u,11u,15u}) {auto request=frameRequest(sample,flags);CHECK(vtcontract::inputRequest(request.data(),uint32_t(request.size()),report,&wrapped)&&report.flags==flags&&report.hasToken&&report.token==124);}
    for(unsigned flags:{16u,17u,0x80000001u}) {auto request=frameRequest(sample,flags);CHECK(!vtcontract::inputRequest(request.data(),uint32_t(request.size()),report,&wrapped)&&report.transportReject);}
    for(auto request:{frameRequest(sample,1,2),frameRequest(sample,1,1,1),frameRequest(sample,1,1,0,71),frameRequest(sample,1,1,0,80)})CHECK(!vtcontract::inputRequest(request.data(),uint32_t(request.size()),report,&wrapped)&&!report.transportReject);
    for(unsigned i=0;i<sample.size();++i)CHECK(!avc::accessUnit(sample.data(),i,c));
  }
  // Multiple slices must retain one picture, strictly increasing MB addresses.
  auto multiple=samples[1];appendNal(multiple,picture(c,0,1,6,120));CHECK(avc::accessUnit(multiple.data(),uint32_t(multiple.size()),c));
  for(auto nal:{picture(c,0,1,6,0),picture(c,0,1,6,300),picture(c,0,2,6,120),picture(c,0,1,8,120),picture(c,0,1,6,120,false,false),picture(c,0,1,6,120,false,true,1)}){
    auto bad=samples[1];appendNal(bad,nal);CHECK(!avc::accessUnit(bad.data(),uint32_t(bad.size()),c));
  }
  auto changed=s;changed.back()^=1;for(auto nal:{changed,makePps(false),std::vector<uint8_t>{0x6c,0x80},std::vector<uint8_t>{0x05,0x80}}){auto bad=std::vector<uint8_t>{};appendNal(bad,nal);appendNal(bad,picture(c,2,0,0,0,true));CHECK(!avc::accessUnit(bad.data(),uint32_t(bad.size()),c));}
  auto empty=std::vector<uint8_t>{};appendNal(empty,s);appendNal(empty,p);CHECK(!avc::accessUnit(empty.data(),uint32_t(empty.size()),c));
  auto protectedHeader=samples[1];protectedHeader[4]|=0x80;CHECK(!avc::accessUnit(protectedHeader.data(),uint32_t(protectedHeader.size()),c));
  auto badLength=samples[1];vtwire::put32(badLength.data(),uint32_t(badLength.size()));CHECK(!avc::accessUnit(badLength.data(),uint32_t(badLength.size()),c));
  // Every bit of the admitted parameter record is bounded even on malformed
  // input; sanitizer runs exercise this same parser used by the kernel.
  for(unsigned i=0;i<conf.size()*8;++i){auto mutated=conf;mutated[i/8]^=uint8_t(1u<<(i%8));avc::Config parsed;(void)avc::config(mutated.data(),uint32_t(mutated.size()),parsed);}
  uint8_t forbidden[]={0x61,0,0,1};CHECK(!avc::ebsp(forbidden,sizeof forbidden));forbidden[3]=3;CHECK(!avc::ebsp(forbidden,sizeof forbidden));
  uint8_t escaped[]={0x61,0,0,3,2};CHECK(avc::ebsp(escaped,sizeof escaped));escaped[4]=4;CHECK(!avc::ebsp(escaped,sizeof escaped));
}
static void hevcContractTests() {
  using namespace vtwire;
  for(unsigned depth:{8u,10u}){
    hevcfixture::Fixture fixture(depth);codec::Config cfg;
    CHECK(codec::parseHevc(fixture.hvcc.data(),uint32_t(fixture.hvcc.size()),cfg));
    std::vector<uint8_t> sample;hevcfixture::append(sample,fixture.slice());
    for(bool stringKey:{false,true}){
      auto body=sampleFormatBody(fixture.hvcc,false,false,cc("hvc1"),1920,1080,stringKey);
      Atom fdsc{cc("fdsc"),body.data(),uint32_t(body.size())};CHECK(vtcontract::sampleFormat(fdsc,cfg));
      for(unsigned flags:{0u,1u,2u,3u}){
        auto request=frameRequest(sample,flags,1,0,72,&body);vtcontract::InputReport report;
        CHECK(vtcontract::inputRequest(request.data(),uint32_t(request.size()),report,&cfg)&&report.picture.idr&&report.flags==flags);
      }
    }
    for(auto body:{sampleFormatBody(fixture.hvcc,true,false,cc("hvc1"),1920,1080),
                   sampleFormatBody(fixture.hvcc,false,true,cc("hvc1"),1920,1080),
                   sampleFormatBody(fixture.hvcc,false,false,cc("hvc1"),1920,1072),
                   sampleFormatBody(fixture.hvcc,false,false,cc("avc1"),1920,1080)}){
      Atom fdsc{cc("fdsc"),body.data(),uint32_t(body.size())};CHECK(!vtcontract::sampleFormat(fdsc,cfg));
      auto request=frameRequest(sample,0,1,0,72,&body);vtcontract::InputReport report;
      CHECK(!vtcontract::inputRequest(request.data(),uint32_t(request.size()),report,&cfg)&&!strcmp(report.decision,"sample-format-change"));
    }
    hevcfixture::Fixture other(depth==8?10:8);auto changed=sampleFormatBody(other.hvcc,false,false,cc("hvc1"),1920,1080);
    Atom fdsc{cc("fdsc"),changed.data(),uint32_t(changed.size())};CHECK(!vtcontract::sampleFormat(fdsc,cfg));
    std::vector<uint8_t> extensions;appendAtom(extensions,cc("hvcC"),fixture.hvcc.data(),uint32_t(fixture.hvcc.size()));extensions.resize(extensions.size()+4);
    ImageDescription d;d.codec=cc("hvc1");d.width=1920;d.height=1080;d.extensions=extensions.data();d.extensionsSize=uint32_t(extensions.size());
    codec::Config parsed;vtcontract::FormatReport report;CHECK(vtcontract::format(d,&report,&parsed)&&parsed.same(cfg)&&report.terminalZero);
    d.codec=cc("hev1");CHECK(!vtcontract::format(d,&report,&parsed));d.codec=cc("hvc1");
    --d.width;CHECK(!vtcontract::format(d,&report,&parsed));++d.width;
    const auto original=extensions;extensions.insert(extensions.end()-4,original.begin(),original.end()-4);
    d.extensions=extensions.data();d.extensionsSize=uint32_t(extensions.size());CHECK(!vtcontract::format(d,&report,&parsed));
    // The parser keeps accepting the wider syntax for analysis, while the
    // native admission boundary must reject it before a hardware lease.
    for(const auto &outside:{hevcfixture::Fixture(depth,true,3840,2160,183),
                            hevcfixture::Fixture(depth,true,3840,2160,120),
                            hevcfixture::Fixture(depth,true,1920,1080,183)}) {
      codec::Config wide;CHECK(codec::parseHevc(outside.hvcc.data(),uint32_t(outside.hvcc.size()),wide));
      CHECK(!codec::nativeAdmitted(wide));
      std::vector<uint8_t> ext;appendAtom(ext,cc("hvcC"),outside.hvcc.data(),uint32_t(outside.hvcc.size()));
      ImageDescription large;large.codec=cc("hvc1");large.width=wide.s.width;large.height=wide.s.height;
      large.extensions=ext.data();large.extensionsSize=uint32_t(ext.size());
      CHECK(!vtcontract::format(large,&report,&parsed)&&!strcmp(report.decision,"hevc-native-envelope"));
    }
    CHECK(codec::nativeAdmitted(cfg));
  }
}
// Independent reader for the bplist object graph emitted to Apple. Check the
// actual nested capability dictionary and inventory, not string substrings.
struct PlistView {
  const uint8_t *p;uint32_t n;uint64_t count,top,table;unsigned offsetBytes,refBytes;
  PlistView(const uint8_t *bytes,uint32_t size):p(bytes),n(size){
    CHECK(n>=40&&!memcmp(p,"bplist00",8));const uint8_t *t=p+n-32;
    offsetBytes=t[6];refBytes=t[7];count=vtwire::be64(t+8);top=vtwire::be64(t+16);table=vtwire::be64(t+24);
    CHECK(offsetBytes&&offsetBytes<=8&&refBytes==1&&top<count&&table+count*offsetBytes<=n-32);
  }
  uint64_t offset(uint64_t id)const{CHECK(id<count);uint64_t at=0;for(unsigned i=0;i<offsetBytes;++i)at=(at<<8)|p[table+id*offsetBytes+i];CHECK(at<table);return at;}
  const uint8_t *body(uint64_t id,unsigned kind,uint64_t &length)const{
    uint64_t at=offset(id);const uint8_t tag=p[at++];CHECK((tag>>4)==kind);length=tag&15;
    if(length==15){const unsigned bytes=1u<<(p[at++]&15);length=0;for(unsigned i=0;i<bytes;++i)length=(length<<8)|p[at++];}
    CHECK(at+length<=table);return p+at;
  }
  bool string(uint64_t id,const char *value)const{uint64_t bytes;const uint8_t *b=body(id,5,bytes);return strlen(value)==bytes&&!memcmp(b,value,bytes);}
  uint64_t key(uint64_t dict,const char *name)const{uint64_t size;const uint8_t *b=body(dict,13,size);for(uint64_t i=0;i<size;++i)if(string(b[i],name))return b[size+i];CHECK(false);return 0;}
  int64_t integer(uint64_t id)const{uint64_t at=offset(id);CHECK(p[at]==0x13);return int64_t(vtwire::be64(p+at+1));}
};
static void capabilitiesTests(){
  using namespace vtwire;uint8_t bytes[4096];Plist p;
  const int items[]={vtcontract::inventoryEntry(p,false),vtcontract::inventoryEntry(p,true)};
  const int top=p.array(items,2);const uint32_t size=p.write(top,bytes,sizeof bytes);CHECK(size);
  PlistView view(bytes,size);uint64_t count;const auto *entries=view.body(view.top,10,count);CHECK(count==2);
  for(unsigned hevc=0;hevc<2;++hevc){
    const auto id=entries[hevc];CHECK(view.string(view.key(id,"CMClassImplementationID"),hevc?"com.unixnu.pianovideo.decoder.hevc":"com.unixnu.pianovideo.decoder.avc"));
    CHECK(view.integer(view.key(id,"VTRating"))==400&&bytes[view.offset(view.key(id,"IsPreferredInternalPlugin"))]==8);
    uint64_t codecs;const auto *types=view.body(view.key(id,"VTCodecType"),10,codecs);CHECK(codecs==1&&view.string(types[0],hevc?"hvc1":"avc1"));
    const auto cap=view.key(id,"VTDecoderCapabilities"),profiles=view.key(cap,"VTSupportedProfiles");
    uint64_t number;const auto *values=view.body(profiles,10,number);CHECK(number==(hevc?2u:3u));
    CHECK(view.integer(values[0])==(hevc?1:66)&&view.integer(values[1])==(hevc?2:77));
    const auto per=view.key(cap,"VTPerProfileSupport");
    for(const char *profile:hevc?std::vector<const char *>{"1","2"}:std::vector<const char *>{"66","77","100"}){
      const auto support=view.key(per,profile);CHECK(bytes[view.offset(view.key(support,"VTIsHardwareAccelerated"))]==9);
      CHECK(view.integer(view.key(support,"VTMaxDecodeLevel"))==(hevc?120:42));
      CHECK(view.integer(view.key(support,"VTMaxPlaybackLevel"))==(hevc?120:42));
    }
  }
}
int main() {
  streamTests();hevcContractTests();capabilitiesTests();
  uint8_t sample[252], au[287];
  vtwire::put32(sample,248); memcpy(sample+4,session::kAccessUnit+38,248);
  CHECK(vtcontract::sample(sample,sizeof sample));
  CHECK(vtcontract::normalize(sample,sizeof sample,au,sizeof au)==287);
  CHECK(vtcontract::normalizedAu(au,sizeof au));
  CHECK(!vtcontract::sample(sample,251));
  sample[4]=0x61; CHECK(!vtcontract::sample(sample,252)); sample[4]=0x65;
  au[4]^=1; CHECK(!vtcontract::normalizedAu(au,287)); au[4]^=1;
  au[80]=0; au[81]=0; au[82]=1; CHECK(!vtcontract::normalizedAu(au,287));
  std::vector<uint8_t> huge(vtcontract::kMaxAuBytes+1); CHECK(!vtcontract::normalizedAu(huge.data(),uint32_t(huge.size())));
  native::Layout l; CHECK(vtcontract::layout(l,l.bytes)); l.uvOffset=92160; CHECK(!vtcontract::layout(l,l.bytes));
  CHECK(vtcontract::extent(0,384,240,320,92160)); CHECK(!vtcontract::extent(UINT64_MAX,384,240,320,92160));
  CHECK(!vtcontract::extent(0,UINT64_MAX,240,320,92160));
  vtcontract::Surface s; s.base=0x1000;s.size=115200;s.id=1;s.format=vtwire::cc("420v");s.planes=2;
  s.plane[0]={320,240,0,320,0};s.plane[1]={160,120,76800,320,0}; CHECK(vtcontract::surface(s));
  const auto allocation=s.size;s.size=uint64_t(vtcontract::kMaxSurfaceBytes)+1;CHECK(!vtcontract::surface(s));s.size=allocation;
  s.plane[1].nonlinear=1;CHECK(!vtcontract::surface(s));s.plane[1].nonlinear=0;
  s.plane[1].offset=76801;CHECK(!vtcontract::surface(s));
  uint8_t locked[0xc68]={};
  auto put64=[&](unsigned at,uint64_t x){memcpy(locked+at,&x,8);};
  auto put32=[&](unsigned at,uint32_t x){memcpy(locked+at,&x,4);};
  put64(0,0x1000);put32(0x18,27);put64(0x20,115200);put32(0x48,vtwire::cc("420v"));put32(0x58,2);
  for(unsigned p=0;p<2;++p){unsigned at=0x88+p*0x98;put64(at,p?160:320);put64(at+8,p?120:240);put64(at+0x18,p?76800:0);put64(at+0x20,320);locked[at+0x6d]=0xa5;}
  vtcontract::Surface parsed;CHECK(vtcontract::surfaceResult(locked,sizeof locked,27,parsed));
  CHECK(!vtcontract::surfaceResult(locked,sizeof locked-1,27,parsed));CHECK(!vtcontract::surfaceResult(locked,sizeof locked,28,parsed));
  locked[0x88+0x6c]=1;CHECK(!vtcontract::surfaceResult(locked,sizeof locked,27,parsed));locked[0x88+0x6c]=0;
  vtcontract::State st; CHECK(!st.accept());st.started=true;CHECK(st.accept());CHECK(!st.accept());CHECK(st.success());
  st.cancel();CHECK(!st.success());CHECK(st.complete());CHECK(!st.complete());CHECK(!st.accept());
  vtcontract::State finished;finished.started=true;finished.finish();CHECK(!finished.terminal&&finished.accept());finished.finish();CHECK(finished.terminal&&!finished.accept());
  uint8_t ext[46]={};vtwire::put32(ext,46);vtwire::put32(ext+4,vtwire::cc("avcC"));
  const uint8_t prefix[]={1,66,0xc0,13,0xff,0xe1,0,22};memcpy(ext+8,prefix,8);memcpy(ext+16,session::kAccessUnit+4,22);
  ext[38]=1;ext[39]=0;ext[40]=5;memcpy(ext+41,session::kAccessUnit+30,5);
  vtwire::ImageDescription d;d.codec=vtwire::cc("avc1");d.width=320;d.height=240;d.extensions=ext;d.extensionsSize=46;
  CHECK(vtcontract::format(d));d.width=321;CHECK(!vtcontract::format(d));d.width=320;ext[9]=100;CHECK(!vtcontract::format(d));
  ext[9]=66;
  // QuickTime's zero-u32 is legal only at an extension boundary and at EOF.
  std::vector<uint8_t> extensions(ext,ext+sizeof ext);extensions.resize(50);
  d.extensions=extensions.data();d.extensionsSize=uint32_t(extensions.size());
  vtcontract::FormatReport report;
  CHECK(vtcontract::format(d,&report)&&report.terminalZero&&report.atomBytes==46&&report.avcBytes==38);
  CHECK(!vtcontract::atoms(d.extensions,d.extensionsSize)); // not a global atoms relaxation
  for(unsigned n=1;n<=8;++n){
    extensions.resize(46+n);d.extensions=extensions.data();d.extensionsSize=uint32_t(extensions.size());
    CHECK(vtcontract::format(d)==(n==4));
  }
  extensions.resize(50);extensions.back()=1;d.extensions=extensions.data();d.extensionsSize=50;CHECK(!vtcontract::format(d));
  extensions.back()=0;
  // Reject hidden atoms after a terminator, duplicate avcC, protection and HEVC.
  extensions.insert(extensions.end(),ext,ext+46);d.extensions=extensions.data();d.extensionsSize=uint32_t(extensions.size());CHECK(!vtcontract::format(d));
  extensions.assign(ext,ext+46);extensions.insert(extensions.end(),ext,ext+46);extensions.resize(96);
  d.extensions=extensions.data();d.extensionsSize=96;CHECK(!vtcontract::format(d));
  for(auto kind:{vtwire::cc("sinf"),vtwire::cc("hvcC")}){
    extensions.assign(ext,ext+46);extensions.resize(58);vtwire::put32(extensions.data()+46,8);vtwire::put32(extensions.data()+50,kind);
    d.extensions=extensions.data();d.extensionsSize=58;CHECK(!vtcontract::format(d));
  }
  // A valid atom body ending in zero4 is not mistaken for a terminator.
  extensions.assign(ext,ext+46);extensions.resize(62);vtwire::put32(extensions.data()+46,16);vtwire::put32(extensions.data()+50,vtwire::cc("free"));
  d.extensions=extensions.data();d.extensionsSize=62;CHECK(vtcontract::format(d,&report)&&!report.terminalZero&&report.atomBytes==62);
  uint8_t request[256]={},requestUuid[16]={};vtwire::Writer requestHeader(request,sizeof request);
  requestHeader.header(vtwire::cc("dsta"),vtwire::FlagRequest,requestUuid,103);
  auto *p=request+vtwire::HeaderBytes;vtwire::put32(p,16+136);vtwire::put32(p+4,vtwire::cc("para"));
  vtwire::put32(p+8,vtwire::cc(",fds"));vtwire::put32(p+12,vtwire::cc("=fds"));
  auto *fds=p+16;vtwire::put32(fds,136);vtwire::put32(fds+4,vtwire::cc("avc1"));fds[32]=1;fds[33]=64;fds[35]=240;
  memcpy(fds+86,ext,46);const uint32_t requestBytes=vtwire::HeaderBytes+16+136;
  CHECK(vtcontract::formatRequest(request,requestBytes,report)&&report.terminalZero&&!strcmp(report.decision,"accept"));
  vtwire::put32(request+8,vtwire::cc("dcan"));CHECK(vtcontract::formatRequest(request,requestBytes,report));
  fds[33]=65;CHECK(!vtcontract::formatRequest(request,requestBytes,report)&&!strcmp(report.decision,"dimensions"));fds[33]=64;
  vtwire::put32(fds+4,vtwire::cc("hvc1"));CHECK(!vtcontract::formatRequest(request,requestBytes,report)&&!strcmp(report.decision,"hvcC-missing-or-duplicate"));vtwire::put32(fds+4,vtwire::cc("avc1"));
  vtwire::put32(fds,135);CHECK(!vtcontract::formatRequest(request,requestBytes,report)&&!strcmp(report.decision,"declared-size"));vtwire::put32(fds,136);
  vtwire::put32(p+12,vtwire::cc("=dat"));CHECK(!vtcontract::formatRequest(request,requestBytes,report)&&!strcmp(report.decision,"fds-type"));vtwire::put32(p+12,vtwire::cc("=fds"));
  CHECK(!vtcontract::formatRequest(request,requestBytes-1,report));
  uint8_t wire[256]={},uuid[16]={};vtwire::Writer msg(wire,sizeof wire);msg.header(vtwire::cc("dfrm"),vtwire::FlagRequest,uuid,1);msg.s64(vtwire::cc(",frm"),0);
  CHECK(vtcontract::message(wire,msg.size()));msg.s64(vtwire::cc(",frm"),1);CHECK(!vtcontract::message(wire,msg.size()));
  CHECK(!vtcontract::message(wire,msg.size()-1));
  // The decoded PV envelope: ,frm=s64, ,dfl=u32, ,sbf=sbf containing
  // sbuf with sibling timing/format metadata and raw sdat. Never an HFI ring.
  std::vector<uint8_t> frame(1024);vtwire::Writer fh(frame.data(),uint32_t(frame.size()));
  fh.header(vtwire::cc("dfrm"),vtwire::FlagRequest,uuid,109);fh.s64(vtwire::cc(",frm"),0x123456789abcdefLL);fh.u32(vtwire::cc(",dfl"),1);
  auto *fp=frame.data()+fh.size();const uint32_t sampleEnvelope=8+8+24+8+sizeof sample;
  vtwire::put32(fp,16+sampleEnvelope);vtwire::put32(fp+4,vtwire::cc("para"));vtwire::put32(fp+8,vtwire::cc(",sbf"));vtwire::put32(fp+12,vtwire::cc("=sbf"));
  auto *sb=fp+16;vtwire::put32(sb,sampleEnvelope);vtwire::put32(sb+4,vtwire::cc("sbuf"));
  vtwire::put32(sb+8,32);vtwire::put32(sb+12,vtwire::cc("opts"));
  auto *sd=sb+40;vtwire::put32(sd,8+sizeof sample);vtwire::put32(sd+4,vtwire::cc("sdat"));memcpy(sd+8,sample,sizeof sample);
  const uint32_t fn=fh.size()+16+sampleEnvelope;vtcontract::InputReport input;
  CHECK(vtcontract::inputRequest(frame.data(),fn,input)&&input.hasToken&&input.token==0x123456789abcdefLL&&input.bytes==252&&!memcmp(input.data,sample,252));
  vtwire::put32(sd+8,512);CHECK(!vtcontract::inputRequest(frame.data(),fn,input)&&input.hasToken&&!strcmp(input.decision,"sample-not-qualified"));vtwire::put32(sd+8,248);
  CHECK(!vtcontract::inputRequest(frame.data(),fn-1,input));
  vtwire::put32(fp+12,vtwire::cc("=dat"));CHECK(!vtcontract::inputRequest(frame.data(),fn,input)&&input.hasToken&&!strcmp(input.decision,"sample-parameter"));vtwire::put32(fp+12,vtwire::cc("=sbf"));
  vtwire::put32(sd+4,vtwire::cc("free"));CHECK(!vtcontract::inputRequest(frame.data(),fn,input)&&!strcmp(input.decision,"sdat-layout"));vtwire::put32(sd+4,vtwire::cc("sdat"));
  vtwire::put32(frame.data()+vtwire::HeaderBytes+12,vtwire::cc("=u32"));
  CHECK(!vtcontract::inputRequest(frame.data(),fn,input)&&!input.hasToken);vtwire::put32(frame.data()+vtwire::HeaderBytes+12,vtwire::cc("=s64"));
  // Exactly the first tablet request's six bytes, not just a mutated valid AU.
  const uint8_t badSample[]={0,0,2,0,0x65,0x88};
  vtwire::put32(sd,14);memcpy(sd+8,badSample,sizeof badSample);vtwire::put32(sb,54);vtwire::put32(fp,70);
  CHECK(!vtcontract::inputRequest(frame.data(),fh.size()+70,input)&&input.hasToken&&input.bytes==6&&!strcmp(input.decision,"sample-not-qualified"));
  // Reproduce malformed -> Wait(empty finish) -> oversize -> Wait -> valid
  // -> repeat -> terminal drain. Reject completions retain their own tokens
  // and do not consume or overwrite the accepted AU's completion state.
  vtcontract::State life;life.started=true;vtcontract::RejectedFrames rejects;vtcontract::RejectedFrame rejected;
  CHECK(rejects.push(11,-12909));life.finish();CHECK(!life.terminal&&!life.accepted);
  CHECK(rejects.pop(rejected)&&rejected.token==11&&rejected.error==-12909);CHECK(!rejects.pop(rejected));
  CHECK(rejects.push(12,-12909));life.finish();CHECK(!life.terminal&&!life.accepted);
  CHECK(rejects.pop(rejected)&&rejected.token==12);CHECK(life.accept());CHECK(life.success());
  CHECK(!life.accept()&&rejects.push(14,-12903));CHECK(rejects.pop(rejected)&&rejected.token==14);CHECK(life.success());
  CHECK(life.complete()&&!life.complete());life.finish();CHECK(life.terminal&&!life.accept());
  for(unsigned i=0;i<vtcontract::RejectedFrames::Capacity;++i)CHECK(rejects.push(100+i,-12909));
  CHECK(!rejects.push(999,-12909)); // fail transport, never silently discard
  for(unsigned i=0;i<vtcontract::RejectedFrames::Capacity;++i)CHECK(rejects.pop(rejected)&&rejected.token==100+i);
  CHECK(!rejects.pop(rejected)&&!rejects.push(1,0));
  // Wraparound and cancellation settle each outstanding rejection separately.
  CHECK(rejects.push(21,-12909)&&rejects.push(22,-12903));life.cancel();
  CHECK(rejects.pop(rejected)&&rejected.token==21);CHECK(rejects.pop(rejected)&&rejected.token==22);CHECK(!rejects.pop(rejected));
  uint8_t plist[48]={};memcpy(plist,"bplist00",8);plist[8]=0x5f;plist[9]=0x1f;plist[16]=8;
  plist[22]=1;plist[23]=1;vtwire::put64(plist+24,1);vtwire::put64(plist+32,0);vtwire::put64(plist+40,16);
  char name[96];CHECK(!vtwire::plistString(plist,sizeof plist,name,sizeof name));
  if(failures)return 1;std::puts("PianoVideo V9 contract: AVC regression HEVC fdsc index112/string profiles metadata sync flags protection DRC layout PASS");return 0;
}
