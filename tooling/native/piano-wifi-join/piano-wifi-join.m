// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano-wifi-join: hands PianoWifi the network to join, once, from a root-only file (docs/drivers/wifi.md §13.2).
//
//   /usr/local/libexec/piano-wifi-join [FILE]      (FILE defaults to /private/var/db/piano-wifi/profile)
//
// Run as root at boot by the LaunchDaemon com.unixnu.piano-wifi-join. The file is the owner's and stays on the tablet: it must
// be a regular file owned by root with no group or other permission bits (like an ssh key), in a directory owned by root with
// none either; anything else is refused without reading it. Its lines, `key=value`, value to the end of the line:
//   ssid=NAME            the network's name, 1 to 32 bytes, as it is (no quoting)
//   security=open        or wpa2-psk
//   psk=PASSPHRASE       for wpa2-psk: 8 to 63 printable characters, or 64 hex digits
//   bssid=aa:bb:cc:dd:ee:ff   optional: the access point to prefer
//   mac=02:..:..:..:..:..     optional: the station's own address (locally administered)
// After airportd adopts the interface, daily boots provision the native saved network.
// CoreWLAN owns any fallback association, which must publish a new SET20.
#import <Foundation/Foundation.h>
#import <CoreWLAN/CoreWLAN.h>
#import <Security/Security.h>
#import <CoreLocation/CoreLocation.h>
#include "BootJoin.h"
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../../package/PianoWifi/WifiProfile.h"

static void timeline(const char *format, ...)
{
  char event[160];
  va_list args;
  va_start(args, format);
  vsnprintf(event, sizeof event, format, args);
  va_end(args);
  const int saved_errno = errno;
  fprintf(stdout, "PianoWifiBootJoinTimelineV1: time=%lld pid=%d %s\n",
          (long long)time(NULL), getpid(), event);
  fflush(stdout);
  errno = saved_errno;
}

static int finish(int status, const char *stage)
{
  timeline("exit status=%d stage=%s", status, stage);
  return status;
}

static void alarm_expired(int signo)
{
  static const char line[] = "PianoWifiBootJoinTimelineV1: exit signal=SIGALRM\n";
  (void)write(STDERR_FILENO, line, sizeof line - 1);
  // SA_RESETHAND restores the default disposition. Preserve the signal observed by launchd.
  (void)kill(getpid(), signo);
}

static void wipe(void *p, size_t n)
{
  volatile unsigned char *at = (volatile unsigned char *)p;
  while (n--) *at++ = 0;
}

// The file's permissions, checked on the open descriptor and on its directory before a byte is read.
static int safe_open(const char *path)
{
  char dir[1024];
  const size_t len = strlen(path);
  if (len >= sizeof dir) return -1;
  memcpy(dir, path, len + 1);
  char *slash = strrchr(dir, '/');
  if (!slash || slash == dir) return -1;
  *slash = 0;
  struct stat ds;
  if (lstat(dir, &ds) != 0) return errno == ENOENT ? -2 : -1;
  if (!S_ISDIR(ds.st_mode) || ds.st_uid != 0 || (ds.st_mode & 077)) {
    fprintf(stderr, "piano-wifi-join: the profile's directory must be root's with mode 700; not read\n");
    return -1;
  }
  const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT ? -2 : -1;
  struct stat fs;
  if (fstat(fd, &fs) != 0 || !S_ISREG(fs.st_mode) || fs.st_uid != 0 || (fs.st_mode & 077) || fs.st_size > 4096) {
    fprintf(stderr, "piano-wifi-join: the profile must be a regular file of root's with mode 600, at most 4 KiB; not read\n");
    close(fd);
    return -1;
  }
  return fd;
}

enum {
  BootJoinDeadline = 480, NativeJoinGraceSeconds = 35, ProvisionDeadline = 25,
  LocationStableSeconds = 10, LocationQuerySeconds = 8
};
static const char kick_receipt[] = "/var/run/piano-airportd-kick";
extern char **environ;

static int native_boot(void)
{
  int value;
  size_t size = sizeof value;
  if (sysctlbyname("machdep.piano_wifi_native_boot", &value, &size, NULL, 0) || size != sizeof value)
    return -1;
  return value == 1;
}

static int kick_completed(const char *path, const char *boot)
{
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return 0;
  struct stat st;
  char body[64];
  ssize_t length = -1;
  if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 077))
    length = read(fd, body, sizeof body);
  close(fd);
  size_t expected = strlen(boot);
  return expected > 0 && expected < sizeof body && length == (ssize_t)expected + 1 &&
         body[expected] == '\n' && !memcmp(body, boot, expected);
}

static int boot_uuid(char *boot, size_t size)
{
  return !sysctlbyname("kern.bootsessionuuid", boot, &size, NULL, 0) && size == 37 && boot[36] == '\0';
}

// Native CoreWLAN writer used by networksetup on 27.3. The native forms are
// used only for readback, so identifier encoding and security normalization
// stay in CoreWiFi.
extern OSStatus CWAddNetworkProfile(CWNetworkProfile *, SFAuthorization *);
extern OSStatus SecTrustedApplicationCreateApplicationGroup(
    const char *, SecCertificateRef, SecTrustedApplicationRef *);
@interface CWFNetworkProfile : NSObject
@property (readonly) NSData *SSID;
@property (readonly) uint64_t supportedSecurityTypes;
@property (readonly, getter=isSystemMode) BOOL systemMode;
@property (readonly, getter=isAutoJoinDisabled) BOOL autoJoinDisabled;
@property (readonly) NSDate *networkDisabledUntilDate;
@property (readonly) NSString *identifier;
- (instancetype)initWithExternalForm:(NSDictionary *)form;
@end
@interface CWNetworkProfile (PianoNativeProfile)
- (CWFNetworkProfile *)coreWiFiNetworkProfile;
@end
@interface CWFKeyValueStore : NSObject
- (instancetype)initWithType:(NSInteger)type identifier:(NSString *)identifier;
- (void)activate;
- (NSDictionary *)dictionaryRepresentation;
@end

enum saved_profile { SavedReady = 0, SavedUnavailable = 1, SavedPreserved = 2 };
enum known_profile { KnownUnavailable, KnownAbsent, KnownMatches, KnownPreserve };

static enum known_profile known_profile_state(NSDictionary *records, NSData *ssid, uint64_t mask, BOOL exact)
{
  if (!records) return KnownUnavailable;
  NSUInteger count = 0;
  BOOL matches = NO;
  Class type = NSClassFromString(@"CWFNetworkProfile");
  if (!type) return KnownUnavailable;
  for (NSString *key in records) {
    if (![key hasPrefix:@"wifi.network.ssid."] || ![records[key] isKindOfClass:NSDictionary.class]) continue;
    CWFNetworkProfile *profile = [[type alloc] initWithExternalForm:records[key]];
    if (![profile.SSID isEqualToData:ssid]) continue;
    ++count;
    const uint64_t security = profile.supportedSecurityTypes;
    matches = [profile.identifier isEqualToString:key] && profile.isSystemMode && !profile.isAutoJoinDisabled &&
              [profile.networkDisabledUntilDate timeIntervalSinceNow] <= 0 &&
              (exact ? security == mask : (security & mask) == mask);
  }
  return !count ? KnownAbsent : count == 1 && matches ? KnownMatches : KnownPreserve;
}

static CWFKeyValueStore *known_store(void)
{
  CWFKeyValueStore *store = [[NSClassFromString(@"CWFKeyValueStore") alloc]
      initWithType:1 identifier:@"com.apple.wifi.known-networks"];
  [store activate];
  return store;
}

// Create only. A preexisting owner credential, including one appearing
// between lookup and creation, stays untouched.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static OSStatus add_system_credential(SecKeychainRef keychain, NSData *ssid, NSData *password)
{
  const char *applications[] = {NULL, "/usr/libexec/airportd"};
  NSMutableArray *trusted = [NSMutableArray array];
  for (unsigned i = 0; i < sizeof applications / sizeof applications[0]; ++i) {
    SecTrustedApplicationRef app = NULL;
    OSStatus status = SecTrustedApplicationCreateFromPath(applications[i], &app);
    if (status != errSecSuccess) return status;
    [trusted addObject:CFBridgingRelease(app)];
  }
  SecTrustedApplicationRef group = NULL;
  OSStatus status = SecTrustedApplicationCreateApplicationGroup("AirPort", NULL, &group);
  if (status != errSecSuccess) return status;
  [trusted addObject:CFBridgingRelease(group)];
  SecAccessRef access = NULL;
  status = SecAccessCreate(CFSTR("AirPort"), (__bridge CFArrayRef)trusted, &access);
  if (status != errSecSuccess) return status;
  const char service[] = "AirPort", kind[] = "AirPort network password";
  SecKeychainAttribute attributes[] = {
    { kSecServiceItemAttr, sizeof service - 1, (void *)service },
    { kSecAccountItemAttr, (UInt32)ssid.length, (void *)ssid.bytes },
    { kSecLabelItemAttr, (UInt32)ssid.length, (void *)ssid.bytes },
    { kSecDescriptionItemAttr, sizeof kind - 1, (void *)kind }
  };
  SecKeychainAttributeList list = { sizeof attributes / sizeof attributes[0], attributes };
  status = SecKeychainItemCreateFromContent(kSecGenericPasswordItemClass, &list, (UInt32)password.length,
                                           password.bytes, keychain, access, NULL);
  CFRelease(access);
  return status;
}

static enum saved_profile ensure_system_credential(NSData *ssid, NSString *password)
{
  if (SecKeychainSetUserInteractionAllowed(false) != errSecSuccess) return SavedUnavailable;
  SecKeychainRef keychain = NULL;
  if (SecKeychainOpen("/Library/Keychains/System.keychain", &keychain) != errSecSuccess) return SavedUnavailable;
  OSStatus status = SecKeychainUnlock(keychain, 0, NULL, false);
  NSString *stored = nil;
  if (status == errSecSuccess) status = CWKeychainFindWiFiPassword(kCWKeychainDomainSystem, ssid, &stored);
  if (status == errSecItemNotFound) {
    status = add_system_credential(keychain, ssid, [password dataUsingEncoding:NSUTF8StringEncoding]);
    if (status == errSecSuccess || status == errSecDuplicateItem)
      status = CWKeychainFindWiFiPassword(kCWKeychainDomainSystem, ssid, &stored);
  }
  CFRelease(keychain);
  if (status != errSecSuccess) return SavedUnavailable;
  return [stored isEqualToString:password] ? SavedReady : SavedPreserved;
}
#pragma clang diagnostic pop

static enum saved_profile provision(const struct piano_wifi_profile *p)
{
  NSData *ssid = [NSData dataWithBytes:p->ssid length:p->ssid_len];
  CWMutableNetworkProfile *profile = [CWMutableNetworkProfile networkProfile];
  profile.ssidData = ssid;
  profile.security = p->psk_len ? kCWSecurityWPA2Personal : kCWSecurityNone;
  CWFNetworkProfile *native = [profile coreWiFiNetworkProfile];
  const uint64_t submitted = native.supportedSecurityTypes;
  const uint64_t compatible = p->psk_len ? 0x10 : submitted; // native WPA2 support
  if (![native.SSID isEqualToData:ssid] || !compatible || (submitted & compatible) != compatible)
    return SavedUnavailable;

  CWFKeyValueStore *store = known_store();
  enum known_profile known = known_profile_state([store dictionaryRepresentation], ssid, compatible, NO);
  if (known == KnownUnavailable) return SavedUnavailable;
  if (known == KnownPreserve) {
    timeline("native known=preserved");
    return SavedPreserved;
  }
  if (p->psk_len) {
    NSString *password = [[NSString alloc] initWithBytes:p->psk length:p->psk_len encoding:NSUTF8StringEncoding];
    const enum saved_profile credential = ensure_system_credential(ssid, password);
    timeline("native system_credential=%s", credential == SavedReady ? "verified" :
             credential == SavedPreserved ? "preserved" : "unavailable");
    if (credential != SavedReady) return credential;
  } else {
    timeline("native system_credential=not-required");
  }
  if (known == KnownMatches) {
    timeline("native known=existing-verified");
    return SavedReady;
  }
  if (CWAddNetworkProfile(profile, nil) != errSecSuccess) {
    timeline("native known=add-failed");
    return SavedUnavailable;
  }
  store = known_store();
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    if (known_profile_state([store dictionaryRepresentation], ssid, submitted, YES) == KnownMatches) {
      timeline("native known=added-verified");
      return SavedReady;
    }
    if (attempt < 7) sleep(1);
  }
  timeline("native known=readback-failed");
  return SavedUnavailable;
}

static int provision_from_stdin(void)
{
  char boot[64];
  if (native_boot() != 1 || !boot_uuid(boot, sizeof boot) || !kick_completed(kick_receipt, boot))
    return SavedUnavailable;
  alarm(ProvisionDeadline);
  // The parent sends a validated regular file via stdin; read directly so
  // stdio does not keep an extra copy of the secret.
  uint8_t bytes[4097];
  size_t length = 0;
  ssize_t n;
  do {
    n = read(STDIN_FILENO, bytes + length, sizeof bytes - length);
    if (n > 0) length += (size_t)n;
  } while (length < sizeof bytes && (n > 0 || (n < 0 && errno == EINTR)));
  struct piano_wifi_profile p = {};
  const char *problem = !length || length == sizeof bytes || n < 0 ? "read" :
                        piano_wifi_profile_parse(bytes, (uint32_t)length, &p);
  wipe(bytes, sizeof bytes);
  int result = SavedUnavailable;
  if (!problem) {
    @autoreleasepool { result = provision(&p); }
  }
  piano_wifi_profile_wipe(&p);
  return result;
}

static enum saved_profile provision_from_file(char *executable, int fd)
{
  posix_spawn_file_actions_t actions;
  if (lseek(fd, 0, SEEK_SET) < 0 || posix_spawn_file_actions_init(&actions)) return SavedUnavailable;
  char *args[] = { executable, "--provision-stdin", NULL };
  pid_t pid;
  int error = posix_spawn_file_actions_adddup2(&actions, fd, STDIN_FILENO);
  if (!error) error = posix_spawn(&pid, executable, &actions, NULL, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (error) return SavedUnavailable;
  int status;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != pid || !WIFEXITED(status)) return SavedUnavailable;
  const int result = WEXITSTATUS(status);
  return result == SavedReady ? SavedReady : result == SavedPreserved ? SavedPreserved : SavedUnavailable;
}

static uint64_t monotonic_seconds(void)
{
  return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / NSEC_PER_SEC;
}

// airportd answers a non-entitled client's GET SSID only through a synchronous CoreLocation proxy. After boot locationd can die
// and respawn every ~3 s (EXC_ARM_PAC_FAIL, LOCATIOND-SSID.md), and that call then blocks for a random time. Before the first
// CoreWLAN call: one live locationd PID for LocationStableSeconds, and a CoreLocation query that returns in a child of its own.
struct location_readiness {
  pid_t pid;
  uint64_t since;
};

static pid_t location_service_pid(void)
{
  const int estimated = proc_listallpids(NULL, 0);
  if (estimated <= 0) return 0;
  const int capacity = estimated + 64;
  pid_t *pids = calloc((size_t)capacity, sizeof *pids);
  if (!pids) return 0;
  const int count = proc_listallpids(pids, capacity * sizeof *pids);
  pid_t result = 0;
  for (int i = 0; i < count && count < capacity; ++i) {
    char name[64];
    if (pids[i] <= 0 || proc_name(pids[i], name, sizeof name) <= 0 || strcmp(name, "locationd")) continue;
    if (result) {
      result = 0;
      break;
    }
    result = pids[i];
  }
  free(pids);
  return result;
}

static int location_query_result(char *executable)
{
  // Isolate the synchronous CoreLocation call from our CoreWLAN client.
  // The child sets its own alarm and checks only the global switch.
  char *args[] = { executable, "--location-query", NULL };
  pid_t pid;
  if (posix_spawn(&pid, executable, NULL, NULL, args, environ)) return 0;
  int status;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int location_ready(struct location_readiness *state, char *executable, unsigned attempt)
{
  const uint64_t now = monotonic_seconds();
  const pid_t pid = location_service_pid();
  if (pid != state->pid) {
    state->pid = pid;
    state->since = now;
  }
  if (!pid || now - state->since < LocationStableSeconds) {
    timeline("location attempt=%u stable=0 present=%d", attempt, pid != 0);
    return 0;
  }
  const int returned = location_query_result(executable);
  const int same_pid = pid == location_service_pid();
  timeline("location attempt=%u stable=1 query_returned=%d same_pid=%d", attempt, returned, same_pid);
  if (returned && same_pid) return 1;
  state->pid = 0;
  return 0;
}

struct apple_join {
  const struct piano_wifi_profile *profile;
  uint64_t kick_time;
  char *executable;
  struct location_readiness location;
  int location_passed;
  CWInterface *interface;
  CWNetwork *network;
  unsigned readiness_attempt;
};
// Raw SET20 acceptance, also used before and after the CoreWLAN call.
static uint32_t receipt(void *unused)
{
  (void)unused;
  char line[256] = {}; size_t n = sizeof line;
  if (sysctlbyname(PIANO_WIFI_PROFILE_SYSCTL, line, &n, NULL, 0)) {
    timeline("SET20 poll result=read-error errno=%d", errno);
    return 0;
  }
  unsigned generation = 0;
  if (!strstr(line, "source apple-set20")) {
    timeline("SET20 poll result=no-apple-generation");
    return 0;
  }
  if (sscanf(line, "set (generation %u)", &generation) != 1) generation = 0;
  timeline("SET20 poll generation=%u", generation);
  return generation;
}

static int native_join_seen(CWInterface *interface)
{
  uint64_t generation = 0;
  size_t size = sizeof generation;
  if (sysctlbyname(PIANO_WIFI_CURRENT_JOIN_SYSCTL, &generation, &size, NULL, 0) == 0) {
    if (size != sizeof generation || generation > UINT32_MAX) {
      timeline("fallback skipped=invalid-current-join");
      return 1;
    }
    timeline("SET20 current generation=%u", (unsigned)generation);
    if (!generation) return 0;
  } else if (errno == ENOENT) {
    // The opt-in OID is absent on the default path and on older kernels.
    if (!receipt(NULL) && !interface.ssidData.length) return 0;
  } else {
    timeline("fallback skipped=current-join-read-error errno=%d", errno);
    return 1;
  }
  timeline("fallback skipped=association-present-or-requested");
  return 1;
}

static int ready(void *context)
{
  struct apple_join *a = context;
  const unsigned attempt = ++a->readiness_attempt;
  const uint64_t since_kick = monotonic_seconds() - a->kick_time;
  if (since_kick < NativeJoinGraceSeconds) return 0;
  // airportd's own auto-join (DRIVER_AVAILABLE, scan TIMESTAMP, ASSOCIATE outcome) usually joins first: the kernel receipt needs no
  // CoreWLAN or CoreLocation call, so it is read before the CoreLocation gate, whose query can stay unanswered for minutes.
  if (native_join_seen(nil)) return -1;
  if (!a->location_passed) {
    if (!location_ready(&a->location, a->executable, attempt)) return 0;
    a->location_passed = 1;
  }
  timeline("interface attempt=%u begin", attempt);
  a->interface = [[CWWiFiClient sharedWiFiClient] interfaceWithName:@"en0"];
  timeline("interface attempt=%u present=%d", attempt, a->interface != nil);
  if (!a->interface) return 0;
  timeline("power attempt=%u begin", attempt);
  const BOOL powered = a->interface.powerOn;
  timeline("power attempt=%u on=%d", attempt, powered);
  if (!powered) {
    timeline("fallback skipped=powered-off");
    return -1;
  }
  if (native_join_seen(a->interface)) return -1;
  NSData *name = [NSData dataWithBytes:a->profile->ssid length:a->profile->ssid_len];
  NSError *error = nil;
  timeline("scan attempt=%u begin", attempt);
  NSSet<CWNetwork *> *found = [a->interface scanForNetworksWithSSID:name error:&error];
  const NSUInteger count = error ? 0 : found.count;
  timeline("scan attempt=%u has_error=%d code=%ld candidates=%lu", attempt, error != nil,
           (long)error.code, (unsigned long)count);
  if (error || !count) return 0;
  for (CWNetwork *network in found) {
    if (![network.ssidData isEqualToData:name]) continue;
    if (a->profile->flags & PianoWifiProfileBssid) {
      char bssid[18]; const uint8_t *b = a->profile->bssid;
      snprintf(bssid, sizeof bssid, "%02x:%02x:%02x:%02x:%02x:%02x", b[0],b[1],b[2],b[3],b[4],b[5]);
      if ([network.bssid caseInsensitiveCompare:[NSString stringWithUTF8String:bssid]] != NSOrderedSame) continue;
    }
    a->network = network;
    timeline("scan attempt=%u match=1", attempt);
    return 1;
  }
  timeline("scan attempt=%u match=0", attempt);
  return 0;
}
static int associate(void *context)
{
  struct apple_join *a = context;
  if (!a->interface.powerOn || native_join_seen(a->interface)) return -1;
  const struct piano_wifi_profile *p = a->profile;
  NSString *password = p->psk_len ? [[NSString alloc] initWithBytes:p->psk length:p->psk_len encoding:NSUTF8StringEncoding] : nil;
  NSError *error = nil;
  puts("piano-wifi-join: Apple boot join invoked (credentials not logged)"); fflush(stdout);
  timeline("associate begin");
  const BOOL ok = [a->interface associateToNetwork:a->network password:password error:&error];
  timeline("associate returned ok=%d has_error=%d code=%ld", ok, error != nil, (long)error.code);
  // Do not print NSError descriptions: Apple may include network identifiers.
  if (!ok) fprintf(stderr, "piano-wifi-join: Apple association failed (code %ld)\n", (long)error.code);
  return ok;
}
static void pause_join(void *unused) { (void)unused; sleep(2); }

int main(int argc, char **argv)
{
  const char *path = argc > 1 ? argv[1] : PIANO_WIFI_PROFILE_PATH;
  if (geteuid() != 0) {
    fprintf(stderr, "piano-wifi-join: run as root\n");
    return finish(1, "root");
  }
  if (argc == 2 && !strcmp(argv[1], "--provision-stdin"))
    return provision_from_stdin();
  if (argc == 2 && !strcmp(argv[1], "--location-query")) {
    alarm(LocationQuerySeconds);
    @autoreleasepool {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      (void)[CLLocationManager locationServicesEnabled];
#pragma clang diagnostic pop
    }
    return 0;
  }
  timeline("start");
  // No PianoWifi in this boot (or not at a level that joins): nothing to do. launchd may start this before the kext has
  // registered its sysctl, so it is looked for during 30 s before that conclusion.
  size_t probe = 0;
  for (int attempt = 1; ; ++attempt) {
    const int result = sysctlbyname(PIANO_WIFI_PROFILE_SYSCTL, NULL, &probe, NULL, 0);
    timeline("OID attempt=%d result=%d errno=%d", attempt, result, result ? errno : 0);
    if (!result) break;
    if (attempt == 30) return finish(0, "OID");
    sleep(1);
  }
  const int fd = safe_open(path);
  timeline("profile open=%s", fd == -2 ? "missing" : fd < 0 ? "refused" : "ok");
  if (fd == -2) return finish(0, "profile-missing");
  if (fd < 0) return finish(1, "profile-open");
  char boot[64];
  if (!boot_uuid(boot, sizeof boot) || !kick_completed(kick_receipt, boot)) {
    close(fd);
    return finish(1, "airportd-kick-missing");
  }
  const uint64_t kick_time = monotonic_seconds();
  timeline("airportd adoption=confirmed");
  const int native = native_boot();
  if (native != 1) {
    close(fd);
    return finish(1, "boot-mode");
  }
  static char text[4097];
  const ssize_t got = pread(fd, text, sizeof text, 0);
  timeline("profile read=%s errno=%d", got > 0 ? "ok" : got == 0 ? "empty" : "error", got < 0 ? errno : 0);
  if (got <= 0 || got == sizeof text) {
    close(fd);
    wipe(text, sizeof text);
    return finish(1, "profile-read");
  }
  // The shared parser (WifiProfile.h): the kext's and this helper's rules are one, the supplicant's the same (a last line
  // without its newline is a file still being written: refused, review of 2026-11-14 R1/R2).
  struct piano_wifi_profile p;
  const char *problem = piano_wifi_profile_parse((const uint8_t *)text, (uint32_t)got, &p);
  timeline("profile parse=%s", problem ? "refused" : "ok");
  wipe(text, sizeof text);
  if (problem) {
    close(fd);
    fprintf(stderr, "piano-wifi-join: the profile is not usable: %s; nothing handed over\n", problem);
    piano_wifi_profile_wipe(&p);
    return finish(1, "profile-parse");
  }
  // Bound both readiness and synchronous CoreWLAN/XPC calls. No retry after an
  // association (including an API failure or missing receipt).
  struct sigaction action = {};
  action.sa_handler = alarm_expired;
  action.sa_flags = SA_RESETHAND;
  sigemptyset(&action.sa_mask);
  const int handler = sigaction(SIGALRM, &action, NULL);
  alarm(BootJoinDeadline);
  timeline("readiness deadline_seconds=%u alarm_logging=%d", BootJoinDeadline, handler == 0);
  int rc;
  @autoreleasepool {
    enum saved_profile saved = SavedPreserved;
    CWInterface *interface = [[CWWiFiClient sharedWiFiClient] interfaceWithName:@"en0"];
    if (interface && !interface.powerOn) {
      timeline("fallback skipped=powered-off");
    } else {
      saved = provision_from_file(argv[0], fd);
      if (saved == SavedUnavailable) {
        timeline("native provisioning=retry");
        // Give the System keychain time to initialize on the first boot.
        while (monotonic_seconds() - kick_time < 20) sleep(1);
        if (interface && !interface.powerOn) saved = SavedPreserved;
        else
        saved = provision_from_file(argv[0], fd);
      }
      timeline("native provisioning=%s", saved == SavedReady ? "verified" :
               saved == SavedPreserved ? "preserved" : "unavailable");
    }
    close(fd);
    if (saved == SavedPreserved) {
      rc = PianoBootJoinSkipped;
    } else {
      struct apple_join a = { .profile = &p, .kick_time = kick_time, .executable = argv[0] };
      struct piano_boot_join_transport transport = { &a, ready, receipt, associate, pause_join };
      rc = piano_boot_join(&transport, BootJoinDeadline / 2);
    }
  }
  piano_wifi_profile_wipe(&p);
  if (rc == PianoBootJoinSkipped) return finish(0, "fallback-skipped");
  if (rc) {
    fprintf(stderr, "piano-wifi-join: boot transaction failed stage %d; no retry\n", rc);
    static const char *const stage[] = { "complete", "readiness", "associate", "SET20" };
    return finish(1, stage[rc]);
  }
  puts("PianoWifiBootAppleJoinV1: fresh SET20/profile receipt; one boot request complete");
  return finish(0, "complete");
}
