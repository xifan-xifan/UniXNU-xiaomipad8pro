// SPDX-License-Identifier: BSD-2-Clause-Patent
// EDID 1.4, CTA-861 and DisplayID timing model. No allocation or I/O.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "DpPort.h"

namespace pianodp {

constexpr size_t kEdidBlockBytes = 128, kEdidMaxBlocks = 4;
constexpr size_t kEdidMaxBytes = kEdidBlockBytes * kEdidMaxBlocks;
constexpr uint32_t kDpRbrKhz = kRateRbr * 100, kDpHbrKhz = kRateHbr * 100, kDpHbr2Khz = kRateHbr2 * 100;
constexpr uint32_t kDpHbr3Khz = 810000, kPianoDpMaxRateKhz = kRateMaxMbps * 100;

struct DpLink {
  uint32_t rateKhz = 0;  // link symbol clock, one symbol per ten wire bits per lane
  unsigned lanes = 0;
};

constexpr bool validDpLaneCount(unsigned lanes) { return lanes == 1 || lanes == 2 || lanes == 4; }
constexpr bool validDpLink(DpLink link)
{
  return validDpLaneCount(link.lanes) &&
         (link.rateKhz == kDpRbrKhz || link.rateKhz == kDpHbrKhz ||
          link.rateKhz == kDpHbr2Khz || link.rateKhz == kDpHbr3Khz);
}

struct DpTiming {
  uint32_t pixelKhz = 0;
  uint16_t hActive = 0, hBlank = 0, hSyncOffset = 0, hSyncWidth = 0;
  // For an interlaced DTD these are the on-wire EDID values per field.
  uint16_t vActive = 0, vBlank = 0, vSyncOffset = 0, vSyncWidth = 0;
  bool hSyncPositive = false, vSyncPositive = false;
  bool separateSync = true, interlaced = false;
};

constexpr bool validDpTiming(const DpTiming &timing)
{
  return timing.pixelKhz && timing.hActive && timing.vActive && timing.hBlank && timing.vBlank &&
         timing.hSyncWidth && timing.vSyncWidth &&
         uint32_t(timing.hActive) + timing.hBlank <= UINT16_MAX &&
         uint32_t(timing.vActive) + timing.vBlank <= UINT16_MAX &&
         uint32_t(timing.hSyncOffset) + timing.hSyncWidth <= timing.hBlank &&
         uint32_t(timing.vSyncOffset) + timing.vSyncWidth <= timing.vBlank;
}

constexpr unsigned kDpRgb8BitsPerPixel = 24;
constexpr unsigned kDpTuSize = 64, kDpTuMaxValidSymbols = kDpTuSize - 2; // reserve FS and FE
constexpr uint32_t kDpNvid = 0x8000;

constexpr bool edidModeFits(const DpTiming &timing, DpLink link)
{
  return validDpTiming(timing) && validDpLink(link) &&
         modeFits(timing.pixelKhz, kDpRgb8BitsPerPixel, link.lanes, link.rateKhz / 100);
}

constexpr uint32_t dpLinkPayloadKbps(DpLink link) { return link.rateKhz * link.lanes * 8; }

constexpr bool dpStreamFits(const DpTiming &timing, DpLink link)
{
  return edidModeFits(timing, link) && !timing.interlaced && timing.separateSync &&
         timing.hSyncWidth <= 0x7fff && timing.vSyncWidth <= 0x7fff &&
         uint64_t(timing.pixelKhz) * kDpNvid + link.rateKhz / 2 >= link.rateKhz &&
         uint64_t(timing.pixelKhz) * kDpRgb8BitsPerPixel * kDpTuSize <=
             uint64_t(dpLinkPayloadKbps(link)) * kDpTuMaxValidSymbols;
}

struct EdidMode {
  DpTiming timing;
  uint8_t vic = 0; // zero for a detailed timing, otherwise a CTA or CTA-equivalent HDMI VIC
  bool rgb = true;
};

struct EdidTile {
  uint8_t columns = 0, rows = 0, column = 0, row = 0;
  uint32_t width = 0, height = 0;
  bool singleEnclosure = false;
  uint8_t topologyId[9]{};

  constexpr bool multiTile() const { return columns > 1 || rows > 1; }
};

struct EdidInfo {
  // Four base DTDs, up to six per extension, and one entry per supported VIC.
  static constexpr size_t kMaxModes = 4 + (kEdidMaxBlocks - 1) * 6 + 8;
  char manufacturer[4]{};
  uint16_t productCode = 0;
  uint16_t widthMm = 0, heightMm = 0;  // both zero when the base block gives only an aspect ratio or no size
  uint8_t extensionCount = 0;
  size_t modeCount = 0;
  EdidMode modes[kMaxModes]{};  // modes[0] is the required base preferred timing
  uint32_t vicBits[8]{};        // includes advertised SVDs outside the model's timing table
  uint64_t nativeVicBits = 0;   // only VICs 1–64 can carry the SVD native flag
  uint8_t hdmiVicBits = 0;     // HDMI VICs 1–4 from the HDMI VSDB
  EdidTile tile{};

  constexpr bool hasVic(uint8_t vic) const { return (vicBits[vic / 32] >> (vic % 32)) & 1; }
  constexpr bool nativeVic(uint8_t vic) const
  {
    return vic >= 1 && vic <= 64 && ((nativeVicBits >> (vic - 1)) & 1);
  }
};

enum class EdidStatus : uint8_t {
  Ok, InvalidSize, BadHeader, BadChecksum, UnsupportedVersion, BadManufacturer, BadTiming, BadExtension,
};

namespace edid_detail {

constexpr size_t kDescriptorBytes = 18;

inline bool allZero(const uint8_t *bytes, size_t length)
{
  for (size_t i = 0; i < length; ++i)
    if (bytes[i]) return false;
  return true;
}

inline uint16_t le16(const uint8_t *bytes) { return uint16_t(bytes[0] | (bytes[1] << 8)); }

inline bool edidChecksumOk(const uint8_t *block)
{
  unsigned sum = 0;
  for (size_t i = 0; i < kEdidBlockBytes; ++i) sum += block[i];
  return (sum & 0xff) == 0;
}

inline bool edidHeaderOk(const uint8_t *block)
{
  constexpr uint8_t header[] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
  for (size_t i = 0; i < sizeof header; ++i)
    if (block[i] != header[i]) return false;
  return true;
}

inline bool isDisplayDescriptor(const uint8_t *descriptor)
{
  const uint8_t tag = descriptor[3];
  return !le16(descriptor) && !descriptor[2] && (tag <= 0x10 || tag >= 0xf7) &&
         (!descriptor[4] || tag == 0xfd);  // EDID 1.4 range limits may use byte 4.
}

inline EdidStatus appendDtd(EdidInfo &info, const uint8_t *dtd)
{
  const bool separateSync = (dtd[17] & 0x18) == 0x18;
  const DpTiming timing{
    uint32_t(le16(dtd)) * 10,
    uint16_t(dtd[2] | ((dtd[4] & 0xf0) << 4)), uint16_t(dtd[3] | ((dtd[4] & 0x0f) << 8)),
    uint16_t(dtd[8] | ((dtd[11] & 0xc0) << 2)), uint16_t(dtd[9] | ((dtd[11] & 0x30) << 4)),
    uint16_t(dtd[5] | ((dtd[7] & 0xf0) << 4)), uint16_t(dtd[6] | ((dtd[7] & 0x0f) << 8)),
    uint16_t((dtd[10] >> 4) | ((dtd[11] & 0x0c) << 2)), uint16_t((dtd[10] & 0x0f) | ((dtd[11] & 3) << 4)),
    separateSync && (dtd[17] & 2), separateSync && (dtd[17] & 4), separateSync, bool(dtd[17] & 0x80),
  };
  if (!validDpTiming(timing) || info.modeCount == EdidInfo::kMaxModes) return EdidStatus::BadTiming;
  info.modes[info.modeCount++] = {timing, 0};
  return EdidStatus::Ok;
}

// CTA-861 1080p60, UHD and the CTA equivalents of the four HDMI VICs.
constexpr DpTiming vicTiming(uint8_t vic)
{
  switch (vic) {
    case 16: return {148500, 1920, 280, 88, 44, 1080, 45, 4, 5, true, true};
    case 93: return {297000, 3840, 1660, 1276, 88, 2160, 90, 8, 10, true, true};
    case 94: return {297000, 3840, 1440, 1056, 88, 2160, 90, 8, 10, true, true};
    case 95: case 105: return {297000, 3840, 560, 176, 88, 2160, 90, 8, 10, true, true};
    case 97: case 107: return {594000, 3840, 560, 176, 88, 2160, 90, 8, 10, true, true};
    case 98: return {297000, 4096, 1404, 1020, 88, 2160, 90, 8, 10, true, true};
    default: return {};
  }
}

inline EdidStatus appendSvd(EdidInfo &info, uint8_t svd)
{
  const bool native = svd >= 129 && svd <= 192;
  const uint8_t vic = native ? uint8_t(svd & 0x7f) : svd;
  if (vic == 0 || vic == 128 || vic >= 254) return EdidStatus::BadExtension;
  if (native) info.nativeVicBits |= uint64_t{1} << (vic - 1);
  if (info.hasVic(vic)) return EdidStatus::Ok;
  info.vicBits[vic / 32] |= uint32_t{1} << (vic % 32);
  const DpTiming timing = vicTiming(vic);
  if (timing.pixelKhz) {
    if (info.modeCount == EdidInfo::kMaxModes) return EdidStatus::BadExtension;
    info.modes[info.modeCount++] = {timing, vic};
  }
  return EdidStatus::Ok;
}

inline EdidStatus parseHdmiVsdb(EdidInfo &info, const uint8_t *payload, size_t length)
{
  if (length < 3) return EdidStatus::BadExtension;
  if (payload[0] != 0x03 || payload[1] != 0x0c || payload[2] != 0x00) return EdidStatus::Ok;
  if (length < 5) return EdidStatus::BadExtension;  // OUI and physical address
  if (length < 8) return EdidStatus::Ok;

  const uint8_t flags = payload[7];
  if ((flags & 0xc0) == 0x40) return EdidStatus::BadExtension;
  const size_t video = 8 + ((flags & 0x80) ? 2 : 0) + ((flags & 0x40) ? 2 : 0);
  if (video > length) return EdidStatus::BadExtension;
  if (!(flags & 0x20)) return EdidStatus::Ok;
  if (length - video < 2) return EdidStatus::BadExtension;
  const unsigned vicLength = payload[video + 1] >> 5, stereoLength = payload[video + 1] & 31;
  if (vicLength + stereoLength > length - video - 2) return EdidStatus::BadExtension;
  constexpr uint8_t ctaVics[] = {95, 94, 93, 98};
  for (unsigned i = 0; i < vicLength; ++i) {
    const unsigned vic = payload[video + 2 + i];
    if (vic < 1 || vic > sizeof ctaVics) return EdidStatus::BadExtension;
    info.hdmiVicBits |= uint8_t(1u << (vic - 1));
    const EdidStatus status = appendSvd(info, ctaVics[vic - 1]);
    if (status != EdidStatus::Ok) return status;
  }
  return EdidStatus::Ok;
}

inline EdidStatus parseCta(EdidInfo &info, const uint8_t *block)
{
  constexpr size_t end = kEdidBlockBytes - 1;  // checksum byte, excluded
  const unsigned revision = block[1];
  const size_t start = block[2];
  if (!revision || revision > 3 || (revision == 1 && block[3])) return EdidStatus::BadExtension;
  if (!start) return allZero(block + 4, end - 4) ? EdidStatus::Ok : EdidStatus::BadExtension;
  if (start < 4 || start > end) return EdidStatus::BadExtension;

  size_t offset = start;
  while (end - offset >= kDescriptorBytes && !allZero(block + offset, kDescriptorBytes)) {
    const uint8_t *descriptor = block + offset;
    if (!isDisplayDescriptor(descriptor)) {
      const EdidStatus status = appendDtd(info, descriptor);
      if (status != EdidStatus::Ok) return status;
    }
    offset += kDescriptorBytes;
  }
  if (!allZero(block + offset, end - offset)) return EdidStatus::BadExtension;

  // Revisions 1 and 2 reserve the area before the DTDs. Revision 3 frames it as data blocks.
  if (revision < 3) return allZero(block + 4, start - 4) ? EdidStatus::Ok : EdidStatus::BadExtension;
  for (offset = 4; offset < start;) {
    const uint8_t header = block[offset++];
    if (!header) return allZero(block + offset, start - offset) ? EdidStatus::Ok : EdidStatus::BadExtension;
    const size_t length = header & 31;
    const unsigned tag = header >> 5;
    if (!length || length > start - offset || !tag) return EdidStatus::BadExtension;
    if (tag == 2) {
      for (size_t i = 0; i < length; ++i) {
        const EdidStatus status = appendSvd(info, block[offset + i]);
        if (status != EdidStatus::Ok) return status;
      }
    } else if (tag == 3) {
      const EdidStatus status = parseHdmiVsdb(info, block + offset, length);
      if (status != EdidStatus::Ok) return status;
    }
    offset += length;
  }
  return EdidStatus::Ok;
}

inline EdidStatus appendDisplayIdTiming(EdidInfo &info, const uint8_t *dtd, bool type7, unsigned revision)
{
  const uint32_t clock = 1u + (uint32_t(dtd[0]) | (uint32_t(dtd[1]) << 8) | (uint32_t(dtd[2]) << 16));
  // Width and total validation below also rejects zero from a 65536 encoded field.
  const DpTiming timing{
    clock * (type7 ? 1u : 10u),
    uint16_t(le16(dtd + 4) + 1), uint16_t(le16(dtd + 6) + 1),
    uint16_t((le16(dtd + 8) & 0x7fff) + 1), uint16_t(le16(dtd + 10) + 1),
    uint16_t(le16(dtd + 12) + 1), uint16_t(le16(dtd + 14) + 1),
    uint16_t((le16(dtd + 16) & 0x7fff) + 1), uint16_t(le16(dtd + 18) + 1),
    bool(dtd[9] & 0x80), bool(dtd[17] & 0x80), true, bool(dtd[3] & 0x10),
  };
  if (!validDpTiming(timing) || info.modeCount == EdidInfo::kMaxModes) return EdidStatus::BadTiming;
  const bool y420 = type7 && revision >= 2 && (dtd[3] & 0x80);
  info.modes[info.modeCount++] = {timing, 0, !y420};
  return EdidStatus::Ok;
}

inline EdidStatus parseDisplayIdTile(EdidTile &tile, const uint8_t *payload, size_t length)
{
  if (length != 22 || tile.columns) return EdidStatus::BadExtension;
  const uint8_t high = payload[3];
  tile.columns = uint8_t(1 + ((payload[1] >> 4) | ((high & 0xc0) >> 2)));
  tile.rows = uint8_t(1 + ((payload[1] & 15) | (high & 0x30)));
  tile.column = uint8_t((payload[2] >> 4) | ((high & 0x0c) << 2));
  tile.row = uint8_t((payload[2] & 15) | ((high & 3) << 4));
  if (tile.column >= tile.columns || tile.row >= tile.rows) return EdidStatus::BadExtension;
  tile.width = uint32_t(le16(payload + 4)) + 1;
  tile.height = uint32_t(le16(payload + 6)) + 1;
  tile.singleEnclosure = payload[0] & 0x80;
  for (size_t i = 0; i < sizeof tile.topologyId; ++i) tile.topologyId[i] = payload[13 + i];
  return EdidStatus::Ok;
}

inline EdidStatus parseDisplayId(EdidInfo &info, const uint8_t *block)
{
  const size_t end = 5 + size_t(block[2]); // DisplayID checksum follows the payload
  if (end >= kEdidBlockBytes - 1) return EdidStatus::BadExtension;
  unsigned checksum = 0;
  for (size_t i = 1; i <= end; ++i) checksum += block[i];
  if (checksum & 0xff) return EdidStatus::BadChecksum;
  if (!allZero(block + end + 1, kEdidBlockBytes - 2 - end)) return EdidStatus::BadExtension;

  const unsigned version = block[1] >> 4;
  for (size_t offset = 5; offset < end;) {
    if (end - offset < 3) return EdidStatus::BadExtension;
    const unsigned tag = block[offset], revision = block[offset + 1], length = block[offset + 2];
    offset += 3;
    if (length > end - offset) return EdidStatus::BadExtension;
    if ((version == 1 && tag == 0x03) || (version == 2 && tag == 0x22)) {
      const bool type7 = version == 2;
      const size_t stride = 20 + (type7 ? ((revision >> 4) & 7) : 0);
      if (!length || length % stride) return EdidStatus::BadExtension;
      for (size_t i = 0; i < length; i += stride) {
        const EdidStatus status = appendDisplayIdTiming(info, block + offset + i, type7, revision & 7);
        if (status != EdidStatus::Ok) return status;
      }
    } else if ((version == 1 && tag == 0x12) || (version == 2 && tag == 0x28)) {
      const EdidStatus status = parseDisplayIdTile(info.tile, block + offset, length);
      if (status != EdidStatus::Ok) return status;
    }
    offset += length;
  }
  return EdidStatus::Ok;
}

} // namespace edid_detail

// Expects exactly the declared blocks, including the base. Clears info on every failure.
inline EdidStatus parseEdid(const uint8_t *bytes, size_t length, EdidInfo &info)
{
  using namespace edid_detail;
  info = {};
  if (!bytes || length < kEdidBlockBytes || length > kEdidMaxBytes || length % kEdidBlockBytes)
    return EdidStatus::InvalidSize;
  if (size_t(bytes[126]) + 1 != length / kEdidBlockBytes) return EdidStatus::InvalidSize;

  if (!edidHeaderOk(bytes)) return EdidStatus::BadHeader;
  for (size_t offset = 0; offset < length; offset += kEdidBlockBytes)
    if (!edidChecksumOk(bytes + offset)) return EdidStatus::BadChecksum;
  if (bytes[18] != 1 || bytes[19] != 4) return EdidStatus::UnsupportedVersion;

  EdidInfo parsed;
  const unsigned pnp = (unsigned(bytes[8]) << 8) | bytes[9];
  if (pnp & 0x8000) return EdidStatus::BadManufacturer;
  for (unsigned i = 0; i < 3; ++i) {
    const unsigned letter = (pnp >> (10 - 5 * i)) & 31;
    if (!letter || letter > 26) return EdidStatus::BadManufacturer;
    parsed.manufacturer[i] = char('A' + letter - 1);
  }
  parsed.productCode = le16(bytes + 10);
  if (bytes[21] && bytes[22]) {
    parsed.widthMm = uint16_t(bytes[21] * 10);
    parsed.heightMm = uint16_t(bytes[22] * 10);
  }
  parsed.extensionCount = bytes[126];
  for (size_t i = 0; i < 4; ++i) {
    const uint8_t *descriptor = bytes + 54 + i * kDescriptorBytes;
    if (i && isDisplayDescriptor(descriptor)) continue;
    const EdidStatus status = appendDtd(parsed, descriptor);
    if (status != EdidStatus::Ok) return status;
  }
  unsigned displayIdVersion = 0, displayIdSectionsLeft = 0;
  for (size_t offset = kEdidBlockBytes; offset < length; offset += kEdidBlockBytes) {
    const uint8_t *block = bytes + offset;
    EdidStatus status = EdidStatus::Ok;
    if (!block[0]) return EdidStatus::BadExtension;
    if (block[0] == 2) status = parseCta(parsed, block);
    if (block[0] == 0x70) {
      if (!displayIdVersion) {
        if (!block[1] || !block[3]) return EdidStatus::BadExtension;
        displayIdVersion = block[1];
        displayIdSectionsLeft = block[4];
      } else {
        if (!displayIdSectionsLeft || block[1] != displayIdVersion || block[3] || block[4])
          return EdidStatus::BadExtension;
        --displayIdSectionsLeft;
      }
      status = parseDisplayId(parsed, block);
    }
    if (status != EdidStatus::Ok) return status;
  }
  if (displayIdSectionsLeft) return EdidStatus::BadExtension;
  info = parsed;
  return EdidStatus::Ok;
}

// Prefer detailed timings in EDID order, then VICs in advertisement order.
inline const EdidMode *preferredFittingMode(const EdidInfo &info, DpLink link)
{
  if (info.modeCount > EdidInfo::kMaxModes || info.tile.multiTile()) return nullptr;
  for (unsigned source = 0; source < 2; ++source) {
    for (size_t i = 0; i < info.modeCount; ++i) {
      const auto &mode = info.modes[i];
      if (unsigned(mode.vic != 0) == source && mode.rgb && dpStreamFits(mode.timing, link)) return &mode;
    }
  }
  return nullptr;
}

} // namespace pianodp
