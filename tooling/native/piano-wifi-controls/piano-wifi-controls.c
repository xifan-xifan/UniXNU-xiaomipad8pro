// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// Hand the boot-ON guard to desktop policy once a real console login and
// published AirPort state are stable. Raw SET19 has no user-origin bit.
#include "DesktopGate.h"
#include <SystemConfiguration/SystemConfiguration.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <syslog.h>
#include <unistd.h>

struct request { char name[16]; int32_t type, value; uint32_t length, pad; uint64_t data; };
_Static_assert(sizeof(struct request) == 40 && offsetof(struct request, data) == 32, "Apple80211 request ABI");
static int raw(int type, void *data, uint32_t length, int set)
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return errno;
  struct request r = {.name = "en0", .type = type, .length = length, .data = (uintptr_t)data};
  int error = ioctl(fd, set ? 0x802869c8UL : 0xc02869c9UL, &r) ? errno : 0;
  close(fd);
  return error ? error : r.length != length ? EPROTO : 0;
}
struct sample { uid_t uid; int desktop, published, on; };
static struct sample sample(SCDynamicStoreRef store)
{
  struct sample s = {0}; gid_t gid = 0;
  CFStringRef user = SCDynamicStoreCopyConsoleUser(store, &s.uid, &gid);
  s.desktop = user && s.uid >= 501 && !CFEqual(user, CFSTR("loginwindow")) && !CFEqual(user, CFSTR("_mbsetupuser"));
  if (user) CFRelease(user);
  CFPropertyListRef value = SCDynamicStoreCopyValue(store, CFSTR("State:/Network/Interface/en0/AirPort"));
  s.published = value && CFGetTypeID(value) == CFDictionaryGetTypeID() && CFDictionaryGetCount(value) > 0;
  if (value) CFRelease(value);
  uint32_t power[6] = {0};
  s.on = !raw(19, power, sizeof power, 0) && power[0] == 1 && power[1] == 1 && power[2] == 1;
  return s;
}
static int armed(void)
{
  int value = 0; size_t size = sizeof value;
  return !sysctlbyname("machdep.piano_wifi_power_explicit", &value, &size, NULL, 0) && size == sizeof value && value == 1;
}
static int probe(SCDynamicStoreRef store, int set_check)
{
  struct sample s = sample(store);
  printf("probe_started=yes\nconsole_desktop=%s\nsc_published=%s\nraw_power_on=%s\nguard_armed=%s\n",
         s.desktop ? "yes" : "no", s.published ? "yes" : "no", s.on ? "yes" : "no", armed() ? "yes" : "no");
  char country[3] = {0}; int error = raw(51, country, sizeof country, 0);
  int valid = !error && country[0] >= 'A' && country[0] <= 'Z' && country[1] >= 'A' && country[1] <= 'Z' && !country[2] &&
              memcmp(country, "XX", 2);
  printf("country_raw_error=%d\ncountry_raw_valid=%s\n", error, valid ? "yes" : "no");
  if (valid) printf("country_raw=%.2s\n", country);
  if (set_check && valid) {
    uint8_t wire[8] = {1, 0, 0, 0, (uint8_t)country[0], (uint8_t)country[1], 0, 0};
    printf("country_same_set_ok=%s\n", raw(51, wire, sizeof wire, 1) == 0 ? "yes" : "no");
    // No foreign-country write: refusal of unsupported changes is covered by
    // the production parser host tests; the hardware probe is idempotent only.
  }
  puts("probe_complete=yes");
  return valid ? 0 : 2;
}
int main(int argc, char **argv)
{
  SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("PianoWifiDesktopControls"), NULL, NULL);
  if (!store) return 1;
  if (argc == 2 && (!strcmp(argv[1], "--probe") || !strcmp(argv[1], "--country-set-check"))) {
    int rc = probe(store, !strcmp(argv[1], "--country-set-check")); CFRelease(store); return rc;
  }
  if (argc != 1 || geteuid()) { CFRelease(store); return 1; }
  openlog("piano-wifi-controls", LOG_PID, LOG_DAEMON);
  struct piano_wifi_desktop_gate gate = {0};
  for (;;) {
    if (armed()) break; // restart or a deliberate manual arm: never reset user policy
    struct sample s = sample(store);
    if (piano_wifi_desktop_ready(&gate, s.uid, s.desktop, s.published, s.on)) {
      // Recheck immediately before the one-way handoff; no power setter and no
      // fabricated dynamic-store state is involved.
      struct sample again = sample(store);
      if (again.desktop && again.published && again.on && again.uid == gate.prior) {
        int one = 1;
        if (!sysctlbyname("machdep.piano_wifi_power_explicit", NULL, NULL, &one, sizeof one) && armed()) {
          syslog(LOG_NOTICE, "PianoWifiUsable2 desktop handoff: stable console, published AirPort, raw power ON; menu OFF enabled");
          break;
        }
      }
      gate.stable = 0;
    }
    sleep(2);
  }
  CFRelease(store); closelog(); return 0;
}
