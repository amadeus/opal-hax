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
//  5. Gesture choke guard: the hand-pose networks stay off while every gesture is off (see below).
//  6. fpsfix.mm: camera controls one command per message -> 30 fps instead of 26.4 at 1080p.
//  7. Explicit STOP_STREAM whenever an "off" is applied (Opal skips it after a reconnect; idle camera kept streaming).
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "depthai/device/Device.hpp"
#include "depthai/pipeline/datatype/CameraControl.hpp"

extern "C" int opal_virtualcam_in_use(void);  // cmio_inuse.m
void opalcore_pipecap_init(void);              // pipecap.mm

// ---------------------------------------------------------------- logging
static dispatch_queue_t gLogQ;
static void clog(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
void opalcore_log(NSString *s);  // for the other translation units
void opalcore_fpsfix_init(void);   // fpsfix.mm
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

void opalcore_log(NSString *s) { clog(@"%@", s); }

static void sendExplicitStop(void);  // 7. explicit stop (below)

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
    sendExplicitStop();
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

static void chokeAfterConnect(void);  // 5. gesture choke guard (below)

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
    if(ok) chokeAfterConnect();
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

// ---------------------------------------------------------------- 7. explicit stop
// Opal applies "off" only when it believes the sensor is on. After a (re)connect it believes "off" while a freshly
// booted camera streams by default, so an idle camera kept streaming for hours (2026-11-04 10:50-18:39 and
// 2026-11-06 03:22-10:32, ~58 °C). Whenever the debounce applies an "off", also send STOP_STREAM to the camera
// ourselves, through the control queue Opal created (a duplicate stop is harmless).
static void sendExplicitStop(void) {
    std::lock_guard<std::mutex> lk(gDevMutex);
    if(!gDev) return;
    try {
        if(gDev->isClosed()) return;
        auto q = gDev->getInputQueue("control");  // the existing queue only; never create one before Opal does
        dai::CameraControl stop;
        stop.setStopStreaming();
        q->send(stop);
        clog(@"[debounce] explicit STOP_STREAM sent to the camera");
    } catch(std::exception &e) {
        clog(@"[debounce] explicit stop not sent: %s", e.what());
    }
}

// ---------------------------------------------------------------- 5. gesture choke guard
// Opal un-chokes the on-camera hand-pose manager Script at every stream start ("CAMERA:: Setting choke on: 1") even
// when every gesture is off. Un-choked, that Script re-enables the palm-detection ImageManip by itself and the three
// hand-pose networks run at full rate for nothing: measured with tools/profile on 2026-11-05: LEON CSS 25 -> 45 %,
// MSS 6 -> 38 % on top of the plain stream (48 % / 65 % with face + segmentation on), the "high load" state of every
// long call recorded so far, and the face/segmentation networks drop from ~25 to ~15 inferences/s. Opal's own
// -[OpalCameraInput setGesturesNNActive:] (called with the gesture setting) chokes/un-chokes too, so the guard only
// has to refuse the un-choke that comes with stream start while gestures are inactive. Disable: OPALCORE_NO_CHOKE_GUARD.
static std::atomic<bool> gGesturesActive{false};
static IMP gOrigSetGestures, gOrigSetGesturesXpc, gOrigSetChoke;
typedef void (*SetBoolFn)(id, SEL, BOOL);

static id gesturesChoke(void) {
    Class cls = NSClassFromString(@"ChokeQueueManager");
    SEL sel = NSSelectorFromString(@"sharedInstanceGestures");
    if(!cls || ![cls respondsToSelector:sel]) return nil;
    return ((id(*)(id, SEL))objc_msgSend)(cls, sel);
}

static void hook_setGesturesNNActive(id self, SEL _cmd, BOOL active) {
    gGesturesActive = active;
    clog(@"[choke] -[OpalCameraInput setGesturesNNActive:%d]", active);
    ((SetBoolFn)gOrigSetGestures)(self, _cmd, active);
}

// the XPC facade has the same selector (it may hold the logic and not forward): track it too
static void hook_setGesturesNNActiveXpc(id self, SEL _cmd, BOOL active) {
    gGesturesActive = active;
    clog(@"[choke] -[OpalCameraDeviceService setGesturesNNActive:%d]", active);
    ((SetBoolFn)gOrigSetGesturesXpc)(self, _cmd, active);
}

// A pipeline restart (Opal rebuilds it on every resolution change) brings the manager Script back in its default
// UN-choked state, and Opal does not send the gesture state again (its cache still says "choke on" from the stream
// start we refused, so it doesn't even call setChokeToActive:) -> the hand-pose networks ran after every switch to 4K
// (seen 2026-11-05 23:07: CSS 55 % / MSS 72 % right after the reconnect). So after every connect, choke it ourselves.
static void chokeAfterConnect(void) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), gDQ, ^{
        if(gGesturesActive || !gOrigSetChoke || getenv("OPALCORE_NO_CHOKE_GUARD")) return;
        id choke = gesturesChoke();
        if(!choke) { clog(@"[choke] after connect: no gestures choke manager"); return; }
        try {
            @try {
                ((SetBoolFn)gOrigSetChoke)(choke, NSSelectorFromString(@"setChokeToActive:"), NO);
                clog(@"[choke] after connect: hand-pose manager choked (new pipeline starts un-choked)");
            } @catch(NSException *e) {
                clog(@"[choke] after connect: failed: %@", e.reason);
            }
        } catch(std::exception &e) {
            clog(@"[choke] after connect: failed: %s", e.what());
        }
    });
}

static void hook_setChokeToActive(id self, SEL _cmd, BOOL active) {
    if(active && !gGesturesActive && self == gesturesChoke() && !getenv("OPALCORE_NO_CHOKE_GUARD")) {
        clog(@"[choke] gestures are off -> hand-pose manager stays choked (Opal asked to un-choke it)");
        active = NO;
    }
    ((SetBoolFn)gOrigSetChoke)(self, _cmd, active);
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
    opalcore_fpsfix_init();
    gDeviceIvar = class_getInstanceVariable(NSClassFromString(@"DeviceManager"), "_device");
    if(!gDeviceIvar) { clog(@"[install] DeviceManager._device not found; tap disabled"); }
    gOrigChange = swap(@"OpalCameraInput", @"changeStreamingState:withState:", (IMP)hook_changeStreaming);
    gOrigSetGestures = swap(@"OpalCameraInput", @"setGesturesNNActive:", (IMP)hook_setGesturesNNActive);
    gOrigSetGesturesXpc = swap(@"OpalCameraDeviceService", @"setGesturesNNActive:", (IMP)hook_setGesturesNNActiveXpc);
    gOrigSetChoke = swap(@"ChokeQueueManager", @"setChokeToActive:", (IMP)hook_setChokeToActive);
    if(!gOrigSetGestures || !gOrigSetChoke) clog(@"[install] choke guard NOT installed");
    if(gDeviceIvar) {
        gOrigConnect = swap(@"DeviceManager", @"connectWithInitialState:andLowPowerMode:andResolution:andImxCustomImageTuningPath:", (IMP)hook_connect);
        gOrigStop = swap(@"DeviceManager", @"stopWithLowPowerMode:", (IMP)hook_stop);
    }
}
