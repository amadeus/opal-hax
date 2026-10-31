// opalspy: injected into Opal.app (DYLD_INSERT_LIBRARIES; Opal allows dyld env vars + disables library
// validation). Records the developer-mode telemetry labels (chip temp, camera/mac fps, Myriad CPU/memory)
// to ~/Library/Logs/opal-watch/telemetry.log, throttled to one line per label per 5 s.
// Does nothing in any process other than Opal's main app.
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

void opal_debounce_install(void);
static IMP gOrigSetStringValue;
static NSMutableDictionary<NSString *, NSDate *> *gLast;
static NSFileHandle *gOut;
static dispatch_queue_t gQ;

static void record(NSString *s) {
    if(s.length == 0) return;
    NSString *key = nil;
    for(NSString *prefix in @[ @"temp:", @"DDR:", @"CMX:", @"CSS H:", @"MSS H:", @"CPU - CSS" ]) {
        if([s containsString:prefix]) { key = prefix; break; }
    }
    if(!key) return;
    dispatch_async(gQ, ^{
        NSDate *now = [NSDate date];
        NSDate *prev = gLast[key];
        if(prev && [now timeIntervalSinceDate:prev] < 5.0) return;
        gLast[key] = now;
        NSDateFormatter *f = [NSDateFormatter new];
        f.dateFormat = @"yyyy-MM-dd HH:mm:ss";
        NSString *flat = [[s componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] componentsJoinedByString:@" | "];
        NSString *line = [NSString stringWithFormat:@"%@ %@\n", [f stringFromDate:now], flat];
        [gOut writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
    });
}

static void spy_setStringValue(id self, SEL _cmd, NSString *value) {
    ((void (*)(id, SEL, NSString *))gOrigSetStringValue)(self, _cmd, value);
    record(value);
}

// ---- camera system-logger feed: -[<app class> newLogDataRecived:] (sent over XPC by the device service)
static NSMutableDictionary<NSString *, NSValue *> *gOrigLogImps;  // class name -> original IMP
static NSDate *gLastLogData;

// Type encoding v60@0:8{?={?=ff}{?=ff}{?=ff}{?=ff}fff}16 : 11 floats = 44 bytes. Composite > 16 bytes and not
// an HFA, so on arm64 it is passed indirectly: x2 holds a pointer to the struct. Forwarding x2 unchanged is exact.
// Field order matches the dev-mode panel: DDR used/total, CMX used/total, CSS heap used/total,
// MSS heap used/total, then CSS cpu, MSS cpu, chip temp (verified against the values below).
static void spy_newLogData(id self, SEL _cmd, const float *p) {
    NSValue *v = gOrigLogImps[NSStringFromClass(object_getClass(self))];
    struct { float v[11]; } sv;
    memcpy(sv.v, p, sizeof sv.v);
    if(v) ((void (*)(id, SEL, const float *))v.pointerValue)(self, _cmd, p);
    dispatch_async(gQ, ^{
        NSDate *now = [NSDate date];
        if(gLastLogData && [now timeIntervalSinceDate:gLastLogData] < 5.0) return;
        gLastLogData = now;
        NSDateFormatter *fmt = [NSDateFormatter new];
        fmt.dateFormat = @"yyyy-MM-dd HH:mm:ss";
        NSString *line = [NSString stringWithFormat:@"%@ SYSLOG raw=[%.2f %.2f | %.2f %.2f | %.2f %.2f | %.2f %.2f | %.3f %.3f %.2f]\n",
                          [fmt stringFromDate:now], sv.v[0], sv.v[1], sv.v[2], sv.v[3], sv.v[4], sv.v[5], sv.v[6], sv.v[7], sv.v[8], sv.v[9], sv.v[10]];
        [gOut writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
    });
}

static void hookLogFeed(void) {
    gOrigLogImps = [NSMutableDictionary new];
    SEL sel = NSSelectorFromString(@"newLogDataRecived:");
    const char *mainImage = class_getImageName(NSClassFromString(@"NSObject")) ? NULL : NULL;
    (void)mainImage;
    unsigned int n = 0;
    Class *classes = objc_copyClassList(&n);
    NSString *exePath = NSBundle.mainBundle.executablePath;
    for(unsigned int i = 0; i < n; i++) {
        Class c = classes[i];
        const char *img = class_getImageName(c);
        if(!img || ![[NSString stringWithUTF8String:img] isEqualToString:exePath]) continue;
        unsigned int mc = 0;
        Method *ms = class_copyMethodList(c, &mc);  // only methods defined directly on this class
        for(unsigned int j = 0; j < mc; j++) {
            if(method_getName(ms[j]) == sel) {
                gOrigLogImps[NSStringFromClass(c)] = [NSValue valueWithPointer:(void *)method_setImplementation(ms[j], (IMP)spy_newLogData)];
                NSString *msg = [NSString stringWithFormat:@"---- hooked -[%@ newLogDataRecived:] types=%s\n", NSStringFromClass(c), method_getTypeEncoding(ms[j])];
                [gOut writeData:[msg dataUsingEncoding:NSUTF8StringEncoding]];
            }
        }
        free(ms);
    }
    free(classes);
}

void opal_launchquiet_install(void);

void opalspy_log(NSString *msg) {
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("opalspy.log", DISPATCH_QUEUE_SERIAL); });
    NSDate *now = NSDate.date;
    dispatch_async(q, ^{
        NSDateFormatter *f = [NSDateFormatter new];
        f.dateFormat = @"yyyy-MM-dd HH:mm:ss.SSS";
        NSString *line = [NSString stringWithFormat:@"%@ %@\n", [f stringFromDate:now], msg];
        NSString *path = [@"~/Library/Logs/opal-watch/opalspy.log" stringByExpandingTildeInPath];
        NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:path];
        if(!h) { [line writeToFile:path atomically:NO encoding:NSUTF8StringEncoding error:nil]; return; }
        [h seekToEndOfFile];
        [h writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
        [h closeFile];
    });
}

__attribute__((constructor)) static void opalspy_init(void) {
    NSString *exe = NSProcessInfo.processInfo.arguments.firstObject.lastPathComponent;
    if(![exe isEqualToString:@"Opal"]) return;
    [NSFileManager.defaultManager createDirectoryAtPath:[@"~/Library/Logs/opal-watch" stringByExpandingTildeInPath]
                            withIntermediateDirectories:YES attributes:nil error:nil];
    opal_launchquiet_install();
    // In the custom build (bundle id com.amadeus.oc.*) the camera-driver service runs opalcore, which debounces
    // at the source; don't stack a second delay here.
    BOOL customBuild = [NSBundle.mainBundle.bundleIdentifier hasPrefix:@"com.amadeus.oc"];
    opalspy_log([NSString stringWithFormat:@"opalspy loaded into %@ pid %d (custom build: %d)", NSBundle.mainBundle.bundleIdentifier, getpid(), customBuild]);
    NSString *dir = [@"~/Library/Logs/opal-watch" stringByExpandingTildeInPath];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *path = [dir stringByAppendingPathComponent:@"telemetry.log"];
    if(![NSFileManager.defaultManager fileExistsAtPath:path]) [NSData.data writeToFile:path atomically:NO];
    gOut = [NSFileHandle fileHandleForWritingAtPath:path];
    [gOut seekToEndOfFile];
    gLast = [NSMutableDictionary new];
    gQ = dispatch_queue_create("opalspy", DISPATCH_QUEUE_SERIAL);
    Method m = class_getInstanceMethod(NSTextField.class, @selector(setStringValue:));
    gOrigSetStringValue = method_setImplementation(m, (IMP)spy_setStringValue);
    NSString *hello = [NSString stringWithFormat:@"---- opalspy loaded into %@ (pid %d)\n", exe, getpid()];
    [gOut writeData:[hello dataUsingEncoding:NSUTF8StringEncoding]];
    if(!getenv("OPALSPY_NO_DEBOUNCE") && !customBuild) opal_debounce_install();
    // Dump the app's settings (the app-group container is unreadable from outside; we're inside Opal).
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), gQ, ^{
        NSMutableString *out = [NSMutableString string];
        for(NSString *suite in @[ @"group.97Z3HJWCRT.com.opalcamera.Opal", @"97Z3HJWCRT.com.opalcamera.v2.Opal", @"UHD99KF9X7.com.amadeus.oc.OpalShared" ]) {
            NSDictionary *d = [[[NSUserDefaults alloc] initWithSuiteName:suite] dictionaryRepresentation];
            [out appendFormat:@"==== suite %@\n", suite];
            for(NSString *k in [d.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
                if([k hasPrefix:@"NS"] || [k hasPrefix:@"Apple"] || [k hasPrefix:@"com.apple"] || [k hasPrefix:@"AK"] || [k hasPrefix:@"PK"]) continue;
                NSString *v = [[d[k] description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
                if(v.length > 300) v = [[v substringToIndex:300] stringByAppendingString:@"…"];
                [out appendFormat:@"%@ = %@\n", k, v];
            }
        }
        NSString *path = [@"~/Library/Logs/opal-watch/opal-settings.txt" stringByExpandingTildeInPath];
        [out writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
    });
    // Swift classes are realized lazily; hook once the app has finished launching.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ hookLogFeed(); });
}
