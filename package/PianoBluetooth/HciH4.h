// SPDX-License-Identifier: BSD-2-Clause-Patent
// HCI over a UART (H4, Bluetooth Core Specification Vol 4 Part A): the packet indicators, an encoder for commands and ACL data, and an
// incremental decoder that turns the byte stream from the controller into whole packets. Portable (host tests in tests/bt_host.cpp);
// no allocation, bounded buffers, every length checked before it is used.
//
// The Qualcomm parts add single-byte in-band sleep messages (IBS) between packets: 0xfc WAKE_ACK, 0xfd WAKE_IND, 0xfe SLEEP_IND (the
// Ubuntu piano kernel's hci_qca.c, read as the specification of the wire protocol). They are not H4 indicators, so the decoder
// reports them as such wherever a packet may start. Command packets are decoded too: the UART loopback self test sees its own.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace pianobt {

enum : uint8_t { kH4Command = 0x01, kH4Acl = 0x02, kH4Sco = 0x03, kH4Event = 0x04, kH4Iso = 0x05 };
enum : uint8_t { kIbsWakeAck = 0xfc, kIbsWakeInd = 0xfd, kIbsSleepInd = 0xfe };
enum : uint8_t { kEvCommandComplete = 0x0e, kEvCommandStatus = 0x0f, kEvHardwareError = 0x10, kEvVendor = 0xff };
inline bool isIbs(uint8_t b) { return b == kIbsWakeAck || b == kIbsWakeInd || b == kIbsSleepInd; }

inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// An H4 command: 01, opcode (little endian), parameter length, parameters. Returns the frame's length, 0 when it does not fit.
inline size_t h4Command(uint8_t *out, size_t cap, uint16_t opcode, const uint8_t *params, uint8_t len)
{
  if (cap < size_t(4) + len || (len && !params)) return 0;
  out[0] = kH4Command;
  out[1] = uint8_t(opcode);
  out[2] = uint8_t(opcode >> 8);
  out[3] = len;
  for (uint8_t i = 0; i < len; ++i) out[4 + i] = params[i];
  return size_t(4) + len;
}
// An H4 ACL packet: 02, handle (12 bits) with packet boundary and broadcast flags (bits 15:12), length (little endian), data.
inline size_t h4Acl(uint8_t *out, size_t cap, uint16_t handle, uint8_t flags, const uint8_t *data, uint16_t len)
{
  if (cap < size_t(5) + len || handle > 0xeff || (len && !data)) return 0;
  const uint16_t h = uint16_t(handle | uint16_t(flags & 0xfu) << 12);
  out[0] = kH4Acl;
  out[1] = uint8_t(h);
  out[2] = uint8_t(h >> 8);
  out[3] = uint8_t(len);
  out[4] = uint8_t(len >> 8);
  for (uint16_t i = 0; i < len; ++i) out[5 + i] = data[i];
  return size_t(5) + len;
}

// The byte-stream decoder. feed() hands each complete packet to sink(type, packet, length), the packet being its header and
// payload without the indicator byte, and each IBS byte to sink(byte, nullptr, 0). A byte that cannot start a packet is skipped and
// counted (resynchronisation); a packet longer than the buffer is skipped whole and counted.
class H4Decoder {
 public:
  static constexpr size_t kMax = 4 + 1024;   // the longest header (ACL/ISO: 4) and 1024 payload bytes
  struct Stats {
    uint64_t packets = 0, ibs = 0, resyncs = 0, oversized = 0, bytes = 0;
  };

  template <class Sink> size_t feed(const uint8_t *in, size_t n, Sink &&sink)
  {
    for (size_t i = 0; i < n; ++i) {
      const uint8_t b = in[i];
      ++stats_.bytes;
      if (!type_) {
        if (isIbs(b)) {
          ++stats_.ibs;
          sink(b, static_cast<const uint8_t *>(nullptr), size_t(0));
          continue;
        }
        header_ = headerLength(b);
        if (!header_) {
          ++stats_.resyncs;
          continue;
        }
        type_ = b;
        have_ = 0;
        need_ = header_;
        skip_ = false;
        continue;
      }
      if (!skip_) buf_[have_] = b;
      ++have_;
      if (have_ < need_) continue;
      if (need_ == header_) {
        const size_t payload = payloadLength();
        need_ = header_ + payload;
        if (need_ > kMax) {
          skip_ = true;
          ++stats_.oversized;
        }
        if (payload) continue;
      }
      if (!skip_) {
        ++stats_.packets;
        sink(type_, static_cast<const uint8_t *>(buf_), have_);
      }
      type_ = 0;
    }
    return n;
  }
  bool idle() const { return type_ == 0; }
  const Stats &stats() const { return stats_; }
  void reset()
  {
    type_ = 0;
    have_ = need_ = header_ = 0;
    skip_ = false;
  }

 private:
  static size_t headerLength(uint8_t type)
  {
    switch (type) {
    case kH4Command: return 3;
    case kH4Acl: return 4;
    case kH4Sco: return 3;
    case kH4Event: return 2;
    case kH4Iso: return 4;
    default: return 0;
    }
  }
  size_t payloadLength() const
  {
    switch (type_) {
    case kH4Command: return buf_[2];
    case kH4Acl: return le16(buf_ + 2);
    case kH4Sco: return buf_[2];
    case kH4Event: return buf_[1];
    case kH4Iso: return le16(buf_ + 2) & 0x3fffu;
    default: return 0;
    }
  }
  uint8_t buf_[kMax] = {};
  uint8_t type_ = 0;
  size_t have_ = 0, need_ = 0, header_ = 0;
  bool skip_ = false;
  Stats stats_;
};

// The host's LE event mask, patched in the host-to-controller byte stream (piano-bt-lemask; docs/drivers/bluetooth.md, "LE link
// ready"). The 27.3 bluetoothd sends a type-zero controller the LE event mask 0x1f (our runtime patch adds 0x1000), which masks LE
// Data Length Change (bit 6) and LE PHY Update Complete (bit 11). Its link-ready state machine still waits for them after the
// remote features (LE_READY_WAIT_FOR_LENGTH, then _WAIT_FOR_PHY): an LE link connects and never reaches GATT. The patcher ORs both
// bits into an LE Set Event Mask (0x2001, 8 parameter bytes) that enables LE Connection Complete (bit 0) and leaves every other byte
// of the stream as it is. It is stateful, so a command split across writes is patched too; it never shifts or drops a byte. A byte
// that cannot start a packet is passed on and counted (the host's stream is well formed; the controller answers such a byte).
class H4LeMaskPatcher {
 public:
  static constexpr uint16_t kOpcode = 0x2001;
  static constexpr uint8_t kAdd0 = 0x40;   // bit 6: LE Data Length Change
  static constexpr uint8_t kAdd1 = 0x08;   // bit 11: LE PHY Update Complete
  struct Seen {
    uint64_t from = 0, to = 0;   // the host's mask and the one sent
  };
  struct Stats {
    uint64_t masks = 0, patched = 0, resyncs = 0;
  };

  // Patches data[0..n) in place when enabled. sink(seen) is called once per complete LE Set Event Mask command.
  template <class Sink> void patch(uint8_t *data, size_t n, bool enabled, Sink &&sink)
  {
    for (size_t i = 0; i < n; ++i) {
      uint8_t &b = data[i];
      if (!type_) {
        if (isIbs(b)) continue;
        header_ = headerLength(b);
        if (!header_) {
          ++stats_.resyncs;
          continue;
        }
        type_ = b;
        have_ = 0;
        mask_ = false;
        continue;
      }
      if (have_ < header_) {
        hdr_[have_++] = b;
        if (have_ < header_) continue;
        remain_ = payloadLength();
        if (type_ == kH4Command && le16(hdr_) == kOpcode && hdr_[2] == 8) {
          mask_ = true;
          enabled_ = enabled;
          seen_ = Seen{};
          at_ = 0;
        }
        if (!remain_) type_ = 0;
        continue;
      }
      if (mask_) {
        seen_.from |= uint64_t(b) << (8 * at_);
        if (at_ == 0) add_ = enabled_ && (b & 0x01);
        if (add_ && at_ == 0) b = uint8_t(b | kAdd0);
        if (add_ && at_ == 1) b = uint8_t(b | kAdd1);
        seen_.to |= uint64_t(b) << (8 * at_);
        ++at_;
      }
      if (--remain_ == 0) {
        if (mask_) {
          ++stats_.masks;
          if (seen_.to != seen_.from) ++stats_.patched;
          sink(seen_);
        }
        type_ = 0;
      }
    }
  }
  const Stats &stats() const { return stats_; }

 private:
  static size_t headerLength(uint8_t type)
  {
    switch (type) {
    case kH4Command: return 3;
    case kH4Acl: return 4;
    case kH4Sco: return 3;
    case kH4Iso: return 4;
    default: return 0;   // events do not travel from the host
    }
  }
  size_t payloadLength() const
  {
    switch (type_) {
    case kH4Command: return hdr_[2];
    case kH4Acl: return le16(hdr_ + 2);
    case kH4Sco: return hdr_[2];
    case kH4Iso: return le16(hdr_ + 2) & 0x3fffu;
    default: return 0;
    }
  }
  uint8_t hdr_[4] = {};
  uint8_t type_ = 0;
  size_t header_ = 0, have_ = 0, remain_ = 0;
  bool mask_ = false, enabled_ = false, add_ = false;
  unsigned at_ = 0;
  Seen seen_;
  Stats stats_;
};

// The LE link-ready assist (piano-bt-lerdy; docs/drivers/bluetooth.md, "LE link ready"). The 27.3 bluetoothd leaves an LE link in LE_READY_WAIT_FOR_LENGTH after the remote features
// when both sides support data length extension, and in LE_READY_WAIT_FOR_PHY when both support 2M, until the controller reports
// LE Data Length Change (subevent 0x07) or LE PHY Update Complete (0x0c). It never requests either procedure itself. A controller
// (or peer) that does not run them leaves the link without GATT forever. The assist watches the controller's events and the host's
// ACL data and, only for a link that has its remote features but on which the host has not started GATT or SMP as a client, hands
// the host the event the controller would have sent had the procedure run and changed nothing: Data Length Change with the
// specification's defaults (27 octets, 328 us both ways) and PHY Update Complete with status 0 on LE 1M both ways. These are the
// link's true values when no procedure ran. Each injection is at most once per kind and step; a real event for the link cancels its
// kind. The second Data Length Change (same values) completes bluetoothd's length probe when the first one made it rescale.
// Portable, no allocation: the host tests drive it with bluetoothd's and the controller's recorded packets.
class LeReadyAssist {
 public:
  static constexpr unsigned kSlots = 8;
  static constexpr uint64_t kStepMs = 750;   // after the remote features: DLC at 1 step, PHY at 2, DLC again at 3
  static constexpr uint16_t kDefaultOctets = 27, kDefaultTimeUs = 328;
  enum : uint8_t { kInjectLength = 1, kInjectPhy = 2 };
  struct Inject {
    uint8_t bytes[16] = {};   // an H4 event, indicator first
    size_t len = 0;
    uint16_t handle = 0;
    uint8_t kind = 0, step = 0;
  };
  struct Stats {
    uint64_t links = 0, features = 0, realLength = 0, realPhy = 0, hostStarted = 0, injectedLength = 0, injectedPhy = 0,
             disconnects = 0, full = 0;
  };

  // A controller event, the packet without its H4 indicator: [code][length][parameters].
  void onEvent(const uint8_t *ev, size_t len, uint64_t nowMs)
  {
    if (len < 2 || size_t(ev[1]) + 2 != len) return;
    if (ev[0] == 0x05 && len >= 6) {   // Disconnection Complete: status, handle, reason
      if (ev[2] == 0) {
        Slot *s = find(le16(ev + 3) & 0xfffu);
        if (s) {
          *s = Slot{};
          ++stats_.disconnects;
        }
      }
      return;
    }
    if (ev[0] != 0x3e || len < 3) return;
    const uint8_t sub = ev[2];
    if ((sub == 0x01 || sub == 0x0a || sub == 0x29) && len >= 6) {   // LE (Enhanced) Connection Complete: status, handle
      if (ev[3] != 0) return;
      const uint16_t h = le16(ev + 4) & 0xfffu;
      Slot *s = find(h);
      if (!s) s = find(kFree);
      if (!s) {
        ++stats_.full;
        return;
      }
      *s = Slot{};
      s->handle = h;
      ++stats_.links;
    } else if (sub == 0x04 && len >= 14) {   // LE Read Remote Features Complete: status, handle, 8 feature bytes
      Slot *s = find(le16(ev + 4) & 0xfffu);
      if (!s || ev[3] != 0 || s->featuresAt) return;
      s->featuresAt = nowMs ? nowMs : 1;
      s->remoteLength = (ev[6] & 0x20) != 0;   // bit 5: LE Data Packet Length Extension
      s->remote2m = (ev[7] & 0x01) != 0;       // bit 8: LE 2M PHY
      ++stats_.features;
    } else if (sub == 0x07 && len >= 5) {   // LE Data Length Change: handle, ...
      Slot *s = find(le16(ev + 3) & 0xfffu);
      if (s && !s->realLength) {
        s->realLength = true;
        ++stats_.realLength;
      }
    } else if (sub == 0x0c && len >= 6) {   // LE PHY Update Complete: status, handle, ...
      Slot *s = find(le16(ev + 4) & 0xfffu);
      if (s && !s->realPhy) {
        s->realPhy = true;
        ++stats_.realPhy;
      }
    }
  }
  // The host's ACL packet without its indicator: [handle+flags][length][L2CAP length][CID][payload]. Only a first fragment's
  // L2CAP header is read: ATT client requests (Exchange MTU, Find Information, Find By Type Value, Read By Type, Read Blob, Read By
  // Group Type, Read Multiple; opcodes 0x02..0x12 even) or an SMP Pairing Request mean bluetoothd's link is ready.
  void onHostAcl(const uint8_t *acl, size_t len)
  {
    if (len < 9) return;
    const uint16_t hf = le16(acl);
    if (((hf >> 12) & 0x3u) == 0x1u) return;   // a continuing fragment
    Slot *s = find(hf & 0xfffu);
    if (!s || s->hostStarted) return;
    const uint16_t cid = le16(acl + 6);
    const uint8_t op = acl[8];
    const bool att = cid == 0x0004 && op >= 0x02 && op <= 0x12 && !(op & 1);
    const bool smp = cid == 0x0006 && op == 0x01;
    if (att || smp) {
      s->hostStarted = true;
      ++stats_.hostStarted;
    }
  }
  // The next injection due at nowMs, if any. Called by the worker between packets; returns true with one event in out.
  bool due(uint64_t nowMs, bool enabled, Inject &out)
  {
    if (!enabled) return false;
    for (Slot &s : slots_) {
      if (s.handle == kFree || !s.featuresAt || s.hostStarted) continue;
      while (s.step < 3 && nowMs >= s.featuresAt + kStepMs * (s.step + 1u)) {   // a step with nothing to do is passed over
        const uint8_t step = ++s.step;
        if ((step == 1 || step == 3) && s.remoteLength && !s.realLength) {
          lengthEvent(s.handle, out);
          out.step = step;
          ++stats_.injectedLength;
          return true;
        }
        if (step == 2 && s.remote2m && !s.realPhy) {
          phyEvent(s.handle, out);
          out.step = step;
          ++stats_.injectedPhy;
          return true;
        }
      }
    }
    return false;
  }
  const Stats &stats() const { return stats_; }

 private:
  static constexpr uint16_t kFree = 0xffff;
  struct Slot {
    uint16_t handle = kFree;
    uint64_t featuresAt = 0;
    uint8_t step = 0;
    bool remoteLength = false, remote2m = false, realLength = false, realPhy = false, hostStarted = false;
  };
  Slot *find(uint16_t h)
  {
    for (Slot &s : slots_)
      if (s.handle == h) return &s;
    return nullptr;
  }
  static void put16(uint8_t *p, uint16_t v)
  {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
  }
  static void lengthEvent(uint16_t h, Inject &out)
  {
    const uint8_t e[] = {kH4Event, 0x3e, 0x0b, 0x07, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < sizeof e; ++i) out.bytes[i] = e[i];
    put16(out.bytes + 4, h);
    put16(out.bytes + 6, kDefaultOctets);
    put16(out.bytes + 8, kDefaultTimeUs);
    put16(out.bytes + 10, kDefaultOctets);
    put16(out.bytes + 12, kDefaultTimeUs);
    out.len = sizeof e;
    out.handle = h;
    out.kind = kInjectLength;
  }
  static void phyEvent(uint16_t h, Inject &out)
  {
    const uint8_t e[] = {kH4Event, 0x3e, 0x06, 0x0c, 0x00, 0, 0, 0x01, 0x01};
    for (size_t i = 0; i < sizeof e; ++i) out.bytes[i] = e[i];
    put16(out.bytes + 5, h);
    out.len = sizeof e;
    out.handle = h;
    out.kind = kInjectPhy;
  }
  Slot slots_[kSlots];
  Stats stats_;
};

// The radio's health as the host sees it (bt-radio1; docs/drivers/bluetooth.md, "Radio health"). It counts every advertising and
// inquiry report the controller sends and buckets its RSSI, so a boot can tell a deaf receiver from a quiet room without a
// bluetoothd trace: on 11-20/11-22 about 90 % of the LE reports sat at -105..-119 dBm, outside short windows that matched
// PianoWifi's background scans (host-checks 20261124-bt-pair, 20261126-bt-hid). Reports: LE Advertising Report (subevent 0x02,
// one report, RSSI after the data), LE Extended Advertising Report (0x0d, 24 fixed bytes plus data per report, RSSI at +13),
// Inquiry Result with RSSI (0x22, 14 bytes per response, RSSI at +13), Extended Inquiry Result (0x2f, RSSI at +13) and the plain
// Inquiry Result (0x02, no RSSI: counted only). Inquiry Complete (0x01) is counted so a classic search with no answer shows. RSSI
// 127 ("not available") is counted apart. No address is kept or logged.
class RadioRssi {
 public:
  // Buckets: [0] >= -60, [1] -61..-75, [2] -76..-90, [3] -91..-100, [4] <= -101 dBm.
  static constexpr unsigned kBuckets = 5;
  struct Stats {
    uint64_t le[kBuckets] = {}, classic[kBuckets] = {};
    uint64_t leReports = 0, classicResults = 0, classicNoRssi = 0, inquiries = 0, unavailable = 0, malformed = 0;
    int leBest = -128, classicBest = -128;   // the strongest RSSI seen (-128: none)
  };

  static unsigned bucket(int rssi)
  {
    if (rssi >= -60) return 0;
    if (rssi >= -75) return 1;
    if (rssi >= -90) return 2;
    if (rssi >= -100) return 3;
    return 4;
  }

  // ev = [code][parameter length][parameters...], bytes = the whole event.
  void onEvent(const uint8_t *ev, size_t bytes)
  {
    if (bytes < 2 || size_t(ev[1]) + 2 != bytes) return;
    const uint8_t code = ev[0];
    if (code == 0x01) {
      ++stats_.inquiries;
    } else if (code == 0x02) {
      if (bytes >= 3) stats_.classicNoRssi += ev[2];
    } else if (code == 0x22 || code == 0x2f) {
      if (bytes < 3) return;
      const unsigned n = ev[2];
      const size_t step = code == 0x22 ? 14 : 0;   // the extended result carries one response and 240 EIR bytes
      if (code == 0x2f && n != 1) {
        ++stats_.malformed;
        return;
      }
      for (unsigned i = 0; i < n; ++i) {
        const size_t at = 3 + i * step + 13;
        if (at >= bytes) {
          ++stats_.malformed;
          return;
        }
        add(int8_t(ev[at]), false);
      }
    } else if (code == 0x3e && bytes >= 4) {
      const uint8_t sub = ev[2];
      const unsigned n = ev[3];
      if (sub == 0x02) {
        // One report: [type][addr type][addr 6][data len][data][rssi]. Several reports in one event are rare and laid out per
        // field in the specification; they are counted as malformed rather than guessed.
        if (n != 1 || bytes < 4 + 9) {
          ++stats_.malformed;
          return;
        }
        const size_t dataLen = ev[4 + 8];
        const size_t at = 4 + 9 + dataLen;
        if (at >= bytes) {
          ++stats_.malformed;
          return;
        }
        add(int8_t(ev[at]), true);
      } else if (sub == 0x0d) {
        size_t o = 4;
        for (unsigned i = 0; i < n; ++i) {
          if (o + 24 > bytes) {
            ++stats_.malformed;
            return;
          }
          add(int8_t(ev[o + 13]), true);
          o += 24 + ev[o + 23];
        }
      }
    }
  }
  const Stats &stats() const { return stats_; }

 private:
  void add(int rssi, bool le)
  {
    if (le) ++stats_.leReports;
    else ++stats_.classicResults;
    if (rssi == 127) {
      ++stats_.unavailable;
      return;
    }
    (le ? stats_.le : stats_.classic)[bucket(rssi)]++;
    int &best = le ? stats_.leBest : stats_.classicBest;
    if (rssi > best) best = rssi;
  }
  Stats stats_;
};

// Events: [code][parameter length][parameters].
struct CommandComplete {
  uint8_t ncmd = 0;
  uint16_t opcode = 0;
  const uint8_t *ret = nullptr;   // the return parameters (the first is the status of most commands)
  size_t retLen = 0;
};
inline bool parseCommandComplete(const uint8_t *ev, size_t len, CommandComplete &cc)
{
  if (len < 5 || ev[0] != kEvCommandComplete || size_t(ev[1]) + 2 != len || ev[1] < 3) return false;
  cc.ncmd = ev[2];
  cc.opcode = le16(ev + 3);
  cc.ret = ev + 5;
  cc.retLen = len - 5;
  return true;
}
struct CommandStatus {
  uint8_t status = 0, ncmd = 0;
  uint16_t opcode = 0;
};
inline bool parseCommandStatus(const uint8_t *ev, size_t len, CommandStatus &cs)
{
  if (len != 6 || ev[0] != kEvCommandStatus || ev[1] != 4) return false;
  cs.status = ev[2];
  cs.ncmd = ev[3];
  cs.opcode = le16(ev + 4);
  return true;
}

}  // namespace pianobt
