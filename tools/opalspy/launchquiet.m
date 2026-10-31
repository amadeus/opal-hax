// launchquiet: Opal opens its menu-bar popover by itself on launch, and an open popover keeps the camera
// streaming (StreamingState isPopoverOpen=true) until someone closes it. This suppresses popover shows that
// happen during the first kWindow seconds after launch WITHOUT user input behind them (no mouse/key event),
// so Opal starts as just a menu-bar icon. Clicking the icon works as normal; later programmatic shows are
// untouched. Log: ~/Library/Logs/opal-watch/opalspy.log. Disable with env OPALSPY_SHOW_POPOVER_ON_LAUNCH=1.
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

static const double kWindow = 20.0;
static NSDate *gLaunch;
static IMP gOrigShow;

void opalspy_log(NSString *msg);

static BOOL userInitiated(void) {
    NSEvent *e = NSApp.currentEvent;
    if(!e) return NO;
    switch(e.type) {
        case NSEventTypeLeftMouseDown: case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseDown: case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseDown: case NSEventTypeOtherMouseUp:
        case NSEventTypeKeyDown: case NSEventTypeKeyUp:
            return YES;
        default:
            return NO;
    }
}

static void quiet_show(id self, SEL _cmd, NSRect rect, NSView *view, NSRectEdge edge) {
    double since = -[gLaunch timeIntervalSinceNow];
    if(since < kWindow && !userInitiated()) {
        opalspy_log([NSString stringWithFormat:@"launchquiet: suppressed automatic popover show %.1fs after launch", since]);
        return;
    }
    ((void (*)(id, SEL, NSRect, NSView *, NSRectEdge))gOrigShow)(self, _cmd, rect, view, edge);
}

void opal_launchquiet_install(void) {
    if(getenv("OPALSPY_SHOW_POPOVER_ON_LAUNCH")) return;
    gLaunch = [NSDate date];
    Method m = class_getInstanceMethod(NSPopover.class, @selector(showRelativeToRect:ofView:preferredEdge:));
    gOrigShow = method_setImplementation(m, (IMP)quiet_show);
    opalspy_log(@"launchquiet: installed");
}
