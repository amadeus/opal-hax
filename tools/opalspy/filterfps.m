// filterfps: the Camera tab's Looks thumbnails play live (like the Windows app's strip) instead of once a second.
// Opal renders each look's thumbnail in OpalCameraVideoService and sends it to the app
// (-[VideoXPCConnectionHelperService receiveWithSurface:forName:]) at a rate the app sets per look with
// -[OpalCameraEffectsFiltersServiceProtocol adjustFPSOfFilterWithID:andFPS:]: 30 while the pointer is over that
// thumbnail, 1 otherwise. build_custom_opal.py patches the "pointer left" 1 to 30; this asks for 30 for every look as its
// thumbnails start arriving and whenever one arrives more than 0.5 s after the previous (at most every 2 s per look).
// Log: "filterfps:" lines in opalspy.log, the measured rates every 10 s while thumbnails arrive.
// Disable: env OPALSPY_NO_FILTER_FPS=1.
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>

void opalspy_log(NSString *msg);

static const NSInteger kFps = 30;
static NSXPCConnection *__weak gConn;  // the app's connection to the video service's effects/filters interface
static dispatch_queue_t gQ;
static NSMutableDictionary<NSString *, NSNumber *> *gLast, *gAsked, *gCount;
static double gWindowStart;

static double now(void) { return [NSProcessInfo processInfo].systemUptime; }

static void (*gOrigSetIface)(id, SEL, NSXPCInterface *);
static void ff_setIface(NSXPCConnection *self, SEL _cmd, NSXPCInterface *iface) {
    gOrigSetIface(self, _cmd, iface);
    // OpalCameraVideoServiceProtocol, which includes OpalCameraEffectsFiltersServiceProtocol (adjustFPS...)
    Protocol *p = iface.protocol;
    if(p && protocol_getMethodDescription(p, sel_registerName("adjustFPSOfFilterWithID:andFPS:"), YES, YES).name) {
        gConn = self;
        opalspy_log([NSString stringWithFormat:@"filterfps: found the video service connection (%@)", NSStringFromProtocol(p)]);
    }
}

static void askFps(NSString *name) {
    NSXPCConnection *c = gConn;
    if(!c) return;
    id proxy = [c remoteObjectProxyWithErrorHandler:^(NSError *e) {
        opalspy_log([NSString stringWithFormat:@"filterfps: adjustFPS failed: %@", e.localizedDescription]);
    }];
    ((void (*)(id, SEL, NSString *, NSInteger))objc_msgSend)(proxy, sel_registerName("adjustFPSOfFilterWithID:andFPS:"), name, kFps);
}

static void (*gOrigRecv)(id, SEL, id, NSString *);
static void ff_recv(id self, SEL _cmd, id surface, NSString *name) {
    gOrigRecv(self, _cmd, surface, name);
    if(![name isKindOfClass:NSString.class]) return;
    NSString *n = [name copy];
    double t = now();
    dispatch_async(gQ, ^{
        double last = gLast[n].doubleValue, asked = gAsked[n].doubleValue;
        gLast[n] = @(t);
        gCount[n] = @(gCount[n].integerValue + 1);
        if((!last || t - last > 0.5) && (!asked || t - asked > 2.0)) {
            gAsked[n] = @(t);
            askFps(n);
        }
        if(!gWindowStart || t - gWindowStart > 30) gWindowStart = t, [gCount removeAllObjects];  // after a pause: new window
        if(t - gWindowStart >= 10) {
            double secs = t - gWindowStart, lo = 1e9, sum = 0;
            for(NSString *k in gCount) {
                double f = gCount[k].doubleValue / secs;
                lo = MIN(lo, f), sum += f;
            }
            opalspy_log([NSString stringWithFormat:@"filterfps: %lu looks, %.1f fps on average, slowest %.1f", (unsigned long)gCount.count,
                                                   sum / MAX(1, gCount.count), lo]);
            gWindowStart = t;
            [gCount removeAllObjects];
        }
    });
}

void opal_filterfps_install(void) {
    if(getenv("OPALSPY_NO_FILTER_FPS")) return;
    gQ = dispatch_queue_create("opalspy.filterfps", DISPATCH_QUEUE_SERIAL);
    gLast = [NSMutableDictionary new], gAsked = [NSMutableDictionary new], gCount = [NSMutableDictionary new];
    Method si = class_getInstanceMethod(NSXPCConnection.class, @selector(setRemoteObjectInterface:));
    gOrigSetIface = (void (*)(id, SEL, NSXPCInterface *))method_setImplementation(si, (IMP)ff_setIface);
    Class c = objc_getClass("_TtC4Opal31VideoXPCConnectionHelperService");
    Method r = c ? class_getInstanceMethod(c, sel_registerName("receiveWithSurface:forName:")) : NULL;
    if(!r) {
        opalspy_log(@"filterfps: VideoXPCConnectionHelperService receiveWithSurface:forName: not found");
        return;
    }
    gOrigRecv = (void (*)(id, SEL, id, NSString *))method_setImplementation(r, (IMP)ff_recv);
    opalspy_log(@"filterfps: installed");
}
