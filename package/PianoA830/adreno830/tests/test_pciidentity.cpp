/* SPDX-License-Identifier: MIT */
/* BR1 (2026-11-24): the browser-facing GPU identity of NativeAdrenoGpuPublished (NativeAdrenoPciIdentity.h). Checked: the vendor
 * and device values as 4-byte little-endian property data, the key names Chrome's ANGLE and Dawn search for, Dawn's QualcommPCI
 * architecture rule (mask 0xFF000000, 0x44 = Adreno 8xx) on the A830 chip id, that the vendor is none Dawn's name fallback or
 * Chrome's macOS lists treat as Apple/Intel/AMD/NVIDIA, and that NativeAdrenoPublish.cpp sets both keys on the published service
 * before registerService (a source check: the publish shell needs the whole kernel stack to run).
 * host-test: NativeAdrenoPciIdentity.h */
#include "NativeAdrenoPciIdentity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <fstream>
#include <sstream>

static int checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static uint32_t readLe(const NativeAdrenoPciBytes &b)
{
    uint32_t v;
    memcpy(&v, b.b, 4);   /* how ANGLE's and Dawn's GetEntryProperty read CFData bytes on arm64 */
    return v;
}

int main()
{
    const NativeAdrenoPciBytes vendor = NativeAdrenoPciLe32(NativeAdrenoPciVendorQualcomm);
    CHECK(vendor.b[0] == 0x43 && vendor.b[1] == 0x51 && vendor.b[2] == 0 && vendor.b[3] == 0);
    CHECK(readLe(vendor) == 0x5143);
    const uint32_t a830 = 0x44050001;
    const NativeAdrenoPciBytes device = NativeAdrenoPciLe32(NativeAdrenoPciDeviceId(a830));
    CHECK(device.b[0] == 0x01 && device.b[1] == 0x00 && device.b[2] == 0x05 && device.b[3] == 0x44);
    CHECK(readLe(device) == a830);
    CHECK((NativeAdrenoPciDeviceId(a830) & 0xFF000000u) == 0x44000000u);   /* Dawn gpu_info.json: Adreno 8xx */
    CHECK(NativeAdrenoPciDeviceId(0) == 0);                                  /* unknown chip: no device-id */
    CHECK(!strcmp(NativeAdrenoPciVendorKey, "vendor-id") && !strcmp(NativeAdrenoPciDeviceKey, "device-id"));
    /* not Apple 0x106b, Intel 0x8086, AMD 0x1002, NVIDIA 0x10de, VMware 0x15ad, VirtualBox 0x80ee (Chrome's macOS entries) */
    const uint32_t others[] = {0x106b, 0x8086, 0x1002, 0x10de, 0x15ad, 0x80ee, 0};
    for (uint32_t o : others) CHECK(NativeAdrenoPciVendorQualcomm != o);

    /* the publish shell: both keys set on the service, inside the macOS block, before its registerService */
    const char *roots[] = {"NativeAdrenoPublish.cpp", "../NativeAdrenoPublish.cpp", "../../NativeAdrenoPublish.cpp"};
    std::string text;
    const char *env = getenv("ADRENO830_ROOT");
    std::string here(__FILE__);
    here = here.substr(0, here.rfind('/') == std::string::npos ? 0 : here.rfind('/'));
    std::string first = env ? std::string(env) + "/NativeAdrenoPublish.cpp" : here + "/../NativeAdrenoPublish.cpp";
    for (int i = -1; i < 3 && text.empty(); ++i) {
        std::ifstream f(i < 0 ? first : std::string(roots[i]));
        if (f) { std::stringstream s; s << f.rdbuf(); text = s.str(); }
    }
    CHECK(!text.empty());
    const size_t vendorAt = text.find("service->setProperty(NativeAdrenoPciVendorKey,data)");
    const size_t deviceAt = text.find("service->setProperty(NativeAdrenoPciDeviceKey,data)");
    const size_t macosAt = text.rfind("#ifdef ADRENO830_MACOS", vendorAt);
    const size_t registerAt = text.find("service->registerService();", vendorAt);
    CHECK(vendorAt != std::string::npos && deviceAt != std::string::npos && macosAt != std::string::npos);
    CHECK(registerAt != std::string::npos && vendorAt < deviceAt && deviceAt < registerAt);
    CHECK(text.find("#endif", vendorAt) > deviceAt && text.find("#endif", vendorAt) < registerAt);
    CHECK(text.find("#include \"NativeAdrenoPciIdentity.h\"") != std::string::npos);
    printf("PASS: browser GPU identity: vendor-id 0x5143 / device-id 0x44050001 as LE32 property data (Dawn: Adreno 8xx), "
           "no Apple/Intel/AMD/NVIDIA identity, both keys on NativeAdrenoGpuPublished before registerService (%d checks)\n", checks);
    return 0;
}
