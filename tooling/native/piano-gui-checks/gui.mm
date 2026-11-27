// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreWLAN/CoreWLAN.h>

#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "WifiApple80211.h"

namespace a80211 = pianowifi::a80211;

@protocol BrightnessReader
- (id)copyPropertyForKey:(NSString *)key;
@end

static int emit(NSDictionary *fields, int rc = 0)
{
    NSData *data = [NSJSONSerialization dataWithJSONObject:fields options:NSJSONWritingSortedKeys error:nil];
    if (!data) return 2;
    printf("GUI %s\n", [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] UTF8String]);
    return rc;
}

static int fail(NSString *reason) { return emit(@{@"error": reason}, 2); }

static id attribute(id node, CFStringRef name)
{
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue((__bridge AXUIElementRef)node, name, &value) != kAXErrorSuccess)
        return nil;
    return CFBridgingRelease(value);
}

static NSArray *children(id node, CFStringRef name)
{
    id value = attribute(node, name);
    return [value isKindOfClass:NSArray.class] ? value : @[];
}

static NSArray *tree(NSArray *roots)
{
    NSMutableOrderedSet *nodes = [NSMutableOrderedSet orderedSetWithArray:roots];
    for (NSUInteger i = 0; i < nodes.count && i < 600; i++)
        [nodes addObjectsFromArray:children(nodes[i], kAXChildrenAttribute)];
    return nodes.count <= 600 ? nodes.array : @[];
}

static BOOL role(id node, NSString *wanted)
{
    return [attribute(node, kAXRoleAttribute) isEqual:wanted];
}

static NSArray<NSString *> *labels(id node)
{
    NSMutableOrderedSet *values = [NSMutableOrderedSet orderedSet];
    for (NSString *key in @[@"AXTitle", @"AXDescription", @"AXLabel", @"AXHelp"]) {
        id value = attribute(node, (__bridge CFStringRef)key);
        if ([value isKindOfClass:NSString.class] && [value length])
            [values addObject:value];
    }
    return values.array;
}

static BOOL labeled(id node, NSString *name)
{
    for (NSString *label in labels(node))
        if ([label isEqualToString:name] || [label hasPrefix:[name stringByAppendingString:@", "]] ||
            [label hasPrefix:[name stringByAppendingString:@": "]])
            return YES;
    return NO;
}

static BOOL identified(id node, NSString *suffix)
{
    id identifier = attribute(node, CFSTR("AXIdentifier"));
    return [identifier isKindOfClass:NSString.class] && [[identifier lowercaseString] hasSuffix:suffix];
}

static NSArray *matching(NSArray *nodes, BOOL (^predicate)(id))
{
    return [nodes filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:
        ^BOOL(id node, NSDictionary *) { return predicate(node); }]];
}

static id only(NSArray *nodes, BOOL (^predicate)(id))
{
    NSArray *found = matching(nodes, predicate);
    return found.count == 1 ? found.firstObject : nil;
}

static BOOL hasPress(id node)
{
    CFArrayRef names = nullptr;
    if (!node || AXUIElementCopyActionNames((__bridge AXUIElementRef)node, &names) != kAXErrorSuccess)
        return NO;
    return [CFBridgingRelease(names) containsObject:@"AXPress"];
}

static AXError press(id node)
{
    return node ? AXUIElementPerformAction((__bridge AXUIElementRef)node, kAXPressAction)
                : kAXErrorNoValue;
}

static id controlCenter()
{
    NSArray<NSRunningApplication *> *apps =
        [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.controlcenter"];
    if (apps.count != 1) return nil;
    id app = CFBridgingRelease(AXUIElementCreateApplication(apps.firstObject.processIdentifier));
    AXUIElementSetMessagingTimeout((__bridge AXUIElementRef)app, 1.0);
    return app;
}

static id menuExtra(id app, BOOL wifi)
{
    NSMutableArray *roots = [NSMutableArray array];
    for (NSString *key in @[@"AXExtrasMenuBar", @"AXMenuBar"]) {
        id bar = attribute(app, (__bridge CFStringRef)key);
        if (bar) [roots addObject:bar];
    }
    [roots addObjectsFromArray:children(app, kAXChildrenAttribute)];
    return only(tree(roots), ^BOOL(id node) {
        return (role(node, @"AXMenuBarItem") || role(node, @"AXMenuButton")) &&
            (identified(node, wifi ? @".wifi" : @".controlcenter") ||
             labeled(node, wifi ? @"Wi-Fi" : @"Control Center") ||
             (wifi && (labeled(node, @"WiFi") || labeled(node, @"Wi‑Fi"))));
    });
}

static NSArray *panel(id app, id extra)
{
    // Menu extras may expose their popover as an AXWindow or under the menu bar item.
    NSArray *roots = [children(app, kAXWindowsAttribute)
        arrayByAddingObjectsFromArray:children(extra, kAXChildrenAttribute)];
    return tree(roots);
}

static id wifiSwitch(NSArray *nodes)
{
    return only(nodes, ^BOOL(id node) {
        return (role(node, @"AXCheckBox") || role(node, @"AXSwitch") ||
                role(node, @"AXToggle") || role(node, @"AXButton")) &&
            hasPress(node) &&
            (labeled(node, @"Wi-Fi") || labeled(node, @"Wi‑Fi") ||
             identified(node, @".wifi.toggle")) &&
            [attribute(node, kAXValueAttribute) isKindOfClass:NSNumber.class];
    });
}

static id brightnessSlider(NSArray *nodes)
{
    return only(nodes, ^BOOL(id node) {
        return role(node, @"AXSlider") && (labeled(node, @"Brightness") ||
            labeled(node, @"Display Brightness") || identified(node, @".brightness"));
    });
}

static id waitFor(double seconds, id (^probe)(void))
{
    const double end = NSProcessInfo.processInfo.systemUptime + seconds;
    do {
        id found = probe();
        if (found) return found;
        [NSThread sleepForTimeInterval:0.15];
    } while (NSProcessInfo.processInfo.systemUptime < end);
    return nil;
}

static NSData *ssidFile(const char *path)
{
    NSData *data = [NSData dataWithContentsOfFile:@(path)];
    return data.length > 0 && data.length <= 32 ? data : nil;
}

static BOOL wifiGet(int fd, int selector, void *data, uint32_t *size)
{
    a80211::Req req{};
    strlcpy(req.name, "en0", sizeof req.name);
    req.type = selector;
    req.length = *size;
    req.data = reinterpret_cast<uint64_t>(data);
    if (ioctl(fd, a80211::kSiocGA80211, &req) < 0) return NO;
    *size = req.length;
    return YES;
}

static NSString *readCommand(const char *command)
{
    FILE *pipe = popen(command, "r");
    if (!pipe) return nil;
    NSMutableData *output = [NSMutableData data];
    char bytes[1024];
    size_t length;
    while ((length = fread(bytes, 1, sizeof bytes, pipe)) > 0)
        [output appendBytes:bytes length:length];
    return pclose(pipe) ? nil : [[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding];
}

static BOOL hasDhcpLease(NSString *address, NSString *packet)
{
    if (!address.length || [address hasPrefix:@"169.254."] || [address hasPrefix:@"0."]) return NO;
    if (!packet) return NO;
    NSString *escaped = [NSRegularExpression escapedPatternForString:address];
    NSString *yiaddr = [NSString stringWithFormat:@"(?m)^\\s*yiaddr\\s*=\\s*%@$", escaped];
    return [packet rangeOfString:yiaddr options:NSRegularExpressionSearch].location != NSNotFound &&
        [packet rangeOfString:@"(?m)^\\s*dhcp_message_type[^:\\n]*:\\s*(?:ACK(?:\\s|$)|0x0?5(?:\\s|$))"
                     options:NSRegularExpressionSearch].location != NSNotFound &&
        [packet rangeOfString:@"(?m)^\\s*lease_time[^:\\n]*:\\s*(?:0x[0-9a-fA-F]*[1-9a-fA-F][0-9a-fA-F]*|[1-9][0-9]*)\\s*$"
                     options:NSRegularExpressionSearch].location != NSNotFound;
}

static NSDictionary *wifiState(NSData *target, BOOL checkLease)
{
    if (!target) return nil;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return nil;
    a80211::Power power{};
    uint32_t length = sizeof power;
    BOOL valid = wifiGet(fd, a80211::kPower, &power, &length) &&
        length == sizeof power && power.version == 1 && power.radios == 1 && power.state[0] <= 1;
    uint8_t ssid[32];
    length = sizeof ssid;
    BOOL associated = wifiGet(fd, a80211::kSsid, ssid, &length);
    valid = valid && (associated ? length > 0 && length <= sizeof ssid :
                      errno == ENOENT || (!power.state[0] && errno == EPWROFF));
    close(fd);
    if (!valid) return nil;
    BOOL matches = associated && length == target.length && !memcmp(ssid, target.bytes, length);
    NSString *state = readCommand("/sbin/ifconfig en0 2>/dev/null");
    if (!state) return nil;
    BOOL active = [state rangeOfString:@"(?m)^\\s*status: active$" options:NSRegularExpressionSearch].location != NSNotFound;
    BOOL lease = NO;
    if (checkLease && active && associated) {
        NSString *address = [readCommand("/usr/sbin/ipconfig getifaddr en0 2>/dev/null")
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        lease = hasDhcpLease(address, readCommand("/usr/sbin/ipconfig getpacket en0 2>/dev/null"));
    }
    return @{@"power": @(power.state[0] != 0), @"associated": @(associated), @"target": @(matches),
             @"active": @(active), @"lease": @(lease), @"t": @(NSProcessInfo.processInfo.systemUptime)};
}

static BOOL rowLabel(id node, NSString *name)
{
    // Match SSID text literally. Accessibility descriptions may append the network metadata.
    if ([attribute(node, kAXTitleAttribute) isEqual:name] ||
        [attribute(node, kAXValueAttribute) isEqual:name]) return YES;
    for (id text in children(node, kAXChildrenAttribute))
        if (role(text, @"AXStaticText") && [attribute(text, kAXValueAttribute) isEqual:name]) return YES;
    return [labels(node) containsObject:name];
}

static BOOL networkRow(id node)
{
    if (!hasPress(node)) return NO;
    return role(node, @"AXButton") || role(node, @"AXMenuItem") ||
        role(node, @"AXCheckBox") || role(node, @"AXRow") || role(node, @"AXRadioButton");
}

static NSString *rowName(id node)
{
    NSString *label = labels(node).firstObject;
    if (label) return label;
    for (id text in children(node, kAXChildrenAttribute)) {
        id value = attribute(text, kAXValueAttribute);
        if (role(text, @"AXStaticText") && [value isKindOfClass:NSString.class] && [value length])
            return value;
    }
    return nil;
}

static NSArray *rows(NSArray *nodes, id toggle)
{
    return matching(nodes, ^BOOL(id node) {
        return ![node isEqual:toggle] && networkRow(node) && rowName(node) &&
            ![attribute(node, CFSTR("AXHidden")) boolValue] &&
            !labeled(node, @"Wi-Fi Settings") && !labeled(node, @"Other Networks") &&
            !labeled(node, @"Other…") && !labeled(node, @"Join Other Network");
    });
}

static int wifiUI(NSString *op, id app, NSData *ssid)
{
    id extra = menuExtra(app, YES);
    if (!extra) return fail(@"menu-bar-wifi-extra");
    id toggle = wifiSwitch(panel(app, extra));
    if (!toggle) {
        if (press(extra) != kAXErrorSuccess) return fail(@"wifi-extra-press");
        toggle = waitFor(4, ^id { return wifiSwitch(panel(app, extra)); });
    }
    if (!toggle) return fail(@"wifi-panel-toggle");
    if ([op isEqual:@"wifi-menu"])
        return emit(@{@"menu": @YES, @"switch": attribute(toggle, kAXValueAttribute)});
    if ([op isEqual:@"wifi-off"] || [op isEqual:@"wifi-on"]) {
        BOOL wanted = [op isEqual:@"wifi-on"];
        if ([attribute(toggle, kAXValueAttribute) boolValue] == wanted)
            return fail(@"wifi-toggle-already-at-target");
        double started = NSProcessInfo.processInfo.systemUptime;
        AXError status = press(toggle);
        return emit(@{@"menu": @YES, @"action": op, @"status": @(status), @"t": @(started)}, status ? 2 : 0);
    }

    NSString *name = [[NSString alloc] initWithData:ssid encoding:NSUTF8StringEncoding];
    if (!name.length) return fail(@"wifi-target-file");
    NSArray *(^inventory)(void) = ^{ return rows(panel(app, extra), toggle); };
    id (^find)(void) = ^id { return only(inventory(), ^BOOL(id row) { return rowLabel(row, name); }); };
    id row = waitFor(3, find);
    if (!row) {
        id disclosure = only(panel(app, extra), ^BOOL(id node) {
            return labeled(node, @"Other Networks") && hasPress(node) &&
                (role(node, @"AXDisclosureTriangle") || role(node, @"AXButton"));
        });
        if (disclosure && ![attribute(disclosure, CFSTR("AXExpanded")) boolValue])
            press(disclosure);
        row = waitFor(12, find);
    }
    NSArray *list = inventory();
    if ([op isEqual:@"wifi-find"] || !row)
        return emit(@{@"menu": @YES, @"row": @(row != nil), @"rowCount": @(list.count)}, row ? 0 : 2);
    NSDictionary *before = wifiState(ssid, NO);
    if (!before || ![before[@"power"] boolValue] || [before[@"target"] boolValue])
        return fail(@"wifi-click-baseline-changed");
    double started = NSProcessInfo.processInfo.systemUptime;
    AXError status = press(row);
    return emit(@{@"menu": @YES, @"row": @YES, @"rowCount": @(list.count), @"before": before,
                  @"action": op, @"status": @(status), @"t": @(started)}, status ? 2 : 0);
}

static NSDictionary *sliderGeometry(id slider)
{
    id value = attribute(slider, kAXValueAttribute);
    id min = attribute(slider, kAXMinValueAttribute);
    id max = attribute(slider, kAXMaxValueAttribute);
    if (![value isKindOfClass:NSNumber.class] || (min && ![min isKindOfClass:NSNumber.class]) ||
        (max && ![max isKindOfClass:NSNumber.class])) return nil;
    // Apple's normalized AX sliders may omit their min and max attributes.
    double low = min ? [min doubleValue] : 0;
    double high = max ? [max doubleValue] : 1;
    double current = [value doubleValue];
    if (!isfinite(low) || !isfinite(high) || !isfinite(current) ||
        high <= low || current < low || current > high) return nil;
    id position = attribute(slider, kAXPositionAttribute), size = attribute(slider, kAXSizeAttribute);
    CGPoint origin{};
    CGSize extent{};
    if (!position || !size || CFGetTypeID((__bridge CFTypeRef)position) != AXValueGetTypeID() ||
        CFGetTypeID((__bridge CFTypeRef)size) != AXValueGetTypeID() ||
        !AXValueGetValue((__bridge AXValueRef)position, kAXValueTypeCGPoint, &origin) ||
        !AXValueGetValue((__bridge AXValueRef)size, kAXValueTypeCGSize, &extent) ||
        extent.width < 60 || extent.height <= 0 || extent.height >= extent.width) return nil;
    return @{@"slider": @((current - low) / (high - low)), @"min": @(low), @"max": @(high),
             @"x": @(origin.x), @"y": @(origin.y), @"width": @(extent.width), @"height": @(extent.height)};
}

static NSDictionary *brightness();

static int displayUI(NSString *op, id app, double level)
{
    id extra = menuExtra(app, NO);
    if (!extra) return fail(@"control-center-extra");
    id slider = brightnessSlider(panel(app, extra));
    if (!slider && [op isEqual:@"display-open"]) {
        if (press(extra) != kAXErrorSuccess) return fail(@"control-center-press");
        slider = waitFor(3, ^id { return brightnessSlider(panel(app, extra)); });
        if (!slider) {
            id display = only(panel(app, extra), ^BOOL(id node) {
                return (labeled(node, @"Display") || identified(node, @".display")) && hasPress(node);
            });
            if (press(display) == kAXErrorSuccess)
                slider = waitFor(3, ^id { return brightnessSlider(panel(app, extra)); });
        }
    }
    if (!slider) return fail(@"display-brightness-slider");
    NSDictionary *geometry = sliderGeometry(slider);
    if (!geometry) return fail(@"display-slider-geometry");
    if ([op isEqual:@"display-sample"]) {
        NSDictionary *core = brightness();
        if (!core) return fail(@"corebrightness-readback");
        return emit(@{@"slider": geometry[@"slider"], @"level": core[@"level"], @"nits": core[@"nits"]});
    }
    if (![op isEqual:@"display-set"]) return emit(geometry);
    if (!isfinite(level) || level < 0 || level > 1) return fail(@"display-target");
    Boolean settable = false;
    if (AXUIElementIsAttributeSettable((__bridge AXUIElementRef)slider, kAXValueAttribute, &settable) ||
        !settable) return fail(@"display-slider-ax-value-readonly");
    const double low = [geometry[@"min"] doubleValue], high = [geometry[@"max"] doubleValue];
    AXError status = AXUIElementSetAttributeValue((__bridge AXUIElementRef)slider, kAXValueAttribute,
        (__bridge CFNumberRef)@(low + level * (high - low)));
    return emit(@{@"action": op, @"status": @(status)}, status ? 2 : 0);
}

static NSDictionary *brightness()
{
    CGDirectDisplayID displays[32], builtin = 0;
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(32, displays, &count)) return nil;
    for (uint32_t i = 0; i < count; i++) {
        if (!CGDisplayIsBuiltin(displays[i])) continue;
        if (builtin) return nil;
        builtin = displays[i];
    }
    if (!builtin) return nil;
    void *ds = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/Versions/A/DisplayServices",
                      RTLD_LOCAL | RTLD_NOW);
    auto get = reinterpret_cast<int32_t (*)(CGDirectDisplayID, float *)>(ds ? dlsym(ds, "DisplayServicesGetBrightness") : nullptr);
    float level = NAN;
    if (!get || get(builtin, &level) || !isfinite(level) || level < 0 || level > 1)
        return nil;
    NSBundle *bundle = [NSBundle bundleWithPath:@"/System/Library/PrivateFrameworks/CoreBrightness.framework"];
    Class cls = [bundle load] ? NSClassFromString(@"DisplayServicesClient") : Nil;
    id<BrightnessReader> client = [[cls alloc] init];
    if (![(id)client respondsToSelector:@selector(copyPropertyForKey:)]) return nil;
    id nits = [client copyPropertyForKey:@"DisplayNitsKey"];
    if (![nits isKindOfClass:NSNumber.class] || !isfinite([nits doubleValue]) || [nits doubleValue] <= 0)
        return nil;
    return @{@"level": @(level), @"nits": nits};
}

static uint64_t uptimeUs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

static int energyReply(NSString *op, NSString *token, NSDictionary *fields, int rc = 0)
{
    NSMutableDictionary *reply = [fields mutableCopy];
    reply[@"op"] = op;
    reply[@"token"] = token;
    reply[@"t_us"] = @(uptimeUs());
    return emit(reply, rc);
}

static NSString *powerSourceLine()
{
    return [[readCommand("/usr/bin/pmset -g batt") componentsSeparatedByString:@"\n"] firstObject];
}

static int energyState(NSString *op, NSString *token)
{
    const uint64_t started = uptimeUs();
    NSMutableDictionary *state = [@{@"started_us": @(started)} mutableCopy];
    const struct { NSString *key; const char *command; } probes[] = {
        {@"pmset", "/usr/bin/pmset -g"},
        {@"custom", "/usr/bin/pmset -g custom"},
        {@"lpm", "/usr/bin/notifyutil -g com.apple.system.lowpowermode"},
        {@"hpm", "/usr/bin/notifyutil -g com.apple.system.highpowermode"},
        {@"policy_lpm", "/usr/sbin/sysctl -n machdep.piano_policy_lpm"},
        {@"policy_hpm", "/usr/sbin/sysctl -n machdep.piano_policy_hpm"},
        {@"cpu", "/usr/sbin/sysctl -n machdep.piano_cpufreq_state"},
        {@"gpu", "/usr/sbin/sysctl -n machdep.piano_gpu_state"},
        {@"policy", "/usr/sbin/sysctl -n machdep.piano_policy"},
    };
    state[@"source"] = powerSourceLine();
    for (const auto &probe : probes) state[probe.key] = readCommand(probe.command);
    state[@"source_after"] = powerSourceLine();
    // Report the relevant argument only. The rest of kern.bootargs stays on the device.
    NSString *args = readCommand("/usr/sbin/sysctl -n kern.bootargs");
    if (args) {
        NSString *rule = @"1";
        for (NSString *arg in [args componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet])
            if ([arg hasPrefix:@"piano-policy-profile="]) {
                rule = [arg substringFromIndex:[@"piano-policy-profile=" length]];
                break;
            }
        state[@"profile_rule"] = rule;
    }
    return energyReply(op, token, state);
}

static NSArray<NSRunningApplication *> *settingsApps()
{
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    return [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.systempreferences"];
}

static id settingsApplication()
{
    NSArray<NSRunningApplication *> *apps = settingsApps();
    if (apps.count != 1) return nil;
    id app = CFBridgingRelease(AXUIElementCreateApplication(apps.firstObject.processIdentifier));
    AXUIElementSetMessagingTimeout((__bridge AXUIElementRef)app, 1.0);
    return app;
}

static NSArray *settingsWindows()
{
    id app = settingsApplication();
    if (!app) return @[];
    id focused = attribute(app, kAXFocusedWindowAttribute) ?: attribute(app, kAXMainWindowAttribute);
    return focused ? @[focused] : children(app, kAXWindowsAttribute);
}

static int energyMode(NSString *name)
{
    if ([name isEqual:@"Automatic"] || [name isEqual:@"Automatic Mode"]) return 0;
    if ([name isEqual:@"Low Power"] || [name isEqual:@"Low Power Mode"]) return 1;
    if ([name isEqual:@"High Power"] || [name isEqual:@"High Power Mode"]) return 2;
    return -1;
}

static int pickerMode(id node)
{
    if (!node) return -1;
    id value = attribute(node, kAXValueAttribute);
    int mode = [value isKindOfClass:NSString.class] ? energyMode(value) : -1;
    if (mode >= 0) return mode;
    for (NSString *label in labels(node)) {
        NSString *ending = [[[label componentsSeparatedByString:@": "] lastObject]
            componentsSeparatedByString:@", "].lastObject;
        mode = energyMode(ending);
        if (mode >= 0) return mode;
    }
    return -1;
}

static BOOL powerLabel(id node, NSString *source)
{
    NSString *name = [source isEqual:@"ac"] ? @"On power adapter" : @"On battery";
    return labeled(node, name) ||
        (role(node, @"AXStaticText") && [attribute(node, kAXValueAttribute) isEqual:name]);
}

static BOOL sourcePicker(id node, NSString *source)
{
    if (powerLabel(node, source)) return YES;
    id title = attribute(node, kAXTitleUIElementAttribute);
    if (title && powerLabel(title, source)) return YES;
    for (id linked in children(node, kAXLinkedUIElementsAttribute))
        if (powerLabel(linked, source)) return YES;
    // Some AX rows expose their source as a sibling label rather than a title.
    id row = attribute(node, kAXParentAttribute);
    NSString *other = [source isEqual:@"ac"] ? @"battery" : @"ac";
    for (unsigned depth = 0; row && depth < 3; ++depth, row = attribute(row, kAXParentAttribute)) {
        NSArray *siblings = children(row, kAXChildrenAttribute);
        if ([siblings indexOfObjectPassingTest:^BOOL(id item, NSUInteger, BOOL *) {
            return powerLabel(item, other);
        }] != NSNotFound) return NO;
        if (powerLabel(row, source) || [siblings indexOfObjectPassingTest:^BOOL(id item, NSUInteger, BOOL *) {
            return powerLabel(item, source);
        }] != NSNotFound) return YES;
    }
    return NO;
}

static id energyPicker(NSArray *windows, NSString *source)
{
    NSArray *pickers = matching(tree(windows), ^BOOL(id node) {
        return (role(node, @"AXPopUpButton") || role(node, @"AXMenuButton")) &&
            ![attribute(node, CFSTR("AXHidden")) boolValue] && pickerMode(node) >= 0 && hasPress(node);
    });
    id picker = only(pickers, ^BOOL(id node) { return sourcePicker(node, source); });
    if (picker) return picker;
    return pickers.count == 1 && labeled(pickers.firstObject, @"Energy Mode") ? pickers.firstObject : nil;
}

static void axHierarchy(id node, unsigned depth, NSMutableArray *lines, NSMutableSet *visited)
{
    if (lines.count >= 220 || [visited containsObject:node]) return;
    [visited addObject:node];
    NSMutableDictionary *line = [@{@"depth": @(depth), @"labels": labels(node)} mutableCopy];
    line[@"role"] = attribute(node, kAXRoleAttribute);
    id identifier = attribute(node, kAXIdentifierAttribute);
    if ([identifier isKindOfClass:NSString.class]) line[@"identifier"] = identifier;
    if (role(node, @"AXPopUpButton") || role(node, @"AXMenuButton"))
        line[@"mode"] = @(pickerMode(node));
    [lines addObject:line];
    for (id child in children(node, kAXChildrenAttribute))
        axHierarchy(child, depth + 1, lines, visited);
}

static int energyAxError(NSString *op, NSString *token, NSString *reason, NSArray *roots)
{
    NSMutableArray *lines = [NSMutableArray array];
    NSMutableSet *visited = [NSMutableSet set];
    for (id root in roots) axHierarchy(root, 0, lines, visited);
    return energyReply(op, token, @{@"error": reason, @"ax": lines}, 2);
}

static NSArray *energyMenu(NSArray *nodes)
{
    NSMutableArray *items = [NSMutableArray array];
    for (int mode = 0; mode < 3; ++mode) {
        id item = only(nodes, ^BOOL(id node) {
            if (!role(node, @"AXMenuItem")) return NO;
            for (NSString *label in labels(node))
                if (energyMode(label) == mode) return YES;
            return NO;
        });
        if (!hasPress(item)) return nil;
        [items addObject:item];
    }
    return items;
}

static int energyUI(NSString *op, NSString *token, NSString *source, int before, int target)
{
    const BOOL opening = [op isEqual:@"energy-open"];
    NSString *expected = [source isEqual:@"ac"] ? @"Now drawing from 'AC Power'" :
                                                  @"Now drawing from 'Battery Power'";
    if (![powerSourceLine() isEqual:expected])
        return energyReply(op, token, @{@"error": @"power-source-changed"}, 2);
    if (opening && ![NSWorkspace.sharedWorkspace openURL:
                     [NSURL URLWithString:@"x-apple.systempreferences:com.apple.Battery-Settings.extension"]])
        return energyAxError(op, token, @"battery-pane-open", settingsWindows());
    id picker = opening ? waitFor(8, ^id { return energyPicker(settingsWindows(), source); }) :
                          energyPicker(settingsWindows(), source);
    if (!picker) return energyAxError(op, token, @"energy-picker-missing-or-ambiguous", settingsWindows());
    const int mode = pickerMode(picker);
    if (opening) return energyReply(op, token, @{@"source": source, @"mode": @(mode)});
    if (mode != before) return energyAxError(op, token, @"energy-picker-baseline-changed", settingsWindows());
    if (press(picker) != kAXErrorSuccess)
        return energyAxError(op, token, @"energy-picker-press", settingsWindows());
    NSArray *items = waitFor(3, ^id {
        NSArray *menu = energyMenu(tree(children(picker, kAXChildrenAttribute)));
        if (menu) return menu;
        id app = settingsApplication();
        NSMutableArray *roots = [settingsWindows() mutableCopy];
        if (app) {
            id focused = attribute(app, kAXFocusedUIElementAttribute);
            for (unsigned depth = 0; focused && depth < 3; ++depth, focused = attribute(focused, kAXParentAttribute))
                if (role(focused, @"AXMenu")) { [roots addObject:focused]; break; }
        }
        return energyMenu(tree(roots));
    });
    if (!items) return energyAxError(op, token, @"energy-picker-options", @[picker]);
    const uint64_t pressed = uptimeUs();
    const AXError status = press(items[target]);
    if (status == kAXErrorSuccess)
        waitFor(3, ^id { return pickerMode(energyPicker(settingsWindows(), source)) == target ? @YES : nil; });
    const int after = pickerMode(energyPicker(settingsWindows(), source));
    return energyReply(op, token, @{@"source": source, @"before": @(before), @"target": @(target),
                                    @"pressed_us": @(pressed), @"status": @(status), @"mode": @(after)});
}

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        alarm(32);
        if (argc < 2) return fail(@"usage");
        NSString *op = @(argv[1]);
        if ([op isEqual:@"fixture-lease"] && argc == 4) {
            NSString *packet = [NSString stringWithContentsOfFile:@(argv[3]) encoding:NSUTF8StringEncoding error:nil];
            BOOL lease = hasDhcpLease(@(argv[2]), packet);
            return emit(@{@"lease": @(lease)}, lease ? 0 : 2);
        }
        if ([op isEqual:@"wifi-state"] && argc == 3) {
            NSDictionary *state = wifiState(ssidFile(argv[2]), YES);
            return state ? emit(state) : fail(@"wifi-driver-state");
        }
        if ([op isEqual:@"wifi-disassociate"]) {
            CWInterface *wifi = [[CWWiFiClient sharedWiFiClient] interfaceWithName:@"en0"];
            if (!wifi) return fail(@"wifi-interface");
            [wifi disassociate];
            return emit(@{@"action": op, @"status": @0});
        }
        if ([op isEqual:@"cursor"]) {
            CGEventRef event = CGEventCreate(nullptr);
            if (!event) return fail(@"cursor");
            CGPoint cursor = CGEventGetLocation(event);
            CFRelease(event);
            return emit(@{@"x": @(cursor.x), @"y": @(cursor.y)});
        }
        if ([op isEqual:@"energy-state"] && argc == 3) return energyState(op, @(argv[2]));
        if ([op isEqual:@"energy-finish"] && argc == 5) {
            return energyReply(op, @(argv[2]), @{@"closed": @([@(argv[3]) isEqual:@"true"]),
                                                @"restored": @([@(argv[4]) isEqual:@"true"])});
        }
        if ([op isEqual:@"energy-quit"] && argc == 2 && getuid() != 0) {
            for (NSRunningApplication *app in settingsApps()) [app terminate];
            return waitFor(5, ^id { return settingsApps().count == 0 ? @YES : nil; }) ? 0 : 2;
        }
        BOOL trusted = AXIsProcessTrusted();
        if ([op isEqual:@"ax-trust"]) return emit(@{@"trusted": @(trusted)}, trusted ? 0 : 2);
        if (!trusted || getuid() == 0) return fail(@"console-user-ax-trust");
        if (([op isEqual:@"energy-open"] && argc == 4) || ([op isEqual:@"energy-select"] && argc == 6)) {
            NSString *source = @(argv[3]);
            if (![source isEqual:@"ac"] && ![source isEqual:@"battery"]) return fail(@"energy-source");
            int before = -1, target = -1;
            if (argc == 6) {
                if (strspn(argv[4], "012") != 1 || strlen(argv[4]) != 1 ||
                    strspn(argv[5], "012") != 1 || strlen(argv[5]) != 1 || argv[4][0] == argv[5][0])
                    return fail(@"energy-mode");
                before = argv[4][0] - '0';
                target = argv[5][0] - '0';
            }
            return energyUI(op, @(argv[2]), source, before, target);
        }
        id app = controlCenter();
        if (!app) return fail(@"control-center-process");
        if ([@[@"wifi-menu", @"wifi-off", @"wifi-on"] containsObject:op] && argc == 2)
            return wifiUI(op, app, nil);
        if ([@[@"wifi-find", @"wifi-click"] containsObject:op] && argc == 3)
            return wifiUI(op, app, ssidFile(argv[2]));
        if ([@[@"display-open", @"display-read", @"display-sample"] containsObject:op] && argc == 2)
            return displayUI(op, app, NAN);
        if ([op isEqual:@"display-set"] && argc == 3) {
            char *end = nullptr;
            const double level = strtod(argv[2], &end);
            if (end == argv[2] || *end) return fail(@"display-target");
            return displayUI(op, app, level);
        }
        return fail(@"usage");
    }
}
