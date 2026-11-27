// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the apple80211 requests (WifiApple80211.h, docs/drivers/wifi-menu.md section 5): the layouts' sizes and offsets,
// a model of user space (a buffer at a fake user address, copyin/copyout by memcpy, a fault for an address outside it) asking
// what airportd's bind and the menu ask, and the answers read back field by field: the identity, the power off/on and what it
// stops, the channel list, a scan and its results until the end, the cache clear, the not-associated answers, the unknown
// request, the scan list's address parse and the rebuilt elements, and the log's one-line-per-kind table.
//   c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. apple80211_host.cpp -o /tmp/apple80211_host && /tmp/apple80211_host
#include "WifiApple80211.h"
#include "PianoWifiUcTransport.h"
#include "WifiUserControl.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pianowifi;
using namespace pianowifi::a80211;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// User space: one buffer at a made-up address.
struct User {
  static constexpr uint64_t kBase = 0x16f000000ull;
  std::vector<uint8_t> mem = std::vector<uint8_t>(8192, 0xa5);
  bool inside(uint64_t at, uint32_t n) const { return at >= kBase && at + n <= kBase + mem.size(); }
};
static int userIn(void *ctx, uint64_t from, void *to, uint32_t n)
{
  User &u = *static_cast<User *>(ctx);
  if (!u.inside(from, n)) return 14;
  std::memcpy(to, u.mem.data() + (from - User::kBase), n);
  return 0;
}
static int userOut(void *ctx, const void *from, uint64_t to, uint32_t n)
{
  User &u = *static_cast<User *>(ctx);
  if (!u.inside(to, n)) return 14;
  std::memcpy(u.mem.data() + (to - User::kBase), from, n);
  return 0;
}

// The IOUserClient SET path owns the staged descriptor in kernel memory.  Its
// copy pair deliberately uses the real address of that allocation, matching
// PianoWifi80211::ucIn's memcpy fallback rather than the fake user address.
struct KernelBuffer {
  std::vector<uint8_t> mem = std::vector<uint8_t>(0x3c30, 0);
  uint64_t base() const { return reinterpret_cast<uint64_t>(mem.data()); }
  bool inside(uint64_t at, uint32_t n) const { return at >= base() && at - base() <= mem.size() && uint64_t(n) <= mem.size() - (at - base()); }
};
static int kernelIn(void *ctx, uint64_t from, void *to, uint32_t n)
{
  KernelBuffer &b = *static_cast<KernelBuffer *>(ctx);
  if (!b.inside(from, n)) return 14;
  std::memcpy(to, b.mem.data() + (from - b.base()), n);
  return 0;
}
static int kernelOut(void *ctx, const void *from, uint64_t to, uint32_t n)
{
  KernelBuffer &b = *static_cast<KernelBuffer *>(ctx);
  if (!b.inside(to, n)) return 14;
  std::memcpy(b.mem.data() + (to - b.base()), from, n);
  return 0;
}

static const Identity kId = {"PianoWifi 1.0 (apple80211 M1)", "WCN7860", "PianoWifi"};

struct Rig {
  User user;
  UserIo io{&user, userIn, userOut};
  State st;
  std::vector<uint8_t> scratch = std::vector<uint8_t>(kScratchBytes);
  Req rq{};
  Answer ask(bool set, int32_t type, uint32_t length, bool buffer = true)
  {
    std::memset(&rq, 0, sizeof rq);
    std::strcpy(rq.name, "en2");
    rq.type = type;
    rq.length = length;
    rq.data = buffer ? User::kBase : 0;
    return handle(st, kId, set, rq, io, scratch.data());
  }
  uint32_t u32(uint32_t at) const
  {
    uint32_t v;
    std::memcpy(&v, user.mem.data() + at, 4);
    return v;
  }
  const uint8_t *at(uint32_t off) const { return user.mem.data() + off; }
  void put16(uint32_t off, uint16_t v) { std::memcpy(user.mem.data() + off, &v, 2); }
  void put32(uint32_t off, uint32_t v) { std::memcpy(user.mem.data() + off, &v, 4); }
  uint32_t wpaFlags = 0x08;   // the upper-auth word of a real 27.3 menu join (wifi-join8 console: "h0c 00000008")
  Answer associate(uint32_t layout = sizeof(AssociateRequest27), uint32_t cipher = 10, uint32_t keyLength = 13)
  {
    std::memset(user.mem.data(), 0, layout);
    const uint32_t network = layout == sizeof(AssociateRequest27) ? 0x10 : 0x0c;
    put32(0, 1);                         // Associate2 record version
    if (layout == sizeof(AssociateRequest27)) {
      put16(4, 2);                       // mode
      put32(8, 1);                       // authentication selector/version word
      put32(0x0c, wpaFlags);             // WPA2/RSN flags for this fixture (0x08 = the captured 27.3 menu join, wifi-join8)
    } else {
      put16(4, 2);                       // legacy mode
      put16(6, 1);                       // open-system selector
      put16(8, 0x10);                    // WPA2-Personal flag
    }
    put32(network, 4);                   // SSID length (the bytes are test-only)
    std::memcpy(user.mem.data() + network + 4, "test", 4);
    const uint8_t bssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::memcpy(user.mem.data() + network + 0x24, bssid, sizeof bssid);
    if (keyLength) {
      put32(network + 0x2c, 1);          // credential version
      put32(network + 0x30, keyLength);
      put32(network + 0x34, cipher);
      for (uint32_t i = 0; i < keyLength; ++i) user.mem[network + 0x3c + i] = uint8_t('a' + (i % 20));
    }
    return ask(true, kAssociate, layout);
  }
};

// Replay the request/effect/owner/notification boundary, rather than merely
// checking handle's effect bit. A synchronous OFF completes the modeled
// physical park before it returns and requests the accepted-SET event.
struct PowerRig : Rig {
  WifiUserControl owner;
  WifiBootPower bootGuard;
  PowerRig() { bootGuard.enabled = false; } // existing unrestricted-lifecycle replays
  unsigned powerEvents = 0, downEvents = 0;
  void publish(bool acceptedPowerSet = false) {
    const bool on = owner.reportedPower(owner.read());
    const bool notify = powerEventRequired(st, on, acceptedPowerSet);
    Association next = st.association;
    if (!owner.allowed(owner.read())) { next = {}; next.result = 2; }
    const Association old = observeAssociation(st, next, on, true);
    if (notify) ++powerEvents;
    if (associationLinkDownEvent(old, next)) {
      ++downEvents;
      st.linkEvent = {};
      st.linkEvent.down = 1; // explicit local leave: involuntary/reason remain zero
    }
  }
  Answer control(int32_t type, uint32_t length) {
    Answer a = ask(true, type, length);
    if (!a.error && (a.effects & (kDoPowerChanged | kDoDisassociate))) {
      if (a.effects & kDoDisassociate) owner.disconnect();
      else if (!bootGuard.setPower(owner, a.powerOn)) a.error = EBUSY;
      else if (!a.powerOn) owner.radioOff(owner.read());
      publish(!a.error && (a.effects & kDoPowerChanged) != 0);
    }
    return a;
  }
  void setPowerRecord(bool on) {
    std::memset(user.mem.data(), 0, sizeof(Power));
    put32(0, 1); put32(4, 1); put32(8, on ? 1 : 0);
  }
  bool getPower() {
    const Answer a = ask(false, kPower, sizeof(Power));
    CHECK(!a.error && a.wrote == 24 && u32(0) == 1 && u32(4) == 1);
    CHECK(u32(12) == 0 && u32(16) == 0 && u32(20) == 0);
    // Apple80211GetPower: any nonzero state among the returned radios is ON.
    bool on = false;
    for (uint32_t i = 0; i < u32(4) && i < 4; ++i) on |= u32(8 + 4 * i) != 0;
    return on;
  }
};

static void testPowerAndAvailabilityEvents()
{
  CHECK(kEvPowerChanged == 1 && kEvDriverAvailable == 55);
  Rig r;
  for (bool on : {false, true}) {
    const Power power = powerRecord(on);
    const DriverAvailability availability = driverAvailabilityRecord(on);
    const uint8_t powerBytes[24] = {1, 0, 0, 0, 1, 0, 0, 0, uint8_t(on)};
    const uint8_t availabilityBytes[248] = {0, 0, 0, 0, 0, 0, 0, 0, uint8_t(on)};
    const LinkEvent link = {};
    const auto event1 = bsdEventPayload(1, link, true, &power, &availability);
    const auto event55 = bsdEventPayload(55, link, true, &power, &availability);
    CHECK(event1.data == &power && event1.size == sizeof powerBytes);
    CHECK(event55.data == &availability && event55.size == sizeof availabilityBytes);
    CHECK(std::memcmp(&power, powerBytes, sizeof powerBytes) == 0);
    CHECK(std::memcmp(&availability, availabilityBytes, sizeof availabilityBytes) == 0);
    CHECK(kBsdEventHeaderSize + kBsdEventNameSize + event1.size == 64);
    CHECK(kBsdEventHeaderSize + kBsdEventNameSize + event55.size == kBsdEventMaxSize);
    const auto missing1 = bsdEventPayload(1, link, true, nullptr, &availability);
    const auto missing55 = bsdEventPayload(55, link, true, &power);
    CHECK(!missing1.data && !missing1.size && !missing55.data && !missing55.size);

    observeAssociation(r.st, Association{}, on, false);
    for (uint32_t length : {1u, 8u, 9u, 23u, 24u, 25u}) {
      std::memset(r.user.mem.data(), 0xa5, 26);
      const Answer get = r.ask(false, kPower, length);
      const uint32_t copied = length < sizeof powerBytes ? length : sizeof powerBytes;
      CHECK(!get.error && get.wrote == copied && std::memcmp(r.at(0), powerBytes, copied) == 0);
      for (uint32_t i = copied; i < 26; ++i) CHECK(*r.at(i) == 0xa5);
    }
    CHECK(r.ask(false, kPower, 0).error == kErrInval);
    CHECK(r.ask(false, kPower, 24, false).error == kErrInval);
    std::memcpy(r.user.mem.data(), powerBytes, sizeof powerBytes);
    const unsigned sets = r.st.powerSets;
    CHECK(r.ask(true, kPower, 23).error == kErrInval);
    CHECK(r.ask(true, kPower, 24, false).error == kErrInval);
    r.put32(4, 2);
    CHECK(r.ask(true, kPower, 24).error == kErrInval);
    r.put32(4, 1);
    r.put32(8, 2);
    CHECK(r.ask(true, kPower, 24).error == kErrInval);
    CHECK(r.st.powerSets == sets);
  }

  WifiUserControl owner;
  State state;
  bool adopted = false, firmwareReady = false;
  int posted = -1;
  std::vector<bool> powerEvents, availabilityEvents;
  auto sample = [&](bool emit = true, bool acceptedSet = false, bool forceAvailable = false) {
    const bool power = owner.reportedPower(owner.read());
    const bool notify = powerEventRequired(state, power, acceptedSet);
    observeAssociation(state, Association{}, power, emit);
    if (!emit) return;
    if (notify) powerEvents.push_back(power);
    const bool usable = firmwareReady && !owner.radioStop();
    if (driverAvailabilityEventRequired(adopted, posted, usable, forceAvailable)) {
      posted = int(usable);
      availabilityEvents.push_back(usable);
    }
  };
  sample(true, false, true);
  CHECK(availabilityEvents.empty()); // no airportd adopter
  adopted = true;
  sample(true, false, true);
  sample();
  CHECK(availabilityEvents == std::vector<bool>{false});
  firmwareReady = true;
  sample();
  sample(true, true, true); // repeated SET19 ON and adoption by a new daemon
  CHECK((availabilityEvents == std::vector<bool>{false, true, true}));
  CHECK(powerEvents == std::vector<bool>{true});

  for (unsigned cycle = 0; cycle < 3; ++cycle) {
    const auto powerCount = powerEvents.size();
    const auto availabilityCount = availabilityEvents.size();
    const uint32_t off = owner.setPower(false);
    sample();
    CHECK(state.power && powerEvents.size() == powerCount);
    CHECK(availabilityEvents.size() == availabilityCount + 1 && !availabilityEvents.back());
    firmwareReady = false;
    owner.radioOff(off);
    sample(false);
    sample(false);
    CHECK(!state.power && state.eventPower && powerEvents.size() == powerCount);
    sample(true, true);
    CHECK(powerEvents.size() == powerCount + 1 && !powerEvents.back());
    sample();
    CHECK(powerEvents.size() == powerCount + 1 && availabilityEvents.size() == availabilityCount + 1);

    owner.setPower(true);
    sample(true, true);
    CHECK(powerEvents.size() == powerCount + 2 && powerEvents.back());
    CHECK(availabilityEvents.size() == availabilityCount + 1);
    firmwareReady = true;
    sample();
    sample();
    CHECK(availabilityEvents.size() == availabilityCount + 2 && availabilityEvents.back());
  }
}

static Bss network(uint8_t last, const char *name, uint32_t freq, int32_t rssi, bool rsn)
{
  Bss b;
  const uint8_t addr[6] = {0x3c, 0x52, 0xa1, 0x10, 0x20, last};
  std::memcpy(b.bssid, addr, 6);
  b.ssidLength = uint8_t(std::strlen(name));
  std::memcpy(b.ssid, name, b.ssidLength);
  b.freq = freq;
  b.channel = uint16_t(channelOfFreq(freq));
  b.rssi = rssi;
  const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c};
  std::memcpy(b.rates, rates, sizeof rates);
  b.rateCount = sizeof rates;
  if (rsn) {   // RSN: version 1, group CCMP, one pairwise CCMP, one AKM PSK, capabilities 0
    const uint8_t e[] = {48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0};
    std::memcpy(b.rsn, e, sizeof e);
    b.rsnLength = sizeof e;
  }
  return b;
}

int main()
{
  testPowerAndAvailabilityEvents();
  // Replay validated SET19 -> production boot guard -> owner -> coherent GET
  // and accepted-SET event. Early OFF must not create even a pending cycle.
  for (bool explicitArm : {false, true}) {
    PowerRig r;
    r.bootGuard.enabled = true;
    CHECK(!r.bootGuard.isArmed() && r.getPower());
    const uint32_t initial = r.owner.read();
    for (unsigned daemon = 0; daemon < 3; ++daemon) {
      r.setPowerRecord(false);
      CHECK(r.control(kPower, sizeof(Power)).error == EBUSY);
      CHECK(r.getPower() && r.owner.read() == initial && !r.owner.radioStop());
      CHECK(r.powerEvents == daemon);
      r.setPowerRecord(true);
      CHECK(!r.control(kPower, sizeof(Power)).error && !r.bootGuard.isArmed());
    }
    CHECK(r.powerEvents == 3 && r.downEvents == 0);
    r.setPowerRecord(false);
    r.put32(0, 2); // invalid payload cannot mutate/arm the guard
    CHECK(r.control(kPower, sizeof(Power)).error == kErrInval);
    CHECK(!r.bootGuard.isArmed() && r.owner.read() == initial && r.powerEvents == 3);
    // Automatic SET20 cannot prove user power intent. Joining must leave the
    // guard in place; only an explicit root write arms real OFF.
    if (!explicitArm) CHECK(!r.associate().error && r.owner.join());
    CHECK(!r.bootGuard.isArmed());
    r.setPowerRecord(false);
    CHECK(r.control(kPower, sizeof(Power)).error == EBUSY && r.getPower() && !r.owner.radioStop());
    r.bootGuard.arm();
    CHECK(r.bootGuard.isArmed());
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
      r.setPowerRecord(false);
      CHECK(!r.control(kPower, sizeof(Power)).error && !r.getPower());
      CHECK(r.owner.radioStop());
      r.owner.radioOff(r.owner.read());
      r.setPowerRecord(true);
      CHECK(!r.control(kPower, sizeof(Power)).error && r.getPower());
      CHECK(r.owner.allowed(r.owner.read()) && r.bootGuard.isArmed());
    }
  }

  // D.1 keeps one explicit boot-argument A/B: omitted/zero is the new asynchronous path;
  // one selects the legacy preassigned/synchronous path.
  CHECK(!useIfnet2Old(0));
  CHECK(!useIfnet2Old(2));
  CHECK(useIfnet2Old(kIfnet2Old));

  // ASSOC_DONE may precede WPA2 authorization. The public link/SSID/BSSID
  // transition waits for authorization. A disconnect emits one down transition.
  {
    Association old = {}, pending = {}, open = {};
    pending.associated = true;
    auto t = associationTransition(old, pending);
    CHECK(t.assocDone && !t.connected && !t.disconnected);
    auto events = associationEvents(old, pending, false);
    CHECK(events.count == 1 && events.codes[0] == 9);
    open = pending;
    open.authorized = true;
    open.result = 1;
    t = associationTransition(pending, open);
    CHECK(!t.assocDone && t.connected && !t.disconnected);
    events = associationEvents(pending, open, false);
    CHECK(events.count == 4 && events.codes[0] == 9 && events.codes[1] == 4 &&
          events.codes[2] == 2 && events.codes[3] == 3);
    t = associationTransition(open, Association{});
    CHECK(!t.assocDone && !t.connected && t.disconnected);
    Association failed;
    failed.result = 2;
    events = associationEvents(open, failed, true);
    CHECK(events.count == 3 && events.codes[0] == 4 && events.codes[1] == 2 && events.codes[2] == 3);
    events = associationEvents(old, failed, false);
    CHECK(events.count == 4 && events.codes[0] == 9 && events.codes[1] == 4 &&
          events.codes[2] == 2 && events.codes[3] == 3);
    Association failedAgain = failed;
    failedAgain.generation = 2;
    events = associationEvents(failed, failedAgain, false);
    CHECK(events.count == 4 && events.codes[0] == 9); // another generation can fail between polls
    CHECK(associationEvents(open, open, false).count == 0);
    events = associationEvents(old, open, false);
    CHECK(events.count == 4 && events.codes[0] == 9 && events.codes[1] == 4 &&
          events.codes[2] == 2 && events.codes[3] == 3);
    events = associationEvents(open, failed, false);
    CHECK(events.count == 3 && events.codes[0] == 4 && events.codes[1] == 2 && events.codes[2] == 3);
  }

  // Post-size checks include both BSD vectors and the 24-byte kernel header.
  {
    CHECK(kBsdEventHeaderSize == 24 && kBsdEventNameSize == 16 && kBsdEventMaxSize == 288);
    CHECK(kEventClass == 6 && kEventSubclass == 1 && kEvPowerChanged == 1);
    LinkEvent link = {};
    link.down = 1;
    link.rssi = -65;
    link.snr = 0x1234;
    link.noise = 0x5678;
    link.involuntary = 1;
    link.reason = 0x11223344;
    link.subreason = 0x55667788;
    const uint8_t linkBytes[] = {
      1, 0, 0, 0, 0xbf, 0xff, 0xff, 0xff, 0x34, 0x12, 0x78, 0x56, 0, 0, 0, 0,
      1, 0, 0, 0, 0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55, 0, 0, 0, 0,
    };
    const uint8_t scanBytes[] = {1, 0, 0, 0, 0, 0, 0, 0};
    for (bool scanStatus : {false, true}) {
      for (const uint32_t code : {kEvPowerChanged, kEvSsidChanged, kEvBssidChanged, kEvLinkChanged,
                                  kEvAssocDone, kEvScanDone, kEvScanCacheUpdated}) {
        Power power = {};
        power.version = 1; power.radios = 1; power.state[0] = 1;
        const auto payload = bsdEventPayload(code, link, scanStatus, code == kEvPowerChanged ? &power : nullptr);
        CHECK(kBsdEventHeaderSize + kBsdEventNameSize + payload.size <= kBsdEventMaxSize);
        if (code == kEvPowerChanged) {
          const uint8_t powerBytes[] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
          CHECK(payload.size == sizeof powerBytes && std::memcmp(payload.data, powerBytes, sizeof powerBytes) == 0);
          CHECK(bsdEventPayload(code, link, scanStatus).size == 0);
        } else if (code == kEvLinkChanged) {
          CHECK(payload.size == sizeof linkBytes && std::memcmp(payload.data, linkBytes, sizeof linkBytes) == 0);
        } else if (code == kEvScanDone && scanStatus) {
          CHECK(payload.size == sizeof scanBytes && std::memcmp(payload.data, scanBytes, sizeof scanBytes) == 0);
        } else {
          CHECK(payload.size == 0 && payload.data == nullptr);
        }
      }
    }
  }

  // ---- the layouts: the request and the answers' fixed sizes and offsets
  CHECK(sizeof(Req) == 40);
  CHECK(sizeof(ScanResult) == 1164 && kScanResultBytes27 == 2280 && offsetof(ScanResult, elements) == 0x8c &&
        offsetof(ScanResult, elementsLength) == 0x8a);
  CHECK(offsetof(ScanResult, channel) == 4 && offsetof(ScanResult, ssid) == 0x61 && offsetof(ScanResult, capability) == 0x1a);
  CHECK(sizeof(Power) == 24 && sizeof(Rssi) == 52 && sizeof(LinkEvent) == 32 && sizeof(Channel) == 12);
  CHECK(kEvScanDone == 10 && kEvScanCacheUpdated == 15);
  CHECK(kChannelsInfoBytes == 0x2720 && kChannelsInfoCountAt == 0x0c && kChannelsInfoChannelAt == 0x32e &&
        kChannelsInfoBitmapAt == 0x12d0 && kChannelCount == 22 && kGapDefault == kGapAll);
  CHECK(kSiocSA80211 == 0x802869c8UL && kSiocGA80211 == 0xc02869c9UL);
  // _IOW('i', 200, 40 bytes) and _IOWR('i', 201, 40 bytes)
  CHECK(((kSiocSA80211 >> 16) & 0x1fff) == 40 && (kSiocSA80211 & 0xff) == 200 && ((kSiocSA80211 >> 8) & 0xff) == 'i');
  CHECK(((kSiocGA80211 >> 16) & 0x1fff) == 40 && (kSiocGA80211 & 0xff) == 201 && (kSiocGA80211 >> 30) == 3);
  CHECK(std::strcmp(requestName(kHeCapability), "HE_CAPABILITY") == 0 && kHeCapability == 570);
  CHECK(std::strcmp(requestName(9999), "?") == 0);

  // ---- the bind's three: DRIVER_VERSION with a 0x180-byte buffer, CARD_CAPABILITIES, STATUS_DEV_NAME
  {
    Rig r;
    // 27.3: a 384-byte buffer, a NUL-terminated string from offset 0, zeros after it
    Answer a = r.ask(false, kDriverVersion, 0x180);
    CHECK(a.error == 0 && a.wrote == 0x180 && std::strcmp(reinterpret_cast<const char *>(r.at(0)), kId.driverVersion) == 0);
    CHECK(r.at(0x17f)[0] == 0 && r.at(0x180)[0] == 0xa5);   // nothing beyond the buffer
    a = r.ask(false, kHardwareVersion, 0x180);
    CHECK(a.error == 0 && std::strcmp(reinterpret_cast<const char *>(r.at(0)), "WCN7860") == 0);
    // the bind's only sanity request on the ioctl path: CARD_CAPABILITIES with 23 bytes, a raw bitmap from offset 0
    std::memset(r.user.mem.data(), 0xa5, 64);
    a = r.ask(false, kCardCapabilities, 23);
    CHECK(a.error == 0 && a.wrote == 23 && r.rq.length == 23 && r.at(23)[0] == 0xa5);
    for (uint8_t b : kCapBits) CHECK(r.at(0)[b / 8] & (1u << (b % 8)));
    CHECK(r.at(0)[kCapTkip / 8] & (1u << (kCapTkip % 8)));
    CHECK(r.at(0)[kCapWpa / 8] & (1u << (kCapWpa % 8)));
    CHECK(r.at(0)[kCapWpa2 / 8] & (1u << (kCapWpa2 % 8)));
    CHECK((r.at(0)[0] & 0x1) == 0);                          // no WEP
    CHECK((r.at(0)[46 / 8] & (1u << (46 % 8))) == 0);        // no wait for a scan-done event (sheng bit 0x4 adds it)
    CHECK((r.at(0)[73 / 8] & (1u << (73 % 8))) == 0);        // directed scans as SCAN_REQ
    // sheng bit 0x4: their bitmap adds 46 — "the framework's scan waits for the scan-done event only with this bit"
    r.st.sheng = kShengCaps46;
    std::memset(r.user.mem.data(), 0xa5, 64);
    a = r.ask(false, kCardCapabilities, 23);
    CHECK(a.error == 0 && (r.at(0)[46 / 8] & (1u << (46 % 8))) != 0);
    CHECK((r.at(0)[45 / 8] & (1u << (45 % 8))) == 0 && (r.at(0)[47 / 8] & (1u << (47 % 8))) == 0);   // bit 46 alone
    r.st.sheng = kShengCaps46 | kShengMedium;   // no other bit leaks into the bitmap
    std::memset(r.user.mem.data(), 0xa5, 64);
    a = r.ask(false, kCardCapabilities, 23);
    CHECK((r.at(0)[45 / 8] & (1u << (45 % 8))) == 0 && (r.at(0)[73 / 8] & (1u << (73 % 8))) == 0);
    a = r.ask(false, kStatusDevName, 1028);
    CHECK(a.error == 0 && std::strcmp(reinterpret_cast<const char *>(r.at(0)), "PianoWifi") == 0);
    // a get without a buffer is refused (the bind always gives one)
    a = r.ask(false, kDriverVersion, 0x180, false);
    CHECK(a.error == kErrInval);
    // a buffer outside user space: the copy's fault
    r.rq = {};
    r.rq.type = kDriverVersion;
    r.rq.length = 0x180;
    r.rq.data = 0x1000;
    a = handle(r.st, kId, false, r.rq, r.io, r.scratch.data());
    CHECK(a.error == kErrFault);
  }

  // ---- a mixed WPA2 network: the scan record carries the AP's RSN IE byte-for-byte
  // (group TKIP, pairwise CCMP, PSK).  Associate2 reads this group value as 2;
  // CARD_CAPABILITIES bit 1 above is what lets that path continue to SET 20.
  {
    Bss b = network(9, "mixed", 5180, -42, true);
    b.rsn[7] = 2;
    ScanResult result = {};
    fillScanResult(result, b);
    const uint8_t want[] = {48, 20, 1, 0, 0x00, 0x0f, 0xac, 2, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0,
                            0x00, 0x0f, 0xac, 2, 0, 0};
    CHECK(result.elementsLength >= sizeof want);
    bool found = false;
    for (uint32_t at = 0; at + sizeof want <= result.elementsLength; ++at) {
      if (std::memcmp(result.elements + at, want, sizeof want) == 0) { found = true; break; }
    }
    CHECK(found);
  }

  // Raw IOC numbers and the exact Apple startup/on/off/on sequence. SET22
  // and SET29 are leave operations, never aliases for SET19 power.
  {
    PowerRig r;
    CHECK(kPower == 19 && kDisassociate == 22 && kDeauth == 29);
    CHECK(r.getPower() && r.powerEvents == 0);
    for (unsigned bootSet = 0; bootSet < 2; ++bootSet) {
      r.setPowerRecord(true);
      CHECK(!r.control(19, 24).error);
      CHECK(r.getPower() && r.powerEvents == bootSet + 1);
    }
    r.publish(); r.publish(); // timer/polls cannot duplicate the SET notification
    CHECK(r.powerEvents == 2);
    r.setPowerRecord(false);
    CHECK(!r.control(19, 24).error && !r.getPower() && r.owner.radioStop());
    CHECK(r.powerEvents == 3);
    r.setPowerRecord(true);
    CHECK(!r.control(19, 24).error && r.getPower() && r.owner.allowed(r.owner.read()));
    CHECK(r.powerEvents == 4);
    r.put32(0, 2); // a refused SET cannot generate an acknowledgement
    CHECK(r.control(19, 24).error == kErrInval && r.getPower() && r.powerEvents == 4);
    for (int32_t leave : {22, 29}) {
      CHECK(r.owner.join());
      Association live{}; live.associated = live.authorized = true; live.result = 1;
      observeAssociation(r.st, live, true, true);
      std::memset(r.user.mem.data(), 0, 16); r.put32(0, 1); r.put32(4, 3);
      const unsigned downBefore = r.downEvents;
      CHECK(!r.control(leave, leave == 29 ? 16 : 12).error);
      CHECK(!r.st.association.associated && !r.st.association.authorized);
      CHECK(r.downEvents == downBefore + 1 && r.st.linkEvent.down == 1 && !r.st.linkEvent.involuntary);
      CHECK(r.getPower() && r.powerEvents == 4);
      CHECK(!r.ask(false, kLinkChangedEventData, 32).error && r.user.mem[0] == 1 && r.u32(16) == 0);
      r.owner.stopped(r.owner.read());
    }
  }

  // Replay the decoded airportd attachment policy (event9, flags bit0,
  // optional global PowerEnabled). The policy model is ours; the request
  // parser, power event decisions and serial drain below are production code.
  // This catches a tempting "ignore first OFF" workaround as well as a lost
  // ON during an unfinished physical stop.
  {
    auto attachPower = [](bool adminUp, int preference) {
      return adminUp && (preference < 0 || preference != 0);
    };
    for (int preference : {-1, 0, 1}) {
      PowerRig oldBoot;
      CHECK(!attachPower(false, preference));
      // The old DOWN publication selects OFF regardless of initial GET=ON.
      CHECK(oldBoot.getPower());
      for (unsigned daemon = 0; daemon < 2; ++daemon) {
        oldBoot.setPowerRecord(attachPower(false, preference));
        CHECK(!oldBoot.control(kPower, sizeof(Power)).error);
      }
      oldBoot.publish();
      CHECK(!oldBoot.getPower() && !oldBoot.owner.allowed(oldBoot.owner.read()));
      CHECK(oldBoot.powerEvents == 2);

      PowerRig fixedBoot;
      for (unsigned daemon = 0; daemon < 2; ++daemon) {
        fixedBoot.setPowerRecord(attachPower(true, preference));
        CHECK(!fixedBoot.control(kPower, sizeof(Power)).error);
      }
      fixedBoot.publish();
      CHECK(fixedBoot.getPower() == (preference != 0));
      CHECK(fixedBoot.owner.allowed(fixedBoot.owner.read()) == (preference != 0));
      CHECK(fixedBoot.powerEvents == 2);
    }
    for (bool finishOffBeforeOn : {false, true}) {
      PowerRig r;
      r.setPowerRecord(false);
      const Answer request = r.ask(true, kPower, sizeof(Power));
      CHECK(!request.error && (request.effects & kDoPowerChanged) && !request.powerOn);
      const uint32_t off = r.owner.setPower(request.powerOn);
      r.publish(); // OFF is still waiting for physical confirmation
      CHECK(r.getPower() && r.powerEvents == 0);
      if (finishOffBeforeOn) {
        r.owner.radioOff(off);
        r.publish(true);
        CHECK(!r.getPower());
      }
      r.setPowerRecord(true);
      CHECK(!r.control(kPower, sizeof(Power)).error && r.getPower());
      if (!finishOffBeforeOn) {
        CHECK(r.owner.radioStop() && !r.owner.allowed(r.owner.read()));
        r.owner.radioOff(off);
        r.publish(true); // the delayed OFF returns while the current power is ON
      }
      r.publish();
      CHECK(r.getPower() && r.owner.allowed(r.owner.read()) && !r.owner.radioStop());
      CHECK(r.powerEvents == 2);
    }
    // A single persisted OFF must survive polls and worker completion forever;
    // administrative UP is an initialization default, not a periodic override.
    PowerRig persistedOff;
    persistedOff.setPowerRecord(false);
    CHECK(!persistedOff.control(kPower, sizeof(Power)).error);
    for (unsigned poll = 0; poll < 100; ++poll) {
      persistedOff.publish();
      CHECK(!persistedOff.getPower());
      CHECK(!persistedOff.owner.join());
      CHECK(persistedOff.ask(true, kScanReq, 0).error == kErrPowerOff);
    }
    CHECK(persistedOff.powerEvents == 1);
    std::puts("boot power replay: DOWN/OFF twice, UP/ON twice, persisted OFF and drained OFF->ON passed");
  }

  // ---- power: on by default; off answers EPWROFF to scans, on again restores them; each set asks for the event
  {
    Rig r;
    Answer a = r.ask(false, kPower, 24);
    CHECK(a.error == 0 && r.u32(0) == 1 && r.u32(4) == 1 && r.u32(8) == 1);
    std::memset(r.user.mem.data(), 0, 24);
    r.put32(0, 1);
    r.put32(4, 1);
    r.put32(8, 0);
    a = r.ask(true, kPower, 24);
    CHECK(a.error == 0 && (a.effects & kDoPowerChanged) && !a.powerOn && r.st.power && r.st.powerSets == 1);
    observeAssociation(r.st, Association{}, a.powerOn, true);
    a = r.ask(false, kPower, 24);
    CHECK(r.u32(8) == 0);
    a = r.ask(true, kScanReq, 0x80);
    CHECK(a.error == kErrPowerOff && !(a.effects & kDoScanStarted));
    a = r.ask(false, kScanResult, sizeof(ScanResult));
    CHECK(a.error == kErrPowerOff);
    CHECK(r.ask(false, kState, 0, false).error == 0 && r.rq.value == int32_t(kStateInit));
    r.put32(8, 1);
    a = r.ask(true, kPower, 24);
    CHECK(a.error == 0 && a.powerOn && !r.st.power && (a.effects & kDoPowerChanged));
    observeAssociation(r.st, Association{}, a.powerOn, true);
    a = r.ask(true, kScanReq, 0x80);
    CHECK(a.error == 0 && (a.effects & kDoScanStarted));
    // a short buffer: what fits of the answer
    a = r.ask(false, kPower, 12);
    CHECK(a.error == 0 && a.wrote == 12);
  }

  // Invalid power records and joins while off must not change the radio or
  // hand credentials to the worker. Both leave APIs carry a real worker effect.
  {
    Rig r;
    r.put32(0, 1); r.put32(4, 1); r.put32(8, 0);
    CHECK(r.ask(true, kPower, 24).error == 0);
    observeAssociation(r.st, Association{}, false, true);
    CHECK(r.associate().error == kErrPowerOff);
    CHECK(!r.st.power);
    r.put32(0, 2); r.put32(8, 1);
    CHECK(r.ask(true, kPower, 24).error == kErrInval && !r.st.power);
    CHECK(r.ask(true, kDisassociate, 0, false).effects & kDoDisassociate);
    r.put32(0, 1); r.put32(4, 3); r.put32(8, 1);
    CHECK(r.ask(true, kDisassociate, 12).effects & kDoDisassociate);
    CHECK(r.ask(true, kDeauth, 16).effects & kDoDisassociate);
    CHECK(r.ask(true, kDisassociate, 4).error == kErrInval);
    r.st.linkEvent.down = 1; r.st.linkEvent.reason = 3;
    CHECK(r.ask(false, kLinkChangedEventData, 32).wrote == 32);
    CHECK(r.user.mem[0] == 1 && r.user.mem[0x10] == 0 && r.u32(0x14) == 3);
  }

  // ---- the idle station: state, mode, PHY, noise, and the not-associated answers
  {
    Rig r;
    CHECK(r.ask(false, kState, 0, false).error == 0 && r.rq.value == int32_t(kStateInit));
    CHECK(r.ask(false, kState, 8).error == 0 && r.u32(0) == 1 && r.u32(4) == kStateInit);
    CHECK(r.ask(false, kOpMode, 0, false).error == 0 && r.rq.value == int32_t(kOpModeStation));
    CHECK(r.ask(false, kPhyMode, 12).error == 0 && r.u32(4) == kPhySupported && r.u32(8) == 0);
    CHECK(r.ask(false, kNoise, 52).error == 0 && int32_t(r.u32(12)) == -95 && int32_t(r.u32(28)) == -95);
    for (int32_t t : {1, 9, 4, 16, 103})
      CHECK(r.ask(false, t, 1164).error == 2);
    for (int32_t t : {96, 97, 115, 469, 490, 575}) {
      CHECK(r.ask(false, t, 16).error == 102);
      CHECK(r.ask(true, t, 16).error == 102);
    }
    CHECK(r.ask(false, kAssociateResult, 0, false).error == 0 && r.rq.value == 0);
    Answer a = r.ask(false, kCountryCode, 3);
    CHECK(a.error == 0 && std::memcmp(r.at(0), "XX", 3) == 0 && r.rq.length == 3);
    r.user.mem.resize(kChannelsInfoBytes + 64, 0xa5);
    a = r.ask(false, kChannelsInfo, kChannelsInfoBytes);
    CHECK(a.error == 0 && a.wrote == kChannelsInfoBytes && r.u32(kChannelsInfoCountAt) == 22);
    CHECK(r.at(kChannelsInfoChannelAt)[0] == 1 && r.at(kChannelsInfoChannelAt + 21 * kChannelsInfoStride)[0] == 165);
    CHECK(r.u32(kChannelsInfoBitmapAt) == (kChan20MHz | kChan2GHz | kChanActive));
    CHECK(r.u32(kChannelsInfoBitmapAt + 13 * sizeof(uint32_t)) == (kChan20MHz | kChan5GHz | kChanActive));
    r.st.gaps = kGapCountryCode;
    CHECK(r.ask(false, kChannelsInfo, kChannelsInfoBytes).error == kErrUnsupported);
    r.st.gaps = kGapChannelsInfo;
    CHECK(r.ask(false, kCountryCode, 3).error == kErrUnsupported);
    // unknown requests, both ways
    CHECK(r.ask(false, kInfraEnumerated, 16).error == kErrUnsupported);
    CHECK(r.ask(false, kVirtualIfRole, 0, false).error == kErrUnsupported);   // the infrastructure interface is not a virtual one
    Answer e = r.ask(true, kInfraEnumerated, 1);                               // accepted by default (kAcceptInfraEnumerated)
    CHECK(e.error == 0 && e.accepted && r.st.acceptedUnknown == 1);
    CHECK(r.associate().error == 0);
    CHECK(r.ask(true, kCipherKey, 148).error == 0);
    CHECK(r.ask(false, 9999, 16).error == kErrUnsupported);
    // Country SET has a version word; an unsupported change must not succeed.
    std::memset(r.user.mem.data(), 0, 8);
    r.put32(0, 1);
    r.user.mem[4] = 'X'; r.user.mem[5] = 'X';
    CHECK(r.ask(true, kCountryCode, 8).error == 0);
    CHECK(r.ask(true, kCountryCode, 3).error == kErrInval);
    r.user.mem[4] = 'U'; r.user.mem[5] = 'S';
    CHECK(r.ask(true, kCountryCode, 8).error == kErrUnsupported);
    CHECK(r.st.country[0] == 'X' && r.st.country[1] == 'X');
    r.user.mem[6] = 'A';
    CHECK(r.ask(true, kCountryCode, 8).error == kErrInval);
    r.user.mem[6] = 0; r.put32(0, 2);
    CHECK(r.ask(true, kCountryCode, 8).error == kErrInval);
    CHECK(r.ask(true, kCountryCode, 8, false).error == kErrInval);
    r.st.gaps = kGapAll;
    const char alpha2[] = {'D', 'E'};
    setRegulatoryCountry(r.st.country, alpha2, sizeof alpha2);
    CHECK(r.ask(false, kCountryCode, 3).error == 0 && std::memcmp(r.at(0), "DE", 3) == 0);
    r.put32(0, 1);
    std::memcpy(r.user.mem.data() + 4, "DE", 3);
    CHECK(r.ask(true, kCountryCode, 8).error == 0);
    r.user.mem[4] = 'd';
    CHECK(r.ask(true, kCountryCode, 8).error == kErrInval);
    r.user.mem[4] = 0;
    CHECK(r.ask(true, kCountryCode, 8).error == kErrInval);
    struct Country { const char *text; uint32_t length; };
    for (Country malformed : {Country{nullptr, 2}, {"", 0}, {"D", 1}, {"DEU", 3}, {"dE", 2}, {"0E", 2}, {"D\0", 2}}) {
      setRegulatoryCountry(r.st.country, alpha2, sizeof alpha2);
      setRegulatoryCountry(r.st.country, malformed.text, malformed.length);
      CHECK(r.ask(false, kCountryCode, 3).error == 0 && std::memcmp(r.at(0), "XX", 3) == 0);
    }
    CHECK(r.ask(true, kPowerSave, 8).error == 0);
  }

  // ---- SET 20: both wire layouts become a level-7 profile; malformed requests are refused with a reason
  {
    Rig r;
    Answer a = r.associate(sizeof(AssociateRequest27), 10, 13);
    CHECK(a.error == 0 && a.associationProfile && a.associationLayout == 656);
    CHECK(a.associationProfileData.security == PianoWifiSecWpa2Psk && a.associationProfileData.ssid_len == 4 &&
          a.associationProfileData.psk_len == 13 && std::memcmp(a.associationProfileData.ssid, "test", 4) == 0);
    CHECK(a.associationHeaderValid && a.associationHeader0 == 1 && a.associationHeader8 == 1 &&
          a.associationHeaderC == 0x08 && a.associationCredentialLength == 13 && a.associationCredentialCipher == 10);
    for (uint32_t f : {0x02u, 0x08u, 0x10u, 0x40u}) { r.wpaFlags = f; CHECK(r.associate().error == 0); }
    r.wpaFlags = 0x00; CHECK(r.associate().error != 0);   // a keyed request without an upper-auth bit is refused
    r.wpaFlags = 0x08;
    a = r.associate(sizeof(AssociateRequestLegacy), 10, 8);
    CHECK(a.error == 0 && a.associationProfile && a.associationLayout == 592 && a.associationProfileData.psk_len == 8);
    a = r.associate(sizeof(AssociateRequest27), 6, 32);
    CHECK(a.error == 0 && a.associationProfile && a.associationProfileData.psk_len == 64 &&
          a.associationProfileData.psk[0] == '6' && a.associationProfileData.psk[1] == '1');
    a = r.associate(sizeof(AssociateRequest27), 7, 8);
    CHECK(a.error == kErrInval && !a.associationProfile && a.associationProblem == kAssociateProblemCipher);
    a = r.associate(sizeof(AssociateRequest27), 10, 13);
    r.put32(8, 2);                       // a selector other than open-system
    a = r.ask(true, kAssociate, sizeof(AssociateRequest27));
    CHECK(a.error == kErrInval && a.associationProblem == kAssociateProblemAuth);
    a = r.associate(sizeof(AssociateRequest27), 10, 13);
    r.put32(0x0c, 0x80);                 // WPA flag without the WPA2 selector
    a = r.ask(true, kAssociate, sizeof(AssociateRequest27));
    CHECK(a.error == kErrInval && a.associationProblem == kAssociateProblemAuth);
    a = r.ask(true, kAssociate, 600);
    CHECK(a.error == kErrInval && a.associationProblem == kAssociateProblemLayout);
    std::memset(r.user.mem.data(), 0, 656);
    r.put32(0, 2);
    a = r.ask(true, kAssociate, 656);
    CHECK(a.error == kErrInval && a.associationProblem == kAssociateProblemVersion);
    a = r.associate(sizeof(AssociateRequest27), 10, 13);
    r.put32(0, 0);                       // wrapper variant: selector word at 0x08 remains one
    a = r.ask(true, kAssociate, sizeof(AssociateRequest27));
    CHECK(a.error == 0 && a.associationProfile && a.associationProfileData.psk_len == 13);

    // Fetch provenance: ioctl uses the caller's user pointer directly, while
    // selector 0 supplies the same request inline at input+0x08 in a kernel
    // buffer.  The latter must not interpret its first eight payload bytes as
    // a second pointer (the tablet's PAC-looking h00/h04 came from that bug).
    (void)r.associate(sizeof(AssociateRequest27), 10, 13);
    KernelBuffer kb;
    constexpr uint32_t ucDataAt = 8;
    std::memcpy(kb.mem.data() + ucDataAt, r.user.mem.data(), sizeof(AssociateRequest27));
    uint64_t inlineData = 0;
    CHECK(ucInlineSetPayload(kb.mem.data(), uint32_t(kb.mem.size()), ucDataAt, sizeof(AssociateRequest27), inlineData));
    Req uc = {};
    std::strcpy(uc.name, "en2");
    uc.type = kAssociate;
    uc.length = sizeof(AssociateRequest27);
    uc.data = inlineData;
    UserIo kernelIo{&kb, kernelIn, kernelOut};
    std::vector<uint8_t> kernelScratch(kScratchBytes);
    Answer fromKernel = handle(r.st, kId, true, uc, kernelIo, kernelScratch.data());
    CHECK(fromKernel.error == 0 && fromKernel.associationProfile && fromKernel.associationLayout == sizeof(AssociateRequest27) &&
          fromKernel.associationHeader0 == 1 && fromKernel.associationCredentialLength == 13 &&
          fromKernel.associationCredentialCipher == 10);
  }

  // ---- the accept modes: unknown sets accepted, unknown gets zeroed at their own length (in pieces), VIRTUAL_IF_ROLE answered
  // ---- an established supplicant link: the state requests expose the same BSS and telemetry airportd uses
  {
    Rig r;
    r.st.association.associated = true;
    r.st.association.authorized = true;
    r.st.association.ssidLength = 4;
    std::memcpy(r.st.association.ssid, "test", 4);
    const uint8_t bssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::memcpy(r.st.association.bssid, bssid, 6);
    r.st.association.channel = 36;
    r.st.association.frequency = 5180;
    r.st.association.rssi = -47;
    r.st.association.noise = -95;
    r.st.association.rate = 433;
    r.st.association.authType = 2;
    r.st.association.cipher = 4;
    r.st.association.state = kStateRun;
    r.st.association.result = 1;
    CHECK(r.ask(false, kState, 0, false).error == 0 && r.rq.value == int32_t(kStateRun));
    CHECK(r.ask(false, kState, 8).error == 0 && r.u32(4) == kStateRun);
    CHECK(r.ask(false, kSsid, 32).error == 0 && r.rq.length == 4 && std::memcmp(r.at(0), "test", 4) == 0);
    CHECK(r.ask(false, kBssid, 6).error == 0 && std::memcmp(r.at(0), bssid, 6) == 0);
    CHECK(r.ask(false, kChannel, 0, false).error == 0 && r.rq.value == 36);
    CHECK(r.ask(false, kRate, 0, false).error == 0 && r.rq.value == 433);
    CHECK(r.ask(false, kRssi, 52).error == 0 && int32_t(r.u32(28)) == -47);
    CHECK(r.ask(false, kAuthType, 16).error == 0 && r.u32(4) == 1 && r.u32(8) == 0x10);
    CHECK(r.ask(false, kNoise, 52).error == 0 && int32_t(r.u32(28)) == -95);
    CHECK(r.ask(false, kAssociateResult, 0, false).error == 0 && r.rq.value == 1);
    CHECK(r.ask(false, kChannel, 16).error == 0 && r.u32(0) == 1 && r.u32(4) == 1 && r.u32(8) == 36 &&
          r.u32(12) == (kChan20MHz | kChan5GHz));
    Answer a = r.ask(false, kCurrentNetwork, kScanResultBytes27);
    CHECK(a.error == 0 && a.wrote >= offsetof(ScanResult, elements) && r.u32(8) == 36);
    ScanResult joined;
    std::memcpy(&joined, r.at(0), sizeof joined);
    CHECK(joined.ssidLength == 4 && std::memcmp(joined.ssid, "test", 4) == 0 && joined.bssid[5] == 0x55 &&
          joined.rssi == -47 && (joined.capability & 0x11) == 0x11);
    auto &candidate = r.st.scan.bss[0];
    std::memcpy(candidate.bssid, bssid, sizeof bssid);
    candidate.rsnLength = 4;
    candidate.rsn[0] = 48;
    candidate.rsn[1] = 2;
    candidate.rsn[2] = 1;
    candidate.beaconInterval = 125;
    r.st.scan.count = 1;
    CHECK(r.ask(false, kCurrentNetwork, kScanResultBytes27).error == 0);
    std::memcpy(&joined, r.at(0), sizeof joined);
    CHECK(joined.beaconInterval == 125 && joined.rssi == -47 && joined.elements[joined.elementsLength - 4] == 48);
    const uint32_t withoutRsn = joined.elementsLength - 4;
    r.st.association.authType = 1;
    candidate.capability = 0x11;
    CHECK(r.ask(false, kCurrentNetwork, kScanResultBytes27).error == 0);
    std::memcpy(&joined, r.at(0), sizeof joined);
    CHECK(joined.elementsLength == withoutRsn && (joined.capability & 0x11) == 1);
    r.st.association.authType = 2;
    for (uint8_t length : {0, 1, 2, 3, 5, 65}) {
      candidate.rsnLength = length;
      candidate.rsn[1] = length >= 2 ? length - 2 : 0;
      CHECK(r.ask(false, kCurrentNetwork, kScanResultBytes27).error == 0);
      std::memcpy(&joined, r.at(0), sizeof joined);
      CHECK(joined.elementsLength == withoutRsn && (joined.capability & 0x11) == 0x11);
    }
    candidate.rsnLength = 3;
    candidate.rsn[1] = 2;
    CHECK(r.ask(false, kCurrentNetwork, kScanResultBytes27).error == 0);
    std::memcpy(&joined, r.at(0), sizeof joined);
    CHECK(joined.elementsLength == withoutRsn && (joined.capability & 0x11) == 0x11);
    CHECK(r.ask(true, kScanCacheClear, 0).error == 0);
    CHECK(r.ask(false, kCurrentNetwork, kScanResultBytes27).error == 0);
    std::memcpy(&joined, r.at(0), sizeof joined);
    CHECK(joined.elementsLength == withoutRsn && joined.beaconInterval == 100 && (joined.capability & 0x11) == 0x11);
    CHECK(r.associate().error == 0 && r.ask(true, kDisassociate, 0).error == 0);
  }

  // ---- the accept modes: unknown sets accepted, unknown gets zeroed at their own length (in pieces), VIRTUAL_IF_ROLE answered
  {
    Rig r;
    CHECK(r.st.accept == kAcceptDefault);
    CHECK(r.ask(true, 85, 2068).error == kErrUnsupported && r.ask(false, 115, 2600).error == kErrUnsupported);
    r.st.accept = kAcceptDefault | kAcceptUnknownSets;
    Answer a = r.ask(true, 85, 2068);
    CHECK(a.error == 0 && a.accepted);
    CHECK(r.ask(false, 115, 2600).error == kErrUnsupported);   // gets still refused
    r.st.accept |= kAcceptUnknownGets;
    std::memset(r.user.mem.data(), 0xa5, 4096);
    a = r.ask(false, 115, 2600);
    CHECK(a.error == 0 && a.accepted && a.wrote == 2600 && r.at(0)[0] == 0 && r.at(2599)[0] == 0 && r.at(2600)[0] == 0xa5);
    a = r.ask(false, 353, 0, false);
    CHECK(a.error == 0 && r.rq.value == 0);
    CHECK(r.ask(false, kVirtualIfRole, 0, false).error == kErrUnsupported);   // not by kAcceptUnknownGets
    r.st.accept |= kAcceptVirtualRole;
    a = r.ask(false, kVirtualIfRole, 0, false);
    CHECK(a.error == 0 && a.accepted && r.rq.value == 0);
    // the handlers of ours and the join's sets (M2) are not changed by any mode
    CHECK(r.ask(false, kRssi, 52).error == kErrNotAssociated && r.associate().error == 0);
    r.st.accept = 0;
    CHECK(r.ask(true, kInfraEnumerated, 1).error == kErrUnsupported && r.associate().error == 0);
    Seen seen;
    CHECK(seen.first(85, true, 2068, 0) && !seen.first(85, true, 2068, 0));
    seen.clear();
    CHECK(seen.count == 0 && seen.first(85, true, 2068, 0));
  }

  // ---- channels: the 22 this station scans, each with its band
  {
    Rig r;
    Answer a = r.ask(false, kSupportedChannels, 4824);
    CHECK(a.error == 0 && r.u32(0) == 1 && r.u32(4) == 22 && a.wrote == 8 + 12 * 22);
    CHECK(r.u32(8) == 1 && r.u32(12) == 1 && r.u32(16) == (kChan20MHz | kChan2GHz | kChanActive));
    CHECK(r.u32(8 + 12 * 13 + 4) == 36 && r.u32(8 + 12 * 13 + 8) == (kChan20MHz | kChan5GHz | kChanActive));
    CHECK(r.u32(8 + 12 * 21 + 4) == 165);
    a = r.ask(false, kHwSupportedChannels, 776);
    CHECK(a.error == 0 && r.u32(4) == 22);
  }

  // ---- the scan: SCAN_REQ, then SCAN_RESULT once per network, then the end (EIO) and the cursor back at the start
  {
    Rig r;
    r.st.scan.bss[0] = network(1, "home-5g", 5180, -48, true);
    r.st.scan.bss[1] = network(2, "cafe", 2437, -71, false);
    r.st.scan.count = 2;
    std::memset(r.user.mem.data(), 0, 0x80);
    r.put32(kScanReqSsidLengthAt, 4);
    std::memcpy(r.user.mem.data() + kScanReqSsidAt, "cafe", 4);
    r.put32(kScanReqTypeAt, kScanTypeCached);   // type 3 at 0x34: the held results (sheng's kScanTypeCached)
    r.rq = {};
    r.rq.type = kScanReq;
    r.rq.length = 0x80;
    r.rq.data = User::kBase;
    uint32_t ssidLength = 99, scanType = 0;
    scanReqInfo(r.rq, r.io, ssidLength, scanType);
    CHECK(ssidLength == 4 && scanType == kScanTypeCached);
    r.put32(kScanReqTypeAt, 1);
    scanReqInfo(r.rq, r.io, ssidLength, scanType);
    CHECK(scanType == 1);
    r.rq.length = 0x20;   // a buffer too short for the type: not read (SCAN_REQ_MULTIPLE has no type field either)
    scanReqInfo(r.rq, r.io, ssidLength, scanType);
    CHECK(scanType == 0xffffffffu && ssidLength == 4);
    Answer a = r.ask(true, kScanReq, 5528);
    CHECK(a.error == 0 && (a.effects & kDoScanStarted) && r.st.scanRequests == 1);
    // The user-client/raw-get loop returns 0 for each complete record, then exactly 5 at the list end.  The tail after the
    // information elements is zeroed even though the caller prefilled it with a marker.
    std::memset(r.user.mem.data(), 0, kScanResultBytes27);
    r.st.nowNs = 0x1122334455667788ull;
    std::memcpy(r.user.mem.data() + 0x88c, "request-7", 10);
    a = r.ask(false, kScanResult, kScanResultBytes27);
    CHECK(a.error == 0 && a.wrote == kScanResultBytes27);
    for (uint32_t i = 0x8c + 47; i < kScanResultBytes27; ++i)
      CHECK(r.at(i)[0] == ((i >= kScanResultTimestampAt && i < kScanResultTimestampAt + 8)
                               ? uint8_t(0x1122334455667788ull >> (8 * (i - kScanResultTimestampAt))) : 0));
    ScanResult got;
    std::memcpy(&got, r.at(0), sizeof got);
    CHECK(got.version == 1 && got.channel.channel == 36 && (got.channel.flags & kChan5GHz) && got.rssi == -48 && got.noise == -95);
    CHECK(got.snr == 47 && got.ssidLength == 7 && std::memcmp(got.ssid, "home-5g", 7) == 0 && got.bssid[5] == 1);
    CHECK(got.capability == 0x11 && got.beaconInterval == 100 && got.rateCount == 12 && got.rates[0] == 1 && got.rates[11] == 54);
    // the elements: SSID, 8 rates, 4 extended rates, the RSN element (no DS parameter set at 5 GHz)
    const uint8_t *e = got.elements;
    CHECK(e[0] == 0 && e[1] == 7 && std::memcmp(e + 2, "home-5g", 7) == 0);
    CHECK(e[9] == 1 && e[10] == 8 && e[11] == 0x82);
    CHECK(e[19] == 50 && e[20] == 4 && e[21] == 0x30);
    CHECK(e[25] == 48 && e[26] == 20 && e[27] == 1);
    CHECK(got.elementsLength == 25 + 22);
    a = r.ask(false, kScanResult, kScanResultBytes27);
    std::memcpy(&got, r.at(0), sizeof got);
    CHECK(a.error == 0 && got.channel.channel == 6 && (got.channel.flags & kChan2GHz) && got.capability == 0x1);
    // 2.4 GHz: the DS parameter set (channel 6) after the rates
    CHECK(got.elements[0] == 0 && got.elements[6] == 1 && got.elements[16] == 3 && got.elements[17] == 1 && got.elements[18] == 6);
    CHECK(got.elementsLength == 6 + 10 + 3 + 6);
    a = r.ask(false, kScanResult, kScanResultBytes27);
    CHECK(a.error == kErrEndOfResults && r.st.cursor == 0 && r.st.listEnds == 1);
    // and again from the first
    a = r.ask(false, kScanResult, kScanResultBytes27);
    std::memcpy(&got, r.at(0), sizeof got);
    CHECK(a.error == 0 && got.bssid[5] == 1);
    // a short buffer: the head of the record
    a = r.ask(false, kScanResult, 0x8c);
    CHECK(a.error == 0 && a.wrote == 0x8c);
    CHECK(resultBytes(got) == kScanResultBytes27);
    // SCANCACHE_CLEAR: nothing until the next scan; then the list again
    CHECK(r.ask(true, kScanCacheClear, 0, false).error == 0);
    CHECK(r.ask(false, kScanResult, kScanResultBytes27).error == kErrEndOfResults);
    r.st.scan.count = 2;   // (the kext refreshes the snapshot before a SCAN_REQ)
    CHECK(r.ask(true, kScanReqMultiple, 0x400).error == 0);
    CHECK(r.ask(false, kScanResult, kScanResultBytes27).error == 0);
    // an empty list: the end at once
    Rig e2;
    CHECK(e2.ask(true, kScanReq, 0x80).error == 0 && e2.ask(false, kScanResult, kScanResultBytes27).error == kErrEndOfResults);
  }

  // ---- the registry's address form; what is not a whole address is refused
  {
    uint8_t m[6];
    CHECK(parseBssid("3c:52:a1:10:20:fe", m) && m[0] == 0x3c && m[5] == 0xfe);
    CHECK(parseBssid("3C:52:A1:10:20:FE", m) && m[0] == 0x3c);
    CHECK(!parseBssid("3c:52:a1:**:**:**", m) && !parseBssid("3c:52:a1:10:20", m) && !parseBssid("3c:52:a1:10:20:fe:00", m));
    CHECK(!parseBssid(nullptr, m) && !parseBssid("", m));
  }

  // ---- the elements fit the space they are given (a long name and a long RSN element never overrun)
  {
    Bss b = network(3, "0123456789abcdef0123456789abcdef", 2412, -60, true);
    uint8_t out[40];
    std::memset(out, 0xee, sizeof out);
    const uint32_t n = buildElements(b, out, 38);
    CHECK(n <= 38 && out[0] == 0 && out[1] == 32 && out[38] == 0xee && out[39] == 0xee);
    b.rsn[1] = 200;   // a length the element does not have: left out
    uint8_t big[400];
    CHECK(buildElements(b, big, sizeof big) == 34 + 10 + 3 + 6);
  }

  // ---- the log's table: a kind once, its repeats not, a full table counted
  {
    Seen s;
    CHECK(s.first(kDriverVersion, false, 0x180, 0) && !s.first(kDriverVersion, false, 0x180, 0));
    CHECK(s.first(kDriverVersion, false, 0x180, kErrFault) && s.first(kDriverVersion, true, 0x180, kErrUnsupported));
    CHECK(s.first(kScanResult, false, 1164, 0) && s.first(kScanResult, false, 1164, kErrEndOfResults));
    for (int i = 0; i < 400; ++i) s.first(1000 + i, false, 4, 0);
    CHECK(s.count == Seen::kMax && s.overflow > 0);
  }

  // ---- the front end's memory: three fixed allocations at its start (the state, the scratch buffer, the kind table), none per
  // request (the handler has no allocator: every answer is built in the scratch buffer). wm3's boot failed for kernel memory
  // (decmpfs ENOMEM); this bounds what PianoWifi80211 can have taken: under 16 KiB for the whole boot, whatever airportd asks.
  {
    const size_t total = sizeof(State) + kScratchBytes + sizeof(Seen);
    CHECK(total < 16 * 1024);
    Rig r;
    r.st.scan.bss[0] = network(1, "a", 2412, -50, false);
    r.st.scan.count = 1;
    r.st.accept = kAcceptInfraEnumerated | kAcceptUnknownSets | kAcceptUnknownGets | kAcceptVirtualRole;
    // every request kind, both ways, many times over: nothing grows (the state is a fixed block; its counters only count)
    for (int round = 0; round < 50; ++round)
      for (int32_t t = 0; t < 700; ++t) {
        r.ask(false, t, 64);
        r.ask(true, t, 64);
      }
    CHECK(r.st.cursor <= r.st.scan.count && r.st.scan.count <= 1);   // (SCANCACHE_CLEAR among the requests empties it)
    // the largest zero answer is clipped at 64 KiB however long a request says it is
    Rig big;
    big.user.mem.resize(80 * 1024, 0xa5);
    big.st.accept = kAcceptUnknownGets;
    Answer a = big.ask(false, 9999, 0x7fffffff);
    CHECK(a.error == 0 && a.wrote == 64 * 1024 && big.rq.length == 64 * 1024);
  }

  // ---- the user client's kernel-buffer test (sheng bit 0x8): a range knows what lies inside it, exactly and without wrapping
  {
    uint8_t buf[64];
    const uintptr_t base = reinterpret_cast<uintptr_t>(buf);
    uint32_t offset = 999;
    CHECK(shengKernelBuffer(base + 8, 16, buf, sizeof buf, offset) && offset == 8);
    CHECK(shengKernelBuffer(base, 64, buf, sizeof buf, offset) && offset == 0);
    CHECK(shengKernelBuffer(base + 63, 1, buf, sizeof buf, offset) && offset == 63);
    CHECK(!shengKernelBuffer(base + 63, 2, buf, sizeof buf, offset));                    // past the end
    CHECK(!shengKernelBuffer(base - 1, 1, buf, sizeof buf, offset));                     // before the start
    CHECK(!shengKernelBuffer(base - 0x100000000ull, uint32_t(0x100000000ull + 64), buf, sizeof buf, offset));   // no wraparound
    CHECK(shengKernelBuffer(base + 64, 0, buf, sizeof buf, offset) && offset == 64);     // an empty copy at the end is inside
    CHECK(!shengKernelBuffer(base + 64, 0, buf, 0, offset));                             // an empty range matches nothing
    CHECK(!shengKernelBuffer(0, 0, nullptr, 0, offset));                                  // null is never a kernel buffer
  }

  if (failures) {
    std::printf("apple80211_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("apple80211_host: all checks passed\n");
  return 0;
}
