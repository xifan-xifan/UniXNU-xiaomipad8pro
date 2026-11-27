// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano-torch: the rear torch from the tablet's macOS (docs/drivers/torch.md section 5). Talks to the PianoTorch kext (kernel boot
// argument piano-torch=2) through its IORegistry properties; the kext checks the caller is an administrator (run with sudo, or
// from the root shell of pianousb-shell) and does every hardware check itself (limits, read-back, time limit, temperatures).
//   piano-torch on [MA [SECONDS]]   on at MA mA (default: the kext's piano-torch-ma) for SECONDS s (default: piano-torch-timeout)
//   piano-torch off
//   piano-torch toggle
//   piano-torch status              (also without arguments)
// Exit status 0 when the state after the request is the one asked for, 1 otherwise, 2 on a usage error or no PianoTorch service.
// Build on the Mac (arm64, ad-hoc signed by the linker):
//   cc -O2 -Wall -Wextra -o piano-torch piano-torch.c -framework IOKit -framework CoreFoundation
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "piano-torch-args.h"

static int property(io_service_t s, const char *key, char *out, size_t size)
{
  CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
  CFTypeRef v = IORegistryEntryCreateCFProperty(s, k, kCFAllocatorDefault, 0);
  CFRelease(k);
  out[0] = 0;
  if (!v) return 0;
  int ok = 0;
  if (CFGetTypeID(v) == CFStringGetTypeID()) ok = CFStringGetCString((CFStringRef)v, out, (CFIndex)size, kCFStringEncodingUTF8);
  else if (CFGetTypeID(v) == CFBooleanGetTypeID()) ok = snprintf(out, size, "%s", CFBooleanGetValue((CFBooleanRef)v) ? "true" : "false") > 0;
  else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
    long long n = 0;
    CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
    ok = snprintf(out, size, "%lld", n) > 0;
  }
  CFRelease(v);
  return ok;
}

static int lit(io_service_t s)
{
  char v[16];
  return property(s, "piano-torch-lit", v, sizeof v) && strcmp(v, "true") == 0;
}

static int reached(io_service_t s, int want)
{
  if (lit(s) != want) return 0;
  if (want) return 1;
  char state[160];
  return property(s, "piano-torch-state", state, sizeof state) &&
         (strcmp(state, "off") == 0 || strncmp(state, "off (", 5) == 0);
}

static void show(io_service_t s)
{
  char level[16], state[160], survey[240];
  property(s, "piano-torch", level, sizeof level);
  property(s, "piano-torch-state", state, sizeof state);
  property(s, "piano-torch-survey", survey, sizeof survey);
  printf("level %s: %s\n", level[0] ? level : "?", state[0] ? state : "(no state yet)");
  if (survey[0]) printf("survey: %s\n", survey);
}

static kern_return_t request(io_service_t s, long long ma, long long seconds)
{
  CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  if (ma < 0) {
    CFDictionarySetValue(d, CFSTR("piano-torch-set"), kCFBooleanTrue);   // the kext's default current
  } else {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongLongType, &ma);
    CFDictionarySetValue(d, CFSTR("piano-torch-set"), n);
    CFRelease(n);
  }
  if (seconds > 0) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongLongType, &seconds);
    CFDictionarySetValue(d, CFSTR("piano-torch-seconds"), n);
    CFRelease(n);
  }
  const kern_return_t r = IORegistryEntrySetCFProperties(s, d);
  CFRelease(d);
  return r;
}

int main(int argc, char **argv)
{
  struct arguments args;
  if (!parse_arguments(argc, argv, &args)) {
    fprintf(stderr, "usage: piano-torch [on [MA [SECONDS]] | off | toggle | status]\n");
    return 2;
  }
  io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PianoTorch"));
  if (!s) {
    fprintf(stderr, "piano-torch: no PianoTorch service (boot with piano-torch=2)\n");
    return 2;
  }
  const int want = args.command == STATUS ? -1 : args.command == TOGGLE ? !lit(s) : args.command == ON;
  if (want >= 0) {
    const kern_return_t r = request(s, want ? args.ma : 0, args.seconds);
    if (r != KERN_SUCCESS) {
      fprintf(stderr, "piano-torch: the kext refused the request: %#x%s\n", r,
              r == kIOReturnNotPrivileged ? " (not an administrator: use sudo)"
              : r == kIOReturnUnsupported ? " (piano-torch=1: read-only)"
              : r == kIOReturnNotReady ? " (control is not up: see the status)" : "");
      IOObjectRelease(s);
      return 1;
    }
    for (int i = 0; i < 20 && !reached(s, want); ++i) usleep(100000);
  }
  show(s);
  const int ok = want < 0 || reached(s, want);
  IOObjectRelease(s);
  return ok ? 0 : 1;
}
