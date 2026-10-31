// opal-launch: replaces Opal's USB-plug agent. Run by launchd (local.opal-launch) at login and whenever an
// Opal C1 attaches (PID f63b = app-booted firmware, f63d = our flashed standalone firmware). Starts Opal with
// the opalspy recorder injected (DYLD_INSERT_LIBRARIES), unless Opal is already running. Drains the
// com.apple.iokit.matching event stream as launchd requires, then exits after a few idle seconds.
#import <AppKit/AppKit.h>
#import <xpc/xpc.h>

static NSString *const kOpal = @"/Applications/Opal.app";
static NSString *kDylib;

static void logLine(NSString *msg) {
    NSString *path = [@"~/Library/Logs/opal-watch/opal-launch.log" stringByExpandingTildeInPath];
    NSDateFormatter *f = [NSDateFormatter new];
    f.dateFormat = @"yyyy-MM-dd HH:mm:ss";
    NSString *line = [NSString stringWithFormat:@"%@ %@\n", [f stringFromDate:NSDate.date], msg];
    NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:path];
    if(!h) { [line writeToFile:path atomically:NO encoding:NSUTF8StringEncoding error:nil]; return; }
    [h seekToEndOfFile];
    [h writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
    [h closeFile];
}

static void ensureOpalRunning(NSString *why) {
    for(NSRunningApplication *a in NSWorkspace.sharedWorkspace.runningApplications) {
        if([a.bundleIdentifier isEqualToString:@"com.opalcamera.OpalCamera"] && !a.terminated) {
            logLine([NSString stringWithFormat:@"%@: Opal already running (pid %d)", why, a.processIdentifier]);
            return;
        }
    }
    NSWorkspaceOpenConfiguration *cfg = [NSWorkspaceOpenConfiguration configuration];
    cfg.activates = NO;
    if([NSFileManager.defaultManager fileExistsAtPath:kDylib]) cfg.environment = @{@"DYLD_INSERT_LIBRARIES" : kDylib};
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:kOpal]
                                        configuration:cfg
                                    completionHandler:^(NSRunningApplication *app, NSError *err) {
                                        logLine([NSString stringWithFormat:@"%@: launched Opal pid=%d recorder=%@ err=%@", why,
                                                                           app.processIdentifier, cfg.environment ? @"yes" : @"MISSING", err]);
                                        dispatch_semaphore_signal(done);
                                    }];
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC));
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSString *here = [[NSString stringWithUTF8String:argv[0]] stringByDeletingLastPathComponent];
        kDylib = [[here stringByAppendingPathComponent:@"../opalspy/opalspy.dylib"] stringByStandardizingPath];
        __block NSDate *lastEvent = NSDate.date;
        __block BOOL sawEvent = NO;
        dispatch_queue_t q = dispatch_queue_create("opal-launch", DISPATCH_QUEUE_SERIAL);
        xpc_set_event_stream_handler("com.apple.iokit.matching", q, ^(xpc_object_t event) {
            const char *name = xpc_dictionary_get_string(event, XPC_EVENT_KEY_NAME);
            lastEvent = NSDate.date;
            sawEvent = YES;
            ensureOpalRunning([NSString stringWithFormat:@"usb attach (%s)", name ?: "?"]);
        });
        // give launchd a moment to deliver a pending event; if none, this was a login (RunAtLoad) start
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), q, ^{
            if(!sawEvent) ensureOpalRunning(@"login");
        });
        while([NSDate.date timeIntervalSinceDate:lastEvent] < 10.0) {
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
        }
    }
    return 0;
}
