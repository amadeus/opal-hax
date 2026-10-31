// Streaming debounce for Opal (part of opalspy.dylib).
// Problem: when the popover opens/closes (or apps toggle the camera), Opal sends
//   -[OpalCameraDeviceServiceProtocol changeStreamingState:(NSInteger)state withState:(struct)]
// to the device service, which immediately sends CameraControl stop/start to the sensor. We observed 13 flips
// in 34 s (two 18 ms apart), and Opal's firmware has an intermittent MSS crash ~2 s after a stream start.
// Fix: hold "off" (state == 1, per -[ControlQueueManager setStreamingState:] = state != 1) for kOffDelay; if any
// other state arrives meanwhile, cancel the pending "off". Everything else is forwarded untouched and
// immediately; nothing is ever dropped except an "off" superseded by a newer call.
// Two dispatch paths are covered: NSXPC's generic -[_NSXPCDistantObject forwardInvocation:], and a direct IMP
// that Foundation may add to __NSXPCInterfaceProxy_OpalCameraDeviceServiceProtocol on first use.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

static const double kOffDelay = 5.0;
static const NSInteger kStateOff = 1;
static NSString *const kProxyClass = @"__NSXPCInterfaceProxy_OpalCameraDeviceServiceProtocol";

int opal_virtualcam_in_use(void);
static void tryForwardOff(uint64_t myGen, const char *path, void (^send)(void), BOOL rechecking);
static dispatch_queue_t gDQ;           // serializes debounce state
static uint64_t gGeneration;           // bumps on every call; a pending "off" fires only if still current
static IMP gOrigFwd, gOrigDirect;
static size_t gStructSize;

static void dlog(NSString *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSDateFormatter *f = [NSDateFormatter new];
    f.dateFormat = @"yyyy-MM-dd HH:mm:ss.SSS";
    NSString *line = [NSString stringWithFormat:@"%@ %@\n", [f stringFromDate:NSDate.date], s];
    NSString *path = [@"~/Library/Logs/opal-watch/debounce.log" stringByExpandingTildeInPath];
    NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:path];
    if(!h) { [line writeToFile:path atomically:NO encoding:NSUTF8StringEncoding error:nil]; return; }
    [h seekToEndOfFile];
    [h writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
    [h closeFile];
}

// Common policy. `send` performs the real call; it is invoked either now (synchronously) or after the delay.
static void debounce(NSInteger state, const char *path, void (^send)(void)) {
    __block uint64_t myGen;
    dispatch_sync(gDQ, ^{ myGen = ++gGeneration; });
    if(state != kStateOff) {
        dlog(@"[%s] state=%ld -> forward now (cancels any pending off)", path, (long)state);
        send();
        // diagnostic: confirm the in-use check works from inside Opal (expect 1 while a meeting app streams)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), gDQ, ^{
            dlog(@"[%s] in_use check 3s after on: %d (1=app using Opal C1, 0=not, -1=lookup failed)", path, opal_virtualcam_in_use());
        });
        return;
    }
    dlog(@"[%s] state=%ld (off) -> deferred %.0fs", path, (long)state, kOffDelay);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kOffDelay * NSEC_PER_SEC)), gDQ, ^{
        tryForwardOff(myGen, path, send, NO);
    });
}

// Runs on gDQ. Forward a pending "off" only if it is still the latest call AND macOS says no process is using
// the "Opal C1" virtual camera. Opal's own detection (polling the extension) misses quick reconnects: an app
// that reopens within ~1 s never triggers startStream, so Opal would stop the sensor under a live call.
static void tryForwardOff(uint64_t myGen, const char *path, void (^send)(void), BOOL rechecking) {
    if(gGeneration != myGen) {
        dlog(@"[%s] pending off superseded -> dropped (sensor stays on)", path);
        return;
    }
    if(opal_virtualcam_in_use() == 1) {
        if(!rechecking) dlog(@"[%s] off due, but an app is still using the Opal C1 camera -> keeping sensor on, rechecking every 2s", path);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), gDQ, ^{ tryForwardOff(myGen, path, send, YES); });
        return;
    }
    dlog(@"[%s] off still current and camera unused (in_use=%d) -> forwarding", path, opal_virtualcam_in_use());
    dispatch_async(dispatch_get_main_queue(), send);
}

// ---- path 1: generic NSXPC forwarding
static void db_forwardInvocation(id self, SEL _cmd, NSInvocation *inv) {
    if(inv.selector == NSSelectorFromString(@"changeStreamingState:withState:") &&
       [NSStringFromClass(object_getClass(self)) isEqualToString:kProxyClass]) {
        NSInteger state = 0;
        [inv getArgument:&state atIndex:2];
        [inv retainArguments];
        id target = self;
        debounce(state, "fwd", ^{ ((void (*)(id, SEL, NSInvocation *))gOrigFwd)(target, _cmd, inv); });
        return;
    }
    ((void (*)(id, SEL, NSInvocation *))gOrigFwd)(self, _cmd, inv);
}

// ---- path 1b: modern CoreFoundation fast path, -[_NSXPCDistantObject _forwardStackInvocation:] (stack-allocated
// NSInvocation, bypasses forwardInvocation:). For a deferred send we copy it into a heap NSInvocation and later
// hand that to the original -forwardInvocation:, the regular (supported) NSXPC send path.
static IMP gOrigStackFwd;
static void db_forwardStackInvocation(id self, SEL _cmd, NSInvocation *inv) {
    if(inv.selector == NSSelectorFromString(@"changeStreamingState:withState:") &&
       [NSStringFromClass(object_getClass(self)) isEqualToString:kProxyClass]) {
        NSInteger state = 0;
        [inv getArgument:&state atIndex:2];
        NSMethodSignature *sig = inv.methodSignature;
        NSInvocation *heap = [NSInvocation invocationWithMethodSignature:sig];
        heap.target = self;
        heap.selector = inv.selector;
        for(NSUInteger i = 2; i < sig.numberOfArguments; i++) {
            NSUInteger sz = 0;
            NSGetSizeAndAlignment([sig getArgumentTypeAtIndex:i], &sz, NULL);
            NSMutableData *buf = [NSMutableData dataWithLength:sz];
            [inv getArgument:buf.mutableBytes atIndex:i];
            [heap setArgument:buf.mutableBytes atIndex:i];
        }
        [heap retainArguments];
        id target = self;
        if(state != kStateOff) {
            // forward on the original fast path, synchronously, exactly as the app intended
            debounce(state, "stk", ^{ ((void (*)(id, SEL, NSInvocation *))gOrigStackFwd)(target, _cmd, inv); });
        } else {
            debounce(state, "stk", ^{ ((void (*)(id, SEL, NSInvocation *))gOrigFwd)(target, @selector(forwardInvocation:), heap); });
        }
        return;
    }
    ((void (*)(id, SEL, NSInvocation *))gOrigStackFwd)(self, _cmd, inv);
}

// ---- path 2: direct IMP on the proxy class. Struct (> 16 bytes, not an HFA) is passed indirectly in x3.
static void db_direct(id self, SEL _cmd, NSInteger state, const void *camState) {
    NSMutableData *copy = [NSMutableData dataWithBytes:camState length:gStructSize];
    id target = self;
    debounce(state, "imp", ^{ ((void (*)(id, SEL, NSInteger, const void *))gOrigDirect)(target, _cmd, state, copy.bytes); });
}

static void pollForDirectImp(void) {
    Class c = NSClassFromString(kProxyClass);
    SEL sel = NSSelectorFromString(@"changeStreamingState:withState:");
    if(c && !gOrigDirect) {
        unsigned int mc = 0;
        Method *ms = class_copyMethodList(c, &mc);
        for(unsigned int i = 0; i < mc; i++) {
            if(method_getName(ms[i]) == sel && method_getImplementation(ms[i]) != (IMP)db_direct) {
                gOrigDirect = method_setImplementation(ms[i], (IMP)db_direct);
                dlog(@"hooked direct IMP on %@ (types %s)", kProxyClass, method_getTypeEncoding(ms[i]));
            }
        }
        free(ms);
    }
    if(!gOrigDirect)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{ pollForDirectImp(); });
}

void opal_debounce_install(void) {
    gDQ = dispatch_queue_create("opal-debounce", DISPATCH_QUEUE_SERIAL);
    NSUInteger size = 0, align = 0;
    NSGetSizeAndAlignment("{?=iiiiiiBBBBBdC}", &size, &align);
    gStructSize = size;
    Method m = class_getInstanceMethod(NSClassFromString(@"_NSXPCDistantObject"), @selector(forwardInvocation:));
    gOrigFwd = method_setImplementation(m, (IMP)db_forwardInvocation);
    Method ms = class_getInstanceMethod(NSClassFromString(@"_NSXPCDistantObject"), NSSelectorFromString(@"_forwardStackInvocation:"));
    if(ms) gOrigStackFwd = method_setImplementation(ms, (IMP)db_forwardStackInvocation);
    dlog(@"---- debounce installed (off delay %.0fs, struct %lu bytes)", kOffDelay, (unsigned long)gStructSize);
    pollForDirectImp();
}
