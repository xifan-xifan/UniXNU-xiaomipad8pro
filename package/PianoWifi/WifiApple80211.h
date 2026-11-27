// SPDX-License-Identifier: BSD-2-Clause-Patent
// The apple80211 requests (docs/drivers/wifi-menu.md section 5, milestones M1/M2): the BSD ioctls SIOCSA80211 / SIOCGA80211 that
// airportd and IO80211.framework send to a Wi-Fi interface (private/host-checks/20261114-io80211/REPORT.md section 2: the 40-byte
// request, the 27.3 request numbers), their answers' layouts and the handler that answers from PianoWifi's own state: the
// identity, power, channels, scan list, and (when sheng bit 0x100 is enabled) the supplicant association snapshot. Portable:
// no kernel header; the kext (PianoWifi80211.h) gives it copyin/copyout, a scratch buffer and its
// lock, and does what the answer asks for (events, the scan's completion). Host tests: tests/apple80211_host.cpp.
//
// Prior art: UniXNU-sheng package/WifiNet.cpp (our own organisation's, BSD-2-Clause-Patent, UPSTREAM), a raw SIOC[SG]A80211
// binding for iPadOS 24A446's wifid. Its layouts are used here where the 27.3 host analysis agrees and marked where it does not
// say (each struct below says which). Nothing of OpenIntelWireless (GPL) is used.
#pragma once
#include <stdint.h>
#include "WifiHw.h"   // kScanEntriesMax, kSsidMax
#include "WifiAssocSnapshot.h"
#include "WifiProfile.h"
#include "WifiWmi.h"

namespace pianowifi {
namespace a80211 {

// ---------------------------------------------------------------------------------------------------------- the transport
// _IOW('i', 200, struct apple80211req) and _IOWR('i', 201, ...): 27.3 confirmed (REPORT.md 2.1).
constexpr unsigned long kSiocSA80211 = 0x802869c8UL;
constexpr unsigned long kSiocGA80211 = 0xc02869c9UL;

// The request as the ioctl carries it (64-bit user space): 27.3 confirmed, 40 bytes.
struct Req {
  char name[16];      // the BSD interface ("en2")
  int32_t type;       // the request number below
  int32_t value;      // a small value in or out (some gets answer here when no buffer is given)
  uint32_t length;    // the buffer's length (some answers set it to the length they have)
  uint32_t pad;
  uint64_t data;      // the user-space buffer
};
static_assert(sizeof(Req) == 40, "apple80211req is 40 bytes");
static_assert(__builtin_offsetof(Req, type) == 16 && __builtin_offsetof(Req, value) == 20 &&
              __builtin_offsetof(Req, length) == 24 && __builtin_offsetof(Req, data) == 32, "apple80211req's fields");

// ------------------------------------------------------------------------------------------ the request numbers (27.3)
// Decoded from 27.3's own name table (REPORT.md 2.2); 7, 8, 28, 29, 54 are from sheng's set (unchanged numbers, not re-read).
enum : int32_t {
  kSsid = 1, kAuthType = 2, kCipherKey = 3, kChannel = 4, kPowerSave = 5, kTxPower = 7, kRate = 8, kBssid = 9, kScanReq = 10,
  kScanResult = 11, kCardCapabilities = 12, kState = 13, kPhyMode = 14, kOpMode = 15, kRssi = 16, kNoise = 17, kPower = 19,
  kAssociate = 20, kAssociateResult = 21, kDisassociate = 22, kStatusDevName = 23, kSupportedChannels = 27, kLocale = 28,
  kDeauth = 29, kDriverVersion = 43, kHardwareVersion = 44, kRsnIe = 46, kCountryCode = 51, kRadioInfo = 54, kRsnConf = 77,
  kPmkCache = 83, kScanReqMultiple = 86, kVirtualIfRole = 96, kScanCacheClear = 90, kCurrentNetwork = 103, kLinkChangedEventData = 156, kOffloadRsn = 177, kChannelsInfo = 207,
  kHwSupportedChannels = 254, kScanControl = 360, kCurPmk = 376, kRsnXe = 457, kInfraEnumerated = 512, kHeCapability = 570,
  kSuspendInterface = 606, kAggressiveEdca = 607,
};

// The name of a request for the log (the 27.3 names of the ones above; "?" for the rest, which the log gives by number).
inline const char *requestName(int32_t type)
{
  switch (type) {
  case kSsid: return "SSID";
  case kAuthType: return "AUTH_TYPE";
  case kCipherKey: return "CIPHER_KEY";
  case kChannel: return "CHANNEL";
  case kPowerSave: return "POWERSAVE";
  case kTxPower: return "TXPOWER";
  case kRate: return "RATE";
  case kBssid: return "BSSID";
  case kScanReq: return "SCAN_REQ";
  case kScanResult: return "SCAN_RESULT";
  case kCardCapabilities: return "CARD_CAPABILITIES";
  case kState: return "STATE";
  case kPhyMode: return "PHY_MODE";
  case kOpMode: return "OP_MODE";
  case kRssi: return "RSSI";
  case kNoise: return "NOISE";
  case kPower: return "POWER";
  case kAssociate: return "ASSOCIATE";
  case kAssociateResult: return "ASSOCIATE_RESULT";
  case kDisassociate: return "DISASSOCIATE";
  case kStatusDevName: return "STATUS_DEV_NAME";
  case kSupportedChannels: return "SUPPORTED_CHANNELS";
  case kLocale: return "LOCALE";
  case kDeauth: return "DEAUTH";
  case kDriverVersion: return "DRIVER_VERSION";
  case kHardwareVersion: return "HARDWARE_VERSION";
  case kRsnIe: return "RSN_IE";
  case kCountryCode: return "COUNTRY_CODE";
  case kRadioInfo: return "RADIO_INFO";
  case kRsnConf: return "RSN_CONF";
  case kPmkCache: return "PMK_CACHE";
  case kScanReqMultiple: return "SCAN_REQ_MULTIPLE";
  case kScanCacheClear: return "SCANCACHE_CLEAR";
  case kVirtualIfRole: return "VIRTUAL_IF_ROLE";
  case kCurrentNetwork: return "CURRENT_NETWORK";
  case kLinkChangedEventData: return "LINK_CHANGED_EVENT_DATA";
  case kOffloadRsn: return "OFFLOAD_RSN";
  case kChannelsInfo: return "CHANNELS_INFO";
  case kHwSupportedChannels: return "HW_SUPPORTED_CHANNELS";
  case kScanControl: return "SCAN_CONTROL";
  case kCurPmk: return "CUR_PMK";
  case kRsnXe: return "RSN_XE";
  case kInfraEnumerated: return "INFRA_ENUMERATED";
  case kHeCapability: return "HE_CAPABILITY";
  case kSuspendInterface: return "SUSPEND_INTERFACE";
  case kAggressiveEdca: return "AGGRESSIVE_EDCA";
  default: return "?";
  }
}

// --------------------------------------------------------------------------------- events (kernel events, class 6 / subclass 1)
// Vendor Apple, class KEV_IEEE80211_CLASS (6), subclass 1, the data: the interface's name (16 bytes) and then the payload.
// The codes are sheng's and AirportItlwm's historical ones (REPORT.md section 4: "prior-art evidence to test", not re-decoded).
constexpr uint32_t kEventClass = 6, kEventSubclass = 1;
// This transport posts BSD kernel events, not the IO80211 internal event
// ordinal table. In 27.3 airportd's BSD callback, 11 is COUNTRY_CODE_CHANGED
// (which clears the scan cache); 15 is SCAN_CACHE_UPDATED. The synchronous
// scan/association paths also drain GET11 and poll GET21, respectively.
enum : uint32_t {
  kEvPowerChanged = 1,
  kEvSsidChanged = 2,
  kEvBssidChanged = 3,
  kEvLinkChanged = 4,
  kEvAssocDone = 9,
  kEvScanDone = 10,
  kEvScanCacheUpdated = 15,
  kEvDriverAvailable = 55,
};

// Link-changed event4 and GET156: 27.3 airportd/CoreWiFi consume the same 32 bytes.
struct LinkEvent {
  uint8_t down, unused0[3];
  int32_t rssi;
  uint16_t snr, noise;
  uint8_t unused1[4];
  uint8_t involuntary, unused2[3];
  uint32_t reason, subreason;
  uint8_t unused3[4];
};
static_assert(sizeof(LinkEvent) == 32 && __builtin_offsetof(LinkEvent, involuntary) == 0x10 &&
              __builtin_offsetof(LinkEvent, reason) == 0x14 && __builtin_offsetof(LinkEvent, subreason) == 0x18,
              "27.3 link event layout");

constexpr uint32_t kBsdEventHeaderSize = 24, kBsdEventNameSize = 16;
constexpr uint32_t kBsdEventMaxSize = 288; // MLEN in the pinned 27.3 kernel
constexpr uint32_t kBsdScanDoneStatus[] = {1, 0}; // diagnostic {version, status}
static_assert(kBsdEventHeaderSize + kBsdEventNameSize + sizeof(LinkEvent) <= kBsdEventMaxSize, "link kernel event fits");
static_assert(kBsdEventHeaderSize + kBsdEventNameSize + sizeof(kBsdScanDoneStatus) <= kBsdEventMaxSize, "scan kernel event fits");

struct BsdEventPayload {
  const void *data;
  uint32_t size;
};

// Power/SSID/BSSID are change notices and their values come from GETs.
// Keep ASSOC_DONE in the BSD sequence as a notice. 27.3 airportd's BSD
// callback has no event-9 payload parser and reads the join result via GET21.
struct Power {
  uint32_t version, radios, state[4];
};
static_assert(sizeof(Power) == 24, "power");

inline Power powerRecord(bool on) { return {1, 1, {on ? 1u : 0u, 0, 0, 0}}; }

// DRIVER_AVAILABLE (BSD event 55). 27.3 airportd's _bsd_80211_event_callback (0x1000723f0) reads the u32 at +8 as `available` (flags at
// +4, reason at +0x10, subreason at +0x14) into its device node's isDriverAvailable byte; __allowAutoJoinWithTrigger refuses every
// auto-join ("driver not available", DriverUnavailable) while it is 0, and a node found through interfaceAdded starts at 0 (owner
// R4, LOCATIOND-AIRPORTD-R4.md). Apple's family posts a 0xf8-byte record; flags stay 0 so no trap/minidump fields are parsed.
struct DriverAvailability {
  uint32_t version, flags, available, unused0, reason, subreason;
  uint8_t unused1[0xf8 - 24];
};
static_assert(sizeof(DriverAvailability) == 0xf8 && __builtin_offsetof(DriverAvailability, available) == 8 &&
              __builtin_offsetof(DriverAvailability, reason) == 0x10 && __builtin_offsetof(DriverAvailability, subreason) == 0x14,
              "27.3 driver availability layout");
static_assert(kBsdEventHeaderSize + kBsdEventNameSize + sizeof(DriverAvailability) <= kBsdEventMaxSize,
              "driver availability kernel event fits");

inline DriverAvailability driverAvailabilityRecord(bool available)
{
  DriverAvailability record = {};
  record.available = available ? 1u : 0u;
  return record;
}

inline bool driverAvailabilityEventRequired(bool adopted, int posted, bool usable, bool force)
{
  return adopted && (force || posted != int(usable));
}

inline BsdEventPayload bsdEventPayload(uint32_t code, const LinkEvent &link, bool scanStatus, const Power *power = nullptr,
                                       const DriverAvailability *availability = nullptr)
{
  if (code == kEvLinkChanged) return {&link, sizeof link};
  // POWER_CHANGED keeps the power structure the POWER get answers (24 bytes, well inside the BSD limit). (The v13.1b boot helper's
  // stall once blamed on its absence was the locationd wait: LOCATIOND-SSID.md.)
  if (code == kEvPowerChanged && power) return {power, sizeof *power};
  if (code == kEvDriverAvailable && availability) return {availability, sizeof *availability};
  if (code == kEvScanDone && scanStatus) return {kBsdScanDoneStatus, sizeof kBsdScanDoneStatus};
  return {nullptr, 0};
}

// Guard start() and take() with the request lock. The delayed callback posts
// one completion per accepted scan, including scans overtaken by power-off.
struct ScanCompletions {
  bool start() { return pending_++ == 0; }
  uint32_t take() {
    const uint32_t pending = pending_;
    pending_ = 0;
    return pending;
  }

 private:
  uint32_t pending_ = 0;
};

template <typename Post>
inline void postScanCompletions(uint32_t pending, bool power, Post post)
{
  while (pending--) {
    if (power) post(kEvScanCacheUpdated);
    post(kEvScanDone);
  }
}

// SET 20 carries one of two fixed association records.  The field meanings and offsets are
// confirmed by the 27.3 Apple80211Associate2 parser and by the Pad 6 Pro binding (PAD6P0125;
// SHENG-JOIN): the modern record has 32-bit authentication/key fields and moves the network
// prefix from 0x0c to 0x10.  The trailing bytes are opaque to us and are deliberately zeroed.
struct AssociateParameters {
  uint32_t ssid_length;
  uint8_t ssid[32];
  uint8_t bssid[6], unused[2];
  uint32_t key_version, key_length, key_cipher;
  uint16_t key_flags, key_index;
  uint8_t key[64];
};
struct AssociateRequestLegacy {
  uint32_t version;
  uint16_t mode, auth_lower, auth_upper, unused;
  AssociateParameters network;
  uint8_t rest[0x250 - 0x88];
};
struct AssociateRequest27 {
  uint32_t version;
  uint16_t mode, unused;
  uint32_t auth_lower, auth_upper;
  AssociateParameters network;
  uint8_t rest[0x290 - 0x8c];
};
static_assert(sizeof(AssociateParameters) == 0x7c && offsetof(AssociateParameters, key) == 0x3c,
              "association network prefix");
static_assert(sizeof(AssociateRequestLegacy) == 592 && offsetof(AssociateRequestLegacy, network) == 0x0c,
              "legacy association layout");
static_assert(sizeof(AssociateRequest27) == 656 && offsetof(AssociateRequest27, network) == 0x10,
              "modern association layout");

enum : uint32_t {
  kAssociateProblemNone = 0,
  kAssociateProblemLayout,
  kAssociateProblemVersion,
  kAssociateProblemAuth,
  kAssociateProblemSsid,
  kAssociateProblemKeyLength,
  kAssociateProblemCipher,
  kAssociateProblemProfile,
};

inline const char *associateProblemName(uint32_t problem)
{
  switch (problem) {
  case kAssociateProblemLayout: return "unsupported association layout";
  case kAssociateProblemVersion: return "association version is not 1";
  case kAssociateProblemAuth: return "authentication selector/flags are inconsistent";
  case kAssociateProblemSsid: return "network name length is not 1 to 32 bytes";
  case kAssociateProblemKeyLength: return "credential length is out of range";
  case kAssociateProblemCipher: return "credential cipher/length is not supported";
  case kAssociateProblemProfile: return "the association did not form a usable join profile";
  default: return "unknown association refusal";
  }
}

// ------------------------------------------------------------------------------------------------------------- the values
enum : uint32_t { kStateInit = 0, kStateScan = 1, kStateAuth = 2, kStateAssoc = 3, kStateRun = 4 };
constexpr uint32_t kOpModeStation = 1;
// PHY modes (bit masks): auto, 11a, 11b, 11g, 11n, 11ac.
enum : uint32_t { kPhyAuto = 0x1, kPhy11a = 0x2, kPhy11b = 0x4, kPhy11g = 0x8, kPhy11n = 0x10, kPhy11ac = 0x80 };
constexpr uint32_t kPhySupported = kPhyAuto | kPhy11a | kPhy11b | kPhy11g | kPhy11n | kPhy11ac;
// Channel flags: 20 MHz, 2.4 GHz, 5 GHz, active (not passive-only). sheng's conservative set (REPORT.md 5: Ventura's menu wanted
// conservative scan-channel flags).
enum : uint32_t { kChan20MHz = 0x2, kChan2GHz = 0x8, kChan5GHz = 0x10, kChanActive = 0x80 };
// Card capabilities, the bit numbers of the bitmap: AES, AES-CCM, power management, short slot, short preamble, WPA2, short
// guard interval at 20 MHz, TSN — sheng's set (what this station does: WPA2-PSK with CCMP, power save, 11n). 27.3 (host analysis
// 2026-11-14, docs/drivers/wifi-menu.md section 5.2): a raw 23-byte bitmap from offset 0, no version word, asked by
// the bind (the only sanity request of the ioctl path: any error fails the bind); bit 46 would make airportd wait for a scan's
// completion event and bit 73 choose SCAN_REQ_MULTIPLE for directed scans: neither is set.
// Apple80211Associate2 also consumes these bytes directly.  Its RSN/TKIP branch at
// 0x1c4648bf8 tests handle[0x58] bit 1 and returns -3903 at 0x1c4648c00 when that
// bit is absent.  The WPA fallback at 0x1c4647bec tests handle[0x59] bit 5; bit 6
// of that byte is the existing WPA2 gate.  Keep all three advertised for the
// mixed WPA/WPA2 networks accepted by the native supplicant.
constexpr uint8_t kCapTkip = 1;  // RSN group cipher 2 (TKIP), confirmed in Associate2.
constexpr uint8_t kCapWpa = 13;   // WPA fallback gate, inferred from handle+0x59 bit 5.
constexpr uint8_t kCapWpa2 = 14;  // RSN/WPA2 gate, confirmed from handle+0x59 bit 6.
constexpr uint8_t kCapBits[] = {kCapTkip, 2, 3, 6, 9, 10, kCapWpa, kCapWpa2, 17, 19};
constexpr uint32_t kCapBytes = 23;
constexpr int kErrPowerOff = 82;      // EPWROFF on Darwin: "device power is off"
constexpr int kErrNotAssociated = 2;  // ENOENT (sheng's answer for what needs an association)
// EIO after the last scan result (sheng's; the cursor starts again). 27.3's synchronous scan ends its result loop well only on a
// get that returns exactly 5: whether the ioctl path hands it errno (then this works) or ioctl()'s -1 (then a scan over ioctls
// always fails and only a user client can end it) was not settled on the host: the M1 boot's first question
// (docs/drivers/wifi-menu.md section 5.7).
constexpr int kErrEndOfResults = 5;
constexpr int kErrUnsupported = 102;  // EOPNOTSUPP on Darwin
constexpr int kErrInval = 22, kErrFault = 14;

// ---------------------------------------------------------------------------------------------------------- the layouts
struct Channel {
  uint32_t version, channel, flags;
};
struct ChannelAnswer {
  uint32_t version;
  Channel channel;
};
static_assert(sizeof(ChannelAnswer) == 16, "channel answer");
static_assert(sizeof(Channel) == 12, "channel");

// {version, count, channels}: 4824 bytes on 27.3 (count at 4, entries from 8, stride 12; user space does not bound the count:
// at most 401), as sheng's 24A446 list. Only the entries we have are written.
constexpr uint32_t kChannelsMax = 64;
struct ChannelList {
  uint32_t version, count;
  Channel list[kChannelsMax];
};
static_assert(sizeof(ChannelList) == 8 + 12 * kChannelsMax, "channel list");

// IO80211's CHANNELS_INFO reply is an out-of-line 0x2720-byte record on 27.3.
// The parser consumes a 16-bit count at +0x0c, six byte arrays at the offsets
// below (one 0x190-byte slot per channel), and one 32-bit bitmap array at +0x12d0.
constexpr uint32_t kChannelsInfoBytes = 0x2720;
constexpr uint32_t kChannelsInfoCountAt = 0x0c;
constexpr uint32_t kChannelsInfoChannelAt = 0x32e;
constexpr uint32_t kChannelsInfoIndoorAt = 0x4be;
constexpr uint32_t kChannelsInfoDfsAt = 0x64e;
constexpr uint32_t kChannelsInfoPassiveAt = 0x7de;
constexpr uint32_t kChannelsInfo40At = 0x96e;
constexpr uint32_t kChannelsInfo80At = 0xafe;
constexpr uint32_t kChannelsInfoBitmapAt = 0x12d0;
constexpr uint32_t kChannelsInfoStride = 0x190;

constexpr uint16_t kChannelFreqs[] = {2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
                                      5180, 5200, 5220, 5240, 5745, 5765, 5785, 5805, 5825};
constexpr uint32_t kChannelCount = sizeof(kChannelFreqs) / sizeof(kChannelFreqs[0]);

// A scan result's head: the fields to the elements are sheng's 24A446 layout, which 27.3 keeps (the element length at 0x8a, preset
// by user space to its capacity, the elements at 0x8c). 27.3's record is 2280 bytes: 2048 bytes of elements, then fields from
// 0x88c (a request identifier user space puts there itself, a timestamp, an RSN priority, the AP's PHY mode, MLO flags). The
// The caller pre-fills the complete 27.3 record (including its tail) before each get.  Return the complete record so that stale
// bytes from that prefill cannot be parsed as a second answer; fillScanResult() and the scratch buffer zero the unused tail.
struct ScanResult {
  uint32_t version;              // 0x00
  Channel channel;               // 0x04
  uint16_t noiseKnown;           // 0x10
  int16_t noise, snr, rssi;      // 0x12 0x14 0x16
  uint16_t beaconInterval;       // 0x18
  uint16_t capability;           // 0x1a
  uint8_t bssid[6];              // 0x1c
  uint8_t rateCount, unused0;    // 0x22
  uint32_t rates[15];            // 0x24
  uint8_t ssidLength;            // 0x60
  uint8_t ssid[32];              // 0x61
  uint8_t unused1[3];
  uint32_t ageMs;                // 0x84
  uint8_t flags, unused2;        // 0x88
  uint16_t elementsLength;       // 0x8a
  uint8_t elements[1024];        // 0x8c
};
constexpr uint32_t kScanResultBytes27 = 2280;   // what airportd asks with (its SCAN_RESULT and CURRENT_NETWORK buffers)
// The record's TIMESTAMP: IO80211's __addScanResultToList (0x1c4644e48) puts the u64 at 0x8d0 under "TIMESTAMP"; CoreWiFi's
// -[NSDictionary(ScanRecord) timestamp] takes it as is and -[CWFScanResult age] is (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) -
// timestamp) / 1000000 ms, which airportd's auto-join compares with its maxAge (20000 ms). Nanoseconds of mach_continuous_time.
constexpr uint32_t kScanResultTimestampAt = 0x8d0;
static_assert(kScanResultTimestampAt + 8 <= kScanResultBytes27, "timestamp inside the record");
static_assert(sizeof(ScanResult) == 1164 && __builtin_offsetof(ScanResult, rssi) == 0x16 &&
              __builtin_offsetof(ScanResult, bssid) == 0x1c && __builtin_offsetof(ScanResult, ssidLength) == 0x60 &&
              __builtin_offsetof(ScanResult, ageMs) == 0x84 && __builtin_offsetof(ScanResult, elementsLength) == 0x8a &&
              __builtin_offsetof(ScanResult, elements) == 0x8c, "scan result layout");

// POWER: {version, radios, state per radio}; 24 bytes on 27.3 as in sheng's. The set reads state[0]; airportd's set first gets the
// radio count, so radios must not be 0.


// PHY_MODE: {version, supported modes, the active mode}.
struct PhyMode {
  uint32_t version, supported, active;
};
// RSSI and NOISE: {version, radios (0 is an error), unit (0 dBm), per chain[4], aggregate, extension per chain[4], its aggregate}:
// 52 bytes on 27.3 as in sheng's.
struct Rssi {
  uint32_t version, radios, unit;
  int32_t chain[4], aggregate, extChain[4], extAggregate;
};
static_assert(sizeof(Rssi) == 52, "rssi");

// DRIVER_VERSION / HARDWARE_VERSION: 27.3 asks with a 384-byte buffer and reads a NUL-terminated UTF-8 string from offset 0 (no
// version word, no number checked; sheng's layout). airportd's interface set-up fails without a DRIVER_VERSION answer.
constexpr uint32_t kVersionBytes = 384;
// STATUS_DEV_NAME: not asked by 27.3's bind; answered like the versions (a string from offset 0), its layout not re-read.
// COUNTRY_CODE: 3 bytes, a C string (27.3, as sheng's).
inline bool validCountryCode(uint8_t a, uint8_t b)
{
  return a >= 'A' && a <= 'Z' && b >= 'A' && b <= 'Z';
}

inline void setRegulatoryCountry(char (&out)[3], const char *text, uint32_t length)
{
  const bool valid = text && length == 2 && validCountryCode(uint8_t(text[0]), uint8_t(text[1]));
  out[0] = valid ? text[0] : 'X';
  out[1] = valid ? text[1] : 'X';
  out[2] = 0;
}

// SCAN_REQ (the parts read; 5528 bytes on 27.3, SCAN_REQ_MULTIPLE 5352): version, BSS type, BSSID, the SSID's length at 16 and
// its bytes at 20, the scan type at 0x34 (27.3, as sheng's). Type 3 asks for the results already held: sheng answers it at once
// from those, without a scan (kScanTypeCached); the type is read for the log, so one boot shows which type CoreWLAN sends.
constexpr uint32_t kScanReqSsidLengthAt = 16, kScanReqSsidAt = 20, kScanReqTypeAt = 0x34;
constexpr uint32_t kScanTypeCached = 3;

// ------------------------------------------------------------------------------------------------- our scan list, compact
// One network of PianoWifi's scan list (the registry property piano-wifi-scan, published by level 6: PianoWifi.cpp publishScan).
struct Bss {
  uint8_t bssid[6] = {};
  uint8_t ssid[kSsidMax] = {};
  uint8_t ssidLength = 0;
  bool hidden = false;
  uint16_t channel = 0;
  uint32_t freq = 0;
  int32_t rssi = 0;
  uint16_t capability = 0;       // the beacon's capability field (0 when the list predates it: then ESS, and privacy with an RSN)
  uint16_t beaconInterval = 0;   // TU (0: 100)
  uint8_t rates[16] = {};        // as the frame carried them (bit 7: basic), 500 kb/s units
  uint8_t rateCount = 0;
  uint8_t rsn[64] = {};          // the RSN element whole (48, length, body)
  uint8_t rsnLength = 0;
};

struct ScanSnapshot {
  Bss bss[kScanEntriesMax];
  uint32_t count = 0;
};

// "aa:bb:cc:dd:ee:ff" (the registry's form) into six bytes; false for anything else (a masked "**" included).
inline bool parseBssid(const char *text, uint8_t out[6])
{
  if (!text) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (unsigned i = 0; i < 6; ++i) {
    const int hi = nibble(text[3 * i]), lo = hi < 0 ? -1 : nibble(text[3 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    if (i < 5 ? text[3 * i + 2] != ':' : text[3 * i + 2] != 0) return false;
    out[i] = uint8_t(hi << 4 | lo);
  }
  return true;
}

inline uint32_t channelOfFreq(uint32_t mhz)
{
  return dot11ChannelOfFreq(mhz);
}

inline uint32_t rsnElementBytes(const Bss &b)
{
  if (b.rsnLength < 4 || b.rsnLength > sizeof b.rsn || b.rsn[0] != kDot11ElementRsn ||
      uint32_t(b.rsn[1]) + 2 != b.rsnLength)
    return 0;
  ScanSecurity security;
  wmiParseRsn(security, b.rsn + 2, b.rsn[1]);
  return security.truncated ? 0 : b.rsnLength;
}

inline void fillChannel(Channel &c, uint32_t mhz, uint32_t channel)
{
  c.version = 1;
  c.channel = channel ? channel : channelOfFreq(mhz);
  c.flags = kChan20MHz | (mhz >= 4900 ? kChan5GHz : kChan2GHz);
}

// The elements of a scan result, rebuilt from what the list keeps (the list does not keep the frame's elements): SSID (0), the
// supported rates (1, at most 8), the DS parameter set (3, 2.4 GHz), the extended rates (50, the rest) and the RSN element (48)
// as the frame carried it. Enough for the menu: the name, the band, the security. Returns the bytes written.
inline uint32_t buildElements(const Bss &b, uint8_t *out, uint32_t capacity, bool *hasRsn = nullptr)
{
  uint32_t n = 0;
  auto put = [&](uint8_t id, const uint8_t *body, uint32_t length) {
    if (n + 2 + length > capacity) return;
    out[n++] = id;
    out[n++] = uint8_t(length);
    for (uint32_t i = 0; i < length; ++i) out[n++] = body[i];
  };
  put(0, b.ssid, b.ssidLength <= kSsidMax ? b.ssidLength : kSsidMax);
  const uint32_t rates = b.rateCount <= sizeof b.rates ? b.rateCount : uint32_t(sizeof b.rates);
  if (rates) put(1, b.rates, rates < 8 ? rates : 8);
  if (b.freq && b.freq < 4900) {
    const uint8_t ch = uint8_t(b.channel ? b.channel : channelOfFreq(b.freq));
    put(3, &ch, 1);
  }
  if (rates > 8) put(50, b.rates + 8, rates - 8);
  const uint32_t rsnBytes = rsnElementBytes(b);
  if (hasRsn) *hasRsn = rsnBytes != 0;
  if (rsnBytes) put(kDot11ElementRsn, b.rsn + 2, rsnBytes - 2);
  return n;
}

// The bytes of a filled result to copy out.  27.3's caller asks for 0x8e8 bytes and parses fields in the tail, so both ioctl and
// user-client paths must return the full record even though our named struct contains only the common 1164-byte prefix.
inline uint32_t resultBytes(const ScanResult &) { return kScanResultBytes27; }

// The full record's TIMESTAMP (the caller's buffer holds kScanResultBytes27): the time the record is handed out. The list is the
// last completed scan's (refreshed at every SCAN_REQ from the published list), so its age as airportd counts it starts here.
inline void stampScanResult(uint8_t *record, uint64_t nowNs)
{
  for (unsigned i = 0; i < 8; ++i) record[kScanResultTimestampAt + i] = uint8_t(nowNs >> (8 * i));
}

inline void fillScanResult(ScanResult &r, const Bss &b)
{
  uint8_t *raw = reinterpret_cast<uint8_t *>(&r);
  for (uint32_t i = 0; i < sizeof r; ++i) raw[i] = 0;
  r.version = 1;
  fillChannel(r.channel, b.freq, b.channel);
  r.noiseKnown = 1;
  r.noise = -95;
  r.rssi = int16_t(b.rssi);
  r.snr = int16_t(b.rssi > -95 ? b.rssi + 95 : 0);
  r.beaconInterval = b.beaconInterval ? b.beaconInterval : 100;
  for (unsigned i = 0; i < 6; ++i) r.bssid[i] = b.bssid[i];
  const uint32_t rates = b.rateCount < 15 ? b.rateCount : 15;
  r.rateCount = uint8_t(rates);
  for (uint32_t i = 0; i < rates; ++i) r.rates[i] = uint32_t(b.rates[i] & 0x7f) / 2;   // Mb/s (the 5.5 rate reads 5)
  r.ssidLength = b.ssidLength <= kSsidMax ? b.ssidLength : kSsidMax;
  for (uint32_t i = 0; i < r.ssidLength; ++i) r.ssid[i] = b.ssid[i];
  r.ageMs = 0;
  bool hasRsn = false;
  r.elementsLength = uint16_t(buildElements(b, r.elements, sizeof r.elements, &hasRsn));
  // Legacy scan entries omit capabilities. Infer ESS and privacy from the RSN.
  r.capability = uint16_t(b.capability ? b.capability : (0x1 | (hasRsn ? 0x10 : 0)));
}

// ----------------------------------------------------------------------------------------------------------- the handler
// The accept modes (machdep.piano_wifi_a80211, piano-wifi-apple80211-accept=N): what the requests airportd asks on the tablet,
// whose layouts we do not have, get. Each one switchable at run time, so one boot compares them (docs/drivers/wifi-menu.md 6).
enum : uint32_t {
  kAcceptInfraEnumerated = 0x1,   // INFRA_ENUMERATED (512) set: accepted (airportd's own value; its result was found ignored)
  kAcceptUnknownSets = 0x2,       // any other set without a handler: accepted (0), the value ignored
  kAcceptUnknownGets = 0x4,       // any other get without a handler: a zeroed buffer of the asked length (0 in req_val without one)
  kAcceptVirtualRole = 0x8,       // VIRTUAL_IF_ROLE (96) get: 0 in req_val (else refused: an answer may make the interface virtual)
};
enum : uint32_t {
  kGapChannelsInfo = 1u << 0,
  kGapCountryCode = 1u << 1,
  kGapAll = kGapChannelsInfo | kGapCountryCode,
};
constexpr uint32_t kGapDefault = kGapAll;
constexpr uint32_t kAcceptDefault = kAcceptInfraEnumerated;

// The UniXNU-sheng binding bits (`piano-wifi-sheng=MASK` at boot, `machdep.piano_wifi_sheng` at run time; each bit its own
// A/B, docs/drivers/wifi-menu.md section 9): what their macOS-27.3 en0 binding — hardware-verified on our exact OS build
// 26C5035r — does that ours does not (private/host-checks/20261115-unixnu-sheng/STUDY.md section 1). Their WifiNet.cpp /
// ShengEthernet.cpp are BSD-2-Clause-Patent prior art; nothing of their code is used here, the values and layouts are
// calibration data. The kext implements 0x1/0x2/0x8/0x10/0x20 around this header; 0x4 is answered here (CARD_CAPABILITIES).
enum : uint32_t {
  kShengMedium = 0x1,    // SIOCGIFMEDIA answers IEEE 802.11 auto ("the framework lists wireless interfaces by this media
                         // type"): the medium dictionary carries kIOMediumIEEE80211Auto and it is the selected medium
  kShengEvents = 0x2,    // retained boot-argument bit; link and identity events are unconditional
  kShengCaps46 = 0x4,    // CARD_CAPABILITIES bit 46: "the framework's scan waits for the scan-done event, up to 20 s, only
                         // with this bit; without it the results are read the moment the request returns"
  kShengUcKernel = 0x8,  // a user-client request's data rides inside the structure itself (a kernel buffer): byte replies
                         // are memcpy'd, not copyout'd (their macOS-27.3 EFAULT fix)
  kShengGrant = 0x10,    // NativeSandboxGrantProcess = "airportd" (their "airportd,sharingd,wifivelocityd" cannot match:
                         // our kernel's native_sandbox_grant compares the whole property with one process name)
  kShengMediaFilter = 0x20,  // SIOCGIFMEDIA answered in the interface filter outright (their non-macOS if_ioctl form), for
                             // the case the family's answer lags the selected medium
  kShengInterfaceBinding = 0x40, // interface primary/class/name/grant plus controller discovery identity A/B
  kShengInterfaceUc = 0x80,     // interface-side IOUserClientClass / open path
  kShengAssocState = 0x100,      // supplicant association state, request answers and transition kevs
  kShengInterfaceName = 0x200,   // publish the BSD name/unit and registry identity before InterfaceNamer waits for IOKit quiet
};
constexpr uint32_t kShengAll = kShengMedium | kShengEvents | kShengCaps46 | kShengUcKernel | kShengGrant | kShengMediaFilter |
                                kShengInterfaceBinding | kShengInterfaceUc | kShengAssocState | kShengInterfaceName;

// piano-wifi-ifnet2-old=1 selects the pre-D.1 synchronous/preassigned path for one A/B boot.
// The default (0, including an omitted argument) is D.1's asynchronous registration path.
constexpr uint32_t kIfnet2Old = 1u;
constexpr bool useIfnet2Old(uint32_t mode) { return (mode & kIfnet2Old) != 0; }

struct Association {
  bool associated = false;
  bool authorized = false;
  uint8_t ssid[kSsidMax] = {};
  uint8_t ssidLength = 0;
  uint8_t bssid[6] = {};
  uint16_t channel = 0;
  uint32_t frequency = 0;
  int32_t rssi = -95;
  int32_t noise = -95;
  uint32_t rate = 0;
  uint8_t authType = 1;
  uint8_t cipher = 0;
  uint8_t opMode = kOpModeStation;
  uint32_t result = 0;
  uint32_t generation = 0;
  uint32_t state = kStateInit;
};

inline Association associationFromSnapshot(const struct piano_wifi_assoc_snapshot &raw)
{
  Association next;
  next.associated = raw.associated != 0;
  next.authorized = raw.authorized != 0;
  next.ssidLength = raw.ssid_length <= kSsidMax ? raw.ssid_length : kSsidMax;
  for (unsigned i = 0; i < next.ssidLength; ++i) next.ssid[i] = raw.ssid[i];
  for (unsigned i = 0; i < sizeof next.bssid; ++i) next.bssid[i] = raw.bssid[i];
  next.channel = raw.channel;
  next.frequency = raw.frequency_mhz;
  next.rssi = raw.rssi_dbm;
  next.noise = raw.noise_dbm;
  next.rate = raw.rate_mbps;
  next.authType = raw.auth_type;
  next.cipher = raw.cipher;
  next.opMode = raw.op_mode ? raw.op_mode : kOpModeStation;
  next.result = raw.associate_result;
  next.generation = raw.join_generation;
  next.state = raw.state;
  return next;
}

constexpr unsigned kAssociateWaitMs = 8000, kAssociateWaitStepMs = 20;

inline const char *associateWaitOutcome(const struct piano_wifi_assoc_snapshot &raw, uint32_t generation, uint64_t elapsedNs)
{
  if (raw.join_generation != generation) return "superseded";
  if (!raw.power) return "power off";
  if (raw.authorized) return "authorized";
  if (raw.associate_result == 2) return "failed";
  if (raw.associate_result == 9) return "timed out";
  if (elapsedNs >= uint64_t(kAssociateWaitMs) * 1000000ull) return raw.associated ? "associated, not yet authorized" : "timeout";
  return nullptr;
}

// Event ordering shared by the kernel front end and its host contract.  The
// custom 802.1X path reports ASSOC_DONE as soon as 802.11 association is
// complete, while SSID/BSSID/LINK-up wait for the private port's AUTHORIZE.
// That lets airportd observe the 540-byte completion without advertising a
// DHCP-capable link before the WPA2 keys are installed.
struct AssociationTransition {
  bool assocDone = false;
  bool connected = false;
  bool disconnected = false;
};

inline AssociationTransition associationTransition(const Association &old, const Association &next)
{
  AssociationTransition t;
  bool bssidChanged = false, ssidChanged = old.ssidLength != next.ssidLength;
  for (unsigned i = 0; i < sizeof old.bssid; ++i) bssidChanged = bssidChanged || old.bssid[i] != next.bssid[i];
  for (unsigned i = 0; i < kSsidMax; ++i) ssidChanged = ssidChanged || old.ssid[i] != next.ssid[i];
  t.assocDone = next.associated && (!old.associated || bssidChanged || ssidChanged);
  t.connected = next.associated && next.authorized &&
                (!old.associated || !old.authorized || bssidChanged || ssidChanged);
  t.disconnected = !next.associated && (old.associated || old.authorized);
  return t;
}

// A new pending result0 is progress, not another link-down indication.
// Failure before association still needs a down notification; an old live
// association going away always does, independently of the result code.
inline bool associationLinkDownEvent(const Association &old, const Association &next)
{
  return associationTransition(old, next).disconnected ||
         (!next.associated && next.result != 0 && (old.result != next.result || old.generation != next.generation));
}

struct AssociationEvents {
  uint32_t codes[4] = {};
  unsigned count = 0;
};

inline AssociationEvents associationEvents(const Association &old, const Association &next, bool voluntary)
{
  AssociationEvents events;
  const auto transition = associationTransition(old, next);
  const bool down = associationLinkDownEvent(old, next);
  const bool completion = transition.assocDone ||
                          ((old.result != next.result || old.generation != next.generation) && next.result &&
                           (!down || (!voluntary && !old.authorized)));
  if (completion) events.codes[events.count++] = kEvAssocDone;
  if (transition.connected || down) {
    events.codes[events.count++] = kEvLinkChanged;
    events.codes[events.count++] = kEvSsidChanged;
    events.codes[events.count++] = kEvBssidChanged;
  }
  return events;
}

// The interface's apple80211 state (one per interface; the kext keeps it under its lock). No pointer in it: the kext keeps it in
// a data allocation.
struct State {
  bool power = true;              // requested power; the worker owns physical off/on
  Association association;        // current GET state
  Association eventAssociation;   // last notified state: GET polling never consumes an event
  uint16_t associationCapability = 0;
  uint8_t associationRsnLength = 0;
  uint8_t associationRsn[PIANO_WIFI_ASSOC_RSN_MAX] = {};
  bool eventPower = true;
  LinkEvent linkEvent = {};        // event4 and GET156 agree, including a voluntary leave
  uint32_t cursor = 0;            // the next SCAN_RESULT
  bool cacheCleared = false;      // SCANCACHE_CLEAR since the last SCAN_REQ: no result until the next scan
  ScanSnapshot scan;              // the scan list as of the last refresh
  uint8_t mac[6] = {};
  char country[3] = {'X', 'X', 0};   // XX until PianoWifi publishes the firmware regulatory alpha2
  uint32_t gaps = kGapDefault;       // A/B bits for this round's 207 and 51 responses
  uint32_t scanRequests = 0, resultsServed = 0, listEnds = 0, powerSets = 0;
  uint32_t accept = kAcceptDefault;   // kAccept*: how requests without a layout of ours are answered (a runtime switch, M1b)
  uint32_t acceptedUnknown = 0;   // requests answered only because of `accept`
  uint32_t sheng = 0;             // kSheng*: the UniXNU-sheng binding bits above (a runtime switch)
  uint64_t nowNs = 0;             // the request's time (mach_continuous_time in ns): the scan records' TIMESTAMP
};

// A successful SET19 must notify even when it repeats the boot-on state:
// airportd's event consumer initializes its cached power and scan clients.
// A GET observes state; it cannot substitute for this notification.
inline bool powerEventRequired(const State &s, bool power, bool acceptedSet = false)
{
  return acceptedSet || s.eventPower != power;
}

// Current GET state and last-notified state are separate: a GET arriving
// before the periodic callback cannot swallow a pending link/power event.
inline Association observeAssociation(State &s, const Association &next, bool power, bool emit)
{
  const Association old = s.eventAssociation;
  s.association = next;
  s.associationCapability = 0;
  s.associationRsnLength = 0;
  s.power = power;
  if (emit) { s.eventAssociation = next; s.eventPower = power; }
  return old;
}

inline Association observeAssociation(State &s, const struct piano_wifi_assoc_snapshot &raw, bool emit)
{
  const auto old = observeAssociation(s, associationFromSnapshot(raw), raw.power != 0, emit);
  s.associationCapability = raw.capability;
  s.associationRsnLength = raw.rsn_length <= sizeof raw.rsn ? raw.rsn_length : 0;
  for (unsigned i = 0; i < s.associationRsnLength; ++i) s.associationRsn[i] = raw.rsn[i];
  return old;
}

// A state as a fresh interface has it, in place (the kext's is a zeroed data allocation, too large for a temporary on the stack).
inline void reset(State &s)
{
  uint8_t *raw = reinterpret_cast<uint8_t *>(&s);
  for (uint32_t i = 0; i < sizeof s; ++i) raw[i] = 0;
  s.power = true;
  s.eventPower = true;
  s.country[0] = 'X';
  s.country[1] = 'X';
  s.gaps = kGapDefault;
  s.accept = kAcceptDefault;
  s.sheng = 0;
}

// The identity strings (the kext's; the host test passes its own).
struct Identity {
  const char *driverVersion;   // "PianoWifi 1.0 (apple80211 M1)"
  const char *hardwareVersion; // "WCN7860"
  const char *devName;         // "PianoWifi"
};

// What the kext does after the answer, outside its lock.
enum : uint32_t {
  kDoPowerChanged = 0x1,   // post kEvPowerChanged
  kDoScanStarted = 0x2,    // arm the scan's completion (kEvScanDone after the time a short scan takes)
  kDoDisassociate = 0x8,   // ask the radio worker to tear down and suppress retry
  kDoRefreshScan = 0x4,    // (asked before the handler runs for SCAN_REQ: the kext refreshes the snapshot first)
};

struct Answer {
  int error = 0;           // the ioctl's result (0 or an errno)
  uint32_t effects = 0;    // kDo*
  uint32_t wrote = 0;      // bytes copied out (the log line's "wrote")
  bool accepted = false;   // answered by an accept mode, not by a handler of ours
  bool associationProfile = false;  // SET 20 parsed into associationProfileData below
  bool powerOn = false;            // requested SET19 state, published only by the owner
  uint32_t associationProblem = kAssociateProblemNone;
  uint32_t associationLayout = 0;
  uint32_t associationHeader0 = 0;
  uint32_t associationHeader4 = 0;
  uint32_t associationHeader8 = 0;
  uint32_t associationHeaderC = 0;
  uint32_t associationCredentialLength = 0;
  uint32_t associationCredentialCipher = 0;
  bool associationHeaderValid = false;
  piano_wifi_profile associationProfileData = {};
};

// The user-space copies (copyin/copyout in the kext, memcpy in the host test); each returns 0 or an errno.
struct UserIo {
  void *ctx;
  int (*copyIn)(void *ctx, uint64_t from, void *to, uint32_t length);
  int (*copyOut)(void *ctx, const void *from, uint64_t to, uint32_t length);
};

// The scratch buffer the kext gives the handler (never on the stack). It holds
// the complete 27.3 scan record and the larger out-of-line CHANNELS_INFO reply.
constexpr uint32_t kScratchBytes = kChannelsInfoBytes;
static_assert(sizeof(ScanResult) <= kScratchBytes && sizeof(ChannelList) <= kScratchBytes && kVersionBytes <= kScratchBytes,
              "the scratch buffer holds every answer");

namespace detail {
inline void zero(uint8_t *p, uint32_t n)
{
  for (uint32_t i = 0; i < n; ++i) p[i] = 0;
}
inline uint32_t clip(uint32_t a, uint32_t b) { return a < b ? a : b; }
// The answer copied into the caller's buffer, as much as fits (sheng's copy_out): a request without a buffer is refused.
inline int copyOut(Req &rq, const UserIo &io, const void *src, uint32_t size, Answer &a)
{
  if (rq.length == 0 || !rq.data) return kErrInval;
  const uint32_t n = clip(rq.length, size);
  const int e = io.copyOut(io.ctx, src, rq.data, n);
  if (!e) a.wrote = n;
  return e ? kErrFault : 0;
}
// A single value: in req_val when no buffer is given, else {version 1, value} (sheng's answer_value).
inline int value(Req &rq, const UserIo &io, uint32_t v, Answer &a)
{
  if (rq.length == 0 || !rq.data) {
    rq.value = int32_t(v);
    return 0;
  }
  const uint32_t w[2] = {1, v};
  return copyOut(rq, io, w, sizeof w, a);
}
// Raw bytes, their own length put in req_len (sheng's answer_bytes).
inline int bytes(Req &rq, const UserIo &io, const void *src, uint32_t size, Answer &a)
{
  if (!rq.data) return kErrInval;
  const uint32_t n = clip(rq.length, size);
  rq.length = size;
  const int e = n ? io.copyOut(io.ctx, src, rq.data, n) : 0;
  if (!e) a.wrote = n;
  return e ? kErrFault : 0;
}
// A zeroed answer of the asked length (kAcceptUnknownGets), in scratch-sized pieces; 0 in req_val when no buffer is given.
inline int zeros(Req &rq, const UserIo &io, uint8_t *scratch, uint32_t scratchBytes, Answer &a)
{
  if (rq.length == 0 || !rq.data) {
    rq.value = 0;
    return 0;
  }
  zero(scratch, scratchBytes);
  const uint32_t total = clip(rq.length, 64 * 1024);
  for (uint32_t at = 0; at < total;) {
    const uint32_t n = clip(total - at, scratchBytes);
    if (io.copyOut(io.ctx, scratch, rq.data + at, n)) return kErrFault;
    at += n;
  }
  a.wrote = total;
  rq.length = total;   // what was written, as bytes() does (review F8: a request over 64 KiB is clipped)
  return 0;
}
inline int copyIn(const Req &rq, const UserIo &io, void *dst, uint32_t size)
{
  zero(static_cast<uint8_t *>(dst), size);
  if (!rq.data || !rq.length) return kErrInval;
  return io.copyIn(io.ctx, rq.data, dst, clip(rq.length, size)) ? kErrFault : 0;
}
inline uint32_t strLen(const char *s, uint32_t max)
{
  uint32_t n = 0;
  while (s && n < max && s[n]) ++n;
  return n;
}

inline void associateProblem(Answer &a, uint32_t problem, uint32_t layout)
{
  a.associationProblem = problem;
  a.associationLayout = layout;
  a.error = kErrInval;
}

// Convert airportd's association request into the profile consumed by level 7 and the
// 802.1X client.  Cipher 10 is the passphrase form; cipher 6 is the 32-byte PMK form
// and is represented as the profile's accepted 64 hexadecimal characters.  No input
// bytes are logged; the caller reports only the refusal class or length metadata.
inline bool parseAssociate(const Req &rq, const UserIo &io, Answer &a)
{
  const uint32_t layout = rq.length;
  if (layout != sizeof(AssociateRequestLegacy) && layout != sizeof(AssociateRequest27)) {
    associateProblem(a, kAssociateProblemLayout, layout);
    return false;
  }
  union {
    AssociateRequestLegacy legacy;
    AssociateRequest27 modern;
  } wire = {};
  if (copyIn(rq, io, &wire, sizeof wire)) {
    associateProblem(a, kAssociateProblemLayout, layout);
    return false;
  }
  const uint8_t *raw = reinterpret_cast<const uint8_t *>(&wire);
  auto le32 = [&](uint32_t at) {
    return uint32_t(raw[at]) | uint32_t(raw[at + 1]) << 8 | uint32_t(raw[at + 2]) << 16 |
           uint32_t(raw[at + 3]) << 24;
  };
  a.associationHeader0 = le32(0);
  a.associationHeader4 = le32(4);
  a.associationHeader8 = le32(8);
  a.associationHeaderC = le32(0x0c);
  a.associationCredentialLength = le32(0x40);
  a.associationCredentialCipher = le32(0x44);
  a.associationHeaderValid = true;
  const uint32_t version = layout == sizeof(AssociateRequest27) ? wire.modern.version : wire.legacy.version;
  const AssociateParameters *network = layout == sizeof(AssociateRequest27) ? &wire.modern.network : &wire.legacy.network;
  // Associate2 emits two version-bearing words in the modern record: the record
  // version at 0x00 and the authentication selector at 0x08 (both are one in
  // the 27.3 request).  Keep the legacy check exact, while accepting a modern
  // request whose wrapper preserved the selector word but did not preserve the
  // first word; the rest of the fixed offsets are unchanged.
  const bool modernVersion = layout == sizeof(AssociateRequest27) &&
                             (version == 1 || (version == 0 && a.associationHeader8 == 1));
  if (!modernVersion && version != 1) {
    associateProblem(a, kAssociateProblemVersion, layout);
    zero(reinterpret_cast<uint8_t *>(&wire), sizeof wire);
    return false;
  }
  // Associate2's selector is open-system (1); the upper word is the upper
  // authentication bit field of a keyed request: WPA_PSK 0x02, WPA2_PSK 0x08
  // (the value a real 27.3 menu join sends: wifi-join8, "h0c 00000008"),
  // and the 0x10 / 0x40 encodings of the host fixtures.  Reject a keyed
  // request without one of them and an open request with any.
  const uint32_t selector = layout == sizeof(AssociateRequest27) ? a.associationHeader8 : uint32_t(wire.legacy.auth_lower);
  const uint32_t flags = layout == sizeof(AssociateRequest27) ? a.associationHeaderC : uint32_t(wire.legacy.auth_upper);
  const bool wpa2Flags = flags == 0x02 || flags == 0x08 || flags == 0x10 || flags == 0x40;
  if (selector != 1 || (network->key_length ? !wpa2Flags : flags != 0)) {
    associateProblem(a, kAssociateProblemAuth, layout);
    zero(reinterpret_cast<uint8_t *>(&wire), sizeof wire);
    return false;
  }
  if (network->ssid_length < 1 || network->ssid_length > kSsidMax) {
    associateProblem(a, kAssociateProblemSsid, layout);
    zero(reinterpret_cast<uint8_t *>(&wire), sizeof wire);
    return false;
  }
  if (network->key_length > sizeof network->key) {
    associateProblem(a, kAssociateProblemKeyLength, layout);
    zero(reinterpret_cast<uint8_t *>(&wire), sizeof wire);
    return false;
  }
  if (network->key_length && !((network->key_cipher == 6 && network->key_length == 32) ||
                               (network->key_cipher == 10 && network->key_length >= 8 && network->key_length <= 63))) {
    associateProblem(a, kAssociateProblemCipher, layout);
    zero(reinterpret_cast<uint8_t *>(&wire), sizeof wire);
    return false;
  }
  piano_wifi_profile &p = a.associationProfileData;
  p.magic = PIANO_WIFI_PROFILE_MAGIC;
  p.version = PIANO_WIFI_PROFILE_VERSION;
  p.security = network->key_length ? PianoWifiSecWpa2Psk : PianoWifiSecOpen;
  p.ssid_len = uint8_t(network->ssid_length);
  for (uint32_t i = 0; i < network->ssid_length; ++i) p.ssid[i] = network->ssid[i];
  bool bssid = false;
  for (unsigned i = 0; i < sizeof p.bssid; ++i) {
    p.bssid[i] = network->bssid[i];
    bssid = bssid || network->bssid[i] != 0;
  }
  if (bssid) p.flags |= PianoWifiProfileBssid;
  if (network->key_cipher == 6 && network->key_length == 32) {
    static const char hex[] = "0123456789abcdef";
    p.psk_len = 64;
    for (uint32_t i = 0; i < 32; ++i) {
      p.psk[2 * i] = uint8_t(hex[network->key[i] >> 4]);
      p.psk[2 * i + 1] = uint8_t(hex[network->key[i] & 0xf]);
    }
  } else if (network->key_length) {
    p.psk_len = uint8_t(network->key_length);
    for (uint32_t i = 0; i < network->key_length; ++i) p.psk[i] = network->key[i];
  }
  const char *problem = piano_wifi_profile_problem(&p, sizeof p);
  if (problem) {
    associateProblem(a, kAssociateProblemProfile, layout);
    piano_wifi_profile_wipe(&p);
    zero(reinterpret_cast<uint8_t *>(&wire), sizeof wire);
    return false;
  }
  a.associationProfile = true;
  a.associationProblem = kAssociateProblemNone;
  a.associationLayout = layout;
  zero(reinterpret_cast<uint8_t *>(&wire), sizeof wire);
  return true;
}
}  // namespace detail

// The channels this station scans (level 6's list: WifiLink.h, 2.4 GHz 1-13 and the 5 GHz non-radar ones).
inline uint32_t fillChannelList(ChannelList &out)
{
  detail::zero(reinterpret_cast<uint8_t *>(&out), sizeof out);
  out.version = 1;
  for (uint16_t f : kChannelFreqs) {
    fillChannel(out.list[out.count], f, 0);
    out.list[out.count].flags |= kChanActive;
    ++out.count;
  }
  return 8 + 12 * out.count;
}

// The 27.3 CHANNELS_INFO parser reads only the prefix selected by count. Keep
// every reserved byte zero and advertise the same 22 active 20-MHz channels as
// the supported-channel answer; no DFS/passive/wide-channel facts are claimed.
inline uint32_t fillChannelsInfo(uint8_t *out, uint32_t capacity)
{
  if (!out || capacity < kChannelsInfoBytes) return 0;
  detail::zero(out, kChannelsInfoBytes);
  const uint16_t count = uint16_t(kChannelCount);
  out[kChannelsInfoCountAt] = uint8_t(count);
  out[kChannelsInfoCountAt + 1] = uint8_t(count >> 8);
  for (uint32_t i = 0; i < kChannelCount; ++i) {
    const uint8_t channel = uint8_t(channelOfFreq(kChannelFreqs[i]));
    out[kChannelsInfoChannelAt + i * kChannelsInfoStride] = channel;
    const uint32_t bitmap = i < 13 ? (kChan20MHz | kChan2GHz | kChanActive) : (kChan20MHz | kChan5GHz | kChanActive);
    for (unsigned byte = 0; byte < sizeof bitmap; ++byte)
      out[kChannelsInfoBitmapAt + i * sizeof(uint32_t) + byte] = uint8_t(bitmap >> (8 * byte));
  }
  return kChannelsInfoBytes;
}

// One request. `scratch` holds kScratchBytes. The kext refreshes s.scan first for a SCAN_REQ / SCAN_REQ_MULTIPLE set.
inline Answer handle(State &s, const Identity &id, bool set, Req &rq, const UserIo &io, uint8_t *scratch)
{
  using namespace detail;
  Answer a;
  zero(scratch, kScratchBytes);
  uint32_t *w = reinterpret_cast<uint32_t *>(scratch);
  if (set) {
    switch (rq.type) {
    case kPower: {
      Power p;
      if (rq.length < sizeof p) { a.error = kErrInval; return a; }
      if ((a.error = copyIn(rq, io, &p, sizeof p))) return a;
      if (p.version != 1 || p.radios != 1 || p.state[0] > 1) { a.error = kErrInval; return a; }
      a.powerOn = p.state[0] != 0;
      ++s.powerSets;
      a.effects |= kDoPowerChanged;
      return a;
    }
    case kScanReq:
    case kScanReqMultiple: {
      if (!s.power) {
        a.error = kErrPowerOff;
        return a;
      }
      // The SSID of a directed scan is read for the log only: the answer is the whole list (the radio's own scans hear them all).
      s.cursor = 0;
      s.cacheCleared = false;
      ++s.scanRequests;
      a.effects |= kDoScanStarted;
      return a;
    }
    case kScanCacheClear:
      s.scan.count = 0;
      s.cursor = 0;
      s.cacheCleared = true;
      return a;
    case kCountryCode: {
      // Firmware owns the regulatory domain. Never acknowledge an unperformed
      // change. SET51 is version1 + alpha2/NUL/pad (8 bytes), unlike GET51.
      uint8_t country[8];
      if (rq.length != sizeof country) { a.error = kErrInval; return a; }
      if ((a.error = copyIn(rq, io, country, sizeof country))) return a;
      if (country[0] != 1 || country[1] || country[2] || country[3] || !validCountryCode(country[4], country[5]) || country[6]) {
        a.error = kErrInval;
        return a;
      }
      if (country[4] != uint8_t(s.country[0]) || country[5] != uint8_t(s.country[1])) a.error = kErrUnsupported;
      return a;
    }
    case kLocale:
    case kPowerSave:
      return a;   // accepted, nothing to change in M1
    case kAssociate:
      if (!s.power) { a.error = kErrPowerOff; return a; }
      // Parse the complete request before acknowledging it.  The front end hands the
      // resulting profile to PianoWifi's level-7 machine after this lock is released.
      a.associationProfile = false;
      a.associationProblem = kAssociateProblemNone;
      a.associationLayout = rq.length;
      (void)parseAssociate(rq, io, a);
      if (a.associationProfile) {
        a.error = 0;
      }
      return a;
    case kDisassociate:
    case kDeauth:
      // Disassociate accepts no data or the extended {version, reason, flush}
      // record. Reason is an Apple policy value, not an IEEE reason to put on air.
      if (rq.length) {
        uint32_t leave[4];
        const uint32_t bytes = rq.type == kDeauth ? 16u : 12u;
        if (rq.length < bytes) { a.error = kErrInval; return a; }
        if ((a.error = copyIn(rq, io, leave, bytes))) return a;
        if (leave[0] != 1) { a.error = kErrInval; return a; }
        if (rq.type == kDisassociate && leave[2]) {
          s.scan.count = s.cursor = 0;
          s.cacheCleared = true;
        }
      }
      a.effects |= kDoDisassociate;
      return a;
    case kCipherKey:
    case kAuthType:
    case kRsnIe:
      // The native supplicant owns the join.  airportd may still issue these
      // legacy commands while binding; accept them as an idempotent request
      // and report the real link through the following gets/events.
      a.error = 0;
      return a;
    case kInfraEnumerated:
      if (s.accept & kAcceptInfraEnumerated) {
        a.accepted = true;
        ++s.acceptedUnknown;
        return a;
      }
      a.error = kErrUnsupported;
      return a;
    default:
      if (s.accept & kAcceptUnknownSets) {
        a.accepted = true;
        ++s.acceptedUnknown;
        return a;
      }
      a.error = kErrUnsupported;
      return a;
    }
  }
  switch (rq.type) {
  case kDriverVersion:
  case kHardwareVersion:
  case kStatusDevName: {   // a NUL-terminated string from offset 0, the rest of the buffer zeros
    const char *text = rq.type == kDriverVersion ? id.driverVersion : rq.type == kHardwareVersion ? id.hardwareVersion : id.devName;
    const uint32_t n = strLen(text, kVersionBytes - 1);
    for (uint32_t i = 0; i < n; ++i) scratch[i] = uint8_t(text[i]);
    a.error = copyOut(rq, io, scratch, kVersionBytes, a);
    return a;
  }
  case kCardCapabilities: {
    for (uint8_t b : kCapBits) scratch[b / 8] |= uint8_t(1u << (b % 8));
    // sheng bit 0x4: their bitmap adds 46 — "the end of a scan is reported by the scan-done event. The framework's scan
    // (IO80211 26C5035r, Apple80211Scan) waits for that event, up to 20 s, only with this bit; without it the results are
    // read the moment the request returns, before the radio scanned" (WifiNet.cpp:646-651). Our scan-done is posted after
    // kScanDoneMs whether or not the bit is set; the bit only changes what the framework waits for.
    if (s.sheng & kShengCaps46) scratch[46 / 8] |= uint8_t(1u << (46 % 8));
    a.error = bytes(rq, io, scratch, kCapBytes, a);
    return a;
  }
  case kPower: {
    const Power p = powerRecord(s.power);
    a.error = copyOut(rq, io, &p, sizeof p, a);
    return a;
  }
  case kLinkChangedEventData:
    a.error = copyOut(rq, io, &s.linkEvent, sizeof s.linkEvent, a);
    return a;
  case kState:
    a.error = value(rq, io, s.association.state, a);
    return a;
  case kOpMode:
    a.error = value(rq, io, s.association.opMode, a);
    return a;
  case kPhyMode: {
    PhyMode *p = reinterpret_cast<PhyMode *>(scratch);
    p->version = 1;
    p->supported = kPhySupported;
    p->active = s.association.associated ? kPhy11n : 0;
    a.error = copyOut(rq, io, p, sizeof *p, a);
    return a;
  }
  case kNoise: {
    Rssi *r = reinterpret_cast<Rssi *>(scratch);
    r->version = 1;
    r->radios = 1;
    for (int i = 0; i < 4; ++i) r->chain[i] = r->extChain[i] = s.association.noise;
    r->aggregate = r->extAggregate = s.association.noise;
    a.error = copyOut(rq, io, r, sizeof *r, a);
    return a;
  }
  case kRssi:
    if (!s.association.associated) { a.error = kErrNotAssociated; return a; }
    { Rssi *r = reinterpret_cast<Rssi *>(scratch); r->version = 1; r->radios = 1; r->unit = 0;
      for (int i = 0; i < 4; ++i) r->chain[i] = r->extChain[i] = s.association.rssi;
      r->aggregate = r->extAggregate = s.association.rssi;
      a.error = copyOut(rq, io, r, sizeof *r, a); }
    return a;
  case kChannel:
    if (!s.association.associated) { a.error = kErrNotAssociated; return a; }
    if (rq.length == 0 || !rq.data) a.error = value(rq, io, s.association.channel, a);
    else { ChannelAnswer *c = reinterpret_cast<ChannelAnswer *>(scratch); c->version = 1;
      fillChannel(c->channel, s.association.frequency, s.association.channel);
      a.error = copyOut(rq, io, c, sizeof *c, a); }
    return a;
  case kRate:
    a.error = s.association.associated ? value(rq, io, s.association.rate, a) : kErrNotAssociated;
    return a;
  case kSsid:
    if (!s.association.associated) { a.error = kErrNotAssociated; return a; }
    a.error = bytes(rq, io, s.association.ssid, s.association.ssidLength, a);
    return a;
  case kBssid:
    a.error = s.association.associated ? bytes(rq, io, s.association.bssid, sizeof s.association.bssid, a) : kErrNotAssociated;
    return a;
  case kCurrentNetwork:
    if (!s.association.associated) { a.error = kErrNotAssociated; return a; }
    { Bss b = {};
      for (uint32_t i = 0; i < s.scan.count; ++i) {
        const Bss &candidate = s.scan.bss[i];
        bool same = true;
        for (unsigned j = 0; j < 6; ++j) same = same && candidate.bssid[j] == s.association.bssid[j];
        if (same) { b = candidate; break; }
      }
      for (unsigned i = 0; i < 6; ++i) b.bssid[i] = s.association.bssid[i];
      b.ssidLength = s.association.ssidLength; for (unsigned i = 0; i < b.ssidLength; ++i) b.ssid[i] = s.association.ssid[i];
      b.channel = s.association.channel; b.freq = s.association.frequency; b.rssi = s.association.rssi;
      // The selected network can come from a newer scan than the front end's
      // snapshot, or the user can clear that snapshot during association.
      if (s.associationCapability) b.capability = s.associationCapability;
      if (s.associationRsnLength && s.associationRsnLength <= sizeof b.rsn) {
        b.rsnLength = s.associationRsnLength;
        for (unsigned i = 0; i < b.rsnLength; ++i) b.rsn[i] = s.associationRsn[i];
      }
      if (s.association.authType == 2) b.capability |= kDot11CapEss | kDot11CapPrivacy;
      if (s.association.authType == 1) {
        b.capability = uint16_t((b.capability | 0x1) & ~0x10u);
        b.rsnLength = 0;
      }
      if (!b.rateCount) {
        b.rateCount = 1;
        b.rates[0] = uint8_t(s.association.rate > 63 ? 126 : s.association.rate * 2);
      }
      ScanResult *r = reinterpret_cast<ScanResult *>(scratch); fillScanResult(*r, b);
      stampScanResult(scratch, s.nowNs);
      a.error = copyOut(rq, io, r, resultBytes(*r), a); }
    return a;
  case kAuthType:
    w[0] = 1; w[1] = 1; w[2] = s.association.authType == 2 ? 0x10 : 0; w[3] = 0;
    a.error = copyOut(rq, io, w, 16, a);
    return a;
  case kRsnIe:
    a.error = s.association.associated ? kErrUnsupported : kErrNotAssociated;
    return a;
  case kAssociateResult:
    a.error = value(rq, io, s.association.result, a);
    return a;
  case kVirtualIfRole:   // an answer may make airportd take the interface for a virtual one: refused unless kAcceptVirtualRole
    if (s.accept & kAcceptVirtualRole) {
      a.accepted = true;
      ++s.acceptedUnknown;
      a.error = value(rq, io, 0, a);
      return a;
    }
    a.error = kErrUnsupported;
    return a;
  case kCountryCode:
    if (!(s.gaps & kGapCountryCode)) {
      a.error = kErrUnsupported;
      return a;
    }
    a.error = bytes(rq, io, s.country, 3, a);
    return a;
  case kChannelsInfo:
    if (!(s.gaps & kGapChannelsInfo)) {
      a.error = kErrUnsupported;
      return a;
    }
    a.error = copyOut(rq, io, scratch, fillChannelsInfo(scratch, kScratchBytes), a);
    return a;
  case kSupportedChannels:
  case kHwSupportedChannels: {
    ChannelList *l = reinterpret_cast<ChannelList *>(scratch);
    const uint32_t n = fillChannelList(*l);
    a.error = copyOut(rq, io, l, n, a);
    return a;
  }
  case kScanResult: {
    if (!s.power) {
      a.error = kErrPowerOff;
      return a;
    }
    if (s.cacheCleared || s.cursor >= s.scan.count) {
      s.cursor = 0;
      ++s.listEnds;
      a.error = kErrEndOfResults;
      return a;
    }
    ScanResult *r = reinterpret_cast<ScanResult *>(scratch);
    fillScanResult(*r, s.scan.bss[s.cursor]);
    stampScanResult(scratch, s.nowNs);
    a.error = copyOut(rq, io, r, resultBytes(*r), a);
    if (a.error) return a;   // the cursor stays where it is: a faulted record is asked for again (review 2026-11-14 F9)
    ++s.cursor;
    ++s.resultsServed;
    return a;
  }
  default:
    if (s.accept & kAcceptUnknownGets) {
      a.accepted = true;
      ++s.acceptedUnknown;
      a.error = zeros(rq, io, scratch, kScratchBytes, a);
      return a;
    }
    a.error = kErrUnsupported;
    return a;
  }
}

// The SSID length a SCAN_REQ names (0: all networks) and its scan type (0xffffffff: not read — a short buffer or a
// SCAN_REQ_MULTIPLE, which has no type field), both for the log; reads the request's buffer through `io`.
inline void scanReqInfo(const Req &rq, const UserIo &io, uint32_t &ssidLength, uint32_t &scanType)
{
  ssidLength = 0;
  scanType = 0xffffffffu;
  uint8_t head[kScanReqTypeAt + 4];
  if (rq.length < kScanReqSsidAt || detail::copyIn(rq, io, head, sizeof head)) return;
  auto word = [&head](uint32_t at) {
    return uint32_t(head[at]) | uint32_t(head[at + 1]) << 8 | uint32_t(head[at + 2]) << 16 | uint32_t(head[at + 3]) << 24;
  };
  ssidLength = word(kScanReqSsidLengthAt);
  if (ssidLength > kSsidMax) ssidLength = 0;
  if (rq.length >= kScanReqTypeAt + 4) scanType = word(kScanReqTypeAt);
}

// The user client's request data may ride inside the structure the client passed — kernel memory the kext holds — instead of
// at a user address; their macOS-27.3 finding is that copyout of it faults (their EFAULT fix: memcpy). True, and the offset
// within [base, base+bytes), when the whole of [at, at+n) lies there; the checks are overflow-safe (an at below base or an
// end above bytes cannot wrap into a match).
inline bool shengKernelBuffer(uint64_t at, uint32_t n, const void *base, uint32_t bytes, uint32_t &offset)
{
  // A null/empty descriptor is not a kernel buffer.  Do not turn (nullptr, 0)
  // into a successful match that would call memcpy with a null destination.
  if (!base || !bytes) return false;
  const uint64_t b = reinterpret_cast<uintptr_t>(base);
  if (at < b) return false;
  const uint64_t into = at - b;
  if (into > bytes || n > bytes - into) return false;
  offset = uint32_t(into);
  return true;
}

// ------------------------------------------------------------------------------------------- the log: one line per kind
// A request's kind for the log: its number, direction, length and result. The kext logs each kind once (and counts the rest).
struct Seen {
  static constexpr uint32_t kMax = 192;
  uint64_t keys[kMax] = {};
  uint32_t count = 0, overflow = 0;
  void clear()
  {
    for (uint32_t i = 0; i < kMax; ++i) keys[i] = 0;
    count = overflow = 0;
  }
  // true the first time a kind is seen (and remembered); false after (and when the table is full: counted in overflow)
  bool first(int32_t type, bool set, uint32_t length, int error)
  {
    // The length is folded, not cut (review F10: two requests of the same kind whose lengths differ above 16 bits were one).
    const uint32_t folded = (length ^ (length >> 16)) & 0xffffu;
    const uint64_t key = uint64_t(uint32_t(type)) << 32 | uint64_t(set ? 1u : 0u) << 31 | uint64_t(folded) << 8 |
                         uint64_t(uint32_t(error) & 0xffu);
    for (uint32_t i = 0; i < count; ++i)
      if (keys[i] == key) return false;
    if (count == kMax) {
      ++overflow;
      return false;
    }
    keys[count++] = key;
    return true;
  }
};

}  // namespace a80211
}  // namespace pianowifi
