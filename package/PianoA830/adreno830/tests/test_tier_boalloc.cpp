/* SPDX-License-Identifier: MIT */
/* The real BoAlloc and BoFree paths over test_poolbypass's software SMMU/CP fixture. */
#define main poolBypassFixtureMain
#include "test_poolbypass.cpp"
#undef main
#include "A830GpuTier.h"
#include "NativeAdrenoKopt.h"
#include <array>

using namespace a830tier;
static constexpr uint64_t MiB=1ull<<20;
using Runs=std::array<Run,3>;
static constexpr Runs smallRuns={{{kRegionLo,kSlab},{kRegionLo+3*kSlab,kSlab},{kRegionLo+5*kSlab,kSlab}}};
static constexpr Runs fullRuns={{{kRegionLo,758*MiB},{0x8b0000000ull,84*MiB},{0x8b5800000ull,28*MiB}}};
static std::vector<a830tier::Span> zeroed;

static NativeAdrenoMemory *allocate(Path &path,uint64_t bytes,uint32_t flags,uint64_t &handle)
{
    const NativeAdrenoAbiBoAlloc request{bytes,12,flags};
    NativeAdrenoAbiBoAllocReply reply{};
    CHECK(path.client.dispatch(NativeAdrenoMethodBoAlloc,&request,sizeof(request),&reply,sizeof(reply),&ringclock)==
          kIOReturnSuccess && reply.handle);
    handle=reply.handle;
    auto *memory=path.client.memoryOf(handle);
    CHECK(memory && memory->bytes()==bytes);
    return memory;
}

static void freeBo(Path &path,uint64_t handle)
{
    const auto *memory=path.client.memoryOf(handle);
    CHECK(memory);
    const bool lockMemory=memory->sharedBacking() || memory->imported() || memory->scanout();
    const int before=memoryTaken;
    const NativeAdrenoAbiBoFree request{handle};
    CHECK(path.client.freeBo(&request)==kIOReturnSuccess);
    CHECK(!memoryDepth && memoryTaken==before+int(lockMemory));
    CHECK(!path.client.memoryOf(handle));
}

static void configure(unsigned mode,unsigned passMask=5,const Runs &runs=smallRuns,bool wc=true,bool coherent=false,
                      unsigned bits=36)
{
    uint8_t property[pianoxram::kMaxRuns * pianoxram::kRecordBytes]={};
    for (size_t i=0;i<runs.size();++i) {
        uint8_t *record=property+i*pianoxram::kRecordBytes;
        for (unsigned b=0;b<8;++b) {
            record[b]=uint8_t(runs[i].base>>(8*b));
            record[8+b]=uint8_t(runs[i].size>>(8*b));
        }
        record[16]=pianoxram::Region1;
        record[20]=pianoxram::UseGpu;
    }
    CHECK(NativeAdrenoMemory::configureTier(mode,property,sizeof property,0x8c0004000ull,0x33fe00000ull)==
          (mode?Parse::ok:Parse::absent));
    struct Probe {unsigned mask,calls;bool inactive;};
    Probe probe{passMask,0,!mode};
    NativeAdrenoMemory::qualifyTier(bits,wc,coherent,[](uint64_t,unsigned run,void *ctx) {
        auto &state=*static_cast<Probe *>(ctx);
        CHECK(!state.inactive && !NativeAdrenoMemory::tierEligible(kSlab));
        ++state.calls;
        return (state.mask&(1u<<run))!=0;
    },&probe,nullptr);
    CHECK(probe.calls==(mode && wc && !coherent && bits==36?6u:0u));
}

static void checkCapability(Path &path,bool available)
{
    NativeAdrenoAbiParams params{};
    CHECK(path.client.query(&params)==kIOReturnSuccess);
    CHECK(params.flags==(available?NativeAdrenoParamsHostImport|NativeAdrenoParamsGpuPrivate:NativeAdrenoParamsHostImport));
}

static void checkCpuMapDenied(Path &path,uint64_t handle)
{
    auto *memory=path.client.memoryOf(handle);
    CHECK(memory && memory->tier() && !memory->mapping() && !memory->retainVmCpuDescriptor());
    const uint32_t token=path.client.tokenOf(handle);
    const NativeAdrenoAbiBoCpuMap request{handle};
    NativeAdrenoAbiBoCpuMapReply view{token,0,kSlab};
    CHECK(path.client.dispatch(NativeAdrenoMethodBoCpuMap,&request,sizeof(request),&view,sizeof(view),&ringclock)==
          kIOReturnUnsupported);
    CHECK(view.token==token && view.bytes==kSlab); // dispatch publishes a reply only on success
    CHECK(!path.client.mappingLeases(token) && !path.client.memoryForToken(token));
}

static void testImports(Path &path)
{
    void *task=reinterpret_cast<void *>(2);
    const NativeAdrenoAbiBoImport request{0x10000000,2*kPage};
    const auto before=NativeAdrenoMemory::tierStats();
    CHECK(before.requests[NativeAdrenoTierHostImport]==0 && before.requests[NativeAdrenoTierScanoutImport]==0);
    const auto liveImports=fixture.imports_live;
    NativeAdrenoAbiBoAllocReply imported{};
    CHECK(path.client.importBo(&request,task,&imported)==kIOReturnSuccess);
    auto *memory=path.client.memoryOf(imported.handle);
    CHECK(memory && memory->imported() && !memory->tier());
    freeBo(path,imported.handle);

    IOMemoryDescriptor *scanout=IOMemoryDescriptor::withAddressRange(0x20000000,request.bytes,kIOMemoryMapperNone,task);
    CHECK(scanout);
    const NativeAdrenoScanoutMapping mapping{task,request.address,request.bytes,scanout};
    path.client.setScanout(&mapping);
    CHECK(path.client.importBo(&request,task,&imported)==kIOReturnSuccess);
    memory=path.client.memoryOf(imported.handle);
    CHECK(memory && memory->scanout() && !memory->tier() && memory->spans()[0].physical==scanout->physical_base);
    freeBo(path,imported.handle);
    const size_t metadata=fixture.metadata.size();
    const auto liveBOs=path.client.liveObjects();

    // The first page is outside the tier and the second aliases a qualified slab.
    scanout->physical_base=kRegionLo-2*kPage;
    CHECK(path.client.importBo(&request,task,&imported)==kIOReturnUnsupported && !imported.handle);
    path.client.setScanout(nullptr);
    scanout->release();
    const auto nextPhysical=fixture.next_physical;
    const auto completed=fixture.completed;
    fixture.next_physical=kRegionLo-2*kPage;
    CHECK(path.client.importBo(&request,task,&imported)==kIOReturnUnsupported && !imported.handle);
    fixture.next_physical=nextPhysical;
    CHECK(fixture.completed==completed+1 && fixture.imports_live==liveImports && fixture.metadata.size()==metadata);
    CHECK(path.client.liveObjects()==liveBOs);
    const auto after=NativeAdrenoMemory::tierStats();
    CHECK(after.requests[NativeAdrenoTierHostImport]==2 && after.requests[NativeAdrenoTierScanoutImport]==2);
    CHECK(after.requests[NativeAdrenoTierImportDenied]==2 && after.allocations==before.allocations &&
          after.refusals==before.refusals && after.used_bytes==before.used_bytes && after.free_bytes==before.free_bytes);
}

static void testFullTier(Path &path,size_t poolFree)
{
    configure(1,7,fullRuns);
    checkCapability(path,true);
    constexpr uint64_t capacity=870*MiB;
    constexpr uint64_t sizes[]={128*MiB,128*MiB,128*MiB,128*MiB,128*MiB,128*MiB,102*MiB};
    uint64_t handles[std::size(sizes)]{};
    const auto liveDescriptors=fixture.live.size();
    const auto metadata=fixture.metadata.size();
    zeroed.clear();
    uint64_t remaining=capacity;
    unsigned run=0;
    uint64_t runOffset=0;
    std::vector<Span> expectedZeroes;
    for (size_t i=0;i<std::size(sizes);++i) {
        auto *memory=allocate(path,sizes[i],NativeAdrenoBoAllocGpuPrivate,handles[i]);
        CHECK(memory->tier() && !memory->pooled() && !memory->unmapped());
        uint64_t offset=0;
        for (size_t span=0;span<memory->spanCount();++span) {
            CHECK(run<fullRuns.size() && offset<sizes[i]);
            const auto &piece=memory->spans()[span];
            const auto bytes=std::min(sizes[i]-offset,fullRuns[run].size-runOffset);
            CHECK(piece.offset==offset && piece.physical==fullRuns[run].base+runOffset && piece.bytes==bytes);
            expectedZeroes.push_back({piece.physical,piece.bytes});
            offset+=bytes;
            runOffset+=bytes;
            if(runOffset==fullRuns[run].size) {++run;runOffset=0;}
        }
        CHECK(offset==sizes[i]);
        remaining-=sizes[i];
        const auto stats=NativeAdrenoMemory::tierStats();
        CHECK(stats.free_bytes==remaining && stats.used_bytes==capacity-remaining && stats.refusals==0);
    }
    CHECK(!remaining && run==fullRuns.size() && fixture.live.size()==liveDescriptors);
    CHECK(path.client.liveObjects()==std::size(sizes));

    // Leave the ordinary BO pool enabled. An exhausted tier request should use pageable XNU backing.
    CHECK(!NativeAdrenoPoolBypassed());
    NativeAdrenoMemory::setUnmappedUser(false);
    const auto pagedAllocs=w(&native_adreno_p0b_alloc[NativeAdrenoPathUser][0]);
    for (uint64_t refusal=1;refusal<=3;++refusal) {
        uint64_t handle=0;
        auto *fallback=allocate(path,kSlab,NativeAdrenoBoAllocGpuPrivate,handle);
        CHECK(fallback->unmapped() && !fallback->tier() && !fallback->pooled() && fallback->spanCount()==kSlab/kPage);
        auto *descriptor=fallback->retainVmCpuDescriptor();
        CHECK(descriptor && descriptor->vm_object && descriptor->bytes==kSlab);
        const auto *contents=static_cast<const uint8_t *>(descriptor->raw);
        CHECK(std::all_of(contents,contents+kSlab,[](uint8_t byte) {return byte==0;}));
        descriptor->release();
        const auto stats=NativeAdrenoMemory::tierStats();
        CHECK(stats.free_bytes==0 && stats.used_bytes==capacity && stats.allocations==std::size(sizes));
        CHECK(stats.refusals==refusal && stats.requests[NativeAdrenoTierPrivate]==std::size(sizes)+refusal);
        CHECK(stats.requests[NativeAdrenoTierHostImport]==0 && stats.requests[NativeAdrenoTierScanoutImport]==0);
        CHECK(w(&native_adreno_p0b_alloc[NativeAdrenoPathUser][0])==pagedAllocs+refusal);
        freeBo(path,handle);
        CHECK(fixture.live.size()==liveDescriptors && NativeAdrenoMemory::poolFreeBytes()==poolFree);
    }
    NativeAdrenoMemory::setUnmappedUser(true);
    CHECK(zeroed.size()==expectedZeroes.size());
    size_t piece=0;
    for (uint64_t handle:handles) {
        const auto *memory=path.client.memoryOf(handle);
        CHECK(memory && memory->tier());
        for (size_t span=0;span<memory->spanCount();++span,++piece) {
            CHECK(piece<expectedZeroes.size());
            CHECK(zeroed[piece].pa==expectedZeroes[piece].pa && zeroed[piece].bytes==expectedZeroes[piece].bytes);
            CHECK(memory->spans()[span].physical==expectedZeroes[piece].pa &&
                  memory->spans()[span].bytes==expectedZeroes[piece].bytes);
        }
    }
    CHECK(piece==expectedZeroes.size());

    // Free the allocation crossing the first boundary while its neighbors are live, then reuse its exact spans.
    constexpr size_t crossing=5;
    freeBo(path,handles[crossing]);
    CHECK(NativeAdrenoMemory::tierStats().free_bytes==sizes[crossing]);
    auto *reused=allocate(path,sizes[crossing],NativeAdrenoBoAllocGpuPrivate,handles[crossing]);
    CHECK(reused->tier() && reused->spanCount()==2 && reused->spans()[0].physical==expectedZeroes[crossing].pa);
    CHECK(reused->spans()[1].physical==fullRuns[1].base && NativeAdrenoMemory::tierStats().free_bytes==0);
    for (uint64_t handle:handles) freeBo(path,handle);
    CHECK(NativeAdrenoMemory::tierStats().free_bytes==capacity && !NativeAdrenoMemory::tierStats().used_bytes);
    CHECK(fixture.live.size()==liveDescriptors && fixture.metadata.size()==metadata && !path.client.liveObjects());
}

static void testModeOneWorkload(Path &path,size_t poolFree)
{
    configure(1,7,fullRuns);
    checkCapability(path,true);
    constexpr uint64_t capacity=870*MiB;
    constexpr uint64_t usedMiB[]={0,2,6,14,30,62,126};
    uint64_t handles[std::size(usedMiB)-1]{};
    const auto liveDescriptors=fixture.live.size();
    const auto metadata=fixture.metadata.size();
    auto stats=NativeAdrenoMemory::tierStats();
    CHECK(stats.mode==1 && std::strcmp(stats.state,"ready")==0 && stats.pass_mask==7);
    CHECK(stats.free_bytes==capacity && !stats.used_bytes && !stats.allocations && !stats.refusals);
    for (auto count:stats.requests) CHECK(count==0);

    for (size_t i=0;i<std::size(handles);++i) {
        const uint64_t previous=usedMiB[i]*MiB;
        const uint64_t used=usedMiB[i+1]*MiB;
        auto *memory=allocate(path,used-previous,NativeAdrenoBoAllocGpuPrivate,handles[i]);
        CHECK(memory->tier() && memory->spanCount()==1);
        CHECK(memory->spans()[0].physical==kRegionLo+previous && memory->spans()[0].bytes==used-previous);
        stats=NativeAdrenoMemory::tierStats();
        CHECK(stats.used_bytes==used && stats.free_bytes==capacity-used && stats.allocations==i+1);
        CHECK(stats.requests[NativeAdrenoTierPrivate]==i+1 && !stats.refusals);
        CHECK(!stats.requests[NativeAdrenoTierImportDenied] && !stats.requests[NativeAdrenoTierCpuMapDenied]);
    }
    CHECK(path.client.liveObjects()==std::size(handles));
    CHECK(path.client.releaseAll(&ringclock,1000)==kIOReturnSuccess);
    stats=NativeAdrenoMemory::tierStats();
    CHECK(stats.free_bytes==capacity && !stats.used_bytes && stats.allocations==std::size(handles));
    CHECK(!path.client.liveObjects() && NativeAdrenoMemory::poolFreeBytes()==poolFree);
    CHECK(fixture.live.size()==liveDescriptors && fixture.metadata.size()<metadata && !memoryDepth);
}

int main()
{
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
    configure(0);
    checkCapability(path,false);
    CHECK(allocate(path,kSlab,0,handle)->pooled());
    freeBo(path,handle);
    CHECK(allocate(path,kSlab,NativeAdrenoBoAllocGpuPrivate,handle)->pooled());
    freeBo(path,handle);
    auto stats=NativeAdrenoMemory::tierStats();
    CHECK(has(stats.state,"off") && !stats.mode && !stats.runs && !stats.tested && !stats.free_bytes && !stats.used_bytes);
    CHECK(!stats.refusals && !stats.allocations && zeroed.empty());
    for(auto count:stats.requests) CHECK(count==0);

    configure(1);
    stats=NativeAdrenoMemory::tierStats();
    CHECK(stats.runs==3 && stats.tested==3 && stats.pass_mask==5 && stats.free_bytes==2*kSlab);
    checkCapability(path,true);
    const NativeAdrenoAbiBoAlloc invalid[]={{UINT64_MAX & ~(kPage-1),12,NativeAdrenoBoAllocGpuPrivate},
        {kSlab,12,1|NativeAdrenoBoAllocGpuPrivate}};
    for(const auto &request:invalid) {
        NativeAdrenoAbiBoAllocReply reply{};
        CHECK(path.client.allocBo(&request,&reply)==kIOReturnBadArgument && !reply.handle);
    }
    CHECK(NativeAdrenoMemory::tierStats().free_bytes==2*kSlab && !path.client.liveObjects());
    CHECK(allocate(path,kSlab,0,handle)->pooled());
    const NativeAdrenoAbiBoCpuMap legacyMap{handle};
    NativeAdrenoAbiBoCpuMapReply legacyView{};
    CHECK(path.client.cpuMap(&legacyMap,&legacyView)==kIOReturnSuccess && legacyView.token && legacyView.bytes==kSlab);
    CHECK(path.client.memoryForToken(legacyView.token)==path.client.memoryOf(handle));
    const NativeAdrenoAbiBoCpuUnmap legacyUnmap{handle,legacyView.token,0};
    CHECK(path.client.cpuUnmap(&legacyUnmap)==kIOReturnSuccess);
    freeBo(path,handle);
    CHECK(allocate(path,kPage,0,handle)->pooled());
    freeBo(path,handle);
    CHECK(allocate(path,kSlab-kPage,NativeAdrenoBoAllocGpuPrivate,handle)->pooled());
    freeBo(path,handle);
    CHECK(allocate(path,kSlab,1,handle)->pooled());
    freeBo(path,handle);

    // With the pool-zero optimization disabled, tier zeroing still happens outside both locks.
    NativeAdrenoMemory::setKopt(NativeAdrenoKoptAll & ~NativeAdrenoKoptPoolZero);
    const auto cacheBefore=fixture.cache_calls;
    const auto poolAllocs=w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][0]);
    auto *memory=allocate(path,kSlab+kPage,NativeAdrenoBoAllocGpuPrivate,handle);
    CHECK(memory->tier() && !memory->pooled() && !memory->unmapped() && memory->writeCombined() && !memory->mapping());
    CHECK(memory->bytes()==kSlab+kPage && memory->spanCount()==2);
    CHECK(memory->spans()[0].physical==kRegionLo && memory->spans()[0].bytes==kSlab);
    CHECK(memory->spans()[1].physical==kRegionLo+5*kSlab && memory->spans()[1].offset==kSlab && memory->spans()[1].bytes==kPage);
    CHECK(zeroed.size()==2 && zeroed[0].pa==kRegionLo && zeroed[1].pa==kRegionLo+5*kSlab);
    CHECK(zeroed[0].bytes==kSlab && zeroed[1].bytes==kSlab);
    CHECK(memory->reassertWriteCombined(nullptr)==kIOReturnSuccess);
    checkCpuMapDenied(path,handle);
    testImports(path);
    CHECK(memory->holdForDevice()==kIOReturnSuccess);
    const NativeAdrenoAbiBoFree held{handle};
    CHECK(path.client.freeBo(&held)==kIOReturnBusy && NativeAdrenoMemory::tierStats().free_bytes==0);
    CHECK(memory->retireAfterQuiescence()==kIOReturnSuccess);
    CHECK(w(&native_adreno_p0b_alloc[NativeAdrenoPathPool][0])==poolAllocs && NativeAdrenoMemory::poolFreeBytes()==poolFree);

    uint64_t fallback=0;
    CHECK(allocate(path,kSlab,NativeAdrenoBoAllocGpuPrivate,fallback)->unmapped());
    stats=NativeAdrenoMemory::tierStats();
    CHECK(stats.refusals==1 && stats.allocations==1 && stats.requests[NativeAdrenoTierPrivate]==3);
    CHECK(stats.requests[NativeAdrenoTierLegacySmall]==1 && stats.requests[NativeAdrenoTierLegacyLarge]==1);
    CHECK(stats.requests[NativeAdrenoTierContiguous]==1 && stats.requests[NativeAdrenoTierCpuMap]==1);
    CHECK(stats.requests[NativeAdrenoTierCpuMapDenied]==1);
    freeBo(path,fallback);
    freeBo(path,handle);
    CHECK(NativeAdrenoMemory::tierStats().free_bytes==2*kSlab && fixture.cache_calls==cacheBefore);

    testFullTier(path,poolFree);
    zeroed.clear();
    native_adreno_pool_bypass=1;
    configure(2);
    checkCapability(path,true);
    CHECK(allocate(path,kPage,0,handle)->unmapped());
    freeBo(path,handle);
    memory=allocate(path,kPage,NativeAdrenoBoAllocGpuPrivate,handle);
    CHECK(memory->tier() && memory->spans()[0].physical==kRegionLo && memory->spans()[0].bytes==kPage);
    CHECK(zeroed.size()==1 && zeroed.back().bytes==kSlab);
    const NativeAdrenoAbiBoMap map{handle,Base,3,1,3,0};
    NativeAdrenoAbiBoMapReply mapped{};
    CHECK(path.client.mapBo(&map,&mapped)==kIOReturnSuccess);
    checkCpuMapDenied(path,handle);
    freeBo(path,handle);

    configure(2,0);
    checkCapability(path,false);
    CHECK(allocate(path,kPage,NativeAdrenoBoAllocGpuPrivate,handle)->unmapped());
    freeBo(path,handle);
    stats=NativeAdrenoMemory::tierStats();
    CHECK(has(stats.state,"closed") && stats.tested==3 && !stats.passed && !stats.pass_mask);
    CHECK(!stats.free_bytes && !stats.used_bytes && !stats.allocations && !stats.refusals && zeroed.size()==1);
    for(auto count:stats.requests) CHECK(count==0);

    configure(2,5,smallRuns,false);
    checkCapability(path,false);
    CHECK(has(NativeAdrenoMemory::tierStats().state,"write-combine-disabled"));
    configure(2,5,smallRuns,true,false,0);
    checkCapability(path,false);
    CHECK(has(NativeAdrenoMemory::tierStats().state,"output-width"));
    configure(2,5,smallRuns,true,true);
    checkCapability(path,false);
    CHECK(allocate(path,kPage,NativeAdrenoBoAllocGpuPrivate,handle)->unmapped());
    CHECK(has(NativeAdrenoMemory::tierStats().state,"cached-coherent") && NativeAdrenoMemory::tierStats().allocations==0);
    freeBo(path,handle);
    configure(2);
    const auto liveDescriptors=fixture.live.size();
    const auto metadata=fixture.metadata.size();
    fixture.metadata_fail=true;
    const NativeAdrenoAbiBoAlloc request{kPage,12,NativeAdrenoBoAllocGpuPrivate};
    NativeAdrenoAbiBoAllocReply reply{};
    CHECK(path.client.allocBo(&request,&reply)!=kIOReturnSuccess && !reply.handle);
    fixture.metadata_fail=false;
    CHECK(NativeAdrenoMemory::tierStats().free_bytes==2*kSlab && NativeAdrenoMemory::tierStats().refusals==1);
    CHECK(fixture.live.size()==liveDescriptors && fixture.metadata.size()==metadata && !path.client.liveObjects());
    testModeOneWorkload(path,poolFree);
    std::puts("PASS: tier BoAlloc/BoFree: mode-1 residency through 126 MiB and disconnect, 870 MiB exhausted and reused, "
              "pageable fallback, mode and S8 isolation, CPU-map and import alias denial, GPU map, slab zeroing and metadata rollback");
}
