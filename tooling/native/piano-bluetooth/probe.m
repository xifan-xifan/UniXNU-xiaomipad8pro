/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. All rights reserved. */
/* Bounded discovery; no pairing, remote-name request or keys. Two bounded connection modes reach only the harness's own Mac
 * (docs/drivers/bluetooth.md 8.5): --le-read connects to a peripheral advertising the LE test service below (le-test/le-peripheral.m),
 * reads its value and writes one back; --classic-sdp pages the one address it is given and runs an SDP query. Neither pairs or bonds,
 * and no address, name or advertisement is printed. --le-pair is --le-read plus a read of the peripheral's encryption-required
 * characteristic (le-peripheral ... secure), so bluetoothd pairs with the harness's own Mac (LE Secure Connections, Just Works) and
 * the bond is the only key it keeps. --le-rssi only scans for the test service and prints the RSSI of its reports (the RF check). */
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
#import <CoreBluetooth/CoreBluetooth.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

@interface PianoBtProbe : NSObject <CBCentralManagerDelegate>
@property(nonatomic, strong) CBCentralManager *central;
@property(nonatomic, strong) IOBluetoothDeviceInquiry *inquiry;
@property(nonatomic, strong) NSMutableSet<NSUUID *> *leSeen;
@property(nonatomic) NSUInteger leReports;
@property(nonatomic) NSUInteger classicReports;
@property(nonatomic) BOOL leStarted;
@property(nonatomic) BOOL classicStarted;
@property(nonatomic) BOOL classicComplete;
@property(nonatomic) IOReturn classicError;
@end
#define LE_TEST_SERVICE @"5484D96C-B44C-4777-BFD1-8AFE8D8B5795"
#define LE_TEST_READ @"742C0C8D-28C8-4B48-A51C-1298A09BE548"
#define LE_TEST_WRITE @"E0AFBED1-AD23-4B2A-9ADA-3AD1DB772E09"
#define LE_TEST_SECURE @"3B1C6E2A-5F0D-4C55-9C1B-7E4A0D2F6B11"

static double msSince(NSDate *t0) { return t0 ? -[t0 timeIntervalSinceNow] * 1000.0 : -1; }

/* --le-read: one GATT round trip with the test peripheral; each step is a line, the summary names the last step reached. */
@interface PianoBtLeRead : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, strong) CBCentralManager *central;
@property(nonatomic, strong) CBPeripheral *peer;
@property(nonatomic, strong) NSDate *t0;
@property(nonatomic, copy) NSString *stage;
@property(nonatomic) BOOL found, connected, services, characteristics, read, written, disconnected, done;
@property(nonatomic) BOOL pair, secureRead;
@property(nonatomic) long error;
@property(nonatomic) unsigned long valueBytes;
@end
@implementation PianoBtLeRead
- (void)step:(NSString *)stage line:(NSString *)line
{
    self.stage = stage;
    printf("piano-bt-path1 le-read: %s t_ms=%.0f\n", line.UTF8String, msSince(self.t0));
}
- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    printf("piano-bt-path1 le-read: le_state=%ld authorization=%ld\n", (long)central.state, (long)CBManager.authorization);
}
- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI
{
    (void)advertisementData;
    if (self.found) return;
    self.found = YES;
    self.peer = peripheral;
    peripheral.delegate = self;
    [central stopScan];
    [self step:@"found" line:[NSString stringWithFormat:@"found=1 rssi=%ld", RSSI.longValue]];
    [central connectPeripheral:peripheral options:nil];
}
- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
    (void)central;
    self.connected = YES;
    [self step:@"connected" line:@"connected=1"];
    [peripheral discoverServices:@[[CBUUID UUIDWithString:LE_TEST_SERVICE]]];
}
- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    (void)central; (void)peripheral;
    self.error = error.code;
    self.done = YES;
    [self step:@"connect-failed" line:[NSString stringWithFormat:@"connect_failed=1 error=%ld", (long)error.code]];
}
- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    (void)central; (void)peripheral;
    self.disconnected = YES;
    if (error && !self.error) self.error = error.code;
    self.done = YES;
    printf("piano-bt-path1 le-read: disconnected=1 error=%ld after=%s t_ms=%.0f\n", (long)error.code, self.stage.UTF8String, msSince(self.t0));
}
- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
    CBService *service = nil;
    for (CBService *s in peripheral.services)
        if ([s.UUID isEqual:[CBUUID UUIDWithString:LE_TEST_SERVICE]]) service = s;
    if (error || !service) {
        self.error = error ? error.code : -1;
        [self step:@"services-failed" line:[NSString stringWithFormat:@"services_failed=1 error=%ld", self.error]];
        [self.central cancelPeripheralConnection:peripheral];
        return;
    }
    self.services = YES;
    [self step:@"services" line:@"services=1"];
    [peripheral discoverCharacteristics:@[[CBUUID UUIDWithString:LE_TEST_READ], [CBUUID UUIDWithString:LE_TEST_WRITE],
        [CBUUID UUIDWithString:LE_TEST_SECURE]] forService:service];
}
- (void)peripheral:(CBPeripheral *)peripheral didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error
{
    CBCharacteristic *read = nil;
    for (CBCharacteristic *c in service.characteristics)
        if ([c.UUID isEqual:[CBUUID UUIDWithString:LE_TEST_READ]]) read = c;
    if (error || !read) {
        self.error = error ? error.code : -1;
        [self step:@"characteristics-failed" line:[NSString stringWithFormat:@"characteristics_failed=1 error=%ld", self.error]];
        [self.central cancelPeripheralConnection:peripheral];
        return;
    }
    self.characteristics = YES;
    [self step:@"characteristics" line:[NSString stringWithFormat:@"characteristics=%lu", (unsigned long)service.characteristics.count]];
    [peripheral readValueForCharacteristic:read];
}
- (void)peripheral:(CBPeripheral *)peripheral didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error
{
    if ([characteristic.UUID isEqual:[CBUUID UUIDWithString:LE_TEST_SECURE]]) {
        if (error) self.error = error.code;
        else self.secureRead = YES;
        [self step:error ? @"secure-read-failed" : @"secure-read" line:[NSString stringWithFormat:@"secure_read=%u error=%ld value_bytes=%lu",
            !error, (long)error.code, (unsigned long)characteristic.value.length]];
        self.done = YES;
        [self.central cancelPeripheralConnection:peripheral];
        return;
    }
    if (error || !characteristic.value) {
        self.error = error ? error.code : -1;
        [self step:@"read-failed" line:[NSString stringWithFormat:@"read_failed=1 error=%ld", self.error]];
        [self.central cancelPeripheralConnection:peripheral];
        return;
    }
    self.read = YES;
    self.valueBytes = characteristic.value.length;
    NSString *text = [[NSString alloc] initWithData:characteristic.value encoding:NSUTF8StringEncoding] ?: @"";
    NSString *nonce = @"none";
    NSRange r = [text rangeOfString:@"nonce="];
    if (r.location != NSNotFound) {
        NSString *rest = [text substringFromIndex:NSMaxRange(r)];
        NSRange sp = [rest rangeOfString:@" "];
        nonce = sp.location == NSNotFound ? rest : [rest substringToIndex:sp.location];
        if (nonce.length > 32) nonce = [nonce substringToIndex:32];
    }
    [self step:@"read" line:[NSString stringWithFormat:@"read=1 value_bytes=%lu nonce=%@ write_max=%lu", self.valueBytes, nonce,
        (unsigned long)[peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse]]];
    CBCharacteristic *write = nil;
    for (CBCharacteristic *c in characteristic.service.characteristics)
        if ([c.UUID isEqual:[CBUUID UUIDWithString:LE_TEST_WRITE]]) write = c;
    if (!write) {
        [self.central cancelPeripheralConnection:peripheral];
        return;
    }
    NSData *reply = [[NSString stringWithFormat:@"tablet-ok nonce=%@", nonce] dataUsingEncoding:NSUTF8StringEncoding];
    [peripheral writeValue:reply forCharacteristic:write type:CBCharacteristicWriteWithResponse];
}
- (void)peripheral:(CBPeripheral *)peripheral didWriteValueForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error
{
    (void)characteristic;
    if (error) self.error = error.code;
    else self.written = YES;
    [self step:error ? @"write-failed" : @"written" line:[NSString stringWithFormat:@"written=%u error=%ld", !error, (long)error.code]];
    CBCharacteristic *secure = nil;
    for (CBCharacteristic *c in characteristic.service.characteristics)
        if ([c.UUID isEqual:[CBUUID UUIDWithString:LE_TEST_SECURE]]) secure = c;
    if (self.pair && !error && secure) {
        [self step:@"pairing" line:@"secure_read_started=1 (encryption required: bluetoothd pairs)"];
        [peripheral readValueForCharacteristic:secure];
        return;
    }
    if (self.pair && !secure) [self step:@"no-secure" line:@"secure_characteristic=0 (peripheral not in secure mode)"];
    [self.central cancelPeripheralConnection:peripheral];
}
@end

/* --le-rssi: the RSSI of every report of the test service (no connection). */
@interface PianoBtLeRssi : NSObject <CBCentralManagerDelegate>
@property(nonatomic, strong) CBCentralManager *central;
@property(nonatomic, strong) NSMutableArray<NSNumber *> *rssi;
@end
@implementation PianoBtLeRssi
- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    printf("piano-bt-path1 le-rssi: le_state=%ld authorization=%ld\n", (long)central.state, (long)CBManager.authorization);
}
- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI
{
    (void)central; (void)peripheral; (void)advertisementData;
    if (RSSI.longValue != 127) [self.rssi addObject:RSSI];
}
@end

/* --classic-sdp: a baseband connection to one given address and an SDP query; no pairing. */
@interface PianoBtClassicSdp : NSObject
@property(nonatomic) BOOL connectDone, sdpDone;
@property(nonatomic) IOReturn connectStatus, sdpStatus;
@end
@implementation PianoBtClassicSdp
- (void)connectionComplete:(IOBluetoothDevice *)device status:(IOReturn)status
{
    (void)device;
    self.connectStatus = status;
    self.connectDone = YES;
}
- (void)sdpQueryComplete:(IOBluetoothDevice *)device status:(IOReturn)status
{
    (void)device;
    self.sdpStatus = status;
    self.sdpDone = YES;
}
@end

@implementation PianoBtProbe
- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    printf("piano-bt-path1 probe: le_state=%ld authorization=%ld\n", (long)central.state, (long)CBManager.authorization);
}
- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI
{
    (void)central; (void)advertisementData; (void)RSSI;
    self.leReports += 1;
    if (peripheral.identifier) [self.leSeen addObject:peripheral.identifier];
}
- (void)deviceInquiryStarted:(IOBluetoothDeviceInquiry *)sender
{
    (void)sender;
    self.classicStarted = YES;
    puts("piano-bt-path1 probe: classic_inquiry_started=1");
}
- (void)deviceInquiryDeviceFound:(IOBluetoothDeviceInquiry *)sender device:(IOBluetoothDevice *)device
{
    (void)sender; (void)device;
    self.classicReports += 1;
}
- (void)deviceInquiryComplete:(IOBluetoothDeviceInquiry *)sender error:(IOReturn)error aborted:(BOOL)aborted
{
    (void)sender;
    self.classicComplete = YES;
    self.classicError = error;
    printf("piano-bt-path1 probe: classic_complete=1 error=0x%08x aborted=%u\n", error, (unsigned)aborted);
}
@end
static void spin(double seconds)
{
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        [NSThread sleepForTimeInterval:0.01];
    }
}
static void deadline(int signalNumber)
{
    (void)signalNumber;
    static const char line[] = "piano-bt-path1 probe: client_ipc_deadline=1\n";
    (void)write(STDERR_FILENO, line, sizeof(line) - 1);
    _exit(6);
}
static int usage(void)
{
    fputs("usage: piano-bt-probe --status | --scan SECONDS (1..30 per radio) | --le-read SECONDS (5..120)"
          " | --le-pair SECONDS (5..120) | --le-rssi SECONDS (5..120) | --classic-sdp XX-XX-XX-XX-XX-XX SECONDS (5..120)\n", stderr);
    return 2;
}
static long boundedSeconds(const char *text)
{
    char *end = NULL;
    long seconds = strtol(text, &end, 10);
    return end && !*end && seconds >= 5 && seconds <= 120 ? seconds : 0;
}
/* Both connection modes share the probe's preflight: a refused scoped grant ends the run before any manager exists. */
static bool authorized(const char *mode)
{
    CBManagerAuthorization a = CBManager.authorization;
    printf("piano-bt-path1 %s: started=1 initial_authorization=%ld\n", mode, (long)a);
    if (a == CBManagerAuthorizationAllowedAlways) return true;
    printf("piano-bt-path1 %s: complete=1 client_policy_unavailable=1\n", mode);
    return false;
}
static int leRssi(int argc, const char *argv[])
{
    long seconds = argc == 3 ? boundedSeconds(argv[2]) : 0;
    if (!seconds) return usage();
    signal(SIGALRM, deadline);
    alarm((unsigned)(seconds + 30));
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!authorized("le-rssi")) return 5;
    PianoBtLeRssi *t = [PianoBtLeRssi new];
    t.rssi = [NSMutableArray array];
    t.central = [[CBCentralManager alloc] initWithDelegate:t queue:nil options:@{CBCentralManagerOptionShowPowerAlertKey:@NO}];
    NSDate *ready = [NSDate dateWithTimeIntervalSinceNow:5.0];
    while (t.central.state == CBManagerStateUnknown && [ready timeIntervalSinceNow] > 0) spin(0.05);
    if (t.central.state != CBManagerStatePoweredOn) {
        printf("piano-bt-path1 le-rssi: complete=1 le_state=%ld stage=not-powered\n", (long)t.central.state);
        return 3;
    }
    [t.central scanForPeripheralsWithServices:@[[CBUUID UUIDWithString:LE_TEST_SERVICE]]
                                      options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];
    spin((double)seconds);
    [t.central stopScan];
    NSArray<NSNumber *> *v = [t.rssi sortedArrayUsingSelector:@selector(compare:)];
    unsigned long n = v.count, strong = 0;
    for (NSNumber *x in v) strong += x.longValue > -100;
    printf("piano-bt-path1 le-rssi: complete=1 reports=%lu min=%ld p10=%ld median=%ld p90=%ld max=%ld above_minus100=%lu seconds=%ld\n", n,
        n ? v[0].longValue : 0, n ? v[n / 10].longValue : 0, n ? v[n / 2].longValue : 0, n ? v[(n * 9) / 10].longValue : 0,
        n ? v[n - 1].longValue : 0, strong, seconds);
    t.central.delegate = nil;
    alarm(0);
    return n ? 0 : 4;
}
static int leRead(int argc, const char *argv[])
{
    const bool pair = !strcmp(argv[1], "--le-pair");
    const char *mode = pair ? "le-pair" : "le-read";
    long seconds = argc == 3 ? boundedSeconds(argv[2]) : 0;
    if (!seconds) return usage();
    signal(SIGALRM, deadline);
    alarm((unsigned)(seconds + 30));
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!authorized(mode)) return 5;
    PianoBtLeRead *t = [PianoBtLeRead new];
    t.pair = pair;
    t.stage = @"start";
    t.central = [[CBCentralManager alloc] initWithDelegate:t queue:nil options:@{CBCentralManagerOptionShowPowerAlertKey:@NO}];
    NSDate *ready = [NSDate dateWithTimeIntervalSinceNow:5.0];
    while (t.central.state == CBManagerStateUnknown && [ready timeIntervalSinceNow] > 0) spin(0.05);
    if (t.central.state != CBManagerStatePoweredOn) {
        printf("piano-bt-path1 le-read: complete=1 le_state=%ld stage=not-powered\n", (long)t.central.state);
        return 3;
    }
    t.t0 = [NSDate date];
    [t.central scanForPeripheralsWithServices:@[[CBUUID UUIDWithString:LE_TEST_SERVICE]] options:nil];
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:(double)seconds];
    while (!t.done && [until timeIntervalSinceNow] > 0) spin(0.05);
    if (!t.found) [t.central stopScan];
    if (t.peer && !t.disconnected) {
        [t.central cancelPeripheralConnection:t.peer];
        spin(1.0);
    }
    printf("piano-bt-path1 le-read: complete=1 found=%u connected=%u services=%u characteristics=%u read=%u written=%u "
           "disconnected=%u value_bytes=%lu error=%ld stage=%s timed_out=%u t_ms=%.0f\n",
        t.found, t.connected, t.services, t.characteristics, t.read, t.written, t.disconnected, t.valueBytes, t.error,
        t.stage.UTF8String, !t.done, msSince(t.t0));
    if (pair) printf("piano-bt-path1 le-pair: complete=1 written=%u secure_read=%u error=%ld stage=%s\n", t.written, t.secureRead, t.error,
                     t.stage.UTF8String);
    t.peer.delegate = nil;
    t.central.delegate = nil;
    alarm(0);
    return t.read && t.written && (!pair || t.secureRead) ? 0 : 4;
}
static int classicSdp(int argc, const char *argv[])
{
    long seconds = argc == 4 ? boundedSeconds(argv[3]) : 0;
    if (!seconds || strlen(argv[2]) != 17) return usage();
    signal(SIGALRM, deadline);
    alarm((unsigned)(2 * seconds + 30));
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!authorized("classic-sdp")) return 5;
    IOBluetoothDevice *device = [IOBluetoothDevice deviceWithAddressString:[NSString stringWithUTF8String:argv[2]]];
    if (!device) {
        puts("piano-bt-path1 classic-sdp: complete=1 bad_address=1");
        return 2;
    }
    PianoBtClassicSdp *t = [PianoBtClassicSdp new];
    NSDate *t0 = [NSDate date];
    IOReturn opened = [device openConnection:t];
    printf("piano-bt-path1 classic-sdp: open=0x%08x paired=%u\n", opened, (unsigned)device.isPaired);
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:(double)seconds];
    while (opened == kIOReturnSuccess && !t.connectDone && [until timeIntervalSinceNow] > 0) spin(0.05);
    double tConnect = msSince(t0);
    printf("piano-bt-path1 classic-sdp: connected=%u status=0x%08x t_ms=%.0f\n", t.connectDone && !t.connectStatus, t.connectStatus, tConnect);
    IOReturn query = kIOReturnNotOpen;
    if (t.connectDone && !t.connectStatus) {
        query = [device performSDPQuery:t];
        until = [NSDate dateWithTimeIntervalSinceNow:(double)seconds];
        while (query == kIOReturnSuccess && !t.sdpDone && [until timeIntervalSinceNow] > 0) spin(0.05);
    }
    unsigned long services = t.sdpDone && !t.sdpStatus ? (unsigned long)device.services.count : 0;
    printf("piano-bt-path1 classic-sdp: complete=1 connected=%u connect_status=0x%08x sdp_start=0x%08x sdp_done=%u sdp_status=0x%08x "
           "services=%lu t_connect_ms=%.0f t_ms=%.0f\n", t.connectDone && !t.connectStatus, t.connectStatus, query, t.sdpDone,
           t.sdpStatus, services, tConnect, msSince(t0));
    if (device.isConnected) (void)[device closeConnection];
    spin(0.5);
    alarm(0);
    return services ? 0 : 4;
}
int main(int argc, const char *argv[])
{
    @autoreleasepool {
        if (argc >= 3 && (!strcmp(argv[1], "--le-read") || !strcmp(argv[1], "--le-pair"))) return leRead(argc, argv);
        if (argc >= 3 && !strcmp(argv[1], "--le-rssi")) return leRssi(argc, argv);
        if (argc >= 4 && !strcmp(argv[1], "--classic-sdp")) return classicSdp(argc, argv);
        bool scan = argc == 3 && !strcmp(argv[1], "--scan");
        if (!scan && !(argc == 2 && !strcmp(argv[1], "--status"))) return usage();
        char *end = NULL;
        long seconds = scan ? strtol(argv[2], &end, 10) : 0;
        if (scan && (!end || *end || seconds < 1 || seconds > 30)) return usage();
        /* Also bound any synchronous framework initialization waiting for daemon IPC. */
        signal(SIGALRM, deadline);
        alarm((unsigned)(2 * seconds + 30));
        setvbuf(stdout, NULL, _IOLBF, 0);
        printf("piano-bt-path1 probe: started=1 scan=%u\n", scan);
        // Preflight before defaultController: its coordinator allocates managers
        // too. If this process's scoped grant is refused, never open a grant dialog.
        CBManagerAuthorization initialAuthorization = CBManager.authorization;
        printf("piano-bt-path1 probe: initial_authorization=%ld\n", (long)initialAuthorization);
        if (initialAuthorization != CBManagerAuthorizationAllowedAlways) {
            puts("piano-bt-path1 probe: le_skipped_client_policy=1 client_initialization_skipped=1");
            if (scan) puts("piano-bt-path1 scan: complete=1 client_policy_unavailable=1 classic_reports=0 le_reports=0");
            return 5;
        }
        IOBluetoothHostController *controller = [IOBluetoothHostController defaultController];
        NSInteger power = controller ? controller.powerState : -1;
        printf("piano-bt-path1 probe: default_object=%u power=%ld\n", controller != nil, (long)power);
        if (!scan) return power == 1 ? 0 : 3;
        if (power != 1) {
            puts("piano-bt-path1 scan: complete=1 controller_not_powered=1 classic_reports=0 le_reports=0");
            return 3;
        }
        PianoBtProbe *probe = [PianoBtProbe new];
        probe.leSeen = [NSMutableSet set];
        probe.inquiry = [IOBluetoothDeviceInquiry inquiryWithDelegate:probe];
        probe.inquiry.updateNewDeviceNames = NO;
        probe.inquiry.inquiryLength = (uint8_t)seconds;
        IOReturn started = [probe.inquiry start];
        printf("piano-bt-path1 probe: classic_start=0x%08x window_s=%ld\n", started, seconds);
        /* Do not conclude early: native inquiry completion may be throttled. */
        spin((double)seconds);
        if (started == kIOReturnSuccess && !probe.classicComplete) (void)[probe.inquiry stop];
        spin(0.2);
        CBManagerAuthorization authorization = CBManager.authorization;
        bool leAuthorized = authorization == CBManagerAuthorizationAllowedAlways;
        if (leAuthorized) {
            probe.central = [[CBCentralManager alloc] initWithDelegate:probe queue:nil
                options:@{CBCentralManagerOptionShowPowerAlertKey:@NO}];
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
            while (probe.central.state == CBManagerStateUnknown && [deadline timeIntervalSinceNow] > 0) spin(0.05);
            if (probe.central.state == CBManagerStatePoweredOn) {
                [probe.central scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey:@YES}];
                probe.leStarted = YES;
                spin((double)seconds);
                [probe.central stopScan];
            }
        } else {
            /* In particular, do not open an unattended TCC grant dialog. */
            printf("piano-bt-path1 probe: le_authorization=%ld le_skipped_client_policy=1\n", (long)authorization);
        }
        printf("piano-bt-path1 scan: complete=1 classic_started=%u classic_reports=%lu classic_error=0x%08x le_started=%u le_reports=%lu le_unique=%lu client_authorized=%u\n",
            (unsigned)probe.classicStarted, (unsigned long)probe.classicReports, probe.classicError,
            (unsigned)probe.leStarted, (unsigned long)probe.leReports, (unsigned long)probe.leSeen.count, (unsigned)leAuthorized);
        probe.inquiry.delegate = nil;
        probe.central.delegate = nil;
        alarm(0);
        return probe.classicReports || probe.leReports ? 0 : leAuthorized ? 4 : 5;
    }
}
