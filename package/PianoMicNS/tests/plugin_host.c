// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// The built bundle as the HAL's driver service takes it (27.3 com.apple.audio.Core-Audio-Driver-Service.helper: SecStaticCodeCreateWithPath
// + SecStaticCodeCheckValidity, then CFBundleCreate, CFPlugInFindFactoriesForPlugInTypeInPlugIn(kAudioServerPlugInTypeUUID),
// CFPlugInInstanceCreate, QueryInterface for the driver interface, Initialize), then the questions the audio server asks a new
// device: the plug-in's device list and UID translation, the device's name, UID, transport, streams by scope, latency, rate, time
// stamp period, the stream's direction, terminal and format, an unknown property refused; StartIO without PianoAudio's tap service
// (this Mac has none) refused with kAudioHardwareNotRunningError. Run by tooling/k27/build-piano-micns.sh on the signed bundle.
// Built arm64e (mic-ns4, LOAD-ANALYSIS.md): on the tablet the driver service runs with pointer authentication on and dyld accepts only
// arm64e slices ("incompatible architecture (have 'arm64', need 'arm64e')"); a third-party arm64e process on the build Mac runs the same
// way, so this host grades the bundle as the tablet's helper does. An arm64 host would load either slice and could not see it.
//   plugin_host BUNDLE              the bundle loads (CFBundle, as the helper's loader) and passes every check below
//   plugin_host --refused BUNDLE    negative control: CFBundle must refuse to load the executable, and the same dlopen must fail
//                                   with dyld's "need 'arm64e'" (the line the tablet's helper logged)
// clang -arch arm64e -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer plugin_host.c -framework CoreAudio -framework CoreFoundation -framework Security -o /tmp/plugin_host && /tmp/plugin_host "$(bash build-host-bundle.sh /tmp/plugin_host arm64e)" && /tmp/plugin_host --refused "$(bash build-host-bundle.sh /tmp/plugin_host arm64)"
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <dlfcn.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do {++checks; if (!(c)) {fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1);}} while (0)

static OSStatus hostChanged(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a)
{(void)h; (void)o; (void)n; (void)a; return 0;}
static OSStatus hostCopy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *d) {(void)h; (void)k; *d = NULL; return 0;}
static OSStatus hostWrite(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) {(void)h; (void)k; (void)d; return 0;}
static OSStatus hostDelete(AudioServerPlugInHostRef h, CFStringRef k) {(void)h; (void)k; return 0;}
static OSStatus hostConfig(AudioServerPlugInHostRef h, AudioObjectID o, UInt64 a, void *i) {(void)h; (void)o; (void)a; (void)i; return 0;}
static const AudioServerPlugInHostInterface kHost = {hostChanged, hostCopy, hostWrite, hostDelete, hostConfig};

static AudioServerPlugInDriverRef drv;
static OSStatus get(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope scope, void *out, UInt32 size, UInt32 *got,
                    const void *q, UInt32 qs)
{
    const AudioObjectPropertyAddress a = {s, scope, kAudioObjectPropertyElementMain};
    return (*drv)->GetPropertyData(drv, o, 0, &a, qs, q, size, got, out);
}
static UInt32 u32(AudioObjectID o, AudioObjectPropertySelector s, AudioObjectPropertyScope scope)
{
    UInt32 v = 0xdeadbeef, got = 0;
    CHECK(get(o, s, scope, &v, sizeof v, &got, NULL, 0) == 0 && got == sizeof v);
    return v;
}
static int str_is(AudioObjectID o, AudioObjectPropertySelector s, const char *want)
{
    CFStringRef v = NULL;
    UInt32 got = 0;
    if (get(o, s, kAudioObjectPropertyScopeGlobal, &v, sizeof v, &got, NULL, 0) || !v) return 0;
    char buf[256];
    return CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8) && !strcmp(buf, want);
}

#if __has_feature(ptrauth_calls)
static const char kHostAbi[] = "arm64e";
#else
static const char kHostAbi[] = "arm64";
#endif

// the load error as one string (malloc'd): CFBundle's code and description plus dyld's message (the user info's debug description)
static char *error_text(CFErrorRef err)
{
    if (!err) return strdup("no error");
    CFStringRef d = CFErrorCopyDescription(err);
    CFDictionaryRef ui = CFErrorCopyUserInfo(err);   // dyld's message is its NSDebugDescription
    CFStringRef dbg = ui ? CFDictionaryGetValue(ui, CFSTR("NSDebugDescription")) : NULL;
    CFStringRef all = CFStringCreateWithFormat(NULL, NULL, CFSTR("code %ld: %@ | %@"), (long)CFErrorGetCode(err), d ? d : CFSTR("-"),
                                               dbg ? (CFTypeRef)dbg : ui ? (CFTypeRef)ui : (CFTypeRef)CFSTR("-"));
    CFIndex size = all ? CFStringGetMaximumSizeForEncoding(CFStringGetLength(all), kCFStringEncodingUTF8) + 1 : 1;
    char *buf = calloc(1, (size_t)size);
    if (buf && all) CFStringGetCString(all, buf, size, kCFStringEncodingUTF8);
    if (all) CFRelease(all);
    if (d) CFRelease(d);
    if (ui) CFRelease(ui);
    return buf ? buf : strdup("no memory");
}

int main(int argc, char **argv)
{
    int refused = argc == 3 && !strcmp(argv[1], "--refused");
    if (argc != 2 && !refused) {fprintf(stderr, "usage: plugin_host [--refused] BUNDLE\n"); return 2;}
    const char *path = argv[argc - 1];
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), true);
    SecStaticCodeRef code = NULL;
    CHECK(SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code) == errSecSuccess);
    CHECK(SecStaticCodeCheckValidity(code, kSecCSDefaultFlags, NULL) == errSecSuccess);   // the driver service refuses an invalid one
    CFRelease(code);
    CFPlugInRef plugin = CFPlugInCreate(NULL, url);
    CHECK(plugin != NULL);
    CFBundleRef bundle = CFPlugInGetBundle(plugin);
    // the executable, loaded the way the helper's CFBundle loader does it (dyld grades the slice against this process)
    CFErrorRef err = NULL;
    Boolean loaded = CFBundleLoadExecutableAndReturnError(bundle, &err);
    char *why = error_text(err);
    if (err) CFRelease(err);
    if (refused) {
        CHECK(!loaded);
        // CFBundle's error carries no dyld text (code 3587); its loader logs dyld's message (com.apple.CFBundle:loading "Error loading
        // ... dlopen(..., 0x0106)"). The same dlopen, with CFBundle's flags, must give dyld's arm64e refusal.
        CFURLRef exe = CFBundleCopyExecutableURL(bundle);
        char exepath[PATH_MAX];
        CHECK(exe && CFURLGetFileSystemRepresentation(exe, true, (UInt8 *)exepath, sizeof exepath));
        CFRelease(exe);
        void *h = dlopen(exepath, RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
        const char *dl = h ? "loaded" : dlerror();
        if (h || !dl || !strstr(dl, "need 'arm64e'")) {
            fprintf(stderr, "FAIL plugin_host --refused (host %s): CFBundle: %s; dlopen: %s\n", kHostAbi, why, dl ? dl : "-");
            return 1;
        }
        const char *at = strstr(dl, "(mach-o file");
        printf("PASS plugin refused: host %s, CFBundle %.10s, dlopen %.100s\n", kHostAbi, why, at ? at : dl);
        free(why);
        CFRelease(plugin);
        CFRelease(url);
        return 0;
    }
    if (!loaded) {fprintf(stderr, "FAIL plugin_host (host %s): the executable did not load: %s\n", kHostAbi, why); return 1;}
    free(why);
    ++checks;
    CFDictionaryRef info = CFBundleGetInfoDictionary(bundle);
    CFArrayRef clients = CFDictionaryGetValue(info, CFSTR("AudioServerPlugIn_IOKitUserClients"));
    CHECK(clients && CFArrayGetCount(clients) == 1 &&
          CFEqual(CFArrayGetValueAtIndex(clients, 0), CFSTR("PianoMicTapUserClient")));   // the sandbox extension's class
    CFArrayRef factories = CFPlugInFindFactoriesForPlugInTypeInPlugIn(kAudioServerPlugInTypeUUID, plugin);
    CHECK(factories && CFArrayGetCount(factories) == 1);
    CFUUIDRef factory = CFArrayGetValueAtIndex(factories, 0);
    void *iunknown = CFPlugInInstanceCreate(NULL, factory, kAudioServerPlugInTypeUUID);
    CHECK(iunknown != NULL);
    IUnknownVTbl **unk = iunknown;
    void *driver = NULL;
    CHECK((*unk)->QueryInterface(unk, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &driver) == S_OK && driver);
    drv = driver;
    const AudioServerPlugInHostInterface *host = &kHost;
    CHECK((*drv)->Initialize(drv, (AudioServerPlugInHostRef)host) == 0);
    // the plug-in object
    AudioObjectID list[4] = {0};
    UInt32 got = 0;
    CHECK(get(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, list, sizeof list, &got, NULL, 0) == 0);
    CHECK(got == sizeof(AudioObjectID) && list[0] == 2);
    CFStringRef uid = CFSTR("PianoMicNS");
    AudioObjectID dev = 0;
    CHECK(get(kAudioObjectPlugInObject, kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, &dev, sizeof dev, &got,
              &uid, sizeof uid) == 0 && dev == 2);
    CHECK(u32(kAudioObjectPlugInObject, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioPlugInClassID);
    // the device
    CHECK(str_is(2, kAudioObjectPropertyName, "Mic (noise reduced)") && str_is(2, kAudioDevicePropertyDeviceUID, "PianoMicNS"));
    CHECK(u32(2, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioDeviceClassID);
    CHECK(u32(2, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal) == kAudioDeviceTransportTypeVirtual);
    CHECK(u32(2, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeInput) == 1);
    CHECK(u32(2, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioObjectPropertyScopeGlobal) == 0);
    CHECK(u32(2, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput) == 3360);
    CHECK(u32(2, kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal) == 16384);
    CHECK(u32(2, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 0);
    Float64 rate = 0;
    CHECK(get(2, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate, sizeof rate, &got, NULL, 0) == 0 && rate == 48000.0);
    CHECK(get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, list, sizeof list, &got, NULL, 0) == 0 &&
          got == sizeof(AudioObjectID) && list[0] == 3);
    CHECK(get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, list, sizeof list, &got, NULL, 0) == 0 && got == 0);
    CHECK(get(2, kAudioObjectPropertyControlList, kAudioObjectPropertyScopeGlobal, list, sizeof list, &got, NULL, 0) == 0 && got == 0);
    const AudioObjectPropertyAddress bogus = {'zzzz', kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CHECK(!(*drv)->HasProperty(drv, 2, 0, &bogus));
    // the stream
    CHECK(u32(3, kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == 1);
    CHECK(u32(3, kAudioStreamPropertyTerminalType, kAudioObjectPropertyScopeGlobal) == kAudioStreamTerminalTypeMicrophone);
    AudioStreamBasicDescription f;
    CHECK(get(3, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, &f, sizeof f, &got, NULL, 0) == 0);
    CHECK(f.mSampleRate == 48000.0 && f.mChannelsPerFrame == 1 && f.mBitsPerChannel == 32 && (f.mFormatFlags & kAudioFormatFlagIsFloat));
    Boolean settable = false;
    const AudioObjectPropertyAddress fa = {kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CHECK((*drv)->IsPropertySettable(drv, 3, 0, &fa, &settable) == 0 && settable);
    CHECK((*drv)->SetPropertyData(drv, 3, 0, &fa, 0, NULL, sizeof f, &f) == 0);
    f.mChannelsPerFrame = 2;
    CHECK((*drv)->SetPropertyData(drv, 3, 0, &fa, 0, NULL, sizeof f, &f) == kAudioDeviceUnsupportedFormatError);
    f.mChannelsPerFrame = 1;
    f.mSampleRate = NAN;
    CHECK((*drv)->SetPropertyData(drv, 3, 0, &fa, 0, NULL, sizeof f, &f) == kAudioDeviceUnsupportedFormatError);
    f.mSampleRate = 48000;
    f.mBytesPerFrame = 8;
    CHECK((*drv)->SetPropertyData(drv, 3, 0, &fa, 0, NULL, sizeof f, &f) == kAudioDeviceUnsupportedFormatError);
    // IO: only ReadInput; StartIO needs PianoAudio's tap, which this Mac does not have
    Boolean will = false, inplace = false;
    CHECK((*drv)->WillDoIOOperation(drv, 2, 0, kAudioServerPlugInIOOperationReadInput, &will, &inplace) == 0 && will && inplace);
    CHECK((*drv)->WillDoIOOperation(drv, 2, 0, kAudioServerPlugInIOOperationWriteMix, &will, &inplace) == 0 && !will);
    CHECK((*drv)->StartIO(drv, 2, 1) == kAudioHardwareNotRunningError);
    CHECK(u32(2, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 0);
    printf("PASS plugin: %u checks in an %s host (signature valid, loaded, factory, interface, properties, StartIO without a tap refused)\n",
           checks, kHostAbi);
    (*drv)->Release(drv);
    (*unk)->Release(unk);
    CFRelease(factories);
    CFRelease(plugin);
    CFRelease(url);
    return 0;
}
