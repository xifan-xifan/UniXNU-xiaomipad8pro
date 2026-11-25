// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Leo Zhang
// Compiles the actual patched backend functions over a fault-injecting peer.
// No shader, GPU, WindowServer or capture fallback is simulated as a PASS.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <pthread.h>

#define TU_HAS_DARWIN 1
#define MESA_TRACE_FUNC() ((void)0)
#define TU_RMV(...) ((void)0)
using VkResult = int;
using kern_return_t = int;
using VkMemoryPropertyFlags = unsigned;
constexpr int VK_SUCCESS=0, VK_ERROR_OUT_OF_DEVICE_MEMORY=-2, VK_ERROR_DEVICE_LOST=-4,
              VK_ERROR_INVALID_EXTERNAL_HANDLE=-1000072003,VK_ERROR_OUT_OF_HOST_MEMORY=-1,VK_ERROR_UNKNOWN=-13;
constexpr int KERN_SUCCESS=0, kIOReturnNoMemory=4, kIOReturnNoResources=5;
enum tu_bo_alloc_flags {TU_BO_ALLOC_NO_FLAGS=0, TU_BO_ALLOC_GPU_READ_ONLY=1,
                       TU_BO_ALLOC_REPLAYABLE=2, TU_BO_ALLOC_INTERNAL_RESOURCE=4,
                       TU_BO_ALLOC_IMPLICIT_SYNC=8};
enum {NativeAdrenoMethodBoAlloc=1, NativeAdrenoMethodBoImport=2, NativeAdrenoMethodBoMap=3,
      NativeAdrenoMethodBoFree=4, NativeAdrenoParamsHostImport=1};
struct NativeAdrenoAbiBoAlloc {uint64_t bytes; unsigned align_log2,contiguous;};
struct NativeAdrenoAbiBoImport {uint64_t address,bytes;};
struct NativeAdrenoAbiBoAllocReply {uint64_t handle;};
struct NativeAdrenoAbiBoMap {uint64_t handle,iova; unsigned permissions,writeback,shareability;};
struct NativeAdrenoAbiBoMapReply {};
struct NativeAdrenoAbiBoFree {uint64_t handle;};
struct vk_object_base {};
struct tu_memory_heap {uint64_t used=0,size=0;};
struct Physical {tu_memory_heap heap;};
struct tu_device {void *darwin_retire=nullptr; Physical *physical_device=nullptr; int vk=0;std::mutex vma_mutex;bool bo_sizes=false;};
static void mtx_lock(std::mutex *m){m->lock();}
static void mtx_unlock(std::mutex *m){m->unlock();}
struct tu_sparse_vma {struct {uint64_t iova; bool backs_lazy_bo;} msm;};
struct tu_bo {
   uint32_t gem_handle=0;
   uint64_t size=0,iova=0;
   const char *name=nullptr;
   int refcnt=0;
   bool implicit_sync=false;
   vk_object_base *base=nullptr;
   uint64_t unique_id=0,darwin_handle=0,darwin_alloc_size=0;
   uint32_t darwin_token=0;
   bool darwin_read_only=false;
   void *map=nullptr;
   bool never_unmap=false;
};
struct darwin_cached_bo {uint64_t handle,size,iova; void *mapping; uint32_t token;};
struct darwin_retire_state {
   pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
   tu_device *dev=nullptr;
   VkResult failure=VK_SUCCESS;
   struct {uint64_t used=0;} va;
   struct {uint64_t retries=0,retry_wins=0;} cache_stats;
};
static struct {struct {unsigned flags=NativeAdrenoParamsHostImport;} params;} dw;
template<class T> static T p_atomic_read(T *p){return *p;}
template<class T,class U> static void p_atomic_set(T *p,U v){*p=v;}
template<class T> static T p_atomic_inc_return(T *p){return ++*p;}
template<class T> static void p_atomic_inc(T *p){++*p;}
template<class T,class U> static T p_atomic_add_return(T *p,U v){return *p+=v;}
template<class T,class U> static void p_atomic_add(T *p,U v){*p+=v;}
static void mesa_loge(const char *,...) {}
static VkResult vk_error(tu_device *,VkResult r){return r;}
static VkResult vk_errorf(tu_device *,VkResult r,const char *,...){return r;}
static VkResult vk_device_set_lost(int *,const char *,...){return VK_ERROR_DEVICE_LOST;}
static bool debug_get_bool_option(const char *,bool){return false;}
static bool darwin_small(uint64_t n){return n<=1048576;}
static uint64_t darwin_size_class(uint64_t n){return n;}
static uint64_t darwin_cache_small_budget(){return 0;}
static uint64_t darwin_cache_budget(){return 0;}
static bool darwin_past_bo_limit(tu_device *,uint64_t,const char *){return false;}
static bool darwin_cache_get(darwin_retire_state *,uint64_t,bool,bool,darwin_cached_bo *){return false;}
static const char *tu_debug_bos_add(tu_device *,uint64_t,const char *n){return n;}
static void tu_dump_bo_init(tu_device *,tu_bo *){}
static void darwin_note_large_bo(tu_device *,uint64_t){}

static uint64_t nowNs=2000000000ull,waitUs=0,vaUsed=0,nextId=1;
static unsigned notifyCount=0,reapCount=0,flushCount=0,nativeCalls[5]={};
static unsigned failures[5]={},vaFailures=0;
static int injectedError=kIOReturnNoMemory;
static bool cleanupFails=false,retireFails=false,freeHeapOnReclaim=false;
static uint64_t idleBytes=0,liveBytes=0,pendingBytes=0;
static uint32_t darwin_memory_pressure=0;
static std::map<uint64_t,uint64_t> native;
static std::map<uint64_t,tu_bo> bos;
static std::vector<std::string> stages;
static uint64_t os_time_get_nano(){return nowNs;}
static void assertUnlocked(darwin_retire_state *r) {
   assert(pthread_mutex_trylock(&r->lock)==0);
   pthread_mutex_unlock(&r->lock);
}
static int simulated_usleep(unsigned us){waitUs+=us;nowNs+=us*1000ull;return 0;}
#define usleep simulated_usleep
static void darwin_capacity_snapshot(tu_device *d,const char *s,uint64_t){
   assertUnlocked((darwin_retire_state *)d->darwin_retire);stages.emplace_back(s);
}
static void darwin_ask_reclaim(darwin_retire_state *r){assertUnlocked(r);++notifyCount;}
static VkResult darwin_reap_locked(tu_device *,darwin_retire_state *){
   ++reapCount;return retireFails?VK_ERROR_DEVICE_LOST:VK_SUCCESS;
}
static bool darwin_cache_flush(tu_device *d,darwin_retire_state *){
   ++flushCount;idleBytes=0;
   if(freeHeapOnReclaim)d->physical_device->heap.used=0;
   return true;
}
static VkResult darwin_collect_retired(tu_device *){return VK_SUCCESS;}
static VkResult darwin_allocate_iova(tu_device *,uint64_t size,uint64_t,tu_bo_alloc_flags,uint64_t *address){
   if(vaFailures){--vaFailures;return VK_ERROR_OUT_OF_DEVICE_MEMORY;}
   *address=0x20000000+vaUsed;vaUsed+=size;return VK_SUCCESS;
}
static void darwin_free_iova(tu_device *,uint64_t,uint64_t size){assert(vaUsed>=size);vaUsed-=size;}
static kern_return_t dw_call(unsigned method,const void *in,size_t,void *out,size_t){
   ++nativeCalls[method];
   if(method==NativeAdrenoMethodBoFree && cleanupFails)return 7;
   if(failures[method]){--failures[method];return injectedError;}
   if(method==NativeAdrenoMethodBoAlloc || method==NativeAdrenoMethodBoImport){
      uint64_t size=method==NativeAdrenoMethodBoAlloc?((const NativeAdrenoAbiBoAlloc *)in)->bytes:
                    ((const NativeAdrenoAbiBoImport *)in)->bytes;
      ((NativeAdrenoAbiBoAllocReply *)out)->handle=nextId;native[nextId++]=size;
   } else if(method==NativeAdrenoMethodBoFree){
      auto id=((const NativeAdrenoAbiBoFree *)in)->handle;assert(native.erase(id)==1);
   }
   return KERN_SUCCESS;
}
static tu_bo *tu_device_lookup_bo(tu_device *,uint32_t id){return &bos[id];}
static void tu_bo_finish(tu_device *d,tu_bo *b){
   assert(native.erase(b->darwin_handle)==1);darwin_free_iova(d,b->iova,b->size);b->gem_handle=0;
}
struct tu_device_memory {
   bool lazy=true,lazy_initialized=false;
   tu_bo *bo=nullptr;
   uint64_t size=4096;
   struct {vk_object_base base;} vk;
   tu_sparse_vma lazy_vma={{0x20000000,false}};
   std::mutex lazy_mutex;
};
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
static VkResult darwin_bo_init(tu_device *,vk_object_base *,tu_bo **,uint64_t,uint64_t,
                              VkMemoryPropertyFlags,tu_bo_alloc_flags,tu_sparse_vma *,const char *);
static VkResult tu_bo_init_new_explicit_iova(tu_device *d,vk_object_base *base,tu_bo **b,
   uint64_t size,uint64_t address,VkMemoryPropertyFlags props,tu_bo_alloc_flags flags,tu_sparse_vma *lazy,const char *name){
   return darwin_bo_init(d,base,b,size,address,props,flags,lazy,name);
}
// Generated from 0012 applied to the real Mesa source; never a second implementation.
#include "capacity_backend.inc"

static Physical physical;
static darwin_retire_state retire;
static tu_device device;
static void reset(){
   assert(native.empty() && vaUsed==0);
   bos.clear();device.darwin_retire=&retire;device.physical_device=&physical;retire.dev=&device;
   retire.failure=VK_SUCCESS;retire.cache_stats={};physical.heap={0,4096};
   nowNs+=2000000000ull;waitUs=0;notifyCount=reapCount=flushCount=0;
   vaFailures=0;std::memset(failures,0,sizeof failures);std::memset(nativeCalls,0,sizeof nativeCalls);
   injectedError=kIOReturnNoMemory;
   cleanupFails=retireFails=freeHeapOnReclaim=false;idleBytes=4096;liveBytes=8192;pendingBytes=16384;stages.clear();
}
static void preserved(){assert(liveBytes==8192 && pendingBytes==16384);}
static VkResult make(bool imported,tu_bo **out){
   return imported?darwin_bo_init_host(&device,out,(void *)0x100000,4096):
      darwin_bo_init(&device,nullptr,out,4096,0,0,TU_BO_ALLOC_NO_FLAGS,nullptr,"capacity test");
}
int main(){
   for(bool imported:{false,true})for(unsigned stage:{0u,1u,2u}) {
      reset();
      if(stage==0)vaFailures=2;
      else failures[stage==1?(imported?NativeAdrenoMethodBoImport:NativeAdrenoMethodBoAlloc):NativeAdrenoMethodBoMap]=2;
      tu_bo *bo=nullptr;assert(make(imported,&bo)==VK_SUCCESS);
      assert(waitUs==12000 && retire.cache_stats.retries==2 && notifyCount==2);
      assert(flushCount==4 && idleBytes==0 && native.size()==1 && vaUsed==4096);
      assert(stages.size()==1 && stages[0]==(stage==0?"iova":stage==1?(imported?"import":"alloc"):"map"));
      preserved();tu_bo_finish(&device,bo);assert(native.empty()&&vaUsed==0);
      std::printf("PASS actual backend: %s %s reclaim + retry + cleanup\n",imported?"host-import":"own BO",stages[0].c_str());
   }
   for(bool imported:{false,true}) {
      reset();vaFailures=100;tu_bo *bo=nullptr;
      assert(make(imported,&bo)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
      assert(waitUs==124000 && retire.cache_stats.retries==5 && notifyCount==6);
      assert(native.empty() && vaUsed==0);preserved();
      const auto waits=waitUs;const auto flushes=flushCount;assert(make(imported,&bo)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
      assert(waitUs==waits && flushCount>flushes); // throttle waits, not reclaim
   }
   std::puts("PASS actual backend: terminal VA failure bounded at 124 ms; cooldown still reclaims");
   reset();darwin_retry_failed_ns=nowNs;physical.heap.used=4096;freeHeapOnReclaim=true;
   failures[NativeAdrenoMethodBoImport]=1;tu_bo *cooldownBO=nullptr;
   assert(make(true,&cooldownBO)==VK_SUCCESS && waitUs==0 && retire.cache_stats.retries==1);
   tu_bo_finish(&device,cooldownBO);
   std::puts("PASS cooldown permits one immediate retry when this reclaim returned capacity");
   reset();physical.heap={4096,4096};tu_bo *bo=nullptr;
   assert(make(false,&bo)==VK_SUCCESS);freeHeapOnReclaim=true;
   assert(tu_add_to_heap(&device,bo)==VK_SUCCESS && physical.heap.used==4096);
   assert(stages.size()==1 && stages[0]=="heap" && waitUs==4000);preserved();tu_bo_finish(&device,bo);
   reset();physical.heap={4096,4096};assert(make(false,&bo)==VK_SUCCESS);
   assert(tu_add_to_heap(&device,bo)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
   assert(physical.heap.used==4096 && native.empty() && vaUsed==0 && waitUs==124000);preserved();
   std::puts("PASS actual heap gate: failed reservation rolled back before reclaim; terminal BO released exactly once");
   reset();failures[NativeAdrenoMethodBoMap]=1;cleanupFails=true;
   assert(make(true,&bo)==VK_ERROR_DEVICE_LOST && retire.failure==VK_ERROR_DEVICE_LOST);
   assert(notifyCount==0 && waitUs==0 && native.size()==1 && vaUsed==4096);
   native.clear();vaUsed=0; // test teardown only; production keeps quarantine
   std::puts("PASS actual map gate: unverified cleanup retains allocation/VA and never retries");
   for(bool imported:{false,true})for(unsigned stage:{1u,2u}) {
      reset();failures[stage==1?(imported?NativeAdrenoMethodBoImport:NativeAdrenoMethodBoAlloc):NativeAdrenoMethodBoMap]=100;
      assert(make(imported,&bo)==VK_ERROR_OUT_OF_DEVICE_MEMORY && waitUs==124000);
      assert(native.empty()&&vaUsed==0);preserved();
   }
   std::puts("PASS actual allocation/import/map terminal failures leave no partial object or VA reservation");
   reset();failures[NativeAdrenoMethodBoImport]=1;injectedError=kIOReturnNoResources;
   assert(make(true,&bo)==VK_SUCCESS && waitUs==4000);tu_bo_finish(&device,bo);
   reset();failures[NativeAdrenoMethodBoImport]=1;injectedError=7;
   assert(make(true,&bo)==VK_ERROR_INVALID_EXTERNAL_HANDLE && notifyCount==0 && waitUs==0);
   assert(native.empty()&&vaUsed==0);
   std::puts("PASS NoResources participates in recovery; invalid import does not retry");
   reset();physical.heap={4096,4096};vaUsed=4096;tu_device_memory lazy;
   assert(tu_allocate_lazy_memory(&device,&lazy)==VK_ERROR_OUT_OF_DEVICE_MEMORY);
   assert(lazy.bo==nullptr && nativeCalls[NativeAdrenoMethodBoFree]==0 && native.empty() && vaUsed==0);
   std::puts("PASS actual lazy allocation: final heap refusal finishes BO once and clears unpublished pointer");
   reset();vaFailures=1;retireFails=true;
   assert(make(false,&bo)==VK_ERROR_DEVICE_LOST && flushCount==0 && waitUs==0);
   preserved();assert(native.empty()&&vaUsed==0);
   std::puts("PASS recovery stops on retirement error; live and pending resources untouched");
}
