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
//  8. ... and Opal's complete off path (mic output off, LED, preview queue) after a reconnect too (idle 46 -> ~34 °C).
//  9. Deep sleep (opt-in experiment).  10. Mute guard: stream start/stop no longer un-mute the camera mic.
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <atomic>
#include <new>
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
// 9. deep sleep experiment (below)
static void scheduleDeepSleep(uint64_t gen, id input);
static void wakeIfAsleep(id input);
static void rememberInput(id input, SEL changeSel, NSData *camState);
static void deepSleepAfterConnect(void);
static void deepSleepAfterTeardown(void);

// ---------------------------------------------------------------- 1. streaming debounce
static const double kOffDelay = 5.0;
static const NSInteger kStateOff = 1;
static dispatch_queue_t gDQ;
static uint64_t gGen;
static IMP gOrigChange;
static size_t gStructSize = 48;

typedef void (*ChangeFn)(id, SEL, NSInteger, const void *);

// 8. Full "off" after a (re)connect. Opal's -[OpalCameraInput changeStreamingState:withState:] does nothing when its
// cached streamingState already equals the request, and after a connect the cache says "off" while the freshly booted
// camera runs. Its off path is not just STOP_STREAM: it also disables the microphone output (AudioInConfig
// disableOutput via AudioControlQueueManager setEnabled:NO), sets the LED and pauses the preview config queue. With
// only our explicit STOP_STREAM (7.) the mic pipeline kept streaming to the Mac: idle camera CSS 6 % and 44-46 °C all
// day (2026-11-11 23:24 -> 11-12 22:25), against CSS 3 % and ~34 °C after any stream start/stop cycle. So before an
// "off" is applied, a cached "off" is turned into "on" and Opal runs its whole off path. Disable: OPALCORE_NO_FULL_OFF.
static void forceFullOffPath(id target) {
    if(getenv("OPALCORE_NO_FULL_OFF")) return;
    SEL get = NSSelectorFromString(@"streamingState"), set = NSSelectorFromString(@"setStreamingState:");
    if(![target respondsToSelector:get] || ![target respondsToSelector:set]) {
        clog(@"[debounce] full off: streamingState accessors missing");
        return;
    }
    NSInteger cur = ((NSInteger(*)(id, SEL))objc_msgSend)(target, get);
    if(cur != kStateOff) return;
    ((void(*)(id, SEL, NSInteger))objc_msgSend)(target, set, 2);
    clog(@"[debounce] Opal's cached state was already off (fresh connect) -> running its full off path (mic, LED, sensor)");
}

static void tryApplyOff(uint64_t myGen, id target, SEL sel, NSData *st, BOOL rechecking) {
    if(gGen != myGen) { clog(@"[debounce] pending off superseded -> dropped (sensor stays on)"); return; }
    int inUse = opal_virtualcam_in_use();
    if(inUse == 1) {
        if(!rechecking) clog(@"[debounce] off due but an app is using Opal C1 -> sensor stays on, rechecking every 2s");
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), gDQ, ^{ tryApplyOff(myGen, target, sel, st, YES); });
        return;
    }
    clog(@"[debounce] applying off (in_use=%d)", inUse);
    forceFullOffPath(target);
    ((ChangeFn)gOrigChange)(target, sel, kStateOff, st.bytes);
    sendExplicitStop();
    scheduleDeepSleep(myGen, target);
}

static void hook_changeStreaming(id self, SEL _cmd, NSInteger state, const void *camState) {
    __block uint64_t myGen;
    dispatch_sync(gDQ, ^{ myGen = ++gGen; });
    NSData *copy = [NSData dataWithBytes:camState length:gStructSize];
    rememberInput(self, _cmd, copy);
    if(state != kStateOff) {
        clog(@"[debounce] state=%ld -> applied now", (long)state);
        wakeIfAsleep(self);
        ((ChangeFn)gOrigChange)(self, _cmd, state, camState);
        return;
    }
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
    if(ok) deepSleepAfterConnect();
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
    deepSleepAfterTeardown();
}

// ---------------------------------------------------------------- 9. deep sleep (experiment, 2026-11-12)
// With the sensor stopped the camera still holds Opal's whole pipeline (5 networks, scripts, mic) and idles ~35 °C.
// Deep sleep unloads it: some time after an "off" was applied (and no app uses Opal C1), opalcore calls Opal's own
// -[OpalCameraInput stop] (the path Opal takes when the screen is locked and the camera is lost): the camera thread
// exits, teardown closes the dai::Device and the camera reboots. mode=bootloader (default) then holds the camera in
// its bootloader (03e7:f63c) with a dai::DeviceBootloader connection, so nothing but the bootloader runs (no firmware,
// no pipeline, no sensor, no mic); mode=standalone lets the bootloader start the flashed webcam program (f63d) instead.
// Any "on" (popover opened, a meeting app opening Opal C1) wakes it: the bootloader connection is released and
// -[OpalCameraInput restart] (Opal's screen-unlock path) searches, boots Opal's firmware + pipeline and streams.
// No flash writes anywhere (DeviceBootloader is opened without allowFlashingBootloader and only held).
// OFF by default: tried as the default on 2026-11-12, the owner went back to light sleep (sensor + mic off, everything
// stays loaded, instant wake) because every wake is a full 5-8 s firmware + pipeline upload, like a fresh plug-in.
// Opt in with the file ~/Library/Logs/opal-watch/deepsleep; optional content: "after=<seconds>" (default 1),
// "mode=bootloader|standalone". Log lines "[sleep] ..." in camera.log, including how long each wake took and the chip
// temperature the camera came back with.
#include "depthai/device/DeviceBootloader.hpp"
#include "depthai/xlink/XLinkConnection.hpp"

static __unsafe_unretained id gInput;            // the OpalCameraInput (lives as long as the service)
static SEL gChangeSel;
static NSData *gLastCamState;
static std::atomic<int> gSleepState{0};          // 0 awake, 1 unloading/asleep, 2 waking
static std::atomic<bool> gHoldBootloader{false};
static std::mutex gBLMutex;
static dai::DeviceBootloader *gBL = nullptr;
static std::string gSleepMxId;
static double gSleepAt = 0, gWakeAt = 0;

static double nowSec(void) { return [NSDate timeIntervalSinceReferenceDate]; }

static void rememberInput(id input, SEL changeSel, NSData *camState) {
    gInput = input;
    gChangeSel = changeSel;
    dispatch_async(gDQ, ^{ gLastCamState = camState; });
}

// nil = disabled
static NSDictionary *deepSleepConfig(void) {
    NSString *path = [@"~/Library/Logs/opal-watch/deepsleep" stringByExpandingTildeInPath];
    if(![NSFileManager.defaultManager fileExistsAtPath:path]) return nil;
    NSMutableDictionary *cfg = [@{@"after": @"1", @"mode": @"bootloader"} mutableCopy];
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] ?: @"";
    for(NSString *tok in [text componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]) {
        NSArray *kv = [tok componentsSeparatedByString:@"="];
        if(kv.count == 2) cfg[kv[0]] = kv[1];
    }
    return cfg;
}

static BOOL cameraThreadRunning(id input) {
    id th = ((id(*)(id, SEL))objc_msgSend)(input, NSSelectorFromString(@"cameraThread"));
    return th && ((BOOL(*)(id, SEL))objc_msgSend)(th, NSSelectorFromString(@"isExecuting"));
}

static void scheduleDeepSleep(uint64_t gen, id input) {
    NSDictionary *cfg = deepSleepConfig();
    if(!cfg) return;
    double after = MAX(1.0, [cfg[@"after"] doubleValue]);
    BOOL hold = ![cfg[@"mode"] isEqualToString:@"standalone"];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(after * NSEC_PER_SEC)), gDQ, ^{
        if(gGen != gen || gSleepState != 0) return;          // something happened since the off
        if(opal_virtualcam_in_use() == 1) { clog(@"[sleep] not unloading: an app is using Opal C1"); return; }
        if(![input respondsToSelector:NSSelectorFromString(@"stop")]) { clog(@"[sleep] -[OpalCameraInput stop] missing"); return; }
        {
            std::lock_guard<std::mutex> lk(gDevMutex);
            if(!gDev) { clog(@"[sleep] not unloading: no camera connected"); return; }
            try { gSleepMxId = gDev->getDeviceInfo().getMxId(); } catch(...) { gSleepMxId.clear(); }
        }
        gHoldBootloader = hold;
        gSleepState = 1;
        gSleepAt = nowSec();
        clog(@"[sleep] idle %.0fs -> unloading the camera (Opal's stop path, mode=%s)", after, hold ? "bootloader" : "standalone");
        ((void(*)(id, SEL))objc_msgSend)(input, NSSelectorFromString(@"stop"));
    });
}

// on the camera thread, right after Opal closed the device during teardown
static void deepSleepAfterTeardown(void) {
    if(gSleepState != 1) return;
    clog(@"[sleep] pipeline unloaded, camera closed and rebooting");
    if(!gHoldBootloader) return;
    std::thread([] {
        auto t0 = nowSec();
        while(gSleepState == 1 && nowSec() - t0 < 15) {
            try {
                for(auto &d : dai::XLinkConnection::getAllConnectedDevices(X_LINK_ANY_STATE, false)) {
                    if(d.state == X_LINK_FLASH_BOOTED) {
                        clog(@"[sleep] camera already started its flashed program (f63d) -> staying in standalone mode");
                        return;
                    }
                    if(d.state != X_LINK_BOOTLOADER) continue;
                    if(!gSleepMxId.empty() && d.getMxId() != gSleepMxId) continue;
                    std::lock_guard<std::mutex> lk(gBLMutex);
                    if(gSleepState != 1) return;                 // woken meanwhile
                    // generous storage: Opal's private depthai may lay the class out larger than our headers do
                    gBL = new(operator new(16384)) dai::DeviceBootloader(d);  // no flashing allowed; only held open
                    clog(@"[sleep] camera held in its bootloader %.1fs after the unload (bootloader %s): nothing else runs",
                         nowSec() - gSleepAt, gBL->getVersion().toString().c_str());
                    return;
                }
            } catch(std::exception &e) {
                clog(@"[sleep] bootloader hold failed: %s", e.what());
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if(gSleepState == 1) clog(@"[sleep] camera did not show up in its bootloader within 15 s");
    }).detach();
}

static void wakeIfAsleep(id input) {
    int expected = 1;
    if(!gSleepState.compare_exchange_strong(expected, 2)) return;
    gWakeAt = nowSec();
    clog(@"[sleep] wake requested after %.1f min unloaded", (gWakeAt - gSleepAt) / 60.0);
    std::thread([input] {
        {
            std::lock_guard<std::mutex> lk(gBLMutex);
            if(gBL) {
                try { gBL->close(); } catch(...) {}
                gBL->~DeviceBootloader();
                operator delete(gBL);
                gBL = nullptr;
                clog(@"[sleep] bootloader released (%.1fs)", nowSec() - gWakeAt);
            }
        }
        for(int i = 0; i < 200 && cameraThreadRunning(input); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        clog(@"[sleep] restarting Opal's camera thread (%.1fs)", nowSec() - gWakeAt);
        ((void(*)(id, SEL))objc_msgSend)(input, NSSelectorFromString(@"restart"));  // = screen-unlock path
        ((void(*)(id, SEL))objc_msgSend)(input, NSSelectorFromString(@"start"));    // also starts the system monitor
    }).detach();
}

static void deepSleepAfterConnect(void) {
    if(gSleepState == 2) {
        double took = nowSec() - gWakeAt;
        gSleepState = 0;
        std::lock_guard<std::mutex> lk(gDevMutex);
        double chip = NAN;
        try { if(gDev) chip = gDev->getChipTemperature().average; } catch(...) {}
        clog(@"[sleep] awake: camera connected %.1fs after the wake request; chip %.1f °C on arrival", took, chip);
    }
    // A camera boots streaming. If Opal's cached state is "off" at connect (no one wants video), route an off through
    // the debounce so the full off path runs (and deep sleep is scheduled again).
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        id input = gInput;
        __block NSData *st;
        dispatch_sync(gDQ, ^{ st = gLastCamState; });
        if(!input || !st || !gChangeSel) return;
        NSInteger cur = ((NSInteger(*)(id, SEL))objc_msgSend)(input, NSSelectorFromString(@"streamingState"));
        if(cur != kStateOff) return;
        clog(@"[sleep] connected with nobody streaming -> off through the debounce");
        hook_changeStreaming(input, gChangeSel, kStateOff, st.bytes);
    });
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

// ---------------------------------------------------------------- 10. mute that stays muted (2026-11-12)
// Opal's Mute sends AudioInConfig gain x0 (-[AudioControlQueueManager mute]). But every stream start AND stop sends
// -[AudioControlQueueManager setEnabled:] = an AudioInConfig with Opal's default gain constant (+ disableOutput when
// off), which replaces the mute. disableOutput does NOT silence the camera's USB microphone (owner, 2026-11-12: with a
// first version that muted via setEnabled:NO, "the mic still seems to record audio"), so the gain is what silences it.
// While muted, every audio config Opal sends is replaced by one with gain x0 and output disabled (built with Opal's own
// dai::AudioInConfig and sent through Opal's queue manager). Unmuting passes through unchanged. Disable:
// env OPALCORE_NO_MUTE_GUARD.
extern "C" void *opal_AudioInConfig_ctor(void *self) __asm__("__ZN3dai13AudioInConfigC1Ev");
extern "C" void *opal_AudioInConfig_setMicGainTimes(void *self, float times) __asm__("__ZN3dai13AudioInConfig15setMicGainTimesEf");
extern "C" void *opal_AudioInConfig_setDisableOutput(void *self, bool disable) __asm__("__ZN3dai13AudioInConfig16setDisableOutputEb");

static std::atomic<bool> gMuted{false};
static IMP gOrigAudioMute, gOrigAudioSetEnabled, gOrigAudioSetGain, gOrigAudioDefaultGain;

static void sendMutedConfig(id mgr, NSString *why) {
    static void *cfg;  // one AudioInConfig for the life of the service (Opal's class; generous storage)
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    if(!cfg) { cfg = calloc(1, 1024); opal_AudioInConfig_ctor(cfg); }
    opal_AudioInConfig_setMicGainTimes(cfg, 0.0f);
    opal_AudioInConfig_setDisableOutput(cfg, true);
    @try {
        ((void(*)(id, SEL, void *))objc_msgSend)(mgr, NSSelectorFromString(@"sendAudioControl:"), cfg);
        clog(@"[mic] %@ -> muted config sent (gain x0, output off)", why);
    } @catch(NSException *e) {
        clog(@"[mic] %@ -> muted config failed (%@), falling back to Opal's mute", why, e.reason);
        ((void(*)(id, SEL))gOrigAudioMute)(mgr, NSSelectorFromString(@"mute"));
    }
}

static BOOL attached(id mgr) {
    SEL s = NSSelectorFromString(@"isAttached");
    return [mgr respondsToSelector:s] && ((BOOL(*)(id, SEL))objc_msgSend)(mgr, s);
}

static void hook_audioSetEnabled(id self, SEL _cmd, BOOL enabled) {
    if(gMuted && attached(self)) {
        sendMutedConfig(self, enabled ? @"stream start while muted" : @"stream stop while muted");
        return;
    }
    ((void(*)(id, SEL, BOOL))gOrigAudioSetEnabled)(self, _cmd, enabled);
}

static void hook_audioMute(id self, SEL _cmd) {
    gMuted = true;
    if(attached(self)) sendMutedConfig(self, @"muted");
    else ((void(*)(id, SEL))gOrigAudioMute)(self, _cmd);
}

static void hook_audioSetGain(id self, SEL _cmd, float db) {
    gMuted = false;
    clog(@"[mic] unmuted (gain %.0f dB)", db);
    ((void(*)(id, SEL, float))gOrigAudioSetGain)(self, _cmd, db);
}

static void hook_audioDefaultGain(id self, SEL _cmd) {
    gMuted = false;
    clog(@"[mic] unmuted (default gain)");
    ((void(*)(id, SEL))gOrigAudioDefaultGain)(self, _cmd);
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
    if(!getenv("OPALCORE_NO_MUTE_GUARD")) {
        gOrigAudioSetEnabled = swap(@"AudioControlQueueManager", @"setEnabled:", (IMP)hook_audioSetEnabled);
        if(gOrigAudioSetEnabled) {
            gOrigAudioMute = swap(@"AudioControlQueueManager", @"mute", (IMP)hook_audioMute);
            gOrigAudioSetGain = swap(@"AudioControlQueueManager", @"setGainTo:", (IMP)hook_audioSetGain);
            gOrigAudioDefaultGain = swap(@"AudioControlQueueManager", @"setDefaultGain", (IMP)hook_audioDefaultGain);
        }
    }
}
