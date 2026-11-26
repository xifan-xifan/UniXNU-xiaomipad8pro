#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
"""Offline acceptance of the actual boot transaction and SET22 owner latch."""
import subprocess
import tempfile
import unittest
from pathlib import Path

W = Path(__file__).resolve().parents[3]

class BootJoinTests(unittest.TestCase):
    def test_airportd_kick_starts_fallback_once(self):
        source = (W / 'tooling/userland/piano-airportd-kick/piano-airportd-kick.sh').read_text()
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            calls = path / 'calls'
            marker = path / 'marker'
            scripts = {
                'sysctl': f"""case "$2" in
  machdep.piano_wifi_native_boot) test -e '{path / 'diagnostic'}' && echo 0 || echo 1 ;;
  machdep.*) echo 1 ;;
  *) echo fixture-boot ;;
esac""",
                'ioreg': f"""echo '"piano-wifi-apple80211-answering"'
echo '"BSD Name" = "en0"'
if test -e '{path / 'adopted'}'; then
  printf '"piano-wifi-airportd-adopted" = %s\n' "$(cat '{path / 'adopted'}')"
fi""",
                'launchctl': f"""echo "$*" >> '{calls}'
case "$1 $2" in
  'kickstart -k'|'kickstart system/com.apple.airportd')
    test ! -e '{path / 'fail-kick'}' || exit 1
    echo 43 > '{path / 'pid'}'
    test -e '{path / 'fail-adoption'}' || echo 43 > '{path / 'adopted'}'
    ;;
  'kickstart system/com.unixnu.piano-wifi-join') test -f '{marker}' ;;
  'print system/com.apple.airportd')
    test -f '{path / 'pid'}' && printf '   pid = %s\n' "$(cat '{path / 'pid'}')"
    ;;
  *) exit 1 ;;
esac""",
            }
            for name, script in scripts.items():
                tool = path / name
                tool.write_text(f'#!/bin/sh\n{script}\n')
                tool.chmod(0o755)
            for original, replacement in {
                '/usr/sbin/sysctl': path / 'sysctl',
                '/usr/sbin/ioreg': path / 'ioreg',
                '/bin/launchctl': path / 'launchctl',
                '/var/run/piano-airportd-kick': marker,
                '/dev/console': path / 'console',
                '/usr/bin/logger': '/usr/bin/true',
                'sleep ': '/usr/bin/true ',
            }.items():
                self.assertIn(original, source)
                source = source.replace(original, str(replacement))
            script = path / 'kick.sh'
            script.write_text(source)
            (path / 'diagnostic').touch()
            subprocess.run(['/bin/sh', str(script)], check=True, capture_output=True)
            self.assertFalse(calls.exists())
            self.assertFalse(marker.exists())
            (path / 'diagnostic').unlink()
            (path / 'pid').write_text('42\n')
            (path / 'adopted').write_text('41\n')  # stale evidence from an earlier daemon
            (path / 'fail-kick').touch()
            self.assertNotEqual(subprocess.run(['/bin/sh', str(script)], capture_output=True).returncode, 0)
            self.assertFalse(marker.exists())
            self.assertNotIn('kickstart system/com.unixnu.piano-wifi-join', calls.read_text())
            (path / 'fail-kick').unlink()
            calls.unlink()
            (path / 'fail-adoption').touch()
            result = subprocess.run(['/bin/sh', str(script)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn('adoption-timeout mode=restarted', result.stdout)
            self.assertFalse(marker.exists())
            (path / 'fail-adoption').unlink()
            calls.unlink()
            result = subprocess.run(['/bin/sh', str(script)], check=True, capture_output=True, text=True)
            self.assertIn('PianoWifiAirportdKickV1 helper=started', result.stdout)
            self.assertIn('complete mode=restarted pid=43', result.stdout)
            self.assertEqual(marker.read_text(), 'fixture-boot\n')
            subprocess.run(['/bin/sh', str(script)], check=True, capture_output=True)
            self.assertEqual([line for line in calls.read_text().splitlines() if line.startswith('kickstart ')],
                             ['kickstart -k system/com.apple.airportd', 'kickstart system/com.unixnu.piano-wifi-join'])
            marker.unlink()
            calls.unlink()
            result = subprocess.run(['/bin/sh', str(script)], check=True, capture_output=True, text=True)
            self.assertIn('complete mode=observed pid=43', result.stdout)
            self.assertNotIn('kickstart -k', calls.read_text())
            marker.unlink()
            calls.unlink()
            (path / 'pid').unlink()
            result = subprocess.run(['/bin/sh', str(script)], check=True, capture_output=True, text=True)
            self.assertIn('complete mode=started pid=43', result.stdout)
            self.assertIn('kickstart system/com.apple.airportd', calls.read_text())
            self.assertNotIn('kickstart -k', calls.read_text())

    def test_native_profile_idempotence(self):
        # A real CoreWiFi external-form round trip backed by a synthetic store.
        # The test never opens the host's Keychain or sends airportd a write.
        source = r'''
#import <Foundation/Foundation.h>
#import <CoreWLAN/CoreWLAN.h>
#import <Security/Security.h>
#include <assert.h>
static NSDictionary *records;
static NSString *credential;
static unsigned adds, credentialAdds;
@interface FixtureStore : NSObject
- (instancetype)initWithType:(NSInteger)type identifier:(NSString *)identifier;
- (void)activate;
- (NSDictionary *)dictionaryRepresentation;
@end
@implementation FixtureStore
- (instancetype)initWithType:(NSInteger)type identifier:(NSString *)identifier {
  assert(type == 1 && [identifier isEqualToString:@"com.apple.wifi.known-networks"]);
  return [super init];
}
- (void)activate {}
- (NSDictionary *)dictionaryRepresentation { return records; }
@end
static Class fixture_class(NSString *name) {
  return [name isEqualToString:@"CWFKeyValueStore"] ? FixtureStore.class : NSClassFromString(name);
}
static OSStatus fixture_interaction(Boolean allowed) {
  assert(!allowed);
  return errSecSuccess;
}
static OSStatus fixture_open(const char *path, SecKeychainRef *keychain) {
  assert(!strcmp(path, "/Library/Keychains/System.keychain"));
  *keychain = (SecKeychainRef)CFRetain(kCFNull);
  return errSecSuccess;
}
static OSStatus fixture_unlock(SecKeychainRef keychain, UInt32 n, const void *password, Boolean usePassword) {
  assert(keychain && !n && !password && !usePassword);
  return errSecSuccess;
}
static OSStatus fixture_find(CWKeychainDomain domain, NSData *ssid, NSString **password) {
  assert(domain == kCWKeychainDomainSystem && ssid.length);
  *password = credential;
  return credential ? errSecSuccess : errSecItemNotFound;
}
static OSStatus fixture_trust(const char *path, SecTrustedApplicationRef *app) {
  assert(!path || !strcmp(path, "/usr/libexec/airportd"));
  *app = (SecTrustedApplicationRef)CFRetain(kCFNull);
  return errSecSuccess;
}
static OSStatus fixture_group(const char *group, SecCertificateRef cert, SecTrustedApplicationRef *app) {
  assert(!strcmp(group, "AirPort") && !cert);
  return fixture_trust(NULL, app);
}
static OSStatus fixture_access(CFStringRef title, CFArrayRef apps, SecAccessRef *access) {
  assert(CFEqual(title, CFSTR("AirPort")) && CFArrayGetCount(apps) == 3);
  *access = (SecAccessRef)CFRetain(kCFNull);
  return errSecSuccess;
}
static OSStatus fixture_create(SecItemClass type, const SecKeychainAttributeList *attrs, UInt32 length,
                               const void *bytes, SecKeychainRef keychain, SecAccessRef access,
                               SecKeychainItemRef *item) {
  assert(type == kSecGenericPasswordItemClass && attrs->count == 4 && keychain && access && !item);
  ++credentialAdds;
  credential = [[NSString alloc] initWithBytes:bytes length:length encoding:NSUTF8StringEncoding];
  return errSecSuccess;
}
#define NSClassFromString fixture_class
#define CWAddNetworkProfile fixture_add
#define SecKeychainSetUserInteractionAllowed fixture_interaction
#define SecKeychainOpen fixture_open
#define SecKeychainUnlock fixture_unlock
#define CWKeychainFindWiFiPassword fixture_find
#define SecTrustedApplicationCreateFromPath fixture_trust
#define SecTrustedApplicationCreateApplicationGroup fixture_group
#define SecAccessCreate fixture_access
#define SecKeychainItemCreateFromContent fixture_create
#define main production_main
#include "tooling/native/piano-wifi-join/piano-wifi-join.m"
#undef main
@interface CWFNetworkProfile (Fixture)
@property (readonly) NSDictionary *externalForm;
- (void)setSystemMode:(BOOL)on;
- (void)setAutoJoinDisabled:(BOOL)disabled;
@end
OSStatus fixture_add(CWNetworkProfile *legacy, SFAuthorization *authorization) {
  assert(!authorization);
  ++adds;
  CWFNetworkProfile *profile = [legacy coreWiFiNetworkProfile];
  [profile setSystemMode:YES];
  NSMutableDictionary *next = [records mutableCopy];
  next[profile.identifier] = profile.externalForm;
  records = next;
  return errSecSuccess;
}
int main(void) {
  @autoreleasepool {
    struct piano_wifi_profile p;
    const char input[] = "ssid=fixture-network\nsecurity=open\n";
    assert(!piano_wifi_profile_parse((const uint8_t *)input,sizeof input-1,&p));
    NSData *ssid = [NSData dataWithBytes:p.ssid length:p.ssid_len];
    NSString *key = @"wifi.network.ssid.fixture-network";
    NSString *other = @"wifi.network.ssid.fixture-other";
    records = @{other:@{}};
    assert(provision(&p) == SavedReady && adds == 1 && records.count == 2 && records[other]);
    assert(provision(&p) == SavedReady && adds == 1);
    CWFNetworkProfile *native = [[fixture_class(@"CWFNetworkProfile") alloc] initWithExternalForm:records[key]];
    assert([native.identifier isEqualToString:key] && native.isSystemMode);
    [native setAutoJoinDisabled:YES];
    records = @{key:native.externalForm};
    assert(provision(&p) == SavedPreserved && adds == 1);
    [native setAutoJoinDisabled:NO];
    records = @{key:native.externalForm, other:native.externalForm};
    assert(provision(&p) == SavedPreserved && adds == 1);
    records = nil;
    assert(provision(&p) == SavedUnavailable && adds == 1);
    piano_wifi_profile_wipe(&p);
    CWMutableNetworkProfile *legacy = [CWMutableNetworkProfile networkProfile];
    legacy.ssidData = ssid;
    legacy.security = kCWSecurityWPA2Personal;
    native = [legacy coreWiFiNetworkProfile];
    [native setSystemMode:YES];
    assert(native.supportedSecurityTypes == 0x50);
    records = @{key:native.externalForm};
    assert(known_profile_state(records,ssid,0x50,YES) == KnownMatches);
    assert(known_profile_state(records,ssid,0x10,NO) == KnownMatches);
    assert(known_profile_state(records,ssid,0x10,YES) == KnownPreserve);
    const char wpa[] = "ssid=fixture-network\nsecurity=wpa2-psk\npsk=fixture-only\n";
    assert(!piano_wifi_profile_parse((const uint8_t *)wpa, sizeof wpa-1, &p));
    records = @{};
    assert(provision(&p) == SavedReady && adds == 2 && credentialAdds == 1 && records.count == 1);
    assert(provision(&p) == SavedReady && adds == 2 && credentialAdds == 1);
    credential = @"owner-replacement";
    assert(provision(&p) == SavedPreserved && adds == 2 && credentialAdds == 1);
    piano_wifi_profile_wipe(&p);
  }
}
'''
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            (path / 'test.m').write_text(source)
            subprocess.run(['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', '-fobjc-arc', '-I', str(W),
                            str(path / 'test.m'), '-framework', 'Foundation', '-framework', 'CoreWLAN',
                            '-framework', 'Security', '-framework', 'CoreLocation', '-o', str(path / 'test')],
                           check=True, capture_output=True)
            subprocess.run([str(path / 'test')], check=True, capture_output=True)

    def test_real_owner_and_one_shot_transport(self):
        source = r'''
#include "package/PianoWifi/WifiUserControl.h"
#include "tooling/native/piano-wifi-join/BootJoin.h"
#include <cassert>
using namespace pianowifi;
struct Fixture { WifiUserControl owner; int polls=0,calls=0,pauses=0; unsigned gen=0; bool fail=false,missing=false,skip=false; };
int ready(void *v) {auto &f=*(Fixture*)v; return f.skip ? -1 : ++f.polls >= 4;}
uint32_t receipt(void *v) {return ((Fixture*)v)->gen;}
int associate(void *v) {auto &f=*(Fixture*)v; ++f.calls; assert(f.polls>=4); if(f.fail)return 0;
  assert(f.owner.join()); if(!f.missing)++f.gen; return 1;}
void pause(void *v) {++((Fixture*)v)->pauses;}
int main() {
 for(int mode=0;mode<3;++mode) {
  Fixture f; auto stop=f.owner.disconnect(); f.owner.stopped(stop);
  assert(!f.owner.allowed(f.owner.read()));
  // Reapplied ON restores permission, but cannot publish the cold-boot profile.
  const auto on=f.owner.setPower(true);
  assert(f.owner.allowed(on) && f.owner.serial(on)==f.owner.serial(stop));
  for(int n=0;n<1000;++n) assert(f.owner.setPower(true)==on);
  assert(!f.gen && !f.calls);
  f.fail=mode==1; f.missing=mode==2;
  piano_boot_join_transport t={&f,ready,receipt,associate,pause};
  assert(piano_boot_join(&t,10)==(mode==0?0:mode==1?2:3)); assert(f.calls==1);
  if(!mode)assert(f.owner.allowed(f.owner.read()));
  stop=f.owner.disconnect(); f.owner.stopped(stop);
  assert(!f.owner.allowed(f.owner.read()));
 }
 Fixture f; piano_boot_join_transport t={&f,ready,receipt,associate,pause};
 assert(piano_boot_join(&t,1)==1 && f.calls==0);
 f.skip=true;
 assert(piano_boot_join(&t,10)==PianoBootJoinSkipped && f.calls==0);
}
'''
        with tempfile.TemporaryDirectory() as d:
            path=Path(d); (path/'test.cpp').write_text(source)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(W),str(path/'test.cpp'),'-o',str(path/'test')],check=True,capture_output=True)
            subprocess.run([str(path/'test')],check=True,capture_output=True)

    def test_actual_profile_safety(self):
        # Ownership and boot UUID are synthesized. The open and validation paths are real.
        source = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
static int fixture_lstat(const char *p, struct stat *s) {int e=lstat(p,s);if(!e)s->st_uid=0;return e;}
static int fixture_fstat(int fd, struct stat *s) {int e=fstat(fd,s);if(!e)s->st_uid=0;return e;}
static int fixture_boot_sysctl(const char *name, void *old, size_t *len, void *new, size_t newlen) {
  static const char uuid[] = "11111111-0000-0000-0000-000000000000";
  assert(!strcmp(name, "kern.bootsessionuuid") && *len >= sizeof uuid && !new && !newlen);
  memcpy(old, uuid, sizeof uuid);
  *len = sizeof uuid;
  return 0;
}
#define lstat fixture_lstat
#define fstat fixture_fstat
#define sysctlbyname fixture_boot_sysctl
#define main production_main
#include "tooling/native/piano-wifi-join/piano-wifi-join.m"
#undef main
int main(int argc,char **argv) {
 (void)argc; int fd=safe_open(argv[1]); assert(fd>=0); close(fd);
 char boot[64]; assert(boot_uuid(boot,sizeof boot));
 FILE *marker=fopen(argv[1],"w"); assert(marker && fprintf(marker,"%s\n",boot)>0); fclose(marker);
 assert(kick_completed(argv[1],boot));
 assert(!kick_completed(argv[1],"01234567-0000-0000-0000-000000000000"));
 assert(!kick_completed(argv[2],boot) && !kick_completed(argv[3],boot));
 chmod(argv[1],0644); assert(!kick_completed(argv[1],boot)); chmod(argv[1],0600);
 chmod(argv[1],0644); assert(safe_open(argv[1])<0); chmod(argv[1],0600);
 assert(safe_open(argv[2])==-2); assert(safe_open(argv[3])<0);
 struct piano_wifi_profile p; const char *good="ssid=fixture\nsecurity=wpa2-psk\npsk=fixture-only\n";
 assert(!piano_wifi_profile_parse((const uint8_t*)good,(uint32_t)strlen(good),&p)); piano_wifi_profile_wipe(&p);
 assert(piano_wifi_profile_parse((const uint8_t*)good,(uint32_t)strlen(good)-1,&p)); piano_wifi_profile_wipe(&p);
}
'''
        with tempfile.TemporaryDirectory() as d:
            path=Path(d); path.chmod(0o700); profile=path/'profile';profile.write_text('fixture\n');profile.chmod(0o600)
            (path/'link').symlink_to(profile);(path/'test.m').write_text(source)
            subprocess.run(['xcrun','clang','-fobjc-arc','-I',str(W),str(path/'test.m'),'-framework','Foundation',
                            '-framework','CoreWLAN','-framework','Security','-framework','CoreLocation',
                            '-o',str(path/'test')],check=True,capture_output=True)
            subprocess.run([str(path/'test'),str(profile),str(path/'missing'),str(path/'link')],check=True,capture_output=True)

    def test_fallback_receipt_sequences(self):
        source = r'''
#import <Foundation/Foundation.h>
#import <CoreWLAN/CoreWLAN.h>
#include <assert.h>
#include <errno.h>
#include <sys/sysctl.h>
#include <time.h>
#include "package/PianoWifi/WifiProfile.h"
static uint64_t seconds;
static unsigned accesses, scans, associations, rawGeneration;
static uint64_t currentGeneration;
static int currentError = ENOENT;
static BOOL powered = YES, cachedName, scanMatch, requestedDuringScan;
static const uint8_t fixtureName[7];

@interface FixtureNetwork : NSObject
- (NSData *)ssidData;
@end
@implementation FixtureNetwork
- (NSData *)ssidData { return [NSData dataWithBytes:fixtureName length:sizeof fixtureName]; }
@end

@interface FixtureClient : NSObject
+ (instancetype)sharedWiFiClient;
- (CWInterface *)interfaceWithName:(NSString *)name;
@end
@implementation FixtureClient
+ (instancetype)sharedWiFiClient { return [self new]; }
- (CWInterface *)interfaceWithName:(NSString *)name {
  assert([name isEqualToString:@"en0"]);
  ++accesses;
  return (CWInterface *)self;
}
- (BOOL)powerOn { return powered; }
- (NSData *)ssidData { return cachedName ? [NSData dataWithBytes:fixtureName length:sizeof fixtureName] : nil; }
- (NSSet<CWNetwork *> *)scanForNetworksWithSSID:(NSData *)ssid error:(NSError **)error {
  assert(ssid.length == sizeof fixtureName && error);
  ++scans;
  if (requestedDuringScan) currentGeneration = ++rawGeneration;
  if (scanMatch) return [NSSet setWithObject:(CWNetwork *)[FixtureNetwork new]];
  return [NSSet set];
}
- (BOOL)associateToNetwork:(CWNetwork *)network password:(NSString *)password error:(NSError **)error {
  assert(network && !password && error);
  ++associations;
  currentGeneration = ++rawGeneration;
  return YES;
}
@end
static uint64_t fixture_clock(clockid_t clock) {
  (void)clock;
  return seconds * 1000000000ULL;
}
static int fixture_sysctl(const char *name, void *old, size_t *len, void *new, size_t newlen) {
  assert(!new && !newlen);
  if (!strcmp(name, PIANO_WIFI_CURRENT_JOIN_SYSCTL)) {
    if (currentError) { errno = currentError; return -1; }
    assert(*len == sizeof currentGeneration);
    memcpy(old, &currentGeneration, *len);
    return 0;
  }
  assert(!strcmp(name, PIANO_WIFI_PROFILE_SYSCTL));
  const int written = rawGeneration ? snprintf(old, *len, "set (generation %u): source apple-set20", rawGeneration)
                                    : snprintf(old, *len, "not set");
  assert(written > 0 && (size_t)written < *len);
  return 0;
}
#define CWWiFiClient FixtureClient
#define clock_gettime_nsec_np fixture_clock
#define sysctlbyname fixture_sysctl
#define main production_main
#include "tooling/native/piano-wifi-join/piano-wifi-join.m"
#undef main

static void unexpected_pause(void *context) { (void)context; assert(0); }

int main(void) {
  @autoreleasepool {
    struct piano_wifi_profile profile = { .ssid_len = sizeof fixtureName };
    struct apple_join a = { .profile = &profile, .kick_time = 100, .location_passed = 1 };
    struct piano_boot_join_transport transport = { &a, ready, receipt, associate, unexpected_pause };
    seconds = 134;
    assert(ready(&a) == 0 && accesses == 0 && scans == 0);
    seconds = 135;
    assert(ready(&a) == 0 && accesses == 1 && scans == 1);
    rawGeneration = 9; // Default-off still treats any Apple SET20 receipt as sufficient.
    assert(ready(&a) == -1 && scans == 1);
    rawGeneration = 0;
    cachedName = YES;
    assert(ready(&a) == -1 && scans == 1);
    cachedName = NO;
    powered = NO;
    assert(ready(&a) == -1 && scans == 1);

    currentError = 0;
    powered = YES;
    scanMatch = YES;
    assert(piano_boot_join(&transport, 1) == 0 && associations == 1 && rawGeneration == 1); // boot
    assert(piano_boot_join(&transport, 1) == PianoBootJoinSkipped && associations == 1); // live duplicate

    // The raw generation survives the power cycle. A lagging CoreWLAN view
    // also keeps reporting an association until the next join.
    currentGeneration = 0;
    cachedName = YES;
    powered = NO;
    assert(piano_boot_join(&transport, 1) == PianoBootJoinSkipped && associations == 1);
    powered = YES;
    assert(piano_boot_join(&transport, 1) == 0 && associations == 2 && rawGeneration == 2);

    currentGeneration = 0; // reported link loss on the same radio pass
    assert(piano_boot_join(&transport, 1) == 0 && associations == 3 && rawGeneration == 3);
    assert(piano_boot_join(&transport, 1) == PianoBootJoinSkipped && associations == 3); // PS change or retained wake
    currentGeneration = 0; // wake after sleep parked the radio
    assert(piano_boot_join(&transport, 1) == 0 && associations == 4 && rawGeneration == 4);

    currentGeneration = 0;
    requestedDuringScan = YES; // airportd publishes SET20 during the helper's scan
    assert(piano_boot_join(&transport, 1) == PianoBootJoinSkipped && associations == 4 && rawGeneration == 5);
    currentError = EIO;
    assert(piano_boot_join(&transport, 1) == PianoBootJoinSkipped && associations == 4);
  }
}
'''
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            (path / 'test.m').write_text(source)
            subprocess.run(['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', '-fobjc-arc', '-I', str(W),
                            str(path / 'test.m'), '-framework', 'Foundation', '-framework', 'CoreWLAN',
                            '-framework', 'Security', '-framework', 'CoreLocation', '-o', str(path / 'test')],
                           check=True, capture_output=True)
            subprocess.run([str(path / 'test')], check=True, capture_output=True)

if __name__=='__main__': unittest.main()
