// CoreBluetooth delivers on a serial queue. One transport thread owns links and pumps immutable events while the
// shared protocol waits for replies. Sync mode works without the host's private Bluetooth address (S2_ADV_TAKE).
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "switch2.h"
#include "switch2_proto.h"

@interface MKWPadLink : NSObject {
@public
    CBPeripheral *pad;
    CBCharacteristic *cmd, *reply, *input, *vib;
    S2Session *session;
    uint16_t pid;
    int remaining;
    BOOL ready, gone, inputOn;
    uint64_t deadline;
}
@end
@implementation MKWPadLink
@end

static NSMutableDictionary<NSUUID *, MKWPadLink *> *links;
static NSMutableArray<dispatch_block_t> *events;
static NSCondition *condition;

static void deliver(dispatch_block_t block) {
    [condition lock];
    [events addObject:[block copy]];
    [condition signal];
    [condition unlock];
}
static void pump(int ms) {
    [condition lock];
    if (!events.count && ms > 0) [condition waitUntilDate:[NSDate dateWithTimeIntervalSinceNow:ms / 1000.0]];
    NSArray *batch = [events copy];
    [events removeAllObjects];
    [condition unlock];
    for (dispatch_block_t event in batch) event();
}

static int send_data(MKWPadLink *l, CBCharacteristic *ch, const uint8_t *data, size_t n) {
    if (!ch || l->gone || !l->pad.canSendWriteWithoutResponse ||
        !(ch.properties & CBCharacteristicPropertyWriteWithoutResponse) ||
        n > [l->pad maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse]) return 0;
    [l->pad writeValue:[NSData dataWithBytes:data length:n] forCharacteristic:ch type:CBCharacteristicWriteWithoutResponse];
    return 1;
}
static int command(S2Link *ptr, const uint8_t *data, size_t n) {
    MKWPadLink *l = (__bridge MKWPadLink *)(void *)ptr;
    return send_data(l, l->cmd, data, n);
}
static int vibrate(S2Link *ptr, const uint8_t *data, size_t n) {
    MKWPadLink *l = (__bridge MKWPadLink *)(void *)ptr;
    return send_data(l, l->vib, data, n);
}
static int subscribe(S2Link *ptr) {
    MKWPadLink *l = (__bridge MKWPadLink *)(void *)ptr;
    [l->pad setNotifyValue:YES forCharacteristic:l->input];
    uint64_t end = switch2_now_ns() + 2000000000;
    while (!l->inputOn && !l->gone && switch2_now_ns() < end) pump(10);
    return l->inputOn && !l->gone;
}
static const S2LinkOps ops = {command, vibrate, subscribe, pump};

@interface MKWTransport : NSObject<CBCentralManagerDelegate, CBPeripheralDelegate>
@end
@implementation MKWTransport
- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    if (central.state == CBManagerStatePoweredOn) {
        [central scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];
        rt_log("input: Switch 2 Bluetooth scan started; hold Sync to connect");
    } else {
        rt_log("input: Switch 2 Bluetooth unavailable (CoreBluetooth state %ld)", (long)central.state);
        deliver(^{ for (MKWPadLink *l in links.allValues) l->gone = YES; });
    }
}
- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)pad
     advertisementData:(NSDictionary *)ad RSSI:(NSNumber *)rssi {
    NSData *manufacturer = ad[CBAdvertisementDataManufacturerDataKey];
    const uint8_t *bytes = manufacturer.bytes;
    s2_advert a;
    if (manufacturer.length < 2 || s2_u16(bytes) != S2_COMPANY_ID ||
        !s2_advert_parse(bytes + 2, manufacturer.length - 2, &a) || s2_advert_use(&a, NULL) != S2_ADV_TAKE) return;
    deliver(^{
        if (links[pad.identifier]) return;
        MKWPadLink *l = [MKWPadLink new];
        l->pad = pad;
        l->pid = a.pid;
        l->deadline = switch2_now_ns() + 15000000000ull;
        links[pad.identifier] = l;
        pad.delegate = self;
        [central connectPeripheral:pad options:nil];
    });
}
- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)pad {
    [pad discoverServices:nil];
}
- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)pad error:(NSError *)error {
    deliver(^{ MKWPadLink *l = links[pad.identifier]; if (l) l->gone = YES; });
}
- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)pad error:(NSError *)error {
    [self centralManager:central didDisconnectPeripheral:pad error:error];
}
- (void)peripheral:(CBPeripheral *)pad didDiscoverServices:(NSError *)error {
    NSArray *services = [pad.services copy];
    deliver(^{
        MKWPadLink *l = links[pad.identifier];
        if (!l) return;
        if (error || !services.count) { l->gone = YES; return; }
        l->remaining = (int)services.count;
        for (CBService *svc in services) [pad discoverCharacteristics:nil forService:svc];
    });
}
- (void)peripheral:(CBPeripheral *)pad didDiscoverCharacteristicsForService:(CBService *)svc error:(NSError *)error {
    NSArray *chars = [svc.characteristics copy];
    deliver(^{
        MKWPadLink *l = links[pad.identifier];
        if (!l) return;
        for (CBCharacteristic *c in chars) {
            NSString *uuid = c.UUID.UUIDString;
            if ([uuid isEqualToString:@S2_UUID_CMD_WR]) l->cmd = c;
            if ([uuid isEqualToString:@S2_UUID_CMD_RSP]) l->reply = c;
            if ([uuid isEqualToString:@S2_UUID_INPUT]) l->input = c;
            if ([uuid isEqualToString:@S2_UUID_VIB_PRO] || [uuid isEqualToString:@S2_UUID_VIB_JOYCON_R] ||
                [uuid isEqualToString:@S2_UUID_VIB_JOYCON_L]) l->vib = c;
        }
        if (--l->remaining == 0) {
            if (l->cmd && l->reply && l->input) [pad setNotifyValue:YES forCharacteristic:l->reply];
            else l->gone = YES;
        }
    });
}
- (void)peripheral:(CBPeripheral *)pad didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)c error:(NSError *)err {
    BOOL on = !err && c.isNotifying;
    deliver(^{
        MKWPadLink *l = links[pad.identifier];
        if (!l) return;
        if (!on) l->gone = YES;
        else if (c == l->reply) l->ready = YES;
        else if (c == l->input) l->inputOn = YES;
    });
}
- (void)peripheral:(CBPeripheral *)pad didUpdateValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)error {
    if (error) return;
    NSData *data = [c.value copy];
    uint64_t now = switch2_now_ns();
    deliver(^{
        MKWPadLink *l = links[pad.identifier];
        if (!l || !l->session || l->gone) return;
        if (c == l->input) switch2_report(l->session, data.bytes, data.length, now);
        if (c == l->reply) switch2_reply(l->session, data.bytes, data.length);
    });
}
@end

static void run(void *unused) {
    (void)unused;
    @autoreleasepool {
        condition = [NSCondition new];
        events = [NSMutableArray new];
        links = [NSMutableDictionary new];
        __attribute__((objc_precise_lifetime)) MKWTransport *transport = [MKWTransport new];
        CBCentralManager *central = [[CBCentralManager alloc] initWithDelegate:transport
                                        queue:dispatch_queue_create("mkw.switch2", DISPATCH_QUEUE_SERIAL)];
        for (;;) @autoreleasepool {
            pump(10);
            for (MKWPadLink *l in links.allValues) {
                if (!l->gone && !l->session && l->ready) {
                    l->session = switch2_new((S2Link *)(__bridge void *)l, &ops, l->pid, S2_ADV_TAKE, NULL);
                    if (!l->session || !switch2_handshake(l->session)) l->gone = YES;
                }
                if (!l->session && switch2_now_ns() >= l->deadline) l->gone = YES;
                if (l->gone) {
                    if (l->session) switch2_close(l->session);
                    [central cancelPeripheralConnection:l->pad];
                    [links removeObjectForKey:l->pad.identifier];
                }
            }
            switch2_tick();
        }
    }
}

void switch2_start(void) {
    if (cfg_int("MKW_SWITCH2") && !plat_thread_spawn(run, NULL, "switch2", 0))
        rt_log("input: Switch 2 Bluetooth transport could not start");
}
