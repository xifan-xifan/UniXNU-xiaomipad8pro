// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Host substitutes for the kernel APIs used by the *real* VTHost translation
// unit. No alternative worker or cancellation implementation lives in the test.
#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
using task_t=void *;
using proc_t=void *;
using IOReturn=int;
using wait_result_t=int;
using thread_t=std::thread *;
constexpr int kIOReturnNotFound=-1,kIOReturnNotPermitted=-2,kIOReturnNoMemory=-3;
constexpr int kIOMapAnywhere=0,kIODirectionInOut=0,kIOExternalMethodArgumentsCurrentVersion=0,KERN_SUCCESS=0,THREAD_UNINT=0;
inline task_t kernel_task=nullptr;
struct IOLock {std::mutex mutex;std::condition_variable cv;};
inline IOLock *IOLockAlloc(){return new IOLock;}
inline void IOLockFree(IOLock *p){delete p;}
inline void IOLockLock(IOLock *p){p->mutex.lock();}
inline void IOLockUnlock(IOLock *p){p->mutex.unlock();}
inline void IOLockWakeup(IOLock *p,void *,bool){p->cv.notify_all();}
inline void IOLockSleep(IOLock *p,void *,int){std::unique_lock<std::mutex> l(p->mutex,std::adopt_lock);p->cv.wait(l);l.release();}
inline thread_local std::function<void()> beforeKernelSleep;
inline std::atomic<uint64_t> testClockMs{0};
inline void IOSleep(unsigned ms){if(beforeKernelSleep)beforeKernelSleep();std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
inline void *IOMalloc(size_t n){return std::malloc(n);}
inline void *IOMallocZero(size_t n){return std::calloc(1,n);}
inline void IOFree(void *p,size_t){std::free(p);}
inline void IOLog(const char *,...){}
inline void clock_get_uptime(uint64_t *t){
  const uint64_t ms=testClockMs.load();
  const auto ticks=ms?std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(ms)):
    std::chrono::steady_clock::now().time_since_epoch();
  *t=uint64_t(ticks.count());
}
inline void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns){*ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::duration(t)).count());}
inline thread_t current_thread(){return nullptr;}
inline void thread_set_thread_name(thread_t,const char *){}
inline void thread_terminate(thread_t){}
inline thread_local std::thread *kernelThreadJoinTarget=nullptr;
inline void thread_deallocate(thread_t t){
  if(kernelThreadJoinTarget)*kernelThreadJoinTarget=std::move(*t);
  else t->detach();
  delete t;
}
inline int kernel_thread_start(void (*f)(void *,wait_result_t),void *p,thread_t *t){*t=new std::thread([=]{f(p,0);});return 0;}
inline bool PE_parse_boot_argn(const char *,void *,size_t){return false;}
extern "C" inline int proc_selfpid(){return 1;}
extern "C" inline task_t proc_task(proc_t){return nullptr;}
inline int proc_name(int,void *,int){return 0;}
inline proc_t proc_find(int){return nullptr;}
inline void proc_rele(proc_t){}
// Hashes are diagnostic only. Allow a stub digest when driving wire admission.
inline thread_local bool stubAdmissionHash=false;
struct SHA256_CTX {};
inline void SHA256_Init(SHA256_CTX *){assert(stubAdmissionHash);}
inline void SHA256_Update(SHA256_CTX *,const void *data,size_t bytes){assert(stubAdmissionHash&&data&&bytes);}
inline void SHA256_Final(uint8_t *digest,SHA256_CTX *){assert(stubAdmissionHash);std::memset(digest,0,32);}
class OSObject {
  std::atomic<unsigned> refs{1};
public:
  virtual ~OSObject()=default;
  void retain(){++refs;}
  void release(){if(!--refs)delete this;}
};
#define OSSafeReleaseNULL(p) do {if(p){(p)->release();(p)=nullptr;}} while(0)
#define OSDynamicCast(T,p) dynamic_cast<T *>(p)
class OSDictionary:public OSObject {};
class OSString:public OSObject {
public:
  const char *getCStringNoCopy(){return "";}
  unsigned getLength(){return 0;}
};
class OSIterator:public OSObject {public:OSObject *getNextObject(){return nullptr;}};
struct IOExternalMethodArguments {
  unsigned version=0,selector=0,scalarInputCount=0;
  const uint64_t *scalarInput=nullptr;
  const void *structureInput=nullptr;
  void *structureOutput=nullptr;
  size_t structureInputSize=0,structureOutputSize=0;
};
using IOExternalMethodArgumentsOpaque=IOExternalMethodArguments;
class IOUserClient:public OSObject {
public:
  virtual IOReturn callExternalMethod(uint32_t,IOExternalMethodArguments *){std::abort();}
  virtual IOReturn clientClose(){return 0;}
  OSObject *copyProperty(const char *){return nullptr;}
};
class IOUserClient2022:public IOUserClient {
public:virtual IOReturn externalMethod(uint32_t s,IOExternalMethodArgumentsOpaque *a){return callExternalMethod(s,a);}
};
class IOService:public OSObject {
public:
  static OSDictionary *nameMatching(const char *){return nullptr;}
  static IOService *waitForMatchingService(OSDictionary *,uint64_t){return nullptr;}
  static IOService *copyMatchingService(OSDictionary *){return nullptr;}
  IOReturn newUserClient(task_t,task_t,unsigned,void *,IOUserClient **){return -1;}
  OSIterator *getClientIterator(){return nullptr;}
  void setProperty(const char *,const char *){}
};
class IOMemoryMap:public OSObject {
public:uintptr_t getVirtualAddress(){std::abort();}uint64_t getLength(){std::abort();}
};
class IOMemoryDescriptor:public OSObject {
public:
  static IOMemoryDescriptor *withAddressRange(uint64_t,uint64_t,int,task_t){std::abort();}
  uint64_t getLength(){std::abort();}
  int prepare(){std::abort();}
  int complete(){std::abort();}
  IOMemoryMap *createMappingInTask(task_t,uint64_t,int,uint64_t=0,uint64_t=0){std::abort();}
};
