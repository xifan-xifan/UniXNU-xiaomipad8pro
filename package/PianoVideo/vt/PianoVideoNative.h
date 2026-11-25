// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Private kernel boundary, not a user ABI. The descriptor and WC view name the
// same V4 arena ledger entry; neither the CPU pointer nor the IOVA is a PA.
#pragma once
#include <stdint.h>
class IOMemoryDescriptor;
namespace pianovideo { namespace codec { struct Config; } namespace native {
struct Layout {
  uint32_t width = 320, height = 240, stride = 384, scanlines = 256;
  uint32_t codedWidth = 320, codedHeight = 240, cropLeft = 0, cropTop = 0;
  uint32_t yOffset = 0, uvOffset = 98304, bytes = 147456;
  uint32_t codec = 0x61766331; // avc1 or hvc1
  uint32_t format = 0x34323076, bitdepth = 8; // 420v, linear NV12
};
struct Frame {
  IOMemoryDescriptor *memory = nullptr; // retained until frameRelease
  const volatile uint8_t *cpu = nullptr; // borrowed WC mapping, valid until release
  uint32_t length = 0, iova = 0;
  uint64_t token = 0;
  uint32_t index = ~0u;
  bool noOutput = false; // explicit firmware NOSHOW, still owns its RAW loan
  Layout layout;
};
} }
extern "C" {
bool piano_video_native_ready(void);
// Reserve the exclusive core without powering it. Native session IDs are fresh.
int piano_video_native_open(uint32_t *session);
// Metadata-only control path, before a successful VT StartSession reply. Opens
// HFI and starts input, with no AU or output. Close also handles zero input.
int piano_video_native_prepare(uint32_t session, const pianovideo::codec::Config *format);
// Worker-only. First submit requires an IDR and completes PSC/output setup;
// subsequent submissions return 1 for input back-pressure, without mutation.
int piano_video_native_submit(uint32_t session, const uint8_t *bytes, uint32_t size, uint64_t token,
                              const pianovideo::codec::Config *format);
// Always pump HFI, including while the guest owes Dbuf/Demt. 0=picture, 1=none.
int piano_video_native_poll(uint32_t session, pianovideo::native::Frame *frame);
int piano_video_native_frame_release(uint32_t session, pianovideo::native::Frame *frame);
void piano_video_native_cancel(uint32_t session);
int piano_video_native_drain(uint32_t session);
// 1=ACK+LAST and inputs returned, 0=pending, negative=error. Explicit no-output
// tokens are returned individually only after the ACK/LAST fence.
int piano_video_native_drain_status(uint32_t session, uint64_t *noOutputToken);
// Always attempts STOP/CLOSE and PAS/routes/TLB teardown. On unconfirmed quiesce
// all DMA and dependencies remain retained and subsequent opens are refused.
int piano_video_native_close(uint32_t session);
}
