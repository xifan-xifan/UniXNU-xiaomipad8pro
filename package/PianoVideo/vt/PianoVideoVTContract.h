// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// V5a qualification contract, deliberately narrower than general AVC decoding.
#pragma once
#include "PianoVideoVTWire.h"
#include "PianoVideoNative.h"
#include "../VideoFixture.h"
#include "PianoVideoCodec.h"
namespace pianovideo { namespace vtcontract {
constexpr uint32_t kMaxAuBytes = 1u << 20;
// Bound snapshots independently of the native AU limit. Rejected frames still
// need completion; a dfrm error reply alone does not retire a VT frame.
constexpr uint32_t kMaxMessageBytes = 4u << 20;
constexpr uint32_t kMaxSurfaceBytes = 64u << 20; // bounded NV12/P010 task wiring
constexpr uint32_t kParameterBytes = 35; // start4+SPS22+start4+PPS5
// Same metadata for inventory registration and the global dcap query. Apple
// profile/level numbers are deliberately separate from the HFI enums. The
// experimental HEVC envelope excludes unqualified 4K cold reopen.
inline int capabilities(vtwire::Plist &p,bool hevc) {
  const int profileValues[]={p.integer(hevc?1:66),p.integer(hevc?2:77),hevc?-1:p.integer(100)};
  const int profiles=p.array(profileValues,hevc?2:3);
  const int keys[]={p.string("VTIsHardwareAccelerated"),p.string("VTMaxDecodeLevel"),p.string("VTMaxPlaybackLevel")};
  const int values[]={p.boolean(true),p.integer(hevc?codec::kHevcMaxLevel:42),p.integer(hevc?codec::kHevcMaxLevel:42)};
  const int support=p.dict(keys,values,3);
  const int perKeys[]={p.string(hevc?"1":"66"),p.string(hevc?"2":"77"),hevc?-1:p.string("100")};
  const int perValues[]={support,support,support};
  const int per=p.dict(perKeys,perValues,hevc?2:3);
  const int rootKeys[]={p.string("VTSupportedProfiles"),p.string("VTPerProfileSupport")};
  const int rootValues[]={profiles,per};return p.dict(rootKeys,rootValues,2);
}
inline int inventoryEntry(vtwire::Plist &p,bool hevc) {
  const int type=p.string(hevc?"hvc1":"avc1");
  const int keys[]={p.string("VTCodecType"),p.string("CMClassImplementationName"),p.string("IsPreferredInternalPlugin"),p.string("VTRating"),p.string("CMClassImplementationID"),p.string("VTDecoderCapabilities")};
  const int values[]={p.array(&type,1),p.string(hevc?"PianoVideo native bounded HEVC Main Main10":"PianoVideo native bounded AVC High"),p.boolean(false),p.integer(400),p.string(hevc?"com.unixnu.pianovideo.decoder.hevc":"com.unixnu.pianovideo.decoder.avc"),capabilities(p,hevc)};
  return p.dict(keys,values,6);
}
inline uint32_t completionInfo(uint32_t flags,bool image,int error) {return (flags&1)|((image&&!error)?0u:2u);}
inline bool atoms(const uint8_t *p, uint32_t n) {
  const uint8_t *at = p, *end = p + n;
  vtwire::Atom a;
  unsigned count = 0;
  while (at != end) if (++count > 128 || !vtwire::next(at, end, a)) return false;
  return true;
}
inline bool unique(const uint8_t *p, uint32_t n, uint32_t type, vtwire::Atom &result) {
  if (!atoms(p, n)) return false;
  const uint8_t *at = p, *end = p + n;
  vtwire::Atom a; bool found = false;
  while (vtwire::next(at, end, a)) if (a.type == type) {
    if (found) return false;
    found = true; result = a;
  }
  return found;
}
inline bool message(const uint8_t *p, uint32_t n) {
  if (!p || n < vtwire::HeaderBytes || n > kMaxMessageBytes || !atoms(p, n)) return false;
  const uint8_t *at = p + vtwire::HeaderBytes, *end = p + n;
  vtwire::Atom a;
  while (vtwire::next(at, end, a)) {
    if (a.type != vtwire::cc("para") || a.size < 8) return false;
    const uint8_t *q = p + vtwire::HeaderBytes;
    vtwire::Atom b;
    while (q < at - a.size - 8 && vtwire::next(q, end, b))
      if (vtwire::be32(b.body) == vtwire::be32(a.body)) return false;
  }
  return true;
}
// Read only the bounded slice-header prefix needed for the supported progressive
// Baseline SPS (POC type2). Emulation prevention is skipped without an extra copy.
struct Bits {
  const uint8_t *p; uint32_t n, byte = 0, bit = 0, zeros = 0; bool good = true;
  uint32_t read(unsigned count) {
    uint32_t v = 0;
    for (unsigned i = 0; i < count; ++i) {
      if (!bit && byte < n && zeros >= 2 && p[byte] == 3) { ++byte; zeros = 0; }
      if (byte >= n) { good = false; return 0; }
      v = (v << 1) | ((p[byte] >> (7-bit)) & 1);
      if (++bit == 8) { zeros = p[byte] ? 0 : zeros + 1; ++byte; bit = 0; }
    }
    return v;
  }
  uint32_t ue() {
    unsigned leading = 0;
    while (good && !read(1)) if (++leading > 20) { good = false; return 0; }
    return good ? ((1u << leading) - 1 + read(leading)) : 0;
  }
};
inline bool idr(const uint8_t *p, uint32_t n) {
  if (!p || n < 4 || n > kMaxAuBytes - kParameterBytes - 4 || p[0] != 0x65) return false;
  Bits b{p + 1, n - 1};
  const uint32_t first = b.ue(), slice = b.ue(), pps = b.ue();
  const uint32_t frame = b.read(4), id = b.ue();
  // No P/B slices or multiple slices/pictures; one IDR starts the only AU.
  return b.good && first == 0 && (slice == 2 || slice == 7) && pps == 0 && frame == 0 && id <= 65535;
}
inline bool sample(const uint8_t *p, uint32_t n) {
  return p && n >= 8 && n <= kMaxAuBytes - kParameterBytes && vtwire::be32(p) == n - 4 && idr(p + 4, n - 4);
}
inline bool normalizedAu(const uint8_t *p, uint32_t n) {
  if (!p || n < kParameterBytes + 8 || n > kMaxAuBytes || memcmp(p, session::kAccessUnit, kParameterBytes)) return false;
  const uint8_t start[4] = {0, 0, 0, 1};
  if (memcmp(p + kParameterBytes, start, 4) || !idr(p + kParameterBytes + 4, n - kParameterBytes - 4)) return false;
  // Another Annex-B start code is a second slice/AU or unsupported NAL. Reject
  // forbidden raw 00 00 {00,01,02}; emulation-prevented fixture passes.
  for (uint32_t i = kParameterBytes + 5; i + 2 < n; ++i)
    if (!p[i] && !p[i+1] && p[i+2] < 3) return false;
  return true;
}
inline uint32_t normalize(const uint8_t *p, uint32_t n, uint8_t *out, uint32_t cap) {
  if (!sample(p, n) || !out || cap < n + kParameterBytes) return 0;
  memcpy(out, session::kAccessUnit, kParameterBytes);
  vtwire::put32(out + kParameterBytes, 1);
  memcpy(out + kParameterBytes + 4, p + 4, n - 4);
  const uint32_t bytes = n + kParameterBytes;
  return normalizedAu(out, bytes) ? bytes : 0;
}
inline uint32_t normalize(const uint8_t *p, uint32_t n, uint8_t *out, uint32_t cap, const codec::Config &cfg, bool prepend) {
  return codec::normalize(p,n,out,cap,cfg,prepend);
}
// Target version6 indexed keys are stable table positions, confirmed in the
// decoded 27.3 initializer: SampleDescriptionExtensionAtoms=49 (0x1972d1cd8),
// avcC=105 and sinf=109 (0x1972d2038). hvcC=112: array starts
// sp+8 at 0x1972d1a48; hvcC constant 0x1e406ca10 is stored at sp+0x388
// at 0x1972d2090. These are target table positions, not FourCC ordering.
inline bool dictionaryEntry(const uint8_t *p, uint32_t n, uint16_t index, const char *name, vtwire::Atom &out, bool *present = nullptr) {
  if (!atoms(p,n)) return false;
  const uint8_t *at=p,*end=p+n; vtwire::Atom item; bool found=false;
  while (vtwire::next(at,end,item)) {
    if (item.type != vtwire::cc("keyv") || !atoms(item.body,item.size)) return false;
    const uint8_t *q=item.body,*e=q+item.size;vtwire::Atom key,value;
    if (!vtwire::next(q,e,key) || !vtwire::next(q,e,value) || q!=e) return false;
    bool match=false;
    if (key.type==vtwire::cc("idxk")) {
      if (key.size!=2) return false;
      match=(uint32_t(key.body[0])|(uint32_t(key.body[1])<<8))==index;
    } else if (key.type==vtwire::cc("strk")) {
      char text[128];if(!vtwire::plistString(key.body,key.size,text,sizeof text))return false;
      match=!strcmp(text,name);
    } else return false;
    if (match) { if(present)*present=true;if(found)return false;found=true;out=value; }
  }
  return found;
}
inline bool plistData(const uint8_t *p,uint32_t n,const uint8_t *&bytes,uint32_t &count) {
  if (!p || n<41 || memcmp(p,"bplist00",8))return false;
  const uint8_t *t=p+n-32;const unsigned os=t[6];
  if (!os || os>8 || t[7]!=1 || vtwire::be64(t+8)!=1 || vtwire::be64(t+16)!=0)return false;
  const uint64_t table=vtwire::be64(t+24);if(table<9 || table>n-32 || os>n-32-table)return false;
  uint64_t at=0;for(unsigned i=0;i<os;++i)at=(at<<8)|p[table+i];
  if(at<8 || at>=table || (p[at]&0xf0)!=0x40)return false;
  uint64_t length=p[at++]&15;
  if(length==15){if(at>=table || (p[at]&0xf0)!=0x10 || (p[at]&15)>3)return false;
    const unsigned size=1u<<(p[at++]&15);if(size>table-at)return false;
    length=0;for(unsigned i=0;i<size;++i)length=(length<<8)|p[at++];
  }
  if(length>UINT32_MAX || length!=table-at)return false;
  bytes=p+at;count=uint32_t(length);return true;
}
inline bool sampleFormat(const vtwire::Atom &fdsc,const codec::Config &cfg) {
  using namespace vtwire; Atom media,codec,dimensions,extensions,dictionary,description,configuration;
  auto le32=[](const uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;};
  if (!unique(fdsc.body,fdsc.size,cc("mdia"),media) || media.size!=4 || le32(media.body)!=cc("vide") ||
      !unique(fdsc.body,fdsc.size,cc("codc"),codec) || codec.size!=4 || le32(codec.body)!=cfg.format() ||
      !unique(fdsc.body,fdsc.size,cc("vdim"),dimensions) || dimensions.size!=8 || le32(dimensions.body)!=cfg.s.width || le32(dimensions.body+4)!=cfg.s.height ||
      !unique(fdsc.body,fdsc.size,cc("extn"),extensions) || !unique(extensions.body,extensions.size,cc("dict"),dictionary))return false;
  Atom protection; bool protectedData=false;
  dictionaryEntry(dictionary.body,dictionary.size,101,"CommonEncryptionProtected",protection,&protectedData);
  dictionaryEntry(dictionary.body,dictionary.size,102,"CommonEncryptionTrackEncryptionBox",protection,&protectedData);
  dictionaryEntry(dictionary.body,dictionary.size,103,"CommonEncryptionOriginalFormat",protection,&protectedData);
  if (protectedData || !dictionaryEntry(dictionary.body,dictionary.size,49,"SampleDescriptionExtensionAtoms",description) || description.type!=cc("dict") ||
      !dictionaryEntry(description.body,description.size,cfg.isHevc()?112:105,cfg.isHevc()?"hvcC":"avcC",configuration) || configuration.type!=cc("datv"))return false;
  dictionaryEntry(description.body,description.size,109,"sinf",protection,&protectedData);
  dictionaryEntry(description.body,description.size,cfg.isHevc()?105:112,cfg.isHevc()?"avcC":"hvcC",protection,&protectedData);
  if(protectedData)return false;
  const uint8_t *p=nullptr;uint32_t n=0;codec::Config incoming;
  return plistData(configuration.body,configuration.size,p,n) &&
    (cfg.isHevc()?codec::parseHevc(p,n,incoming):codec::parseAvc(p,n,incoming)) && cfg.same(incoming);
}
struct InputReport {
  int64_t token = 0;
  bool hasToken = false, transportReject = false;
  uint32_t flags = 0;
  codec::Picture picture;
  uint32_t parameterType = 0, bytes = 0;
  const uint8_t *data = nullptr;
  const char *decision = "frame-parameter";
};
inline bool inputRequest(const uint8_t *m, uint32_t n, InputReport &r, const codec::Config *cfg = nullptr) {
  r = InputReport{};
  if (!message(m,n) || !(r.hasToken = vtwire::paramS64(m,n,vtwire::cc(",frm"),r.token))) return false;
  vtwire::Param p; vtwire::Atom sbuf,sdat;
  if (cfg) {
    if (!vtwire::param(m,n,vtwire::cc(",frm"),p) || p.type != vtwire::cc("=s64") || p.size != 8) { r.transportReject = true; return false; }
    r.decision = "decode-flags";
    if (!vtwire::param(m,n,vtwire::cc(",dfl"),p) || p.type != vtwire::cc("=u32") || p.size != 4) { r.transportReject = true; return false; }
    r.flags = vtwire::be32(p.value);
    // selector3 only enqueues; a separate worker owns synchronous completion.
    if (r.flags & ~15u) { r.transportReject = true; return false; }
    r.decision = "unsupported-frame-options";
    if (vtwire::param(m,n,vtwire::cc(",fop"),p)) { r.transportReject = true; return false; }
  }
  r.decision = "sample-parameter";
  if (!vtwire::param(m,n,vtwire::cc(",sbf"),p)) return false;
  r.parameterType = p.type;
  if (p.type != vtwire::cc("=sbf")) return false;
  r.decision = "sbuf-layout";
  if (!unique(p.value,p.size,vtwire::cc("sbuf"),sbuf)) return false;
  r.decision = "sdat-layout";
  if (!unique(sbuf.body,sbuf.size,vtwire::cc("sdat"),sdat)) return false;
  r.data = sdat.body; r.bytes = sdat.size; r.decision = "sample-not-qualified";
  if (cfg) {
    r.decision = "sample-metadata";
    vtwire::Atom number, sizes, timing, outputPts, protectedAtom;
    auto le32 = [](const uint8_t *q) { uint32_t v; memcpy(&v,q,4); return v; };
    // Payloads here are memory blocks in the target serializer, unlike BE atoms
    // and PV scalar parameters (decoded 27.3 AppendNumSamples/SizeArray).
    if (!unique(sbuf.body,sbuf.size,vtwire::cc("nsmp"),number) || number.size != 4 || le32(number.body) != 1 ||
        !unique(sbuf.body,sbuf.size,vtwire::cc("ssiz"),sizes) || sizes.size != 4 || le32(sizes.body) != r.bytes ||
        !unique(sbuf.body,sbuf.size,vtwire::cc("stia"),timing) || timing.size != 72) return false;
    auto finiteTime = [&](const uint8_t *q, bool required) {
      const uint32_t scale = le32(q+8), flags = le32(q+12);
      if (!required && flags == 0) return true; // explicitly invalid DTS is legal
      return (flags & 1) && !(flags & ~3u) && scale && scale <= 0x7fffffffu;
    };
    if (!finiteTime(timing.body,false) || !finiteTime(timing.body+24,true) || !finiteTime(timing.body+48,false)) return false;
    if (vtwire::find(sbuf.body,sbuf.size,vtwire::cc("opts"),outputPts) &&
        (!unique(sbuf.body,sbuf.size,vtwire::cc("opts"),outputPts) || outputPts.size != 24 || !finiteTime(outputPts.body,true))) return false;
    if (vtwire::find(sbuf.body,sbuf.size,vtwire::cc("crpt"),protectedAtom) ||
        vtwire::find(sbuf.body,sbuf.size,vtwire::cc("kdcr"),protectedAtom)) return false;
    r.decision = "sample-format-change";
    vtwire::Atom fdsc;
    if (vtwire::find(sbuf.body,sbuf.size,vtwire::cc("fdsc"),fdsc) &&
        (!unique(sbuf.body,sbuf.size,vtwire::cc("fdsc"),fdsc) || !sampleFormat(fdsc,*cfg)))return false;
    r.decision = "sample-not-qualified";
    if (!codec::accessUnit(r.data,r.bytes,*cfg,&r.picture)) return false;
  } else if (!sample(r.data,r.bytes)) return false;
  r.decision = "accept"; return true;
}
struct FormatReport {
  uint32_t parameterType = 0, bytes = 0, declared = 0, codec = 0, width = 0, height = 0;
  uint32_t extensionBytes = 0, atomBytes = 0, avcBytes = 0;
  bool terminalZero = false;
  const char *decision = "missing-fds";
};
// QuickTime ImageDescription extensions alone may end in a zero u32. The
// native CM bridge includes it in the declared size. Recognize it only at an
// atom boundary and at the exact end; message/sample atoms remain strict.
inline bool imageExtensions(const uint8_t *p, uint32_t n, uint32_t &atomBytes, bool &terminalZero) {
  atomBytes = 0; terminalZero = false;
  if (!p) return false;
  const uint8_t *at = p, *end = p + n;
  vtwire::Atom a;
  unsigned count = 0;
  while (at != end) {
    if (end - at == 4 && !vtwire::be32(at)) { terminalZero = true; atomBytes = n - 4; return true; }
    if (++count > 128 || !vtwire::next(at, end, a)) return false;
  }
  atomBytes = n;
  return true;
}
inline bool formatParsed(const vtwire::ImageDescription &d, FormatReport *report, codec::Config &parsed) {
  using namespace vtwire;
  FormatReport local;
  FormatReport &r = report ? *report : local;
  r.codec = d.codec; r.width = d.width; r.height = d.height; r.extensionBytes = d.extensionsSize;
  r.avcBytes = 0;
  r.decision = "extension-layout";
  if (!imageExtensions(d.extensions, d.extensionsSize, r.atomBytes, r.terminalZero)) return false;
  r.decision = "codec";
  const bool hevc=d.codec==cc("hvc1");
  if (d.codec != cc("avc1") && !hevc) return false;
  r.decision = "dimensions";
  if (!d.width || !d.height || d.width > (hevc?3840u:1920u) || d.height > (hevc?2160u:1080u) || ((d.width | d.height) & 1)) return false;
  r.decision = hevc?"hvcC-missing-or-duplicate":"avcC-missing-or-duplicate";
  Atom parameter;
  if (!unique(d.extensions, r.atomBytes, hevc?cc("hvcC"):cc("avcC"), parameter)) return false;
  r.avcBytes = parameter.size;
  r.decision = hevc?"hvcC-configuration":"avcC-configuration";
  if (!(hevc?codec::parseHevc(parameter.body,parameter.size,parsed):codec::parseAvc(parameter.body,parameter.size,parsed)) ||
      d.width != parsed.s.width || d.height != parsed.s.height) return false;
  r.decision = "hevc-native-envelope";
  if (!codec::nativeAdmitted(parsed)) return false;
  Atom protectedAtom;
  r.decision = "protected-or-codec-mismatch";
  if (find(d.extensions, r.atomBytes, cc("sinf"), protectedAtom) ||
      find(d.extensions, r.atomBytes, hevc?cc("avcC"):cc("hvcC"), protectedAtom)) return false;
  Atom colr;
  if(find(d.extensions,r.atomBytes,cc("colr"),colr)) {
    if(!unique(d.extensions,r.atomBytes,cc("colr"),colr)||colr.size<10)return false;
    if(vtwire::be32(colr.body)==cc("nclx") && (colr.size!=11 || (colr.body[10]&0x80)))return false;
  }
  r.decision = "accept";
  return true;
}
// Keep the optional result's large parser record off callers that already
// supplied storage. Kernel paths always provide their heap/session object.
__attribute__((noinline)) inline bool formatUnstored(const vtwire::ImageDescription &d,FormatReport *report) {
  codec::Config parsed;return formatParsed(d,report,parsed);
}
inline bool format(const vtwire::ImageDescription &d, FormatReport *report = nullptr, codec::Config *config = nullptr) {
  return config?formatParsed(d,report,*config):formatUnstored(d,report);
}
// Shared by dsta/dcan and by the real CoreMedia serializer host test.
inline bool formatRequest(const uint8_t *m, uint32_t n, FormatReport &r, codec::Config *config = nullptr) {
  using namespace vtwire;
  r = FormatReport{};
  r.decision = "message-layout";
  if (!message(m, n)) return false;
  Param p;
  r.decision = "missing-fds";
  if (!param(m, n, cc(",fds"), p)) return false;
  r.parameterType = p.type; r.bytes = p.size;
  if (p.size >= 4) r.declared = be32(p.value);
  r.decision = "fds-type";
  if (p.type != cc("=fds")) return false;
  ImageDescription d;
  r.decision = "image-description";
  if (!imageDescription(p.value, p.size, d)) return false;
  r.codec = d.codec; r.width = d.width; r.height = d.height; r.extensionBytes = d.extensionsSize;
  r.decision = "declared-size";
  if (r.declared != p.size) return false;
  return format(d, &r, config);
}
inline bool extent(uint64_t offset, uint64_t stride, uint64_t rows, uint64_t width, uint64_t size) {
  return rows && width && stride >= width && offset <= size && width <= size-offset && rows-1 <= (size-offset-width)/stride;
}
inline bool layout(const native::Layout &l, uint64_t length) {
  const bool hevc=l.codec==vtwire::cc("hvc1"), ten=l.bitdepth==10;
  const uint32_t sampleBytes=ten?2:1, maxWidth=hevc?3840:1920, maxHeight=hevc?2160:1080;
  const uint32_t expectedStride=((l.codedWidth*sampleBytes+(ten?255:127))&~(ten?255u:127u));
  return (hevc||l.codec==vtwire::cc("avc1")) && (!ten||hevc) && (l.bitdepth==8||ten) &&
    l.width >= 96 && l.height >= 96 && l.width <= maxWidth && l.height <= maxHeight && !((l.width|l.height)&1) &&
    l.codedWidth >= l.width && l.codedWidth <= maxWidth && l.codedHeight >= l.height && l.codedHeight <= (hevc?2176u:1088u) &&
    !((l.codedWidth|l.codedHeight|l.cropLeft|l.cropTop)&1) &&
    l.cropLeft <= l.codedWidth-l.width && l.cropTop <= l.codedHeight-l.height &&
    l.stride == expectedStride && l.scanlines == ((l.codedHeight+31)&~31u) &&
    uint64_t(l.yOffset) == uint64_t(l.cropTop)*l.stride+uint64_t(l.cropLeft)*sampleBytes &&
    uint64_t(l.uvOffset) == uint64_t(l.stride)*l.scanlines+uint64_t(l.cropTop/2)*l.stride+uint64_t(l.cropLeft)*sampleBytes &&
    l.bytes <= length && l.bytes <= kMaxSurfaceBytes && l.format == vtwire::cc(ten?"x420":"420v") &&
    !(l.yOffset & (sampleBytes-1)) && !(l.uvOffset & (sampleBytes-1)) &&
    extent(l.yOffset,l.stride,l.height,uint64_t(l.width)*sampleBytes,l.bytes) &&
    extent(l.uvOffset,l.stride,l.height/2,uint64_t(l.width)*sampleBytes,l.bytes) &&
    uint64_t(l.yOffset)+uint64_t(l.height-1)*l.stride+uint64_t(l.width)*sampleBytes <= l.uvOffset;
}
struct Plane { uint64_t width = 0, height = 0, offset = 0, stride = 0; uint32_t nonlinear = 0; };
struct Surface {
  uint64_t base = 0, size = 0; uint32_t id = 0, format = 0, planes = 0; Plane plane[2];
};
inline bool surface(const Surface &s, uint32_t width = 320, uint32_t height = 240, uint32_t format = vtwire::cc("420v")) {
  const bool ten=format==vtwire::cc("x420");const uint64_t rowBytes=uint64_t(width)*(ten?2:1);
  return (ten||format==vtwire::cc("420v")) && width >= 96 && height >= 96 && width <= 3840 && height <= 2160 && !((width|height)&1) &&
    s.base && s.size && s.size <= kMaxSurfaceBytes && s.base <= UINT64_MAX-s.size && s.format == format && s.planes == 2 &&
    s.plane[0].width == width && s.plane[0].height == height && s.plane[1].width == width/2 && s.plane[1].height == height/2 &&
    !s.plane[0].nonlinear && !s.plane[1].nonlinear &&
    !(s.plane[0].offset&(ten?1u:0u)) && !(s.plane[1].offset&(ten?1u:0u)) &&
    !(s.plane[0].stride&(ten?1u:0u)) && !(s.plane[1].stride&(ten?1u:0u)) &&
    extent(s.plane[0].offset,s.plane[0].stride,height,rowBytes,s.size) && extent(s.plane[1].offset,s.plane[1].stride,height/2,rowBytes,s.size) &&
    s.plane[1].offset >= s.plane[0].offset + uint64_t(height-1)*s.plane[0].stride + rowBytes;
}
// Native 27.3 IOSurface lookup lock result. Plane+0x6c is a byte:
// IOSurfaceClientGetBytesPerRowOfPlane 0x1876b7978 uses ldrb, as does
// GetBaseAddressOfPlane 0x1876b7c2c. Neighbouring bytes are not this flag.
inline bool surfaceResult(const uint8_t *b, uint32_t n, uint32_t id, Surface &v, uint32_t width = 320, uint32_t height = 240, uint32_t format = vtwire::cc("420v")) {
  if (!b || n != 0xc68) return false;
  auto u64 = [&](uint32_t at) { uint64_t x; memcpy(&x,b+at,8); return x; };
  auto u32 = [&](uint32_t at) { uint32_t x; memcpy(&x,b+at,4); return x; };
  v.base=u64(0); v.id=u32(0x18); v.size=u64(0x20); v.format=u32(0x48); v.planes=u32(0x58);
  for (unsigned p=0;p<2;++p) {
    const uint32_t at=0x88+p*0x98;
    v.plane[p]={u64(at),u64(at+8),u64(at+0x18),u64(at+0x20),b[at+0x6c]};
  }
  return v.id==id && surface(v,width,height,format);
}
// All calls under the session lock. Invalid input does not consume the one-AU
// budget. Cancellation suppresses success, settles an accepted frame once.
struct RejectedFrame { int64_t token = 0; int error = 0; };
// Independent of the single accepted AU. A full queue fails the transport, so
// the guest's send-error path completes its frame without losing a token.
struct RejectedFrames {
  static constexpr unsigned Capacity = 8;
  RejectedFrame frames[Capacity];
  unsigned head = 0, count = 0;
  bool push(int64_t token, int error) {
    if (!error || count == Capacity) return false;
    frames[(head + count) % Capacity] = {token,error}; ++count; return true;
  }
  bool pop(RejectedFrame &frame) {
    if (!count) return false;
    frame = frames[head]; head = (head + 1) % Capacity; --count; return true;
  }
};
struct State {
  bool started = false, accepted = false, completed = false, terminal = false, cancelled = false;
  bool accept() { if (!started || accepted || terminal || cancelled) return false; accepted = true; return true; }
  bool complete() { if (!accepted || completed) return false; completed = true; return true; }
  bool success() const { return accepted && !completed && !cancelled; }
  // VT's WaitForAsynchronousFrames calls FinishDelayedFrames even after a
  // rejected sample. No native input means nothing to drain or terminate.
  void finish() { if (accepted) terminal = true; }
  void cancel() { cancelled = terminal = true; }
};
} }
