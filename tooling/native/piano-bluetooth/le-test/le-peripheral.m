/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. All rights reserved. */
/* The hands-free LE test's peripheral (docs/drivers/bluetooth.md 8.5): runs on a Mac next to the tablet, advertises one GATT service
 * with the test UUID below for a bounded time, answers reads of a 180-byte value that carries a per-run nonce and records writes.
 * No pairing, no encryption requirement, no bonding. Every line goes to stdout (one event per line, stable prefixes for the run's
 * greps). It needs the Mac's Bluetooth permission: run it from a Terminal window (le-peripheral.command) or a session started
 * from one (tmux), not over ssh.
 *   le-peripheral SECONDS NONCE [plain|secure|hid]
 * plain (the default): the read and write characteristics only. secure: also a characteristic whose read requires encryption, so a
 * central that reads it must pair (LE Secure Connections, Just Works: the Mac has no display or keyboard role here); the line
 * "secure-read" shows the pairing completed and the read was answered. hid: also tries to publish the HID service (0x1812) with a
 * keyboard report map, which CoreBluetooth may refuse for a third-party process ("hid_service_added=0"): the run records which. */
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LE_TEST_SERVICE @"5484D96C-B44C-4777-BFD1-8AFE8D8B5795"
#define LE_TEST_READ @"742C0C8D-28C8-4B48-A51C-1298A09BE548"
#define LE_TEST_WRITE @"E0AFBED1-AD23-4B2A-9ADA-3AD1DB772E09"
#define LE_TEST_SECURE @"3B1C6E2A-5F0D-4C55-9C1B-7E4A0D2F6B11"

static void say(NSString *line)
{
    char stamp[16];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&now));
    printf("%s le-peripheral: %s\n", stamp, line.UTF8String);
    fflush(stdout);
}

@interface LePeripheral : NSObject <CBPeripheralManagerDelegate>
@property(nonatomic, strong) CBPeripheralManager *manager;
@property(nonatomic, strong) CBMutableService *service;
@property(nonatomic, strong) NSData *value;
@property(nonatomic) NSUInteger reads, writes;
@property(nonatomic) BOOL added, hidTried;
@property(nonatomic, copy) NSString *mode;
@property(nonatomic) NSUInteger secureReads;
@end

@implementation LePeripheral
- (void)peripheralManagerDidUpdateState:(CBPeripheralManager *)peripheral
{
    say([NSString stringWithFormat:@"state=%ld authorization=%ld", (long)peripheral.state, (long)CBManager.authorization]);
    if (peripheral.state != CBManagerStatePoweredOn || self.added) return;
    CBMutableCharacteristic *read = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:LE_TEST_READ]
        properties:CBCharacteristicPropertyRead value:nil permissions:CBAttributePermissionsReadable];
    CBMutableCharacteristic *write = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:LE_TEST_WRITE]
        properties:CBCharacteristicPropertyWrite value:nil permissions:CBAttributePermissionsWriteable];
    self.service = [[CBMutableService alloc] initWithType:[CBUUID UUIDWithString:LE_TEST_SERVICE] primary:YES];
    if ([self.mode isEqualToString:@"secure"] || [self.mode isEqualToString:@"hid"]) {
        CBMutableCharacteristic *secure = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:LE_TEST_SECURE]
            properties:CBCharacteristicPropertyRead value:nil permissions:CBAttributePermissionsReadEncryptionRequired];
        self.service.characteristics = @[read, write, secure];
    } else {
        self.service.characteristics = @[read, write];
    }
    self.added = YES;
    if ([self.mode isEqualToString:@"hid"]) [self addHid:peripheral];
    [peripheral addService:self.service];
}
/* A boot-protocol keyboard's report map (one input report: modifiers, reserved, six keys); HID Information 1.11, normally
 * connectable. The characteristics need encryption, as HID over GATT requires. */
- (void)addHid:(CBPeripheralManager *)peripheral
{
    static const uint8_t map[] = {0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
                                  0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08,
                                  0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0};
    static const uint8_t info[] = {0x11, 0x01, 0x00, 0x02};
    CBMutableCharacteristic *reportMap = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:@"2A4B"]
        properties:CBCharacteristicPropertyRead value:[NSData dataWithBytes:map length:sizeof map]
        permissions:CBAttributePermissionsReadEncryptionRequired];
    CBMutableCharacteristic *hidInfo = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:@"2A4A"]
        properties:CBCharacteristicPropertyRead value:[NSData dataWithBytes:info length:sizeof info]
        permissions:CBAttributePermissionsReadEncryptionRequired];
    CBMutableCharacteristic *report = [[CBMutableCharacteristic alloc] initWithType:[CBUUID UUIDWithString:@"2A4D"]
        properties:CBCharacteristicPropertyRead | CBCharacteristicPropertyNotify value:nil
        permissions:CBAttributePermissionsReadEncryptionRequired];
    CBMutableService *hid = [[CBMutableService alloc] initWithType:[CBUUID UUIDWithString:@"1812"] primary:YES];
    hid.characteristics = @[hidInfo, reportMap, report];
    self.hidTried = YES;
    @try {
        [peripheral addService:hid];
    } @catch (NSException *e) {
        say([NSString stringWithFormat:@"hid_service_added=0 exception=%@", e.name]);
    }
}
- (void)peripheralManager:(CBPeripheralManager *)peripheral didAddService:(CBService *)service error:(NSError *)error
{
    if ([service.UUID isEqual:[CBUUID UUIDWithString:@"1812"]]) {
        say([NSString stringWithFormat:@"hid_service_added=%d error=%ld", error == nil, (long)error.code]);
        return;
    }
    say([NSString stringWithFormat:@"service_added=%d mode=%@ error=%ld", error == nil, self.mode, (long)error.code]);
    if (error) return;
    [peripheral startAdvertising:@{CBAdvertisementDataServiceUUIDsKey: @[[CBUUID UUIDWithString:LE_TEST_SERVICE]],
                                   CBAdvertisementDataLocalNameKey: @"PianoLE1"}];
}
- (void)peripheralManagerDidStartAdvertising:(CBPeripheralManager *)peripheral error:(NSError *)error
{
    (void)peripheral;
    say([NSString stringWithFormat:@"advertising=%d error=%ld", error == nil, (long)error.code]);
}
- (void)peripheralManager:(CBPeripheralManager *)peripheral didReceiveReadRequest:(CBATTRequest *)request
{
    if (request.offset > self.value.length) {
        [peripheral respondToRequest:request withResult:CBATTErrorInvalidOffset];
        return;
    }
    request.value = [self.value subdataWithRange:NSMakeRange(request.offset, self.value.length - request.offset)];
    [peripheral respondToRequest:request withResult:CBATTErrorSuccess];
    if ([request.characteristic.UUID isEqual:[CBUUID UUIDWithString:LE_TEST_SECURE]]) {
        self.secureReads += 1;
        say([NSString stringWithFormat:@"secure-read offset=%lu secure_reads=%lu (the link is encrypted: paired)",
            (unsigned long)request.offset, (unsigned long)self.secureReads]);
        return;
    }
    self.reads += 1;
    say([NSString stringWithFormat:@"read offset=%lu reads=%lu mtu_payload=%lu", (unsigned long)request.offset,
        (unsigned long)self.reads, (unsigned long)request.central.maximumUpdateValueLength]);
}
- (void)peripheralManager:(CBPeripheralManager *)peripheral didReceiveWriteRequests:(NSArray<CBATTRequest *> *)requests
{
    for (CBATTRequest *request in requests) {
        self.writes += 1;
        NSString *text = [[NSString alloc] initWithData:request.value encoding:NSUTF8StringEncoding] ?: @"(binary)";
        if (text.length > 64) text = [text substringToIndex:64];
        say([NSString stringWithFormat:@"write bytes=%lu writes=%lu value=%@", (unsigned long)request.value.length,
            (unsigned long)self.writes, text]);
    }
    [peripheral respondToRequest:requests.firstObject withResult:CBATTErrorSuccess];
}
@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        const char *mode = argc == 4 ? argv[3] : "plain";
        if ((argc != 3 && argc != 4) || (strcmp(mode, "plain") && strcmp(mode, "secure") && strcmp(mode, "hid"))) {
            fputs("usage: le-peripheral SECONDS NONCE [plain|secure|hid] (1..3600; NONCE 1..32 characters [A-Za-z0-9-])\n", stderr);
            return 2;
        }
        long seconds = strtol(argv[1], NULL, 10);
        size_t n = strlen(argv[2]);
        if (seconds < 1 || seconds > 3600 || n < 1 || n > 32 || strspn(argv[2], "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-") != n) {
            fputs("le-peripheral: bad arguments\n", stderr);
            return 2;
        }
        LePeripheral *p = [LePeripheral new];
        p.mode = [NSString stringWithUTF8String:mode];
        NSMutableString *text = [NSMutableString stringWithFormat:@"piano-le1 nonce=%s ", argv[2]];
        while (text.length < 180) [text appendString:@"0123456789"];
        p.value = [[text substringToIndex:180] dataUsingEncoding:NSUTF8StringEncoding];
        say([NSString stringWithFormat:@"start seconds=%ld nonce=%s value_bytes=%lu", seconds, argv[2], (unsigned long)p.value.length]);
        p.manager = [[CBPeripheralManager alloc] initWithDelegate:p queue:nil];
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
        while ([until timeIntervalSinceNow] > 0)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
        [p.manager stopAdvertising];
        [p.manager removeAllServices];
        say([NSString stringWithFormat:@"end reads=%lu writes=%lu secure_reads=%lu", (unsigned long)p.reads, (unsigned long)p.writes,
            (unsigned long)p.secureReads]);
    }
    return 0;
}
