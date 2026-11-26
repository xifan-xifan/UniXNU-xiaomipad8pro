# RAM16G R1 region1 GPU memory

R1 gives eligible Adreno 830 buffer objects (BOs) access to reserved RAM below
XNU's single contiguous RAM window. The captured memory layout has three
candidate ranges totalling 870 MiB after rounding inward to 2 MiB boundaries.
DarwinBoot's live UEFI map determines the reservations. Only GPU-qualified runs
become allocatable.

## Reference path

Ubuntu v1.4.0 pins Linux `83f0f89bed14` in `kernel/source.json`. The Linux and
sheng paths below establish the GPU allocation and mapping model. The 27.3
Metal/IOKit boundary follows the `AGXDeviceUserClient` ABI in
[gpu-userspace.md](gpu-userspace.md). R1 requires no change to Metal, the plugin
or Turnip. The Android capture has no stock allocator source for this path.

| Step | Ubuntu / sheng reference | Piano R1 |
|---|---|---|
| GPU and SMMU | Linux `arch/arm64/boot/dts/qcom/sm8750.dtsi:2318–2328` identifies Adreno 830 and the GPU SIDs | Same GPU and SIDs. `NativeAdrenoGpuContext` uses the existing CB0 page tables and TLB protocol. |
| BO backing | Linux `drivers/gpu/drm/msm/msm_gem.c:204–245` gets normal pages and builds a scatterlist. sheng's Adreno740 `7487f72` `NativeAdrenoUserClient.cpp:236` starts allocation in its user-client core. | Different source for eligible BOs. `NativeAdrenoMemory::allocateTier` reserves whole slabs, clips the last exposed span to the requested length and falls back through the existing allocation chain on refusal. |
| GPU mapping | Linux `msm_gem.c:505–532` maps the scatterlist. Cached-coherent backing explicitly opts in to cached IOMMU access. | Same scatter/gather principle. `NativeAdrenoVm::attributes` assigns tier BOs non-cacheable leaves. Paged VM pressure and the client cap still count their mapped bytes. |
| CPU view | Linux `msm_gem.c:357–361,1192–1194` sets write combining for WC objects. sheng's Adreno740 `NativeAdrenoPublish.cpp:532` creates a Physical64 descriptor with `withAddressRanges`. | Same write-combined CPU access and Physical64 ranges. Tier views use the factory's designated initializer in a descriptor that enforces WC because IOKit accepts caller-supplied cache bits. |
| Admission | Linux GEM obtains already-managed pages. | Different ownership. DarwinBoot reserves the runs before ExitBootServices. PianoA830 validates the handoff and probes both endpoint pages with the GPU before exposing a run. |

## GPU tier controls

The combined analysis gate and RAM-boot reads are in [RAM16G extra RAM](../extra-ram.md).
PianoA830 validates the shared 24-byte records against its physical window and uses bank 1/use 1 only.
Mode 2 additionally tries sub-2 MiB BOs, each occupying at least one slab. Full cached coherence
(`piano-gpu-coherent=2`, upstream `pagani-gpu-coherent=2`) closes the tier. A probe failure closes the affected
run. If recovery or TLB cleanup cannot quiesce the probe, S8 stops before Metal publication.
