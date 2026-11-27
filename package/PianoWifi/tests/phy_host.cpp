// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of level 7's HT/VHT choice (WifiHtVht.h, boot 10r) and of what it puts on the wire: the association request's
// elements (WifiDot11.h), the peer's association command and the started channel (WifiAssoc.h), read back as the firmware
// reads them.
//   c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. phy_host.cpp -o /tmp/phy_host && /tmp/phy_host
#include <cstdio>
#include <cstring>
#include "WifiAssoc.h"
#include "WifiDot11.h"
#include "WifiHtVht.h"

using namespace pianowifi;

static int failures;
#define CHECK(x)                                                         \
  do {                                                                   \
    if (!(x)) {                                                          \
      ++failures;                                                        \
      std::printf("phy_host: FAILED %s (line %d)\n", #x, __LINE__);      \
    }                                                                    \
  } while (0)

// An access point of channel 36, 80 MHz (centre channel 42), HT 40 above, VHT MCS 0-9 on two streams, WMM.
static ScanEntry vhtAp()
{
  ScanEntry e;
  e.channel = 36;
  e.freq = 5180;
  e.htCapSeen = e.htOpSeen = e.vhtCapSeen = e.vhtOpSeen = e.wmm = true;
  const uint16_t htCap = kDot11HtCapLdpc | kDot11HtCap40 | kDot11HtCapSmPsDisabled | kDot11HtCapSgi20 | kDot11HtCapSgi40 |
                         kDot11HtCapTxStbc | (1u << kDot11HtCapRxStbcShift);
  e.htCap[0] = uint8_t(htCap);
  e.htCap[1] = uint8_t(htCap >> 8);
  e.htCap[2] = 0x17;              // A-MPDU 64 KiB, spacing code 5 (4 us)
  e.htCap[3] = 0xff;
  e.htCap[4] = 0xff;              // MCS 0-15
  e.htCap[5] = 0xff;              // and 16-23 (three streams: limited to ours)
  e.htOp[0] = 36;
  e.htOp[1] = 0x05;               // secondary above, any width
  const uint32_t vhtCap = 0x0f8001b0;   // its own: A-MPDU exponent 7 among the rest
  for (int i = 0; i < 4; ++i) e.vhtCap[i] = uint8_t(vhtCap >> (8 * i));
  e.vhtCap[4] = 0xea;             // rx map: 1-3 streams MCS 0-9, the rest none (0xffea)
  e.vhtCap[5] = 0xff;
  e.vhtCap[8] = 0xea;
  e.vhtCap[9] = 0xff;
  e.vhtOp[0] = 1;                 // 80 MHz
  e.vhtOp[1] = 42;                // its centre segment: channel 42, 5210 MHz
  return e;
}

int main()
{
  // ---- the choice
  {
    const ScanEntry ap = vhtAp();
    const PhyChoice legacy = phyChoose(ap, kPhyLegacy, 2, true, 5180);
    CHECK(legacy.kind == kPhyLegacy && legacy.phyMode == kWmiModeLegacy5 && !legacy.qos);
    const PhyChoice ht = phyChoose(ap, kPhyHt, 2, true, 5180);
    CHECK(ht.kind == kPhyHt && ht.qos && ht.bw40 && ht.ht40Above && ht.phyMode == kWmiModeHt40 && ht.centerFreq == 5190);
    CHECK(ht.htRateCount == 16 && ht.htRates[0] == 0 && ht.htRates[15] == 15 && ht.nss == 2);   // three streams cut to two
    CHECK((ht.peerFlags & kWmiPeerFlagHt) && (ht.peerFlags & kWmiPeerFlag40) && (ht.peerFlags & kWmiPeerFlagSpatialMux));
    CHECK((ht.peerFlags & kWmiPeerFlagLdpc) && (ht.peerFlags & kWmiPeerFlagStbc) && !(ht.peerFlags & kWmiPeerFlagVht));
    CHECK((ht.rateCaps & kWmiRcHt) && (ht.rateCaps & kWmiRcSgi) && (ht.rateCaps & kWmiRcCw40) && (ht.rateCaps & kWmiRcDs));
    CHECK(ht.maxMpdu == 65535 && ht.mpduDensity == 4);
    const PhyChoice vht = phyChoose(ap, kPhyVht, 2, true, 5180);
    CHECK(vht.kind == kPhyVht && vht.bw80 && vht.phyMode == kWmiModeVht80 && vht.centerFreq == 5210 && vht.nss == 2);
    CHECK((vht.peerFlags & kWmiPeerFlagVht) && (vht.peerFlags & kWmiPeerFlag80) && (vht.peerFlags & kWmiPeerFlagHt));
    CHECK(vht.vhtTxMcs == 0xfffa && vht.vhtRxMcs == 0xfffa);       // MCS 0-9 on two streams both ways
    CHECK(vht.maxMpdu == (1u << 20) - 1);                          // the VHT exponent (7) wins over HT's
    // one chain: one stream, MCS 0-7
    const PhyChoice one = phyChoose(ap, kPhyHt, 1, true, 5180);
    CHECK(one.htRateCount == 8 && one.nss == 1);
    // no WMM: legacy, and why
    ScanEntry noWmm = ap;
    noWmm.wmm = false;
    const PhyChoice nw = phyChoose(noWmm, kPhyVht, 2, true, 5180);
    CHECK(nw.kind == kPhyLegacy && std::strstr(nw.why, "WMM"));
    // 2.4 GHz: HT only
    ScanEntry g = ap;
    g.channel = 6;
    g.htOp[1] = 0x00;   // 20 MHz
    const PhyChoice g24 = phyChoose(g, kPhyVht, 2, false, 2437);
    CHECK(g24.kind == kPhyHt && g24.phyMode == kWmiModeHt20G && !g24.bw40 && std::strstr(g24.why, "5 GHz"));
    // an HT network with an empty receive mask: MCS 0-7 all the same
    ScanEntry empty = ap;
    for (int i = 3; i < 13; ++i) empty.htCap[i] = 0;
    const PhyChoice ec = phyChoose(empty, kPhyHt, 2, true, 5180);
    CHECK(ec.htRateCount == 8 && ec.nss == 1);
  }
  // ---- the association request: the three elements, in order after RSN
  {
    ScanEntry ap = vhtAp();
    ap.capability = 0x0011;   // ESS, privacy
    ap.rates[0] = 0x8c;
    ap.rates[1] = 0x12;
    ap.rates[2] = 0x98;
    ap.rateCount = 3;
    const uint8_t own[6] = {0x02, 0x50, 0x49, 0x41, 0x4e, 0x04}, bssid[6] = {0x3c, 1, 2, 3, 4, 5};
    const uint8_t ssid[4] = {'n', 'e', 't', '1'};
    AssocRequest r;
    r.own = own;
    r.bssid = bssid;
    r.ssid = ssid;
    r.ssidLength = 4;
    r.band5 = true;
    r.net = &ap;
    r.rsn = true;
    uint8_t f[512] = {};
    const uint32_t legacyLen = dot11BuildAssocRequest(f, sizeof f, r);
    r.ht = r.vht = r.wmm = true;
    const uint32_t len = dot11BuildAssocRequest(f, sizeof f, r);
    CHECK(len == legacyLen + 2 + kDot11HtCapBytes + 2 + kDot11VhtCapBytes + 9);
    const uint8_t *ht = f + legacyLen;
    CHECK(ht[0] == kDot11ElementHtCap && ht[1] == kDot11HtCapBytes && (ht[2] | ht[3] << 8) == kOwnHtCap && ht[4] == kOwnAmpduParams);
    CHECK(ht[5] == 0xff && ht[6] == 0xff && ht[7] == 0 && ht[2 + 15] == 0x01);
    const uint8_t *vht = ht + 2 + kDot11HtCapBytes;
    CHECK(vht[0] == kDot11ElementVhtCap && vht[1] == kDot11VhtCapBytes && (vht[6] | vht[7] << 8) == kOwnVhtMcsMap);
    const uint8_t *wmm = vht + 2 + kDot11VhtCapBytes;
    CHECK(wmm[0] == kDot11ElementVendor && wmm[1] == 7 && wmm[2] == 0x00 && wmm[3] == 0x50 && wmm[4] == 0xf2 && wmm[5] == 2 &&
          wmm[6] == 0 && wmm[7] == 1);
    // too small a buffer: refused, not cut
    CHECK(dot11BuildAssocRequest(f, len - 1, r) == 0);
  }
  // ---- the peer's association and the started channel, read back
  {
    const PhyChoice c = phyChoose(vhtAp(), kPhyVht, 2, true, 5180);
    PeerAssoc a;
    a.vdevId = 0;
    a.aid = 20;
    a.qos = true;
    a.htFlags = c.peerFlags;
    a.htCaps = c.htCaps;
    a.maxMpdu = c.maxMpdu;
    a.mpduDensity = c.mpduDensity;
    a.rateCaps = c.rateCaps;
    a.nss = c.nss;
    a.vhtCaps = c.vhtCaps;
    std::memcpy(a.htRates, c.htRates, c.htRateCount);
    a.htRateCount = c.htRateCount;
    a.vht = true;
    a.vhtRxMcs = c.vhtRxMcs;
    a.vhtTxMcs = c.vhtTxMcs;
    a.phyMode = c.phyMode;
    a.legacyRateCount = 3;
    uint8_t buf[1024] = {};
    WmiWriter w;
    const uint32_t n = assocBuildPeerAssoc(w, buf, sizeof buf, a);
    CHECK(n > 0);
    uint32_t size = 0;
    const uint8_t *cmd = wmiFind(buf + kWmiCmdHeaderBytes, n - kWmiCmdHeaderBytes, kWmiTagPeerAssocCmd, &size);
    CHECK(cmd != nullptr);
    if (cmd) {
      const uint32_t flags = wmiGet(cmd, 5);
      CHECK((flags & kWmiPeerFlagQos) && (flags & kWmiPeerFlagHt) && (flags & kWmiPeerFlagVht) && (flags & kWmiPeerFlag80));
      CHECK(wmiGet(cmd, 12) == 2 && wmiGet(cmd, 14) == kWmiModeVht80 && wmiGet(cmd, 18) == 16 && wmiGet(cmd, 9) == c.maxMpdu);
    }
    uint32_t vsize = 0;
    const uint8_t *vrs = wmiFind(buf + kWmiCmdHeaderBytes, n - kWmiCmdHeaderBytes, kWmiTagVhtRateSet, &vsize);
    CHECK(vrs && wmiGet(vrs, 1) == 0xfffa && wmiGet(vrs, 3) == 0xfffa);
    // without QoS the HT/VHT flags stay off (the firmware's own host sets them only for a WMM peer)
    a.qos = false;
    const uint32_t n2 = assocBuildPeerAssoc(w, buf, sizeof buf, a);
    const uint8_t *cmd2 = wmiFind(buf + kWmiCmdHeaderBytes, n2 - kWmiCmdHeaderBytes, kWmiTagPeerAssocCmd, &size);
    CHECK(cmd2 && !(wmiGet(cmd2, 5) & kWmiPeerFlagHt));
    // the started channel: VHT80's mode, its centre, HT and VHT allowed
    VdevStart v;
    v.freq = 5180;
    v.mode = c.phyMode;
    v.centerFreq = c.centerFreq;
    v.allowHt = v.allowVht = true;
    const uint32_t m = assocBuildVdevStart(w, buf, sizeof buf, v);
    const uint8_t *ch = wmiFind(buf + kWmiCmdHeaderBytes, m - kWmiCmdHeaderBytes, kWmiTagChannel, &size);
    CHECK(ch && wmiGet(ch, 0) == 5180 && wmiGet(ch, 1) == 5210);
    CHECK(ch && (wmiGet(ch, 3) & kWmiChanInfoModeMask) == kWmiModeVht80 && (wmiGet(ch, 3) & kWmiChanInfoAllowHt) &&
          (wmiGet(ch, 3) & kWmiChanInfoAllowVht) && !(wmiGet(ch, 3) & kWmiChanInfoHt40Plus));
    // HT40 above: the plus bit
    v.mode = kWmiModeHt40;
    v.centerFreq = 5190;
    v.ht40Above = true;
    v.allowVht = false;
    const uint32_t m2 = assocBuildVdevStart(w, buf, sizeof buf, v);
    const uint8_t *ch2 = wmiFind(buf + kWmiCmdHeaderBytes, m2 - kWmiCmdHeaderBytes, kWmiTagChannel, &size);
    CHECK(ch2 && (wmiGet(ch2, 3) & kWmiChanInfoHt40Plus) && wmiGet(ch2, 1) == 5190);
  }
  std::printf(failures ? "phy_host: %d check(s) failed\n" : "phy_host: all passed\n", failures);
  return failures ? 1 : 0;
}
