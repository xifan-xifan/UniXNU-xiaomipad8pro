// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
#pragma once
#include <stdint.h>
#include "PianoVideoVTContract.h"

namespace pianovideo { namespace vtsurface {
// Shared checked copy used by the worker and host gradients. Source and guest
// rowBytes/plane offsets are independently validated; only active samples move.
inline bool copy(const native::Layout &l,uint64_t length,const uint8_t *src,
                 const vtcontract::Surface &surface,uint8_t *dst) {
  if(!src||!dst||!vtcontract::layout(l,length)||!vtcontract::surface(surface,l.width,l.height,l.format))return false;
  const uint32_t rowBytes=l.width*(l.bitdepth==10?2:1);
  for(unsigned p=0;p<2;++p)for(uint32_t y=0,rows=p?l.height/2:l.height;y<rows;++y)
    memcpy(dst+surface.plane[p].offset+uint64_t(y)*surface.plane[p].stride,
      src+(p?l.uvOffset:l.yOffset)+uint64_t(y)*l.stride,rowBytes);
  return true;
}
// A retained GPU import need not perform another CPU cache clean on reuse.
// Publish both written plane extents while their temporary CPU mapping is valid.
// Padding inside a plane extent is safe to clean, but is never overwritten.
template<class Publisher>
inline bool copyAndPublish(const native::Layout &l,uint64_t length,const uint8_t *src,
                           const vtcontract::Surface &surface,uint8_t *dst,Publisher &publisher) {
  if(!copy(l,length,src,surface,dst))return false;
  const uint64_t rowBytes=uint64_t(l.width)*(l.bitdepth==10?2:1);
  for(unsigned p=0;p<2;++p){
    const uint64_t rows=p?l.height/2:l.height;
    publisher.clean(dst+surface.plane[p].offset,(rows-1)*surface.plane[p].stride+rowBytes);
  }
  publisher.finish();
  return true;
}
// Native 27.3 external methods, not the similarly numbered trap ABI.
constexpr uint32_t WriteLock=2, WriteUnlock=3;
struct WriteRequest { uint32_t id,options,reserved; };
static_assert(sizeof(WriteRequest)==12,"IOSurface writable request ABI");
// The 27.3 lookup acquires a handle and a client-buffer reference. Its public
// framework drops these with trap4 then trap5; method56 then method1 are the
// equivalent external methods. Closing the user client also stops its task's
// creator authorization, so it is never a per-picture release operation.
constexpr uint32_t Invalidate = 56, Release = 1;
struct Lease {
  uint32_t id = 0;
  bool handle = false, buffer = false;
  bool held() const { return handle || buffer; }
  bool acquire(uint32_t value) {
    if (!value || held()) return false;
    id = value; handle = buffer = true; return true;
  }
  template<class Methods> bool dispose(Methods &methods) {
    // Attempt both even if invalidation fails. Keep failed obligations visible
    // until the worker's final client close; never overwrite them with a new ID.
    if (handle && methods.call(Invalidate, id) == 0) handle = false;
    if (buffer && methods.call(Release, id) == 0) buffer = false;
    if (!held()) id = 0;
    return !held();
  }
  void closed() { id = 0; handle = buffer = false; }
};
} }
