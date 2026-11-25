// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the AppleSMC stand-in's answers (Smc.h) against the 27.3 clients' call sequences, as read on the decoded shared cache
// and in powerd (docs/policy.md section 12): IOKit.framework's _smcReadKey (key info, then a read of that size, the bytes copied in
// reverse) and smcSupportsHPM (true only for "read succeeded and the byte is 1", cached per process); powerd's own read (with its
// byte-order flag), its two-step write (key info first: a missing key is never written) and its 168-byte 'zEPD' probe;
// thermalmonitord's 168-byte sensor exchange writes; key by index; refused sizes; 'MSHP' off (piano-smc-mshp=0); the client types
// per level (libSMC's type 0 refused at level 1).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. smc_host.cpp -o /tmp/smc_host && /tmp/smc_host
#include "Smc.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pianosmc;
static int failures, checks;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// The kext's selector 2 in one place: the structure answered in place in a buffer of the caller's output size.
struct Smc {
  Config config;
  int calls = 0;
  int call(const uint8_t *in, size_t inBytes, uint8_t *out, size_t outBytes)
  {
    ++calls;
    std::vector<uint8_t> copy(in, in + inBytes);   // IOUserClient hands separate input and output buffers
    const Call c = answer(config, copy.data(), inBytes, out, outBytes);
    return c.ret == 0 ? 0 : int(0xe00002c2);   // kIOReturnBadArgument
  }
};

// IOKit.framework _smcReadKey (0x184efb730): returns kIOReturnSuccess (0), 0xe00002f0 (not found) or 0xe00002c9; *size is in/out.
static uint32_t iokitReadKey(Smc &smc, uint32_t key, uint8_t *dest, uint8_t *size)
{
  uint8_t in[kSmallBytes] = {}, out[kSmallBytes] = {};
  std::memset(dest, 0, *size);
  putU32(in + kKeyAt, key);
  in[kCommandAt] = kCmdKeyInfo;
  smc.call(in, sizeof in, out, sizeof out);
  if (out[kResultAt]) return out[kResultAt] == kKeyNotFound ? 0xe00002f0u : 0xe00002c9u;
  const uint32_t info = getU32(out + kDataSizeAt);
  std::memset(in, 0, sizeof in);
  putU32(in + kKeyAt, key);
  in[kCommandAt] = kCmdRead;
  putU32(in + kDataSizeAt, info);
  std::memset(out, 0, sizeof out);
  const uint32_t ret = uint32_t(smc.call(in, sizeof in, out, sizeof out));
  if (out[kResultAt]) return out[kResultAt] == kKeyNotFound ? 0xe00002f0u : 0xe00002c9u;
  // the count is min(*size, the size it sent), the bytes reversed
  uint32_t n = getU32(in + kDataSizeAt);
  if (n < *size) *size = uint8_t(n);
  for (uint32_t i = 0; i < *size; ++i) dest[i] = out[kBytesAt + (*size - 1 - i)];
  return ret;
}

// IOKit.framework smcSupportsHPM (0x184efb8a4): one read of 'MSHP' per process, cached.
struct Process {
  uint32_t cached = 0xe0000001u;
  bool byteWasOne = false;
  bool supportsHPM(Smc *smc)
  {
    bool one = true;   // a later call answers only "the first read succeeded" (the byte flag starts at 1)
    if (cached == 0xe0000001u) {
      uint8_t byte = 0, size = 1;
      cached = smc ? iokitReadKey(*smc, kKeyMSHP, &byte, &size) : 0xe00002f0u;   // no service: callSMCFunction's 0xe00002f0
      byteWasOne = byte == 1;
      one = byteWasOne;
    }
    return cached == 0 && one;
  }
};

// powerd's two-step write (sub_10005ff50 / sub_1000606bc): key info; result 0 -> command 6 with the caller's bytes; else the error.
static uint32_t powerdWrite(Smc &smc, uint32_t key, const uint8_t *data, uint32_t len, size_t bytes, int *writes)
{
  std::vector<uint8_t> in(bytes, 0), out(bytes, 0);
  putU32(in.data() + kKeyAt, key);
  in[kCommandAt] = kCmdKeyInfo;
  smc.call(in.data(), bytes, out.data(), bytes);
  if (out[kResultAt]) return out[kResultAt] == kKeyNotFound ? 0xe00002f0u : 0xe00002c9u;
  ++*writes;
  std::fill(in.begin(), in.end(), 0);
  putU32(in.data() + kKeyAt, key);
  in[kCommandAt] = kCmdWrite;
  putU32(in.data() + kDataSizeAt, getU32(out.data() + kDataSizeAt));
  std::memcpy(in.data() + kBytesAt, data, len);
  std::fill(out.begin(), out.end(), 0);
  const uint32_t ret = uint32_t(smc.call(in.data(), bytes, out.data(), bytes));
  return out[kResultAt] ? 0xe00002c9u : ret;
}

int main()
{
  // 1. The Energy Mode path: IOKit reads 'MSHP' and gets 1; smcSupportsHPM true, and stays true (cached).
  {
    Smc smc;
    uint8_t byte = 0, size = 1;
    CHECK(iokitReadKey(smc, kKeyMSHP, &byte, &size) == 0);
    CHECK(byte == 1 && size == 1);
    CHECK(smc.calls == 2);
    Process p;
    CHECK(p.supportsHPM(&smc));
    CHECK(p.supportsHPM(&smc));
    CHECK(smc.calls == 4);   // the second answer came from the cache
    // a caller asking for more than the key's size still sees one byte of value, the rest 0
    uint8_t in[kSmallBytes] = {}, out[kSmallBytes];
    std::memset(out, 0xee, sizeof out);
    putU32(in + kKeyAt, kKeyMSHP);
    in[kCommandAt] = kCmdRead;
    putU32(in + kDataSizeAt, 4);
    std::memset(in + kBytesAt, 0x55, 32);   // the caller's own bytes are never reflected
    CHECK(smc.call(in, sizeof in, out, sizeof out) == 0);
    CHECK(out[kResultAt] == kSuccess && out[kBytesAt] == 1 && out[kBytesAt + 1] == 0 && out[kBytesAt + 31] == 0);
    CHECK(getU32(out + kKeyAt) == kKeyMSHP && out[kCommandAt] == kCmdRead);
    // key info: size 1, type 'ui8 ', readable, no byte-order bit (powerd's sub_100060954 tests bit 0x04)
    std::memset(in, 0, sizeof in);
    putU32(in + kKeyAt, kKeyMSHP);
    in[kCommandAt] = kCmdKeyInfo;
    CHECK(smc.call(in, sizeof in, out, sizeof out) == 0);
    CHECK(out[kResultAt] == kSuccess && getU32(out + kDataSizeAt) == 1 && getU32(out + kDataTypeAt) == kTypeUi8);
    CHECK((out[kDataAttrAt] & 0x04) == 0);
  }
  // 2. No service (piano-smc=0) and the service without 'MSHP' (piano-smc-mshp=0): no High Power Mode, never a cached "1".
  {
    Process none;
    CHECK(!none.supportsHPM(nullptr));
    Smc smc;
    smc.config.mshp = false;
    Process p;
    CHECK(!p.supportsHPM(&smc));
    CHECK(!p.supportsHPM(&smc));
    uint8_t byte = 7, size = 1;
    CHECK(iokitReadKey(smc, kKeyMSHP, &byte, &size) == 0xe00002f0u);
    CHECK(byte == 0);
  }
  // 3. Every other key IOKit or powerd reads is "not found" (smcSupportsSilentRunning's 'WKTP'; powerd's 'CLKU', 'CLWK'), in both
  //    structure sizes, with nothing in the answer's key info or bytes.
  {
    Smc smc;
    for (const uint32_t key : {key4('W', 'K', 'T', 'P'), key4('C', 'L', 'K', 'U'), key4('C', 'L', 'W', 'K'), key4('#', 'K', 'E', 'Y'),
                               key4('B', 'C', 'L', 'M'), key4('M', 'S', 'H', 'p'), 0u, 0xffffffffu}) {
      uint8_t byte = 0x5a, size = 1;
      CHECK(iokitReadKey(smc, key, &byte, &size) == 0xe00002f0u);
      CHECK(byte == 0);
      for (const size_t bytes : {kSmallBytes, kLargeBytes}) {
        std::vector<uint8_t> in(bytes, 0), out(bytes, 0xee);
        putU32(in.data() + kKeyAt, key);
        in[kCommandAt] = kCmdKeyInfo;
        putU32(in.data() + kDataSizeAt, 8);
        CHECK(smc.call(in.data(), bytes, out.data(), bytes) == 0);
        CHECK(out[kResultAt] == kKeyNotFound);
        CHECK(getU32(out.data() + kDataSizeAt) == 0 && getU32(out.data() + kDataTypeAt) == 0 && out[kDataAttrAt] == 0);
        bool zero = true;
        for (size_t i = kBytesAt; i < bytes; ++i) zero = zero && out[i] == 0;
        CHECK(zero);
      }
    }
  }
  // 4. Writes never happen: powerd's 'CLWK' write at sleep (2 bytes) and its 168-byte 'zEPD' write stop at the key info; a direct
  //    write command (thermalmonitord's sensor exchange: 'zETM' and the others, 168 bytes) is answered "not found", 'MSHP' included.
  {
    Smc smc;
    int writes = 0;
    const uint8_t clwk[2] = {0x00, 0x01};
    CHECK(powerdWrite(smc, key4('C', 'L', 'W', 'K'), clwk, 2, kSmallBytes, &writes) == 0xe00002f0u);
    const uint8_t on = 1;
    CHECK(powerdWrite(smc, key4('z', 'E', 'P', 'D'), &on, 1, kLargeBytes, &writes) == 0xe00002f0u);
    CHECK(writes == 0);
    for (const uint32_t key : {key4('z', 'E', 'T', 'M'), key4('z', 'E', 'C', 'm'), key4('z', 'E', 'A', 'R'), kKeyMSHP}) {
      std::vector<uint8_t> in(kLargeBytes, 0x33), out(kLargeBytes, 0);
      putU32(in.data() + kKeyAt, key);
      in[kCommandAt] = kCmdWrite;
      putU32(in.data() + kDataSizeAt, 120);
      CHECK(smc.call(in.data(), kLargeBytes, out.data(), kLargeBytes) == 0);
      CHECK(out[kResultAt] == kKeyNotFound && out[kBytesAt] == 0);
    }
    // MSHP still reads 1 after the attempted write: nothing is stored
    uint8_t byte = 0, size = 1;
    CHECK(iokitReadKey(smc, kKeyMSHP, &byte, &size) == 0 && byte == 1);
  }
  // 5. Other commands (key by index, unknown) are "not found"; in-place answers (out == in) behave the same.
  {
    for (const uint8_t command : {uint8_t(kCmdKeyByIndex), uint8_t(0), uint8_t(7), uint8_t(0xff)}) {
      uint8_t buf[kSmallBytes] = {};
      putU32(buf + kKeyAt, kKeyMSHP);
      buf[kCommandAt] = command;
      const Call c = answer(Config(), buf, sizeof buf, buf, sizeof buf);
      CHECK(c.ret == 0 && c.result == kKeyNotFound && !c.mshp && buf[kResultAt] == kKeyNotFound);
    }
    uint8_t buf[kSmallBytes] = {};
    putU32(buf + kKeyAt, kKeyMSHP);
    buf[kCommandAt] = kCmdRead;
    putU32(buf + kDataSizeAt, 1);
    const Call c = answer(Config(), buf, sizeof buf, buf, sizeof buf);
    CHECK(c.ret == 0 && c.mshp && buf[kResultAt] == kSuccess && buf[kBytesAt] == 1);
  }
  // 6. Sizes: below the data bytes, above the inline limit, or no buffer: refused; a larger output than input is zero-filled.
  {
    uint8_t in[kMaxBytes + 8] = {}, out[kMaxBytes + 8];
    putU32(in + kKeyAt, kKeyMSHP);
    in[kCommandAt] = kCmdKeyInfo;
    CHECK(answer(Config(), in, kBytesAt, out, kSmallBytes).ret == kBadSize);
    CHECK(answer(Config(), in, kSmallBytes, out, kBytesAt).ret == kBadSize);
    CHECK(answer(Config(), in, kMaxBytes + 1, out, kSmallBytes).ret == kBadSize);
    CHECK(answer(Config(), nullptr, kSmallBytes, out, kSmallBytes).ret == kBadSize);
    CHECK(answer(Config(), in, kSmallBytes, nullptr, kSmallBytes).ret == kBadSize);
    std::memset(out, 0xee, sizeof out);
    const Call c = answer(Config(), in, kSmallBytes, out, kLargeBytes);
    CHECK(c.ret == 0 && c.mshp && out[kResultAt] == kSuccess && getU32(out + kDataSizeAt) == 1 && out[kLargeBytes - 1] == 0);
    CHECK(answer(Config(), in, kMinBytes, out, kMinBytes).ret == 0);
  }
  // 7. Client types per level: level 1 lets only type 1 open (IOKit.framework, powerd), level 2 also libSMC's type 0.
  {
    CHECK(typeMaskForLevel(0) == 0 && !typeAllowed(typeMaskForLevel(0), 1));
    CHECK(typeAllowed(typeMaskForLevel(1), 1) && !typeAllowed(typeMaskForLevel(1), 0) && !typeAllowed(typeMaskForLevel(1), 2));
    CHECK(typeAllowed(typeMaskForLevel(2), 0) && typeAllowed(typeMaskForLevel(2), 1) && !typeAllowed(typeMaskForLevel(2), 2));
    CHECK(!typeAllowed(0xffffffffu, 32) && !typeAllowed(0xffffffffu, 0xffffffffu));
    char t[5];
    keyText(kKeyMSHP, t);
    CHECK(std::strcmp(t, "MSHP") == 0);
    keyText(0x01020304u, t);
    CHECK(std::strcmp(t, "????") == 0);
  }
  std::printf("smc_host: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
