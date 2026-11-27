// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the receive block-ack agreements (WifiBa.h, boot 10s): an ADDBA request and a DELBA read as the air sends them,
// the decision under each piano-wifi-rxba value, the ADDBA response read back field by field, the queue descriptor of an
// agreement's window and the firmware's command for it, and the queue memory pool that never hands an address out twice
// before it starts again.
//   c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. ba_host.cpp -o /tmp/ba_host && /tmp/ba_host
#include <cstdio>
#include <cstring>
#include "WifiAssoc.h"
#include "WifiBa.h"

using namespace pianowifi;

static int failures;
#define CHECK(x)                                                         \
  do {                                                                   \
    if (!(x)) {                                                          \
      ++failures;                                                        \
      std::printf("ba_host: FAILED %s (line %d)\n", #x, __LINE__);       \
    }                                                                    \
  } while (0)

static const uint8_t kOwn[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kAp[6] = {0x0a, 0xbb, 0xcc, 0xdd, 0xee, 0x01};

// An ADDBA request of the access point: TID, window, A-MSDU, policy, timeout, starting sequence.
static uint32_t request(uint8_t *f, uint8_t dialog, uint32_t tid, uint32_t window, bool amsdu, bool immediate, uint16_t timeout,
                        uint16_t ssn)
{
  std::memset(f, 0, 64);
  f[0] = 0xd0;
  std::memcpy(f + 4, kOwn, 6);
  std::memcpy(f + 10, kAp, 6);
  std::memcpy(f + 16, kAp, 6);
  uint8_t *b = f + 24;
  b[0] = 3;
  b[1] = 0;
  b[2] = dialog;
  const uint32_t params = (amsdu ? 1u : 0u) | (immediate ? 2u : 0u) | tid << 2 | window << 6;
  b[3] = uint8_t(params);
  b[4] = uint8_t(params >> 8);
  b[5] = uint8_t(timeout);
  b[6] = uint8_t(timeout >> 8);
  const uint32_t ssc = uint32_t(ssn) << 4;
  b[7] = uint8_t(ssc);
  b[8] = uint8_t(ssc >> 8);
  return 24 + 9;
}

int main()
{
  uint8_t f[64];
  BaRequest r;
  BaDelbaInfo d;
  // ---- the request as 10r's access point sent it: TID 0, window 64, A-MSDU, immediate
  uint32_t n = request(f, 7, 0, 64, true, true, 0, 0x123);
  CHECK(baParse(f, n, kOwn, kAp, r, d) == kBaAddbaRequest);
  CHECK(r.dialog == 7 && r.tid == 0 && r.window == 64 && r.amsdu && r.immediate && r.timeout == 0 && r.ssn == 0x123);
  CHECK(baParse(f, n - 1, kOwn, kAp, r, d) == kBaNone);   // cut short
  uint8_t other[6] = {0x02, 0, 0, 0, 0, 9};
  CHECK(baParse(f, n, other, kAp, r, d) == kBaNone);      // not to us
  CHECK(baParse(f, n, kOwn, other, r, d) == kBaNone);     // not the network's
  f[1] = 0x40;
  CHECK(baParse(f, n, kOwn, kAp, r, d) == kBaNone);       // protected: not read
  f[1] = 0;
  f[24] = 4;
  CHECK(baParse(f, n, kOwn, kAp, r, d) == kBaNone);       // another category
  // TID 7, window 32, timeout 5000 TU, sequence 4095
  n = request(f, 200, 7, 32, false, true, 5000, 4095);
  CHECK(baParse(f, n, kOwn, kAp, r, d) == kBaAddbaRequest);
  CHECK(r.dialog == 200 && r.tid == 7 && r.window == 32 && !r.amsdu && r.timeout == 5000 && r.ssn == 4095);

  // ---- the decisions
  r = BaRequest{};
  r.tid = 3;
  r.window = 64;
  r.amsdu = true;
  r.immediate = true;
  BaAnswer a = baDecide(r, kBaModeOff);
  CHECK(!a.answer);
  a = baDecide(r, kBaModeDecline);
  CHECK(a.answer && !a.accept && a.status == kDot11StatusDeclined);
  a = baDecide(r, kBaModeAccept);
  CHECK(a.answer && a.accept && a.status == 0 && a.window == 64 && !a.amsdu);
  a = baDecide(r, kBaModeAcceptAmsdu);
  CHECK(a.accept && a.amsdu);
  r.window = 0;
  CHECK(baDecide(r, kBaModeAccept).window == 64);           // 0: the recipient chooses
  r.window = 256;
  CHECK(baDecide(r, kBaModeAccept).window == 64);           // HE's sizes: ours is 64
  r.window = 16;
  CHECK(baDecide(r, kBaModeAccept).window == 16);
  r.immediate = false;
  a = baDecide(r, kBaModeAccept);
  CHECK(a.answer && !a.accept && a.status == kDot11StatusDeclined);
  r.immediate = true;
  r.tid = 9;
  CHECK(!baDecide(r, kBaModeAccept).accept);

  // ---- the response, read back
  r = BaRequest{};
  r.dialog = 42;
  r.tid = 6;
  r.window = 64;
  r.amsdu = true;
  r.immediate = true;
  r.timeout = 3000;
  a = baDecide(r, kBaModeAccept);
  uint8_t resp[kBaFrameSlot];
  n = baBuildResponse(resp, sizeof resp, kOwn, kAp, 77, r, a);
  CHECK(n == 33);
  CHECK(resp[0] == 0xd0 && resp[1] == 0);
  CHECK(std::memcmp(resp + 4, kAp, 6) == 0 && std::memcmp(resp + 10, kOwn, 6) == 0 && std::memcmp(resp + 16, kAp, 6) == 0);
  CHECK(uint32_t(resp[22] | resp[23] << 8) == 77u << 4);
  CHECK(resp[24] == 3 && resp[25] == 1 && resp[26] == 42);
  CHECK(resp[27] == 0 && resp[28] == 0);                    // success
  const uint32_t params = uint32_t(resp[29] | resp[30] << 8);
  CHECK((params & 1) == 0 && (params & 2) && ((params >> 2) & 0xf) == 6 && ((params >> 6) & 0x3ff) == 64);
  CHECK(uint32_t(resp[31] | resp[32] << 8) == 3000);
  // declined: the status, no window
  a = baDecide(r, kBaModeDecline);
  n = baBuildResponse(resp, sizeof resp, kOwn, kAp, 78, r, a);
  CHECK(n == 33 && uint32_t(resp[27] | resp[28] << 8) == 37 && ((uint32_t(resp[29] | resp[30] << 8) >> 6) & 0x3ff) == 0);
  CHECK(baBuildResponse(resp, 32, kOwn, kAp, 1, r, a) == 0);

  // ---- a DELBA of the access point (the originator) for TID 0, reason 37 (the peer's end of the agreement)
  std::memset(f, 0, sizeof f);
  f[0] = 0xd0;
  std::memcpy(f + 4, kOwn, 6);
  std::memcpy(f + 10, kAp, 6);
  std::memcpy(f + 16, kAp, 6);
  f[24] = 3;
  f[25] = 2;
  const uint32_t dp = 1u << 11 | 0u << 12;
  f[26] = uint8_t(dp);
  f[27] = uint8_t(dp >> 8);
  f[28] = 37;
  CHECK(baParse(f, 30, kOwn, kAp, r, d) == kBaDelba);
  CHECK(d.initiator && d.tid == 0 && d.reason == 37);
  CHECK(baParse(f, 29, kOwn, kAp, r, d) == kBaNone);

  // ---- an agreement's queue: the window in the descriptor, the command carries it and the queue's address
  alignas(512) static uint8_t q[kReoQueueQosBytes];
  reoQueueInit(q, 6, 64, 0x123);
  const uint32_t info0 = wmiGet(q, 2);
  CHECK(((info0 >> kReoQueueBaWindowShift) & kReoQueueBaWindowMask) == 63);
  CHECK((info0 & kReoQueueRetry) == 0 && (info0 & kReoQueueIgnoreAmpdu));
  CHECK(((wmiGet(q, 3) >> kReoQueueSsnShift) & kReoQueueSsnMask) == 0x123);
  reoQueueInit(q, 6, 1, 0);   // a DELBA: back to the window of one (two for a QoS TID)
  CHECK(((wmiGet(q, 2) >> kReoQueueBaWindowShift) & kReoQueueBaWindowMask) == 1);
  uint8_t cmd[256];
  WmiWriter w;
  n = assocBuildReorderQueueSetup(w, cmd, sizeof cmd, 0, kAp, 6, 0x12345600ull, 64);
  CHECK(n > kWmiCmdHeaderBytes);
  uint32_t size = 0;
  const uint8_t *c = wmiFind(cmd + kWmiCmdHeaderBytes, n - kWmiCmdHeaderBytes, kWmiTagReorderQueueSetupCmd, &size);
  CHECK(c && size >= kWmiReorderQueueSetupWords * 4);
  if (c) CHECK(wmiGet(c, 3) == 6 && wmiGet(c, 4) == 0x12345600u && wmiGet(c, 7) == 1 && wmiGet(c, 8) == 64);

  // ---- the pool: every address once, 512-byte steps, zeroed, then it starts again
  alignas(512) static uint8_t mem[8 * 512];
  ReoPool p;
  reoPoolInit(p, mem, 0x80000000ull, sizeof mem + 100);   // a size that is not a step's multiple is cut down
  CHECK(p.bytes == sizeof mem);
  uint8_t *va = nullptr;
  uint64_t bus = 0, last = 0;
  bool wrapped = true;
  std::memset(mem, 0xa5, sizeof mem);
  CHECK(reoPoolTake(p, 17 * 128, &va, &bus, &wrapped) && !wrapped);   // 2176 bytes: 5 steps
  CHECK(va == mem && bus == 0x80000000ull && p.used == 5 * 512 && va[2175] == 0);
  last = bus;
  CHECK(reoPoolTake(p, 512, &va, &bus, &wrapped) && !wrapped && bus == last + 5 * 512 && (bus & 511) == 0);
  CHECK(reoPoolTake(p, 512, &va, &bus, &wrapped) && bus == last + 6 * 512);
  CHECK(reoPoolTake(p, 512, &va, &bus, &wrapped) && bus == last + 7 * 512 && !wrapped);
  CHECK(reoPoolTake(p, 512, &va, &bus, &wrapped) && wrapped && bus == 0x80000000ull && p.wraps == 1);
  CHECK(!reoPoolTake(p, 9 * 512, &va, &bus, &wrapped));   // more than the pool
  CHECK(p.takes == 5);
  ReoPool empty;
  CHECK(!reoPoolTake(empty, 512, &va, &bus, &wrapped));

  if (failures) return 1;
  std::printf("ba_host: all checks passed\n");
  return 0;
}
