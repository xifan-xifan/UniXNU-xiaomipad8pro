// SPDX-License-Identifier: MIT
// Copyright (c) 2026, Leo Zhang.
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <notify.h>
#include <dlfcn.h>
#include <unistd.h>

struct NmbDevice {};
struct NmbBuffer {void *context;void (*release)(void *,void *,uint64_t);void *base;uint64_t bytes;};
static NmbDevice mockDevice,*gDevice=&mockDevice;
static std::string gLoaderPath;
static thread_local std::string mockError;
static std::atomic<uint64_t> mockNow{1000000},mockImports{0},mockMade{0},mockReleased{0},mockAttempts{0};
static std::atomic<int> failImports{0};
static std::atomic<bool> reenterCapacity{false};
static std::mutex barrierLock;
static std::condition_variable barrierCV;
static bool racing=false;
static unsigned arrived=0;
static std::atomic<uint64_t> gTrimmedBlocks,gSurfaceImportsMade,gSurfaceImportsReleased,gNoCopyAll,gNoCopyMade,gNoCopyReleased;
static uint64_t nowUs(){return mockNow;}
static const char *nmb_last_error(){return mockError.c_str();}
static NSString *lastError(){return [NSString stringWithUTF8String:nmb_last_error()];}
#define NBLOG(...) do {} while(0)
#define NBNOTE(...) do {} while(0)
static bool nmb_device_memory_counts(NmbDevice *,uint64_t out[10]) {
 uint64_t values[10]={8,3,16,32,mockImports.load(),999999,1,1,1,0};std::copy(values,values+10,out);return true;
}
static size_t nmb_device_hold_back_memory(NmbDevice *,bool);
static uint32_t nmb_device_trim_memory(NmbDevice *,uint32_t);
static size_t nmb_device_release_kept_memory(NmbDevice *);
static NmbBuffer *nmb_buffer_new_no_copy_releasing(NmbDevice *,void *,uint64_t,uint64_t,void (*)(void *,void *,uint64_t),void *);
static void nmb_buffer_release(NmbBuffer *);
static void releaseSurface(void *context,void *,uint64_t){gSurfaceImportsReleased++;CFRelease((CFTypeRef)context);}
@interface NBBuffer : NSObject
@property(nonatomic) NmbBuffer *handle;
@property(nonatomic) uint64_t options;
@property(nonatomic) IOSurfaceRef importedSurface;
@property(nonatomic) void *wrapped;
@end
@implementation NBBuffer
- (void)dealloc {nmb_buffer_release(_handle);}
@end
// A private notification stub stores the actual registered frontend handler. No cross-process host event is posted.
static dispatch_queue_t mockNotifyQueue;
static void (^mockNotifyHandler)(int);
static uint64_t mockPosts;
static uint32_t test_notify_register_dispatch(const char *,int *token,dispatch_queue_t queue,void (^handler)(int)) {
 *token=7;mockNotifyQueue=queue;mockNotifyHandler=handler;return NOTIFY_STATUS_OK;
}
static uint32_t test_notify_post(const char *){mockPosts++;return NOTIFY_STATUS_OK;}
#define notify_register_dispatch test_notify_register_dispatch
#define notify_post test_notify_post
// System pressure events are not inputs to a deterministic host test; all pressure is injected explicitly below.
#define dispatch_source_create(...) nullptr
#include "frontend_pressure_actual.inc"
#undef dispatch_source_create
#undef notify_post
#undef notify_register_dispatch
static void releaseBlock(void *context,void *pointer,uint64_t length) {
 gNoCopyReleased++;
 void (^deallocator)(void *,NSUInteger)=(__bridge_transfer void (^)(void *,NSUInteger))context;deallocator(pointer,(NSUInteger)length);
}
@interface TestDevice : NSObject
@end
@implementation TestDevice
#include "frontend_pressure_nocopy.inc"
@end

static size_t nmb_device_hold_back_memory(NmbDevice *,bool on) {
 native_bridge_metal_trim_surface_imports(); // deadlocks if the caller holds the actual import lock
 if(on && reenterCapacity.exchange(false)) {
  unsigned calls=0;
  void *nested=allocationWithReclaim("deallocator-reentry",[&]()->void *{calls++;mockError="buffer: OutOfMemory(Device)";return nullptr;});
  assert(!nested && calls==1); // reclaim must not sync to its own queue or recursively retry
 }
 return 0;
}
static uint32_t nmb_device_trim_memory(NmbDevice *,uint32_t){native_bridge_metal_trim_surface_imports();return 0;}
static size_t nmb_device_release_kept_memory(NmbDevice *){native_bridge_metal_trim_surface_imports();return 0;}
static NmbBuffer *nmb_buffer_new_no_copy_releasing(NmbDevice *,void *base,uint64_t bytes,uint64_t,void (*release)(void *,void *,uint64_t),void *context) {
 mockAttempts++;
 native_bridge_metal_trim_surface_imports(); // exercises actual allocation-outside-import-lock requirement
 {
  std::unique_lock<std::mutex> lock(barrierLock);
  if(racing){arrived++;barrierCV.notify_all();barrierCV.wait(lock,[]{return !racing;});}
 }
 if(failImports.load()>0){failImports--;mockError="no-copy buffer: OutOfMemory(Device)";return nullptr;}
 mockMade++;mockImports+=bytes;return new NmbBuffer{context,release,base,bytes};
}
static void nmb_buffer_release(NmbBuffer *buffer) {
 if(!buffer)return;
 native_bridge_metal_trim_surface_imports(); // final runtime release also must happen outside the import lock
 mockReleased++;mockImports-=buffer->bytes;if(buffer->release)buffer->release(buffer->context,buffer->base,buffer->bytes);delete buffer;
}
static IOSurfaceRef makeSurface() {
 IOSurfaceRef result=IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@16,(id)kIOSurfaceHeight:@16,
  (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@64,(id)kIOSurfaceAllocSize:@4096});
 assert(result);return result;
}
static void policyTests() {
 HeldImportPolicy<int,int> cache;std::vector<int> gone;
 cache.hold(1,1,20,0,64,gone);cache.hold(2,2,30,1,64,gone);cache.hold(1,1,20,2,64,gone);
 cache.hold(3,3,40,3,64,gone);assert(cache.bytes()==60 && cache.count()==2 && gone==std::vector<int>{2});gone.clear();
 cache.hold(4,4,65,4,64,gone);cache.hold(5,5,0,4,64,gone);assert(cache.count()==2 && gone.empty());
 cache.prune(12,64,10,false,[](int){return false;},gone);assert(gone==std::vector<int>{1} && cache.bytes()==40);gone.clear();
 cache.prune(13,64,10,false,[](int){return false;},gone);assert(gone==std::vector<int>{3} && cache.bytes()==0);gone.clear();
 cache.hold(6,6,64,20,64,gone);cache.prune(20,64,10,false,[](int){return true;},gone);assert(cache.count()==0 && gone==std::vector<int>{6});
 // UINT64_MAX sizing must not overflow the budget arithmetic.
 gone.clear();cache.hold(7,7,UINT64_MAX-1,30,UINT64_MAX,gone);cache.hold(8,8,2,31,UINT64_MAX,gone);
 assert(cache.bytes()==2 && gone==std::vector<int>{7});
 struct Token {bool *inside;unsigned *drops;~Token(){assert(!*inside);++*drops;}};
 bool locked=false;unsigned drops=0;auto live=std::make_shared<Token>(Token{&locked,&drops});drops=0;
 HeldImportPolicy<int,std::shared_ptr<Token>> owned;std::vector<std::shared_ptr<Token>> retired;
 owned.hold(1,live,1,0,1,retired);locked=true;owned.prune(0,0,1,true,[](auto){return false;},retired);
 locked=false;retired.clear();assert(drops==0);live.reset();assert(drops==1);
 ImportPressureWindow pressure;assert(!pressure.active(10));pressure.raise(10);assert(pressure.active(5000009));
 pressure.system=true;assert(pressure.active(9000000));pressure.system=false;assert(!pressure.active(9000000));
 pressure.raise(10000000);pressure.raise(14000000);assert(pressure.active(18000000));assert(!pressure.active(19000000));
 assert(importCapacityError("texture: OutOfMemory(Device)"));assert(importCapacityError("OutOfMemory(Host)"));
 assert(importCapacityError("VK_ERROR_OUT_OF_DEVICE_MEMORY"));assert(importCapacityError("ERROR_OUT_OF_HOST_MEMORY"));
 assert(!importCapacityError("DeviceLost(OutOfMemory(Device))"));assert(!importCapacityError("unsupported host import"));
 assert(!importCapacityError("invalid shape exceeds limit"));assert(!importCapacityError(nullptr));
 puts("PASS policy: byte/LRU/idle/dead/overflow/lifetime/hysteresis/capacity classification");
}
static void retryTests() {
 assert(runtimeAllocatedBytes()==56);unsigned attempts=0;uint64_t retries=gCapacityRetries;
 assert(allocationWithReclaim("success",[&]()->void *{attempts++;return (void *)1;})==(void *)1 && attempts==1);
 attempts=0;assert(!allocationWithReclaim("invalid",[&]()->void *{attempts++;mockError="unsupported shape";return nullptr;}));
 assert(attempts==1 && gCapacityRetries==retries);
 attempts=0;assert(allocationWithReclaim("recover",[&]()->void *{attempts++;mockError="texture: OutOfMemory(Device)";return attempts==2?(void *)1:nullptr;}));
 assert(attempts==2 && gCapacityRecovered==1 && gNoHoldImports);
 attempts=0;assert(!allocationWithReclaim("permanent",[&]()->void *{attempts++;mockError="texture: OutOfMemory(Device)";return nullptr;}));
 assert(attempts==2 && gCapacityRetryFailures==1 && mockPosts==1);
 reenterCapacity=true;dispatch_sync(importPressureQueue(),^{reclaimGpuMemory("reentry-test");});assert(!reenterCapacity);
 auto delivered=gGpuReclaimDelivered.load();dispatch_sync(mockNotifyQueue,^{mockNotifyHandler(7);});assert(gGpuReclaimDelivered==delivered+1);
 puts("PASS retry: once only, invalid errors untouched, pressure delivery, coalesced requests, deallocator reentry");
}
static void noCopyTests() {
 TestDevice *device=[TestDevice new];assert([device currentAllocatedSize]==56);__block unsigned callbacks=0;char page[4096]={};void *expected=page;
 void (^deallocator)(void *,NSUInteger)=^(void *p,NSUInteger n){assert(p==expected && n==4096);callbacks++;};
 failImports=1;uint64_t attempts=mockAttempts;
 @autoreleasepool {
  NBBuffer *buffer=(NBBuffer *)[device newBufferWithBytesNoCopy:page length:sizeof page options:0 deallocator:deallocator];
  assert(buffer && buffer.handle && mockAttempts==attempts+2 && callbacks==0);
 }
 assert(callbacks==1 && gNoCopyMade==1 && gNoCopyReleased==1);
 failImports=2;attempts=mockAttempts;
 @autoreleasepool {assert(![device newBufferWithBytesNoCopy:page length:sizeof page options:0 deallocator:deallocator]);}
 assert(mockAttempts==attempts+2 && callbacks==1 && gNoCopyMade==1 && gNoCopyReleased==1);
 @autoreleasepool {
  NBBuffer *first=(NBBuffer *)[device newBufferWithBytesNoCopy:page length:sizeof page options:0 deallocator:deallocator];
  NBBuffer *second=(NBBuffer *)[device newBufferWithBytesNoCopy:page length:sizeof page options:0 deallocator:deallocator];
  assert(first.handle && second.handle && first.handle!=second.handle && mockImports==8192 && callbacks==1);
 }
 assert(callbacks==3 && mockImports==0);
 puts("PASS actual no-copy method: failed attempts never invoke caller deallocator; successful context releases once");
}
struct ImportResult {NBBuffer *buffer;NSString *error;};
static void concurrentImports(IOSurfaceRef surface,ImportResult (&results)[2]) {
 const auto joined=gSurfaceImportJoined.load();
 {
  std::lock_guard<std::mutex> lock(barrierLock);racing=true;arrived=0;
 }
 auto import=[&](unsigned index){@autoreleasepool{
  NSString *failure=nil;
  results[index].buffer=surfaceBuffer(surface,IOSurfaceGetBaseAddress(surface),4096,&failure);
  results[index].error=failure;
 }};
 std::thread first(import,0);
 {
  std::unique_lock<std::mutex> lock(barrierLock);
  assert(barrierCV.wait_for(lock,std::chrono::seconds(5),[]{return arrived==1;}));
 }
 std::thread second(import,1);
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while(gSurfaceImportJoined==joined && std::chrono::steady_clock::now()<deadline)
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
 {
  std::lock_guard<std::mutex> lock(barrierLock);assert(gSurfaceImportJoined==joined+1 && arrived==1);racing=false;
 }
 barrierCV.notify_all();first.join();second.join();
}
static void importTests() {
 IOSurfaceRef surface=makeSurface();const CFIndex retains=CFGetRetainCount(surface);uint64_t released=mockReleased;
 // Actual surfaceBuffer failed attempt -> one retry. Failed callback is not invoked; success owns exactly one surface retain.
 failImports=1;const uint64_t initialAttempts=mockAttempts;@autoreleasepool {
  NBBuffer *live=surfaceBuffer(surface,IOSurfaceGetBaseAddress(surface),4096);
  assert(live && mockAttempts==initialAttempts+2 && CFGetRetainCount(surface)==retains+1);
  assert(mockReleased==released && gHeldImportCount==0); // pressure stops new optional holds
  trimHeldImports(true);assert(live.handle && mockReleased==released);
 }
 assert(mockReleased==released+1 && CFGetRetainCount(surface)==retains && runtimeAllocatedBytes()==56);
 failImports=2;uint64_t attempts=mockAttempts;
 @autoreleasepool {assert(!surfaceBuffer(surface,IOSurfaceGetBaseAddress(surface),4096));}
 assert(mockAttempts==attempts+2 && CFGetRetainCount(surface)==retains && gSurfaceImportFailures==1);
 dispatch_sync(importPressureQueue(),^{gNoHoldImports=false;});
 ImportResult results[2]={};released=mockReleased;uint64_t made=mockMade;
 attempts=mockAttempts;concurrentImports(surface,results);
 assert(results[0].buffer && results[0].buffer==results[1].buffer);
 assert(mockAttempts==attempts+1 && mockMade==made+1 && mockReleased==released && gHeldImportCount==1 && gHeldImportBytes==4096);
 trimHeldImports(true);assert(gHeldImportCount==0 && mockReleased==released);
 results[0].buffer=nil;results[1].buffer=nil;
 assert(mockReleased==released+1 && CFGetRetainCount(surface)==retains && mockImports==0);
 failImports=2;attempts=mockAttempts;concurrentImports(surface,results);
 assert(!results[0].buffer && !results[1].buffer && mockAttempts==attempts+2);
 assert([results[0].error isEqualToString:@"no-copy buffer: OutOfMemory(Device)"] && [results[0].error isEqualToString:results[1].error]);
 assert(CFGetRetainCount(surface)==retains && mockImports==0);
 assert(gNoHoldImports);
 attempts=mockAttempts;const auto hits=gSurfaceImportHits.load();
 for(unsigned frame=0;frame<4;frame++) {
  @autoreleasepool {
   NBBuffer *first=surfaceBuffer(surface,IOSurfaceGetBaseAddress(surface),4096);
   NBBuffer *second=surfaceBuffer(surface,IOSurfaceGetBaseAddress(surface),4096);
   assert(first && first==second && mockImports==4096);
  }
  assert(CFGetRetainCount(surface)==retains && mockImports==0 && gHeldImportCount==0);
 }
 assert(mockAttempts==attempts+4 && gSurfaceImportHits==hits+4);
 // Optional cache ownership alone is sufficient for a pressure reclaim to return measured current backing.
 dispatch_sync(importPressureQueue(),^{gNoHoldImports=false;});
 @autoreleasepool {assert(surfaceBuffer(surface,IOSurfaceGetBaseAddress(surface),4096));}
 assert(gHeldImportCount==1 && mockImports==4096);
 uint64_t freed=gGpuReclaimFreed;
 dispatch_sync(importPressureQueue(),^{reclaimGpuMemory("held-only-test");});
 assert(gHeldImportCount==0 && mockImports==0 && gGpuReclaimFreed==freed+4096 && gNoHoldImports);
 CFRelease(surface);assert(gSurfaceImportsMade==gSurfaceImportsReleased);
 puts("PASS actual imports: concurrent same-surface calls allocate once, share the retry result, and release outside the lock");
}
int main(){@autoreleasepool{policyTests();retryTests();noCopyTests();importTests();}return 0;}
