// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host contract test for the IOUserClient OOL structure path. The kernel
// receives an IOExternalMethodArguments block whose large input/output sides
// are descriptors; this fake keeps the same readBytes/writeBytes contract.
//   c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. iouc_host.cpp -o /tmp/iouc_host && /tmp/iouc_host
#include "PianoWifiUcTransport.h"
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <vector>

struct FakeDescriptor {
  std::vector<uint8_t> bytes;
  unsigned prepareAttempts = 0;
  unsigned prepares = 0;
  unsigned completes = 0;
  bool prepared = false;
  bool refusePrepare = false;
  int prepare(uint32_t)
  {
    ++prepareAttempts;
    if (refusePrepare) return -1;
    ++prepares;
    prepared = true;
    return 0;
  }
  int complete(uint32_t)
  {
    assert(prepared);
    ++completes;
    prepared = false;
    return 0;
  }
  uint64_t readBytes(uint64_t offset, void *out, uint64_t length)
  {
    if (!prepared) return 0;
    if (offset > bytes.size() || length > bytes.size() - offset) return 0;
    for (uint64_t i = 0; i < length; ++i) static_cast<uint8_t *>(out)[i] = bytes[size_t(offset + i)];
    return length;
  }
  uint64_t writeBytes(uint64_t offset, const void *in, uint64_t length)
  {
    if (!prepared) return 0;
    if (offset > bytes.size() || length > bytes.size() - offset) return 0;
    for (uint64_t i = 0; i < length; ++i) bytes[size_t(offset + i)] = static_cast<const uint8_t *>(in)[i];
    return length;
  }
};

struct FakeArguments {
  const void *structureInput = nullptr;
  uint32_t structureInputSize = 0;
  FakeDescriptor *structureInputDescriptor = nullptr;
  void *structureOutput = nullptr;
  uint32_t structureOutputSize = 0;
  FakeDescriptor *structureOutputDescriptor = nullptr;
  uint32_t structureOutputDescriptorSize = 0;
};

int main()
{
  using pianowifi::a80211::DescriptorPreparation;
  using pianowifi::a80211::kUcGetCmd;
  using pianowifi::a80211::kUcSetCmd;
  using pianowifi::a80211::readDescriptor;
  using pianowifi::a80211::ucCommandAccepted;
  using pianowifi::a80211::writeDescriptor;
  constexpr uint32_t requestBytes = 0x3c30;
  constexpr uint32_t replyBytes = 0x2720;

  // Match the exact 40-byte request header that 27.3's IO80211 wrapper puts
  // in the selector-0 structure.  The upper direction/size bits are part of
  // the command and must survive the user-client decode.
  std::vector<uint8_t> request(requestBytes, 0);
  const uint32_t builtGet = kUcGetCmd;
  std::memcpy(request.data(), &builtGet, sizeof builtGet);
  uint32_t decodedGet = 0;
  std::memcpy(&decodedGet, request.data(), sizeof decodedGet);
  assert(decodedGet == 0xc02869c9u && decodedGet == kUcGetCmd);
  assert(ucCommandAccepted(decodedGet));
  assert(!ucCommandAccepted(0x000069c9u));
  const uint32_t builtSet = kUcSetCmd;
  assert(builtSet == 0x802869c8u);

  FakeDescriptor input{std::vector<uint8_t>(requestBytes, 0x5a)};
  FakeArguments args;
  args.structureInputDescriptor = &input;
  assert(args.structureInput == nullptr && args.structureInputSize == 0);
  std::vector<uint8_t> staged(requestBytes, 0);
  assert(!readDescriptor(args.structureInputDescriptor, input.bytes.size(), staged.data(), requestBytes));
  {
    DescriptorPreparation<FakeDescriptor> prepared(args.structureInputDescriptor, 2 /* kIODirectionOut */);
    assert(prepared.ok());
    assert(readDescriptor(args.structureInputDescriptor, input.bytes.size(), staged.data(), requestBytes));
  }
  assert(input.prepares == 1 && input.completes == 1 && !input.prepared);
  assert(staged[0] == 0x5a && staged[requestBytes - 1] == 0x5a);

  FakeDescriptor shortInput{std::vector<uint8_t>(requestBytes - 1, 0)};
  assert(!readDescriptor(&shortInput, shortInput.bytes.size(), staged.data(), requestBytes));
  assert(shortInput.prepares == 0 && shortInput.completes == 0);

  FakeDescriptor refusedInput{std::vector<uint8_t>(requestBytes, 0)};
  refusedInput.refusePrepare = true;
  {
    DescriptorPreparation<FakeDescriptor> refused(&refusedInput, 2 /* kIODirectionOut */);
    assert(!refused.ok());
    assert(!readDescriptor(&refusedInput, refusedInput.bytes.size(), staged.data(), requestBytes));
  }
  assert(refusedInput.prepareAttempts == 1 && refusedInput.prepares == 0 && refusedInput.completes == 0);

  FakeDescriptor output{std::vector<uint8_t>(replyBytes, 0)};
  args.structureOutputDescriptor = &output;
  args.structureOutputDescriptorSize = replyBytes;
  const uint8_t marker[4] = {0x22, 0x11, 0xaa, 0x55};
  {
    DescriptorPreparation<FakeDescriptor> prepared(args.structureOutputDescriptor, 1 /* kIODirectionIn */);
    assert(prepared.ok());
    assert(writeDescriptor(args.structureOutputDescriptor, args.structureOutputDescriptorSize, 0x271c, marker, sizeof marker));
    assert(!writeDescriptor(&output, args.structureOutputDescriptorSize, 0x271f, marker, sizeof marker));
  }
  assert(output.prepares == 1 && output.completes == 1 && !output.prepared);
  assert(output.bytes[0x271c] == 0x22 && output.bytes[0x271f] == 0x55);
  assert(!writeDescriptor(&output, args.structureOutputDescriptorSize, 0x271f, marker, sizeof marker));
  std::puts("iouc_host: all checks passed");
  return 0;
}
