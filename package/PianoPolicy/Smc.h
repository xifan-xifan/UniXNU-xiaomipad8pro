// SPDX-License-Identifier: BSD-2-Clause-Patent
// Smc.h: what piano's AppleSMC stand-in answers (docs/policy.md section 12), portable C++ so the host test (tests/smc_host.cpp)
// runs the same code as the kext (AppleSmc.cpp).
//
// Why a stand-in at all: macOS offers the three-option Energy Mode (Automatic / Low Power / High Power) in System Settings > Battery
// only when IOKit's IOPMFeatureIsAvailable("HighPowerMode") is true on AC and on battery. For a model that is not on IOKit's own
// list (Mac17,11 is not) that is true only if an IOService of class AppleSMC answers the SMC key 'MSHP' with the byte 1 through its
// user client (IOKit.framework smcSupportsHPM -> _smcReadKey -> callSMCFunction; 27.3, read on the decoded shared cache). piano has no
// SMC, so this answers that one key and says "not found" for every other key and command: nothing else is emulated, nothing is
// stored, no write has an effect.
//
// The call it serves: IOServiceOpen(AppleSMC, type), then IOConnectCallMethod selector 0 (open, no arguments), selector 2 (one
// parameter structure in, the same structure out), selector 1 (close). The structure (only the fields below are read or written):
//   0x00 u32 key (four characters, the first in the high byte: 'MSHP' = 0x4d534850)
//   0x1c u32 keyInfo.dataSize    (command 9 answers it; commands 5 and 6 carry the caller's size in)
//   0x20 u32 keyInfo.dataType    (command 9 answers it: 'ui8 ')
//   0x24 u8  keyInfo.dataAttributes
//   0x28 u8  result              (0 success, 0x84 key not found)
//   0x2a u8  command             (5 read, 6 write, 8 key by index, 9 key info)
//   0x30 ... data bytes          (32 in the 80-byte form IOKit and powerd send, 120 in the 168-byte form of powerd's and
//                                 thermalmonitord's larger calls)
// IOKit reads a key as command 9 (key info: dataSize) then command 5 with that size, and copies the data bytes in reverse order;
// for one byte the order does not matter. smcSupportsHPM caches the first read per process (a later read only repeats whether the
// first one succeeded), so "off" must be "key absent" (or no service), never the byte 0.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pianosmc {

constexpr size_t kKeyAt = 0x00, kDataSizeAt = 0x1c, kDataTypeAt = 0x20, kDataAttrAt = 0x24, kResultAt = 0x28, kStatusAt = 0x29,
                 kCommandAt = 0x2a, kBytesAt = 0x30;
constexpr size_t kSmallBytes = 0x50;   // IOKit.framework's callSMCFunction, powerd's reads and writes
constexpr size_t kLargeBytes = 0xa8;   // powerd's 'zEPD' probe, thermalmonitord's sensor exchange writes
constexpr size_t kMinBytes = kBytesAt + 1, kMaxBytes = 0x200;   // what selector 2 accepts (the structure is inline)

enum : uint8_t { kCmdRead = 5, kCmdWrite = 6, kCmdKeyByIndex = 8, kCmdKeyInfo = 9 };
enum : uint8_t { kSuccess = 0x00, kKeyNotFound = 0x84 };
// The user client's selectors.
enum : uint32_t { kSelOpen = 0, kSelClose = 1, kSelCall = 2 };

constexpr uint32_t key4(char a, char b, char c, char d)
{
  return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 | uint32_t(uint8_t(c)) << 8 | uint32_t(uint8_t(d));
}
constexpr uint32_t kKeyMSHP = key4('M', 'S', 'H', 'P');   // High Power Mode supported (byte, 1)
constexpr uint32_t kTypeUi8 = key4('u', 'i', '8', ' ');
constexpr uint8_t kAttrRead = 0x80;                          // readable; no 0x04 (byte order) bit: a single byte either way

// The four characters of a key for a log line ('?' for anything not printable).
inline void keyText(uint32_t key, char out[5])
{
  for (int i = 0; i < 4; ++i) {
    const uint8_t c = uint8_t(key >> (24 - 8 * i));
    out[i] = (c >= 0x20 && c < 0x7f) ? char(c) : '?';
  }
  out[4] = 0;
}

inline uint32_t getU32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline void putU32(uint8_t *p, uint32_t v)
{
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

struct Config {
  bool mshp = true;   // piano-smc-mshp: answer 'MSHP' (0: every key not found, the service still there: the "SMC without keys" A/B)
};

// The client types the service lets open (piano-smc): bit N = type N. IOKit.framework's callSMCFunction and powerd open type 1
// (all the Energy Mode path needs); libSMC (CoreBrightness, thermalmonitord's sensor exchange and other libSMC users) opens type 0,
// which level 1 refuses, so those clients stay on their "cannot open the SMC" path exactly as without the stand-in.
constexpr uint32_t typeMaskForLevel(uint32_t level) { return level >= 2 ? 0x3u : level == 1 ? 0x2u : 0u; }
constexpr bool typeAllowed(uint32_t mask, uint32_t type) { return type < 32 && (mask >> type & 1u); }

// What one selector-2 call was, for the counters and the log.
struct Call {
  int ret = 0;          // 0 answered; kBadSize when the structure sizes are refused (the kext returns kIOReturnBadArgument)
  uint32_t key = 0;
  uint8_t command = 0;
  uint8_t result = 0;
  bool mshp = false;    // the MSHP byte was given (command 5) or its size (command 9)
};
constexpr int kBadSize = -1;

// One selector-2 call: `in` (inBytes) is answered into `out` (outBytes). The output starts as a copy of the input (the rest zero),
// so a caller sees its own key and command back, as with Apple's driver; then the result, and for 'MSHP' the key info or the byte.
// `out` may be the same buffer as `in`.
inline Call answer(const Config &config, const uint8_t *in, size_t inBytes, uint8_t *out, size_t outBytes)
{
  Call c;
  if (!in || !out || inBytes < kMinBytes || inBytes > kMaxBytes || outBytes < kMinBytes || outBytes > kMaxBytes) {
    c.ret = kBadSize;
    return c;
  }
  c.key = getU32(in + kKeyAt);
  c.command = in[kCommandAt];
  const uint32_t sizeIn = getU32(in + kDataSizeAt);
  if (out != in) {
    const size_t n = inBytes < outBytes ? inBytes : outBytes;
    for (size_t i = 0; i < n; ++i) out[i] = in[i];
    for (size_t i = n; i < outBytes; ++i) out[i] = 0;
  } else {
    for (size_t i = inBytes; i < outBytes; ++i) out[i] = 0;
  }
  // Nothing of the caller's data is reflected: the data bytes are the answer's.
  for (size_t i = kBytesAt; i < outBytes; ++i) out[i] = 0;
  out[kStatusAt] = 0;
  const bool ours = config.mshp && c.key == kKeyMSHP && (c.command == kCmdKeyInfo || c.command == kCmdRead);
  if (!ours) {
    putU32(out + kDataSizeAt, 0);
    putU32(out + kDataTypeAt, 0);
    out[kDataAttrAt] = 0;
    out[kResultAt] = c.result = kKeyNotFound;
    return c;
  }
  c.mshp = true;
  putU32(out + kDataTypeAt, kTypeUi8);
  out[kDataAttrAt] = kAttrRead;
  if (c.command == kCmdKeyInfo) {
    putU32(out + kDataSizeAt, 1);
  } else {
    // The caller's size comes from command 9 (1); whatever it asked for, one byte is the key's value and the rest stays 0.
    putU32(out + kDataSizeAt, sizeIn ? sizeIn : 1);
    out[kBytesAt] = 1;
  }
  out[kResultAt] = c.result = kSuccess;
  return c;
}

// The U1 gate (docs/policy.md 12.3): Apple's IOUSBHostFamily (AppleUSBHostResourcesTypeC) probes only when the device tree's
// product node carries usb-c-smc-pwr; then, for a port with usb-c-port-number or smc-port-number, it waits for an AppleSMC service
// without a timeout and calls it as an AppleSMCFamily without a null check. This stand-in is no AppleSMCFamily, so it is not
// published on a device tree with that property.
constexpr const char *kUsbcSmcPowerProperty = "usb-c-smc-pwr";

}  // namespace pianosmc
