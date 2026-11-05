// launchsetup: at launch Opal calls [appDelegate togglePopover:nil]. Besides showing the popover, that call is what
// sets Opal up: it loads the popover's camera view controller (connects the camera; until then the menu-bar icon
// stays faded) before NSPopover -showRelativeToRect:… and NSApp -activateIgnoringOtherApps:. The owner wants the
// popover only when the icon is clicked, so a togglePopover: WITHOUT a sender (launch; the icon's button always passes
// itself) runs Opal's code with those two display calls skipped: everything is set up, nothing appears, focus stays
// where it was. Clicks are untouched. Opal's other automatic shows (camera connect) are removed in the binary
// (build_custom_opal.py CODE_PATCHES).
// Log: ~/Library/Logs/opal-watch/opalspy.log.
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

void opalspy_log(NSString *msg);

static BOOL gSetupOnly;
static IMP gOrigToggle, gOrigShow, gOrigActivate;

static void ls_toggle(id self, SEL _cmd, id sender) {
    if(sender) {
        ((void (*)(id, SEL, id))gOrigToggle)(self, _cmd, sender);
        return;
    }
    gSetupOnly = YES;
    ((void (*)(id, SEL, id))gOrigToggle)(self, _cmd, sender);
    gSetupOnly = NO;
    opalspy_log(@"launchsetup: ran Opal's popover setup without showing it");
}

static void ls_show(id self, SEL _cmd, NSRect rect, NSView *view, NSRectEdge edge) {
    if(gSetupOnly) return;
    ((void (*)(id, SEL, NSRect, NSView *, NSRectEdge))gOrigShow)(self, _cmd, rect, view, edge);
}

static void ls_activate(id self, SEL _cmd, BOOL flag) {
    if(gSetupOnly) return;
    ((void (*)(id, SEL, BOOL))gOrigActivate)(self, _cmd, flag);
}

void opal_launchsetup_install(void) {
    Class app = NSClassFromString(@"_TtC4Opal11AppDelegate");
    Method toggle = app ? class_getInstanceMethod(app, @selector(togglePopover:)) : NULL;
    if(!toggle) {
        opalspy_log(@"launchsetup: AppDelegate.togglePopover: not found, not installed");
        return;
    }
    gOrigToggle = method_setImplementation(toggle, (IMP)ls_toggle);
    gOrigShow = method_setImplementation(class_getInstanceMethod(NSPopover.class, @selector(showRelativeToRect:ofView:preferredEdge:)), (IMP)ls_show);
    gOrigActivate = method_setImplementation(class_getInstanceMethod(NSApplication.class, @selector(activateIgnoringOtherApps:)), (IMP)ls_activate);
    opalspy_log(@"launchsetup: installed");
}
