// opalcore: fixes + diagnostics built into Opal's camera-driver XPC service (OpalCameraDeviceService),
// loaded via an added LC_LOAD_DYLIB. Writes ~/Library/Logs/opal-watch/camera.log.
//
//  1. Streaming debounce at the source: -[OpalCameraInput changeStreamingState:withState:] — state 1 ("off")
//     is held kOffDelay seconds; any newer call cancels it; when due it is applied only if no app is using the
//     "Opal C1" virtual camera (CoreMediaIO). Everything else is applied immediately and unchanged.
//  2. Camera log tap: adds a depthai log callback to the live dai::Device (DeviceManager->_device) so the
//     camera's own warnings/errors (e.g. "Fatal error on MSS CPU") and periodic info lines are recorded.
//  3. Idle telemetry: chip temperature + LEON CPU via RPC every 30 s while a device is connected (even when
//     the sensor is stopped), guarded by a lock against device teardown.
//  4. pipecap.mm: optional capture of the pipeline + control messages sent to the camera (Windows port reference).
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "depthai/device/Device.hpp"

extern "C" int opal_virtualcam_in_use(void);  // cmio_inuse.m
void opalcore_pipecap_init(void);              // pipecap.mm

// ---------------------------------------------------------------- logging
static dispatch_queue_t gLogQ;
static void clog(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
static void clog(NSString *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSDate *now = NSDate.date;
    dispatch_async(gLogQ, ^{
        static NSDateFormatter *f;
        if(!f) { f = [NSDateFormatter new]; f.dateFormat = @"yyyy-MM-dd HH:mm:ss.SSS"; }
        NSString *line = [NSString stringWithFormat:@"%@ %@\n", [f stringFromDate:now], s];
        NSString *path = [@"~/Library/Logs/opal-watch/camera.log" stringByExpandingTildeInPath];
        NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:path];
        if(!h) { [line writeToFile:path atomically:NO encoding:NSUTF8StringEncoding error:nil]; return; }
        [h seekToEndOfFile];
        [h writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
        [h closeFile];
    });
}

// ---------------------------------------------------------------- 1. streaming debounce
static const double kOffDelay = 5.0;
static const NSInteger kStateOff = 1;
static dispatch_queue_t gDQ;
static uint64_t gGen;
static IMP gOrigChange;
static size_t gStructSize = 48;

typedef void (*ChangeFn)(id, SEL, NSInteger, const void *);

static void tryApplyOff(uint64_t myGen, id target, SEL sel, NSData *st, BOOL rechecking) {
    if(gGen != myGen) { clog(@"[debounce] pending off superseded -> dropped (sensor stays on)"); return; }
    int inUse = opal_virtualcam_in_use();
    if(inUse == 1) {
        if(!rechecking) clog(@"[debounce] off due but an app is using Opal C1 -> sensor stays on, rechecking every 2s");
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), gDQ, ^{ tryApplyOff(myGen, target, sel, st, YES); });
        return;
    }
    clog(@"[debounce] applying off (in_use=%d)", inUse);
    ((ChangeFn)gOrigChange)(target, sel, kStateOff, st.bytes);
}

static void hook_changeStreaming(id self, SEL _cmd, NSInteger state, const void *camState) {
    __block uint64_t myGen;
    dispatch_sync(gDQ, ^{ myGen = ++gGen; });
    if(state != kStateOff) {
        clog(@"[debounce] state=%ld -> applied now", (long)state);
        ((ChangeFn)gOrigChange)(self, _cmd, state, camState);
        return;
    }
    NSData *copy = [NSData dataWithBytes:camState length:gStructSize];
    id target = self;
    clog(@"[debounce] state=1 (off) -> held %.0fs", kOffDelay);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kOffDelay * NSEC_PER_SEC)), gDQ, ^{
        tryApplyOff(myGen, target, _cmd, copy, NO);
    });
}

// ---------------------------------------------------------------- 2+3. device log tap + telemetry
static std::mutex gDevMutex;           // guards gDev against teardown
static dai::Device *gDev = nullptr;
static std::atomic<bool> gPollerStarted{false};
static IMP gOrigConnect, gOrigStop;
static Ivar gDeviceIvar;

static dai::Device *deviceOf(id deviceManager) {
    // _device is std::unique_ptr<dai::Device>: a single pointer
    return *reinterpret_cast<dai::Device **>(reinterpret_cast<uint8_t *>((__bridge void *)deviceManager) + ivar_getOffset(gDeviceIvar));
}

static void attachLogTap(dai::Device *dev) {
    try {
        dev->addLogCallback([](dai::LogMessage m) {
            static std::mutex mu;
            static std::map<std::string, double> last;  // throttle routine info lines per 12-char prefix
            bool important = m.level >= dai::LogLevel::WARN;
            if(!important) {
                std::lock_guard<std::mutex> lk(mu);
                std::string key = m.payload.substr(0, 12);
                double now = [NSDate timeIntervalSinceReferenceDate];
                if(now - last[key] < 30.0) return;
                last[key] = now;
            }
            NSString *payload = [[NSString alloc] initWithBytes:m.payload.data() length:m.payload.size() encoding:NSUTF8StringEncoding] ?: @"<non-utf8>";
            clog(@"[camera %s %s] %@", m.nodeIdName.c_str(), important ? "WARN+" : "info", payload);
        });
        clog(@"[tap] log callback attached");
    } catch(std::exception &e) {
        clog(@"[tap] addLogCallback failed: %s", e.what());
    }
}

static void startPoller(void) {
    if(gPollerStarted.exchange(true)) return;
    std::thread([] {
        while(true) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            std::lock_guard<std::mutex> lk(gDevMutex);
            if(!gDev) continue;
            try {
                if(gDev->isClosed()) continue;
                auto t = gDev->getChipTemperature();
                auto css = gDev->getLeonCssCpuUsage();
                auto mss = gDev->getLeonMssCpuUsage();
                clog(@"[telemetry] chip avg=%.1fC css=%.1f mss=%.1f upa=%.1f dss=%.1f | cpu css=%.0f%% mss=%.0f%%", t.average, t.css, t.mss,
                     t.upa, t.dss, css.average * 100, mss.average * 100);
            } catch(std::exception &e) {
                clog(@"[telemetry] rpc failed: %s", e.what());
            }
        }
    }).detach();
}

typedef BOOL (*ConnectFn)(id, SEL, const void *, BOOL, uint64_t, id);
static BOOL hook_connect(id self, SEL _cmd, const void *initialState, BOOL lowPower, uint64_t resolution, id tuningPath) {
    {
        std::lock_guard<std::mutex> lk(gDevMutex);
        gDev = nullptr;  // original may replace/destroy the previous device
    }
    BOOL ok = ((ConnectFn)gOrigConnect)(self, _cmd, initialState, lowPower, resolution, tuningPath);
    dai::Device *dev = deviceOf(self);
    clog(@"[tap] connect returned %d, device=%p lowPower=%d", ok, dev, lowPower);
    if(ok && dev) {
        attachLogTap(dev);
        std::lock_guard<std::mutex> lk(gDevMutex);
        gDev = dev;
    }
    startPoller();
    return ok;
}

typedef void (*StopFn)(id, SEL, BOOL);
static void hook_stop(id self, SEL _cmd, BOOL lowPower) {
    {
        std::lock_guard<std::mutex> lk(gDevMutex);
        gDev = nullptr;  // original closes + deletes the device
    }
    clog(@"[tap] stopWithLowPowerMode -> device teardown (camera will be reset by Opal)");
    ((StopFn)gOrigStop)(self, _cmd, lowPower);
}

// ---------------------------------------------------------------- install
static IMP swap(NSString *cls, NSString *sel, IMP repl) {
    Method m = class_getInstanceMethod(NSClassFromString(cls), NSSelectorFromString(sel));
    if(!m) { clog(@"[install] MISSING -[%@ %@]", cls, sel); return nullptr; }
    clog(@"[install] hooked -[%@ %@] types=%s", cls, sel, method_getTypeEncoding(m));
    return method_setImplementation(m, repl);
}

__attribute__((constructor)) static void opalcore_init(void) {
    gLogQ = dispatch_queue_create("opalcore.log", DISPATCH_QUEUE_SERIAL);
    gDQ = dispatch_queue_create("opalcore.debounce", DISPATCH_QUEUE_SERIAL);
    [NSFileManager.defaultManager createDirectoryAtPath:[@"~/Library/Logs/opal-watch" stringByExpandingTildeInPath]
                            withIntermediateDirectories:YES attributes:nil error:nil];
    NSUInteger sz = 0;
    NSGetSizeAndAlignment("{?=iiiiiiBBBBBdC}", &sz, NULL);
    gStructSize = sz;
    clog(@"---- opalcore loaded into %@ (pid %d), struct %lu bytes", NSProcessInfo.processInfo.processName, getpid(), (unsigned long)sz);
    opalcore_pipecap_init();
    if(getenv("OPALCORE_DISABLE")) { clog(@"[install] disabled by OPALCORE_DISABLE"); return; }
    gDeviceIvar = class_getInstanceVariable(NSClassFromString(@"DeviceManager"), "_device");
    if(!gDeviceIvar) { clog(@"[install] DeviceManager._device not found; tap disabled"); }
    gOrigChange = swap(@"OpalCameraInput", @"changeStreamingState:withState:", (IMP)hook_changeStreaming);
    if(gDeviceIvar) {
        gOrigConnect = swap(@"DeviceManager", @"connectWithInitialState:andLowPowerMode:andResolution:andImxCustomImageTuningPath:", (IMP)hook_connect);
        gOrigStop = swap(@"DeviceManager", @"stopWithLowPowerMode:", (IMP)hook_stop);
    }
}
