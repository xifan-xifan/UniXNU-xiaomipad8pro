/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_PCI_IDENTITY_H
#define NATIVE_ADRENO_PCI_IDENTITY_H
#include <stdint.h>

/* UniXNU-piano (2026-11-24, BR1, private host-checks/20261124-browser-gpu/ANALYSIS.md): the GPU identity browsers read from the
 * IORegistry. Chrome's GPU info (ANGLE SystemInfo_macos.mm GetIORegistryDevices) matches IOServiceMatching("AGXAccelerator") and
 * searches the entry and its parents for "vendor-id"; without one it lists no GPU at all. Dawn's Metal adapter (PhysicalDeviceMTL.mm
 * GetDeviceIORegistryPCIInfo) reads "vendor-id" and "device-id" from the accelerator's parent and its ancestors, and with no vendor
 * there falls back to the MTLDevice name, which must contain AMD, Apple, Radeon, Intel, Geforce or Quadro: "Adreno (TM) 830" has
 * none, so the adapter fails to initialise and navigator.gpu.requestAdapter() has nothing to give. Both read the values as 4-byte
 * little-endian OSData, the PCI layout. They go on NativeAdrenoGpuPublished, the AGXAccelerator's provider, so both searches find
 * them. The values are Qualcomm's Vulkan/PCI vendor 0x5143 and the Adreno chip id as device, as Turnip's VkPhysicalDeviceProperties
 * and Dawn's gpu_info.json ("QualcommPCI", mask 0xFF000000, 0x44 = Adreno 8xx) name them. No Apple identity is claimed. */
inline constexpr uint32_t NativeAdrenoPciVendorQualcomm = 0x5143;
inline constexpr char NativeAdrenoPciVendorKey[] = "vendor-id";
inline constexpr char NativeAdrenoPciDeviceKey[] = "device-id";

struct NativeAdrenoPciBytes {
    uint8_t b[4];
};

/* The 4-byte little-endian property value (IOPCIDevice's layout) of v. */
constexpr NativeAdrenoPciBytes NativeAdrenoPciLe32(uint32_t v)
{
    return NativeAdrenoPciBytes{{uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}};
}

/* The device id published for a chip id: the chip id itself (0x44050001 for the A830), 0 when the chip is unknown (no device-id is
 * then published, only the vendor, as on an Apple AGXAccelerator). */
constexpr uint32_t NativeAdrenoPciDeviceId(uint32_t chip_id)
{
    return chip_id;
}

#endif
