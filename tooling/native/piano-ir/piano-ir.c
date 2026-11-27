// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano-ir: the IR transmitter from the tablet's macOS (docs/drivers/ir.md). Talks to the PianoIR kext (kernel boot argument
// piano-ir=2) through its IORegistry properties (package/PianoIR/PianoIRClient.h); the kext checks the caller is an administrator
// (run with sudo, or from the root shell of pianousb-shell) and checks and encodes every send itself.
//   piano-ir status                                 (also without arguments)
//   piano-ir raw [--carrier HZ] [--duty PCT] D ...  durations in us, mark first, odd count; ir-ctl's "+9000 -4500" signs and
//                                                   "pulse"/"space" words are accepted; "-" reads them from standard input
//   piano-ir nec ADDR CMD [--repeat N] [--carrier HZ]  an NEC frame (ADDR 0..0xff, or 0x100..0xffff extended) and N repeat codes
//                                                   108 ms apart
//   piano-ir blink [SECONDS]                        500 ms of carrier, 500 ms dark, for SECONDS (default 5): the camera check
// Exit status 0 when every send went out, 1 when one failed, 2 on a usage error or no PianoIR service.
// Build on the Mac (arm64, ad-hoc signed by the linker):
//   cc -O2 -Wall -Wextra -I package/PianoIR -o piano-ir piano-ir.c -framework IOKit -framework CoreFoundation
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "IrWave.h"
#include "PianoIRClient.h"

static int property(io_service_t s, const char *key, char *out, size_t size)
{
  CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
  CFTypeRef v = IORegistryEntryCreateCFProperty(s, k, kCFAllocatorDefault, 0);
  CFRelease(k);
  out[0] = 0;
  if (!v) return 0;
  int ok = 0;
  if (CFGetTypeID(v) == CFStringGetTypeID()) ok = CFStringGetCString((CFStringRef)v, out, (CFIndex)size, kCFStringEncodingUTF8);
  else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
    long long n = 0;
    CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
    ok = snprintf(out, size, "%lld", n) > 0;
  }
  CFRelease(v);
  return ok;
}

static void show(io_service_t s)
{
  char level[16], state[220], survey[260], last[260], sends[16];
  property(s, kPianoIRLevel, level, sizeof level);
  property(s, kPianoIRState, state, sizeof state);
  property(s, kPianoIRSurvey, survey, sizeof survey);
  property(s, kPianoIRLast, last, sizeof last);
  property(s, kPianoIRSends, sends, sizeof sends);
  printf("level %s: %s\n", level[0] ? level : "?", state[0] ? state : "(no state yet)");
  if (survey[0]) printf("survey: %s\n", survey);
  printf("sends %s; last: %s\n", sends[0] ? sends : "0", last[0] ? last : "none");
}

static void setNumber(CFMutableDictionaryRef d, const char *key, long long v)
{
  CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
  CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongLongType, &v);
  CFDictionarySetValue(d, k, n);
  CFRelease(n);
  CFRelease(k);
}

static kern_return_t sendList(io_service_t s, const uint32_t *d, unsigned n, long long carrier, long long duty)
{
  CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)d, (CFIndex)(n * sizeof *d));
  CFDictionarySetValue(dict, CFSTR(kPianoIRSend), data);
  CFRelease(data);
  if (carrier > 0) setNumber(dict, kPianoIRCarrier, carrier);
  if (duty > 0) setNumber(dict, kPianoIRDuty, duty);
  const kern_return_t r = IORegistryEntrySetCFProperties(s, dict);
  CFRelease(dict);
  return r;
}

static kern_return_t sendBurst(io_service_t s, long long ms)
{
  CFMutableDictionaryRef dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  setNumber(dict, kPianoIRBurst, ms);
  const kern_return_t r = IORegistryEntrySetCFProperties(s, dict);
  CFRelease(dict);
  return r;
}

static int report(io_service_t s, kern_return_t r)
{
  char last[260];
  property(s, kPianoIRLast, last, sizeof last);
  if (r == KERN_SUCCESS) {
    printf("ok: %s\n", last);
    return 0;
  }
  if (r == kIOReturnNotPrivileged) fprintf(stderr, "piano-ir: not privileged (run with sudo)\n");
  else if (r == kIOReturnNotReady) fprintf(stderr, "piano-ir: the transmitter is not ready (piano-ir=2? see 'piano-ir status')\n");
  else if (r == kIOReturnUnsupported) fprintf(stderr, "piano-ir: PianoIR does not take sends (level 1, or an old kext)\n");
  else fprintf(stderr, "piano-ir: send failed (0x%x): %s\n", r, last);
  return 1;
}

static long long number(const char *s)
{
  char *end = NULL;
  errno = 0;
  const long long v = strtoll(s, &end, 0);
  return (!errno && end != s && !*end && v >= 0) ? v : -1;
}

static long long monoUs(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void sleepUs(long long us)
{
  if (us <= 0) return;
  struct timespec ts = {(time_t)(us / 1000000), (long)(us % 1000000) * 1000};
  nanosleep(&ts, NULL);
}

static int usage(void)
{
  fprintf(stderr, "usage: piano-ir status | raw [--carrier HZ] [--duty PCT] D ... | nec ADDR CMD [--repeat N] [--carrier HZ] | blink [SECONDS]\n");
  return 2;
}

static io_service_t openService(void)
{
  io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kPianoIRClass));
  if (!s) fprintf(stderr, "piano-ir: no PianoIR service (boot with piano-ir=2)\n");
  return s;
}

// Durations from words (argv or standard input): numbers with an optional sign; "pulse"/"space" words skipped.
static int addWord(const char *w, uint32_t *list, unsigned *n)
{
  if (!strcmp(w, "pulse") || !strcmp(w, "space")) return 0;
  uint32_t us;
  if (!ir_parse_duration(w, &us)) {
    fprintf(stderr, "piano-ir: not a duration: '%s'\n", w);
    return -1;
  }
  if (*n >= IR_MAX_DURATIONS) {
    fprintf(stderr, "piano-ir: more than %u durations\n", IR_MAX_DURATIONS);
    return -1;
  }
  list[(*n)++] = us;
  return 0;
}

int main(int argc, char **argv)
{
  io_service_t s;
  int rc = 0;
  const char *cmd = argc > 1 ? argv[1] : "status";
  long long carrier = IR_DEFAULT_CARRIER, duty = IR_DEFAULT_DUTY, repeat = 0;
  if (!strcmp(cmd, "status")) {
    if (argc > 2) return usage();
    if (!(s = openService())) return 2;
    show(s);
  } else if (!strcmp(cmd, "raw")) {
    static uint32_t list[IR_MAX_DURATIONS];
    unsigned n = 0;
    for (int i = 2; i < argc && rc == 0; i++) {
      if (!strcmp(argv[i], "--carrier") && i + 1 < argc) {
        carrier = number(argv[++i]);
        if (!ir_carrier_valid((uint64_t)carrier)) return usage();
      } else if (!strcmp(argv[i], "--duty") && i + 1 < argc) {
        duty = number(argv[++i]);
        if (!ir_duty_valid((uint64_t)duty)) return usage();
      } else if (!strcmp(argv[i], "-")) {
        char w[64];
        while (rc == 0 && scanf("%63s", w) == 1) rc = addWord(w, list, &n) ? 2 : 0;
      } else rc = addWord(argv[i], list, &n) ? 2 : 0;
    }
    if (rc) return rc;
    if (n && !(n & 1)) n--;   // a trailing space carries nothing: dropped (LIRC files often end with one)
    {
      struct ir_wave_plan p;
      // The kext checks the size at the actual engine rate.
      const int e = ir_wave_check(list, n, (uint32_t)carrier, (uint32_t)duty, IR_MIN_RATE, &p);
      if (e) {
        fprintf(stderr, "piano-ir: %s\n", ir_error_name(e));
        return 2;
      }
    }
    if (!(s = openService())) return 2;
    rc = report(s, sendList(s, list, n, carrier, duty));
  } else if (!strcmp(cmd, "nec")) {
    if (argc < 4) return usage();
    const long long addr = number(argv[2]), code = number(argv[3]);
    for (int i = 4; i < argc; i++) {
      if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = number(argv[++i]);
      else if (!strcmp(argv[i], "--carrier") && i + 1 < argc) {
        carrier = number(argv[++i]);
        if (!ir_carrier_valid((uint64_t)carrier)) return usage();
      } else return usage();
    }
    if (addr < 0 || addr > 0xffff || code < 0 || code > 0xff || repeat < 0 || repeat > 50) return usage();
    if (!(s = openService())) return 2;
    uint32_t frame[IR_NEC_ENTRIES], rep[IR_NEC_REPEAT_ENTRIES];
    const int n = ir_nec_frame((uint32_t)addr, (uint8_t)code, frame, IR_NEC_ENTRIES);
    ir_nec_repeat(rep, IR_NEC_REPEAT_ENTRIES);
    long long start = monoUs();   // the send returns once the frame is on the wire: each next one starts 108 ms after the last start
    rc = report(s, sendList(s, frame, (unsigned)n, carrier, 0));
    for (long long r = 0; r < repeat && rc == 0; r++) {
      sleepUs(start + IR_NEC_PERIOD_US - monoUs());
      start = monoUs();
      rc = report(s, sendList(s, rep, IR_NEC_REPEAT_ENTRIES, carrier, 0));
    }
  } else if (!strcmp(cmd, "blink")) {
    if (argc > 3) return usage();
    const long long seconds = argc > 2 ? number(argv[2]) : 5;
    if (seconds < 1 || seconds > 120) return usage();
    if (!(s = openService())) return 2;
    for (long long i = 0; i < seconds && rc == 0; i++) {
      rc = report(s, sendBurst(s, 500));
      sleepUs(500000);
    }
  } else {
    return usage();
  }
  IOObjectRelease(s);
  return rc;
}
