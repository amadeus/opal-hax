// opalprobe: one-off exploration of how Opal sends changeStreamingState:withState: over NSXPC.
// Logs to ~/Library/Logs/opal-watch/probe.log. Read-only (no behavior changes).
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import <dlfcn.h>

static void plog(NSString *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSString *path = [@"~/Library/Logs/opal-watch/probe.log" stringByExpandingTildeInPath];
    NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:path];
    if(!h) { [NSData.data writeToFile:path atomically:NO]; h = [NSFileHandle fileHandleForWritingAtPath:path]; }
    [h seekToEndOfFile];
    [h writeData:[[s stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding]];
    [h closeFile];
}

static void explore(void) {
    SEL sel = NSSelectorFromString(@"changeStreamingState:withState:");
    unsigned int pc = 0;
    Protocol *__unsafe_unretained *protos = objc_copyProtocolList(&pc);
    for(unsigned int i = 0; i < pc; i++) {
        for(int req = 0; req < 2; req++) {
            struct objc_method_description d = protocol_getMethodDescription(protos[i], sel, req, YES);
            if(d.name) plog(@"protocol %s declares %s required=%d types=%s", protocol_getName(protos[i]), sel_getName(sel), req, d.types);
        }
    }
    free(protos);
    unsigned int n = 0;
    Class *classes = objc_copyClassList(&n);
    for(unsigned int i = 0; i < n; i++) {
        const char *name = class_getName(classes[i]);
        if(strstr(name, "XPC") && (strstr(name, "Proxy") || strstr(name, "Distant"))) {
            unsigned int mc = 0;
            Method *ms = class_copyMethodList(classes[i], &mc);
            BOOL hasSel = NO, hasFwd = NO;
            for(unsigned int j = 0; j < mc; j++) {
                if(method_getName(ms[j]) == sel) hasSel = YES;
                if(method_getName(ms[j]) == @selector(forwardInvocation:)) hasFwd = YES;
            }
            free(ms);
            if(strstr(name, "Opal")) {
                plog(@"class %s methods=%u implements-sel=%d", name, mc, hasSel);
                Method *ms2 = class_copyMethodList(classes[i], &mc);
                for(unsigned int j = 0; j < mc; j++) {
                    Dl_info di; dladdr((void *)method_getImplementation(ms2[j]), &di);
                    plog(@"   %s  imp=%s", sel_getName(method_getName(ms2[j])), di.dli_sname ?: "?");
                }
                free(ms2);
            }
        }
    }
    free(classes);
}

static NSMutableDictionary<NSString *, NSValue *> *gOrigFwds;
static void probe_forwardInvocation(id self, SEL _cmd, NSInvocation *inv) {
    // find the original IMP for the nearest hooked class in self's hierarchy
    IMP orig = NULL;
    for(Class c = object_getClass(self); c && !orig; c = class_getSuperclass(c)) {
        NSValue *v = gOrigFwds[NSStringFromClass(c)];
        if(v) orig = (IMP)v.pointerValue;
    }
    const char *cls = object_getClassName(self);
    if(strstr(cls, "Opal") || inv.selector == NSSelectorFromString(@"changeStreamingState:withState:")) {
        if(inv.selector == NSSelectorFromString(@"changeStreamingState:withState:")) {
            NSInteger state = 0;
            [inv getArgument:&state atIndex:2];
            plog(@"%@ XPC[%s] -> changeStreamingState state=%ld", NSDate.date, cls, (long)state);
        } else {
            plog(@"%@ XPC[%s] -> %@", NSDate.date, cls, NSStringFromSelector(inv.selector));
        }
    }
    if(orig) ((void (*)(id, SEL, NSInvocation *))orig)(self, _cmd, inv);
}

static void hookAllDistantObjects(void) {
    gOrigFwds = [NSMutableDictionary new];
    unsigned int n = 0;
    Class *classes = objc_copyClassList(&n);
    for(unsigned int i = 0; i < n; i++) {
        const char *name = class_getName(classes[i]);
        if(strncmp(name, "_NSXPCDistantObject", 19) != 0) continue;
        unsigned int mc = 0;
        Method *ms = class_copyMethodList(classes[i], &mc);
        for(unsigned int j = 0; j < mc; j++) {
            if(method_getName(ms[j]) == @selector(forwardInvocation:)) {
                gOrigFwds[[NSString stringWithUTF8String:name]] = [NSValue valueWithPointer:(void *)method_setImplementation(ms[j], (IMP)probe_forwardInvocation)];
                plog(@"hooked -[%s forwardInvocation:]", name);
            }
        }
        free(ms);
    }
    free(classes);
}


static NSMutableDictionary<NSString *, NSValue *> *gOrigCSS;
static void probe_changeStreaming(id self, SEL _cmd, NSInteger state, const void *cameraState) {
    plog(@"%@ LOCAL -[%s changeStreamingState:%ld withState:]", NSDate.date, object_getClassName(self), (long)state);
    IMP orig = NULL;
    for(Class c = object_getClass(self); c && !orig; c = class_getSuperclass(c)) {
        NSValue *v = gOrigCSS[NSStringFromClass(c)];
        if(v) orig = (IMP)v.pointerValue;
    }
    if(orig) ((void (*)(id, SEL, NSInteger, const void *))orig)(self, _cmd, state, cameraState);
}
static void hookImplementors(void) {
    gOrigCSS = [NSMutableDictionary new];
    SEL sel = NSSelectorFromString(@"changeStreamingState:withState:");
    unsigned int n = 0;
    Class *classes = objc_copyClassList(&n);
    for(unsigned int i = 0; i < n; i++) {
        unsigned int mc = 0;
        Method *ms = class_copyMethodList(classes[i], &mc);
        for(unsigned int j = 0; j < mc; j++) {
            if(method_getName(ms[j]) == sel) {
                Dl_info di; dladdr((void *)method_getImplementation(ms[j]), &di);
                gOrigCSS[NSStringFromClass(classes[i])] = [NSValue valueWithPointer:(void *)method_setImplementation(ms[j], (IMP)probe_changeStreaming)];
                plog(@"implementor: %s types=%s imp-image=%s", class_getName(classes[i]), method_getTypeEncoding(ms[j]), di.dli_fname);
            }
        }
        free(ms);
    }
    free(classes);
}

__attribute__((constructor)) static void opalprobe_init(void) {
    if(![NSProcessInfo.processInfo.arguments.firstObject.lastPathComponent isEqualToString:@"Opal"]) return;
    plog(@"---- opalprobe loaded pid %d", getpid());
    hookAllDistantObjects();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ hookImplementors(); });
    (void)explore;
}
