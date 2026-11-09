// statushud: a small status readout over the bottom-left corner of the live preview in Opal's popover, so a click on
// the menu-bar icon shows how the camera is doing:  "42.6 °C · CPU 38/36 % · 30 fps 1080p · Mac 23 %"
//   °C, CPU   camera chip temperature and its two CPUs (CSS/MSS), from the telemetry Opal already receives
//             (-[DeviceXPCConnectionHelperService newLogDataRecived:], fed in by opalspy.m)
//   fps       frames Opal's app receives per second (-[… imageStreamResponseWithYUVSurface:…], counted here),
//             with the frame size
//   Mac       CPU of Opal's processes on the Mac (app, camera driver, video service, camera extension when readable),
//             in % of one core like Activity Monitor
// The tooltip adds camera memory, per-sensor detail, latency and whether a meeting app is using "Opal C1".
// Only runs while the popover is visible. Disable with env OPALSPY_NO_STATUS_HUD=1.
#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>
#import <libproc.h>
#import <mach/mach_time.h>
#import <objc/runtime.h>
#import <stdatomic.h>

void opalspy_log(NSString *msg);
int opal_virtualcam_in_use(void);

// ---- data in
static os_unfair_lock gLock = OS_UNFAIR_LOCK_INIT;
static float gTele[11];               // DDR used/total, CMX used/total, CSS heap used/total, MSS heap used/total, CSS %, MSS %, chip °C
static double gTeleAt;                // CACurrentMediaTime of the last telemetry
static _Atomic uint64_t gFrames;      // frames received
static _Atomic uint32_t gFrameW, gFrameH;
static _Atomic int64_t gVideoLatency;

void opal_statushud_telemetry(const float *v) {
    os_unfair_lock_lock(&gLock);
    memcpy(gTele, v, sizeof gTele);
    gTeleAt = CACurrentMediaTime();
    os_unfair_lock_unlock(&gLock);
}

typedef void (*FrameFn)(id, SEL, id, id, void *, void *, int64_t, int64_t);
static FrameFn gOrigFrame;

// v120@0:8 @16 @24 {?=Ifffff}32 {?=fiffBBBifIffff}56 q104 q112: both structs are > 16 bytes and not HFAs, so arm64
// passes them as pointers (x4, x5); forwarding the pointers unchanged is exact.
static void hud_frame(id self, SEL _cmd, id yuv, id depth, void *detection, void *info, int64_t videoLatency, int64_t audioLatency) {
    gOrigFrame(self, _cmd, yuv, depth, detection, info, videoLatency, audioLatency);
    atomic_fetch_add(&gFrames, 1);
    if(yuv) {
        atomic_store(&gFrameW, (uint32_t)IOSurfaceGetWidth((__bridge IOSurfaceRef)yuv));
        atomic_store(&gFrameH, (uint32_t)IOSurfaceGetHeight((__bridge IOSurfaceRef)yuv));
    }
    atomic_store(&gVideoLatency, videoLatency);
}

// ---- Mac CPU of Opal's processes
static double machToSec(uint64_t t) {
    static mach_timebase_info_data_t tb;
    if(!tb.denom) mach_timebase_info(&tb);
    return (double)t * tb.numer / tb.denom / 1e9;  // rusage times are mach ticks on Apple silicon
}

static double opalCpuSeconds(void) {
    int n = proc_listallpids(NULL, 0);
    if(n <= 0) return -1;
    pid_t *pids = calloc((size_t)n + 64, sizeof(pid_t));
    n = proc_listallpids(pids, (int)(((size_t)n + 64) * sizeof(pid_t)));
    double total = 0;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    for(int i = 0; i < n; i++) {
        if(proc_pidpath(pids[i], path, sizeof path) <= 0) continue;
        if(!strstr(path, "/Opal.app/") && !strstr(path, "com.amadeus.oc")) continue;
        struct rusage_info_v2 ri;
        if(proc_pid_rusage(pids[i], RUSAGE_INFO_V2, (rusage_info_t *)&ri) != 0) continue;  // e.g. other users' processes
        total += machToSec(ri.ri_user_time + ri.ri_system_time);
    }
    free(pids);
    return total;
}

// ---- view
@interface OpalStatusHUD : NSView
@property(copy, nonatomic) NSString *text;
@property(strong, nonatomic) NSColor *color;
- (NSSize)hudSize;
@end

// Draws its own text (an NSTextField adds an invisible 2 pt cell inset on the sides and reserves descender/leading
// space, which made the padding uneven): the caps sit centred vertically, with the same visual gap on every side.
@implementation OpalStatusHUD {
    NSFont *_font;
}
static const CGFloat kPadX = 8, kPadY = 6;
- (instancetype)initWithFrame:(NSRect)r {
    if((self = [super initWithFrame:r])) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = [NSColor colorWithWhite:0 alpha:0.45].CGColor;
        _font = [NSFont monospacedDigitSystemFontOfSize:10.5 weight:NSFontWeightMedium];
        _color = [NSColor colorWithWhite:1 alpha:0.92];
        _text = @"";
    }
    return self;
}
- (BOOL)isFlipped { return NO; }
- (NSDictionary *)attrs { return @{ NSFontAttributeName: _font, NSForegroundColorAttributeName: _color }; }
- (void)setText:(NSString *)t { if(![t isEqualToString:_text]) { _text = [t copy]; self.needsDisplay = YES; } }
- (void)setColor:(NSColor *)c { if(![c isEqual:_color]) { _color = c; self.needsDisplay = YES; } }
// Ink bounds (glyph outlines, not advances), so the first and last glyph sit exactly kPadX from the edges.
- (NSRect)ink { return [_text boundingRectWithSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX) options:NSStringDrawingUsesDeviceMetrics attributes:[self attrs] context:nil]; }
- (NSSize)hudSize { return NSMakeSize(ceil(NSWidth([self ink])) + 2 * kPadX, round(_font.capHeight + 2 * kPadY)); }
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; self.layer.cornerRadius = MIN(6, s.height / 2); }
- (void)drawRect:(NSRect)dirty {
    // Without NSStringDrawingUsesLineFragmentOrigin the rect's origin is the baseline origin.
    CGFloat scale = self.window.backingScaleFactor ?: 2;
    CGFloat baseline = round((self.bounds.size.height - _font.capHeight) / 2 * scale) / scale;
    [_text drawWithRect:NSMakeRect(kPadX - NSMinX([self ink]), baseline, self.bounds.size.width, _font.capHeight)
                options:0 attributes:[self attrs] context:nil];
}
@end

static OpalStatusHUD *gHUD;
static NSTimer *gTimer;
static uint64_t gLastFrames;
static double gLastTick, gLastCpu = -1, gFps, gMacCpu = -1;
static BOOL gLoggedTree;

// the view's own layer is a CAMetalLayer, or it carries one as an extra sublayer (Opal adds its "metal preview layer"
// to a view's layer); layers that belong to subviews don't count
static BOOL hasMetalLayer(NSView *v) {
    CALayer *l = v.layer;
    if(!l) return NO;
    if([l isKindOfClass:CAMetalLayer.class]) return YES;
    NSMutableSet *own = [NSMutableSet set];
    for(NSView *s in v.subviews) if(s.layer) [own addObject:s.layer];
    for(CALayer *s in l.sublayers) {
        if([own containsObject:s]) continue;
        if([s isKindOfClass:CAMetalLayer.class]) return YES;
        for(CALayer *ss in s.sublayers) if([ss isKindOfClass:CAMetalLayer.class]) return YES;
    }
    return NO;
}

// the live preview: the largest view drawing with a CAMetalLayer (Opal's "metal preview layer")
static NSView *findPreview(NSView *v, NSView *best) {
    if(v == gHUD) return best;
    if(hasMetalLayer(v) && v.frame.size.width * v.frame.size.height > best.frame.size.width * best.frame.size.height) best = v;
    for(NSView *s in v.subviews) best = findPreview(s, best);
    return best;
}

static void logTree(NSView *v, int depth, NSMutableString *out) {
    if(depth > 8) return;
    NSRect f = v.frame;
    [out appendFormat:@"%*s%@ (%.0f,%.0f %.0fx%.0f)%@\n", depth * 2, "", NSStringFromClass(v.class), f.origin.x, f.origin.y,
                      f.size.width, f.size.height, hasMetalLayer(v) ? @" [metal]" : @""];
    for(NSView *s in v.subviews) logTree(s, depth + 1, out);
}

// Opal's main popover (the visible popover window that holds the live preview; SwiftUI popovers inside it are
// separate popover windows without one)
static NSWindow *popoverWindow(void) {
    NSWindow *any = nil;
    for(NSWindow *w in NSApp.windows) {
        if(!w.isVisible || ![NSStringFromClass(w.class) containsString:@"Popover"]) continue;
        if(findPreview(w.contentView, nil)) return w;
        if(!any || w.frame.size.height > any.frame.size.height) any = w;
    }
    return any;
}

static NSString *fmtTemp(float c) { return c > 0 ? [NSString stringWithFormat:@"%.1f °C", c] : @"– °C"; }

static void hudTick(void) {
    NSWindow *w = popoverWindow();
    if(!w) {
        gLastCpu = -1;
        gLastTick = 0;
        return;
    }
    NSView *root = w.contentView;
    if(!gLoggedTree) {
        gLoggedTree = YES;
        NSMutableString *t = [NSMutableString stringWithString:@"statushud: popover view tree\n"];
        logTree(root, 0, t);
        opalspy_log(t);
    }
    NSView *preview = findPreview(root, nil);
    NSView *host = preview ? preview.superview : root;
    if(gHUD.superview != host) {
        [gHUD removeFromSuperview];
        [host addSubview:gHUD positioned:NSWindowAbove relativeTo:preview];
    }

    // rates
    double now = CACurrentMediaTime();
    uint64_t frames = atomic_load(&gFrames);
    double cpu = opalCpuSeconds();
    if(gLastTick > 0) {
        double dt = now - gLastTick;
        if(dt > 0.2) {
            gFps = (frames - gLastFrames) / dt;
            gMacCpu = (cpu >= 0 && gLastCpu >= 0) ? (cpu - gLastCpu) / dt * 100 : -1;
        }
    }
    gLastTick = now;
    gLastFrames = frames;
    gLastCpu = cpu;

    float t[11];
    double teleAt;
    os_unfair_lock_lock(&gLock);
    memcpy(t, gTele, sizeof t);
    teleAt = gTeleAt;
    os_unfair_lock_unlock(&gLock);
    BOOL teleFresh = teleAt > 0 && now - teleAt < 5;
    uint32_t fw = atomic_load(&gFrameW), fh = atomic_load(&gFrameH);
    BOOL streaming = gFps > 0.5;

    NSMutableArray *parts = [NSMutableArray array];
    [parts addObject:teleFresh ? fmtTemp(t[10]) : @"– °C"];
    [parts addObject:teleFresh ? [NSString stringWithFormat:@"CPU %.0f/%.0f %%", t[8], t[9]] : @"CPU –"];
    [parts addObject:streaming ? [NSString stringWithFormat:@"%.0f fps %up", gFps, fh] : @"no video"];
    if(gMacCpu >= 0) [parts addObject:[NSString stringWithFormat:@"Mac %.0f %%", gMacCpu]];
    NSString *text = [parts componentsJoinedByString:@"  ·  "];

    int inUse = opal_virtualcam_in_use();
    gHUD.toolTip = [NSString stringWithFormat:
        @"Camera chip %@ (65 °C is when Opal starts warning)\n"
         "Camera CPUs: CSS %.0f %%, MSS %.0f %%\n"
         "Camera memory: DDR %.0f / %.0f MB, CMX %.2f / %.2f MB\n"
         "Video: %.1f fps, %u×%u, latency %lld\n"
         "Opal on this Mac: %@ of one core\n"
         "Opal C1 in a meeting app: %@",
        teleFresh ? fmtTemp(t[10]) : @"unknown", t[8], t[9], t[0], t[1], t[2], t[3], gFps, fw, fh,
        (long long)atomic_load(&gVideoLatency), gMacCpu >= 0 ? [NSString stringWithFormat:@"%.0f %%", gMacCpu] : @"–",
        inUse == 1 ? @"yes" : inUse == 0 ? @"no" : @"unknown"];

    NSColor *c = [NSColor colorWithWhite:1 alpha:0.92];
    if(teleFresh && t[10] >= 75) c = NSColor.systemRedColor;
    else if(teleFresh && t[10] >= 65) c = NSColor.systemOrangeColor;
    gHUD.color = c;
    gHUD.text = text;

    NSSize hs = gHUD.hudSize;
    NSRect pf = preview ? preview.frame : host.bounds;
    CGFloat inset = 8;
    // bottom-left of the preview; without a preview found, top-left of the popover (over where the preview sits)
    BOOL bottom = preview != nil;
    CGFloat low = NSMinY(pf) + inset, high = NSMaxY(pf) - inset - hs.height;
    gHUD.frame = NSIntegralRect(NSMakeRect(NSMinX(pf) + inset, (bottom != host.isFlipped) ? low : high, hs.width, hs.height));
}

void opal_statushud_install(void) {
    if(getenv("OPALSPY_NO_STATUS_HUD")) return;
    Class c = NSClassFromString(@"_TtC4Opal32DeviceXPCConnectionHelperService");
    SEL sel = NSSelectorFromString(@"imageStreamResponseWithYUVSurface:andDepthSurface:andDetection:andDeviceInfo:andVideoLatency:andAudioLatency:");
    Method m = c ? class_getInstanceMethod(c, sel) : NULL;
    if(m && !strcmp(method_getTypeEncoding(m), "v120@0:8@16@24{?=Ifffff}32{?=fiffBBBifIffff}56q104q112"))
        gOrigFrame = (FrameFn)method_setImplementation(m, (IMP)hud_frame);
    else
        opalspy_log(@"statushud: frame callback not found or changed; fps will be missing");
    dispatch_async(dispatch_get_main_queue(), ^{
        gHUD = [[OpalStatusHUD alloc] initWithFrame:NSZeroRect];
        gTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) { hudTick(); }];
        [NSRunLoop.mainRunLoop addTimer:gTimer forMode:NSRunLoopCommonModes];
        [NSNotificationCenter.defaultCenter addObserverForName:NSPopoverDidShowNotification object:nil queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification *n) { hudTick(); }];
        opalspy_log(@"statushud: installed");
    });
}
