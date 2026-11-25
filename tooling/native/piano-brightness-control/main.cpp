// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Control.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <cstdio>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {
using namespace pianobrightness;
constexpr char kBuild[] = "26C5035r";

// Signatures and layout verified against the arm64e macOS 27.3 cache for kBuild.
struct FramebufferBrightnessInfo {
  uint32_t type, flags;
  double maxNits, minNits;
};
static_assert(sizeof(FramebufferBrightnessInfo) == 24);
using FramebufferCallback = void (*)(CFTypeRef, uint32_t, void *);

struct AppleBrightness27_3 {
  bool (*canChange)(CGDirectDisplayID);
  int32_t (*get)(CGDirectDisplayID, float *);
  int32_t (*set)(CGDirectDisplayID, float);
  IOReturn (*mainDisplay)(CFTypeRef *);
  IOReturn (*setCallback)(CFTypeRef, FramebufferCallback, void *);
  IOReturn (*info)(CFTypeRef, FramebufferBrightnessInfo *);
  CFDictionaryRef (*capabilities)(CFTypeRef);
};

template <typename T> bool symbol(void *library, const char *name, T &entry)
{
  entry = reinterpret_cast<T>(dlsym(library, name));
  if (!entry) std::fprintf(stderr, "27.3 export unavailable: %s\n", name);
  return entry != nullptr;
}

bool loadAppleBrightness(AppleBrightness27_3 &api, bool probe)
{
  char build[64] = {};
  size_t size = sizeof build;
  if (sysctlbyname("kern.osversion", build, &size, nullptr, 0) || std::strcmp(build, kBuild)) {
    std::fprintf(stderr, "native API requires macOS 27.3 build %s\n", kBuild);
    return false;
  }
  auto *ds = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/Versions/A/DisplayServices", RTLD_LOCAL | RTLD_NOW);
  if (!ds) {
    std::fprintf(stderr, "DisplayServices: %s\n", dlerror());
    return false;
  }
  if (!symbol(ds, "DisplayServicesCanChangeBrightness", api.canChange)) return false;
  if (!probe) return symbol(ds, "DisplayServicesSetBrightness", api.set);
  auto *fb = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/Versions/A/IOMobileFramebuffer", RTLD_LOCAL | RTLD_NOW);
  if (!fb) {
    std::fprintf(stderr, "IOMobileFramebuffer: %s\n", dlerror());
    return false;
  }
  return symbol(ds, "DisplayServicesGetBrightness", api.get)
      && symbol(fb, "IOMobileFramebufferGetMainDisplay", api.mainDisplay)
      && symbol(fb, "IOMobileFramebufferSetBrightnessControlCallback", api.setCallback)
      && symbol(fb, "IOMobileFramebufferGetBrightnessControlInfo", api.info)
      && symbol(fb, "IOMobileFramebufferGetBrightnessControlCapabilities", api.capabilities);
}

bool panelDisplay(CGDirectDisplayID &display)
{
  CGDirectDisplayID displays[32];
  uint32_t count = 0;
  const CGError status = CGGetOnlineDisplayList(32, displays, &count);
  if (status) {
    std::fprintf(stderr, "CGGetOnlineDisplayList status=%d\n", status);
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!CGDisplayIsBuiltin(displays[i])) continue;
    if (display) {
      std::fprintf(stderr, "more than one built-in display\n");
      return false;
    }
    display = displays[i];
  }
  if (!display) std::fprintf(stderr, "built-in display unavailable\n");
  return display != 0;
}

CFTypeRef value(CFDictionaryRef d, CFStringRef key, CFTypeID type)
{
  CFTypeRef object = d ? CFDictionaryGetValue(d, key) : nullptr;
  return object && CFGetTypeID(object) == type ? object : nullptr;
}

bool number(CFDictionaryRef d, CFStringRef key, double &output)
{
  auto *n = static_cast<CFNumberRef>(value(d, key, CFNumberGetTypeID()));
  return n && CFNumberGetValue(n, kCFNumberDoubleType, &output) && std::isfinite(output);
}

struct RegistryRange {
  double min = NAN, max = NAN, applied = NAN;
};

bool registryRange(CFDictionaryRef properties, CFStringRef key, const char *name, RegistryRange &out)
{
  auto *ranges = static_cast<CFDictionaryRef>(value(properties, CFSTR("IODisplayParameters"), CFDictionaryGetTypeID()));
  auto *range = static_cast<CFDictionaryRef>(value(ranges, key, CFDictionaryGetTypeID()));
  const bool valid = number(range, CFSTR("min"), out.min) && number(range, CFSTR("max"), out.max) &&
      number(range, CFSTR("value"), out.applied) && out.min >= 0 && out.min < out.max && out.applied >= out.min && out.applied <= out.max;
  std::printf("PianoBacklight %s valid=%d min=%.0f max=%.0f value=%.0f\n", name, valid, out.min, out.max, out.applied);
  return valid;
}

bool probeRegistry(RegistryRange &luminance)
{
  const io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PianoBacklight"));
  if (!service) {
    std::fprintf(stderr, "PianoBacklight service unavailable\n");
    return false;
  }
  CFMutableDictionaryRef properties = nullptr;
  const IOReturn status = IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0);
  IOObjectRelease(service);
  if (status || !properties) {
    std::fprintf(stderr, "PianoBacklight snapshot status=%#x\n", unsigned(status));
    return false;
  }
  const bool enabled = value(properties, CFSTR("backlight-control"), CFBooleanGetTypeID()) == kCFBooleanTrue;
  double model = 0, crc = 0;
  if (number(properties, CFSTR("piano-backlight-reference"), model) &&
      number(properties, CFSTR("piano-backlight-map-crc"), crc) && crc >= 0 && crc <= UINT32_MAX)
    std::printf("PianoBacklight model=%s map_crc=%08x\n", model == 1 ? "reference" : "linear", uint32_t(crc));
  RegistryRange codeRange;
  const bool code = registryRange(properties, CFSTR("brightness"), "brightness", codeRange);
  const bool nits = registryRange(properties, CFSTR("BrightnessMilliNits"), "BrightnessMilliNits", luminance);
  CFRelease(properties);
  return enabled && code && nits;
}

// The monitor can deliver asynchronously on its private dispatch queue.
void framebufferReady(CFTypeRef, uint32_t present, void *context)
{
  if (present) dispatch_semaphore_signal(static_cast<dispatch_semaphore_t>(context));
}

Exit probeFramebuffer(const AppleBrightness27_3 &api, const RegistryRange &luminance)
{
  CFTypeRef framebuffer = nullptr;
  const IOReturn opened = api.mainDisplay(&framebuffer);
  if (opened || !framebuffer) {
    std::fprintf(stderr, "IOMobileFramebufferGetMainDisplay status=%#x\n", unsigned(opened));
    return opened ? ioExit(opened) : unavailable;
  }
  // GetBrightnessControlInfo is client-local. Subscribe on this connection before reading it.
  static dispatch_semaphore_t ready = dispatch_semaphore_create(0);
  const IOReturn registered = ready ? api.setCallback(framebuffer, framebufferReady, ready) : kIOReturnNoMemory;
  const bool notified = !registered && dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) == 0;
  std::printf("IOMobileFramebufferSetBrightnessControlCallback status=%#x notified=%d\n", unsigned(registered), notified);
  FramebufferBrightnessInfo info{};
  const IOReturn status = api.info(framebuffer, &info);
  const bool range = !status && std::isfinite(info.minNits) && std::isfinite(info.maxNits) &&
      info.minNits > 0 && info.maxNits > info.minNits;
  CFDictionaryRef capabilities = api.capabilities(framebuffer);
  const bool dictionary = capabilities && CFGetTypeID(capabilities) == CFDictionaryGetTypeID();
  const bool builtin = dictionary && value(capabilities, CFSTR("BuiltIn"), CFBooleanGetTypeID()) == kCFBooleanTrue;
  double min = NAN, max = NAN;
  const bool coherent = dictionary && number(capabilities, CFSTR("MinNits"), min) &&
      number(capabilities, CFSTR("MaxNits"), max) && range && std::abs(min - info.minNits) < 0.01 && std::abs(max - info.maxNits) < 0.01 &&
      std::abs(luminance.min / 1000 - min) < 0.01 && std::abs(luminance.max / 1000 - max) < 0.01;
  std::printf("IOMobileFramebufferGetBrightnessControlInfo status=%#x type=%u flags=%#x min_nits=%.6f max_nits=%.6f valid=%d\n",
              unsigned(status), info.type, info.flags, info.minNits, info.maxNits, range);
  std::printf("IOMobileFramebufferGetBrightnessControlCapabilities present=%d builtin=%d min_nits=%.6f max_nits=%.6f coherent=%d\n",
              dictionary, builtin, min, max, coherent);
  if (!registered) api.setCallback(framebuffer, nullptr, nullptr);
  CFRelease(framebuffer);
  if (registered) return ioExit(registered);
  if (status) return ioExit(status);
  return range && builtin && coherent ? ok : unavailable;
}

Exit displayServices(const Command &command)
{
  const bool probe = command.action == Action::probe;
  RegistryRange luminance;
  const bool registry = probe && probeRegistry(luminance);
  AppleBrightness27_3 api{};
  if (!loadAppleBrightness(api, probe)) return unavailable;
  CGDirectDisplayID display = 0;
  if (!panelDisplay(display)) return unavailable;
  const bool canChange = api.canChange(display);
  std::printf("DisplayServicesCanChangeBrightness display=%u builtin=1 value=%d\n", display, canChange);
  if (!probe) {
    if (!canChange) return unavailable;
    const int32_t status = api.set(display, command.level);
    std::printf("DisplayServicesSetBrightness display=%u level=%.8g status=%d\n", display, command.level, status);
    return status ? apiError : ok;
  }
  float actual = NAN;
  const int32_t status = api.get(display, &actual);
  std::printf("DisplayServicesGetBrightness display=%u level=%.8g status=%d\n", display, actual, status);
  const Exit fb = probeFramebuffer(api, luminance);
  if (!canChange || !registry) return unavailable;
  if (status || !std::isfinite(actual) || actual < 0 || actual > 1) return apiError;
  return fb;
}

Exit setRegistry(const Command &command)
{
  const char *name = nullptr;
  CFStringRef key = nullptr;
  switch (command.action) {
  case Action::adapterNits: name = "root-direct-adapter-16.16"; key = CFSTR("brightness-nits"); break;
  case Action::reference: name = "reference"; key = CFSTR("piano-backlight-reference"); break;
  case Action::automatic: name = "automatic"; key = CFSTR("piano-backlight-auto"); break;
  default: return usage;
  }
  const io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PianoBacklight"));
  if (!service) {
    std::fprintf(stderr, "PianoBacklight service unavailable\n");
    return unavailable;
  }
  CFNumberRef setting = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &command.value);
  const void *keys[] = {key}, *values[] = {setting};
  CFDictionaryRef request = setting ? CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                          &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : nullptr;
  const IOReturn status = request ? IORegistryEntrySetCFProperties(service, request) : kIOReturnNoMemory;
  if (request) CFRelease(request);
  if (setting) CFRelease(setting);
  IOObjectRelease(service);
  std::printf("PianoBacklight %s value=%d status=%#x\n", name, command.value, unsigned(status));
  return ioExit(status);
}

}  // namespace

int main(int argc, const char *argv[])
{
  Command command{};
  if (!parse(argc, argv, command)) {
    std::fprintf(stderr, "usage: piano-brightness-control --probe | --level 0..1 | --adapter-nits N | --reference 0|1 | --auto 0|1\n");
    return usage;
  }
  if (!authorized(command.action, geteuid())) {
    std::fprintf(stderr, "this PianoBacklight property write requires root\n");
    return unauthorized;
  }
  return command.action == Action::probe || command.action == Action::level ? displayServices(command) : setRegistry(command);
}
