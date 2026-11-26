/* SPDX-License-Identifier: MIT */
/* The real BoAlloc and BoFree paths over test_poolbypass's software SMMU/CP fixture. */
#define main poolBypassFixtureMain
#include "test_poolbypass.cpp"
#undef main
#include "A830GpuTier.h"
#include "NativeAdrenoKopt.h"

static std::vector<a830tier::Span> zeroed;

static NativeAdrenoMemory *allocate(Path &path,uint64_t bytes,bool contiguous,uint64_t &handle)
{
    NativeAdrenoAbiBoAlloc request{bytes,12,uint32_t(contiguous)};
    NativeAdrenoAbiBoAllocReply reply{};
    CHECK(path.client.allocBo(&request,&reply)==kIOReturnSuccess && reply.handle);
    handle=reply.handle;
    return path.client.memoryOf(handle);
}

static void freeBo(Path &path,uint64_t handle)
{
    const auto *memory=path.client.memoryOf(handle);
    CHECK(memory);
    const bool shared=memory->sharedBacking();
    const int before=memoryTaken;
    const NativeAdrenoAbiBoFree request{handle};
    CHECK(path.client.freeBo(&request)==kIOReturnSuccess);
    CHECK(!memoryDepth && memoryTaken==before+int(shared));
}

static void configure(unsigned mode,bool coherent=false)
{
    using namespace a830tier;
    uint8_t property[pianoxram::kMaxRuns * pianoxram::kRecordBytes]={};
    const pianoxram::Run runs[]={
        {0x89e00000,0x11200000,pianoxram::Region0,pianoxram::UseVideo},
        {kRegionLo,kSlab,pianoxram::Region1,pianoxram::UseGpu},
        {kRegionLo+3*kSlab,kSlab,pianoxram::Region1,pianoxram::UseGpu},
        {kRegionLo+5*kSlab,kSlab,pianoxram::Region1,pianoxram::UseGpu},
    };
    for (size_t i=0;i<sizeof(runs)/sizeof(runs[0]);++i) {
        uint8_t *record=property+i*pianoxram::kRecordBytes;
        for (unsigned b=0;b<8;++b) {
            record[b]=uint8_t(runs[i].base>>(8*b));
            record[8+b]=uint8_t(runs[i].size>>(8*b));
        }
        record[16]=uint8_t(runs[i].bank);
        record[20]=uint8_t(runs[i].use);
    }
    CHECK(NativeAdrenoMemory::configureTier(mode,property,sizeof property,0x8c0004000ull,0x33fe00000ull)==Parse::ok);
    NativeAdrenoMemory::qualifyTier(36,true,coherent,[](uint64_t,unsigned run,void *) {return run!=1;},nullptr,nullptr);
}

int main()
{
    using namespace a830tier;
    NativeAdrenoMemory::setUnmappedUser(true);
    fixture.mode=ScatterUnlessContiguous;
    Path path;
    CHECK(exposed(path)==kIOReturnSuccess);
    uint32_t mib=0;
    CHECK(NativeAdrenoMemory::reservePoolBestEffort(36,16,16,true,&mib)==kIOReturnSuccess && mib==16);
    const size_t poolFree=NativeAdrenoMemory::poolFreeBytes();
    const NativeAdrenoSubmitLocks locks={nullptr,
        [](void *,NativeAdrenoSubmitLock which) {
            if(which==NativeAdrenoSubmitLockMemory) {CHECK(!memoryDepth);++memoryDepth;++memoryTaken;}
        },
        [](void *,NativeAdrenoSubmitLock which) {
            if(which==NativeAdrenoSubmitLockMemory) {CHECK(memoryDepth==1);--memoryDepth;}
        }};
    CHECK(path.execbuf.attachLocks(&locks)==kIOReturnSuccess);
    fixture.zero_physical=[](uint64_t pa,size_t bytes) {
        CHECK(memoryDepth==0);
        CHECK(NativeAdrenoMemory::tierStats().used_bytes>=bytes); // the Tier lock is also free
        zeroed.push_back({pa,bytes});
    };

    uint64_t handle=0;
    CHECK(allocate(path,kSlab,false,handle)->pooled());
    freeBo(path,handle);
    configure(1);
    auto stats=NativeAdrenoMemory::tierStats();
    CHECK(stats.runs==3 && stats.tested==3 && stats.pass_mask==5 && stats.free_bytes==2*kSlab);
    CHECK(allocate(path,kSlab-kPage,false,handle)->pooled());
    freeBo(path,handle);
    CHECK(allocate(path,kSlab,true,handle)->pooled());
    freeBo(path,handle);

    // With the pool-zero optimization disabled, tier zeroing still happens outside both locks.
    NativeAdrenoMemory::setKopt(NativeAdrenoKoptAll & ~NativeAdrenoKoptPoolZero);
    const auto cacheBefore=fixture.cache_calls;
    const auto poolAllocs=w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][0]);
    auto *memory=allocate(path,kSlab+kPage,false,handle);
    CHECK(memory->tier() && !memory->pooled() && !memory->unmapped() && memory->writeCombined() && !memory->mapping());
    CHECK(memory->bytes()==kSlab+kPage && memory->spanCount()==2);
    CHECK(memory->spans()[0].physical==kRegionLo && memory->spans()[0].bytes==kSlab);
    CHECK(memory->spans()[1].physical==kRegionLo+5*kSlab && memory->spans()[1].offset==kSlab && memory->spans()[1].bytes==kPage);
    CHECK(zeroed.size()==2 && zeroed[0].pa==kRegionLo && zeroed[1].pa==kRegionLo+5*kSlab);
    CHECK(zeroed[0].bytes==kSlab && zeroed[1].bytes==kSlab);
    CHECK(memory->reassertWriteCombined(nullptr)==kIOReturnSuccess);
    NativeAdrenoMemory alias;
    const auto nextPhysical=fixture.next_physical;
    fixture.next_physical=kRegionLo;
    CHECK(alias.importTask(reinterpret_cast<void *>(2),kPage,kPage,36,kPage)==kIOReturnUnsupported && !alias.backed());
    fixture.next_physical=nextPhysical;
    CHECK(memory->holdForDevice()==kIOReturnSuccess);
    const NativeAdrenoAbiBoFree held{handle};
    CHECK(path.client.freeBo(&held)==kIOReturnBusy && NativeAdrenoMemory::tierStats().free_bytes==0);
    CHECK(memory->retireAfterQuiescence()==kIOReturnSuccess);
    CHECK(w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][0])==poolAllocs && NativeAdrenoMemory::poolFreeBytes()==poolFree);

    native_adreno_pool_bypass=1;
    uint64_t fallback=0;
    CHECK(allocate(path,kSlab,false,fallback)->unmapped());
    CHECK(NativeAdrenoMemory::tierStats().refusals==1);
    freeBo(path,fallback);
    freeBo(path,handle);
    CHECK(NativeAdrenoMemory::tierStats().free_bytes==2*kSlab && fixture.cache_calls==cacheBefore);

    configure(2);
    memory=allocate(path,kPage,false,handle);
    CHECK(memory->tier() && memory->spans()[0].physical==kRegionLo && memory->spans()[0].bytes==kPage);
    CHECK(zeroed.size()==3 && zeroed.back().bytes==kSlab);
    const NativeAdrenoAbiBoMap map{handle,Base,3,1,3,0};
    NativeAdrenoAbiBoMapReply mapped{};
    CHECK(path.client.mapBo(&map,&mapped)==kIOReturnSuccess);
    freeBo(path,handle);

    configure(2,true);
    CHECK(allocate(path,kPage,false,handle)->unmapped());
    CHECK(has(NativeAdrenoMemory::tierStats().state,"cached-coherent") && NativeAdrenoMemory::tierStats().allocations==0);
    freeBo(path,handle);
    configure(2);
    fixture.metadata_fail=true;
    const NativeAdrenoAbiBoAlloc request{kPage,12,0};
    NativeAdrenoAbiBoAllocReply reply{};
    CHECK(path.client.allocBo(&request,&reply)!=kIOReturnSuccess && !reply.handle);
    fixture.metadata_fail=false;
    CHECK(NativeAdrenoMemory::tierStats().free_bytes==2*kSlab && NativeAdrenoMemory::tierStats().refusals==1);
    CHECK(path.client.releaseAll(&ringclock,1000)==kIOReturnSuccess);
    std::puts("PASS: tier BoAlloc/BoFree: opt-in modes, tier before pool, slab zeroing outside locks, clipped Physical64 spans, "
              "map and free, import alias refusal, exhaustion and metadata rollback, cached-coherent fallback");
}
