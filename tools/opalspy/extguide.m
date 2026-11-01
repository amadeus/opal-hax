// extguide: macOS only lets the user approve the "Opal C1" camera extension, in a hard-to-find place (System
// Settings > General > Login Items & Extensions > Camera Extensions (i)), and its own "Camera Extension Blocked"
// alert points at Privacy & Security instead. When Opal starts and our extension is waiting for that approval, show
// plain step-by-step instructions with a button that opens the right pane, then confirm once it's switched on.
// Custom build only. Log: ~/Library/Logs/opal-watch/opalspy.log
#import <AppKit/AppKit.h>

void opalspy_log(NSString *msg);

static NSString *const kExt = @"com.amadeus.oc.OpalCamera.opalCameraExtension";
static BOOL gShown;

// "enabled", "waiting", or nil (not registered yet / unknown)
static NSString *extState(void) {
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/systemextensionsctl"];
    t.arguments = @[ @"list" ];
    NSPipe *p = [NSPipe pipe];
    t.standardOutput = p;
    t.standardError = [NSFileHandle fileHandleWithNullDevice];
    if(![t launchAndReturnError:nil]) return nil;
    NSData *d = [p.fileHandleForReading readDataToEndOfFile];
    [t waitUntilExit];
    NSString *out = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
    NSString *state = nil;
    for(NSString *line in [out componentsSeparatedByString:@"\n"]) {
        if(![line containsString:kExt] || [line containsString:@"uninstall"]) continue;
        if([line containsString:@"[activated enabled]"]) return @"enabled";
        if([line containsString:@"waiting for user"]) state = @"waiting";
    }
    return state;
}

static void openPane(void) {
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.LoginItems-Settings.extension"]];
}

static void waitForEnabled(int secondsLeft) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSString *s = extState();
        dispatch_async(dispatch_get_main_queue(), ^{
            if([s isEqualToString:@"enabled"]) {
                opalspy_log(@"extguide: camera extension enabled");
                NSAlert *a = [NSAlert new];
                a.messageText = @"Opal C1 is ready";
                a.informativeText = @"The camera now shows up as \"Opal C1\" in your video apps.";
                [NSApp activateIgnoringOtherApps:YES];
                [a runModal];
                return;
            }
            if(secondsLeft > 0)
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ waitForEnabled(secondsLeft - 2); });
        });
    });
}

static void guide(void) {
    if(gShown) return;
    gShown = YES;
    opalspy_log(@"extguide: camera extension waiting for approval, showing instructions");
    NSAlert *a = [NSAlert new];
    a.messageText = @"One more step: allow the Opal C1 camera";
    a.informativeText =
        @"macOS needs your OK before other apps can use the Opal C1 camera. You only do this once.\n\n"
         "1. Click Open Settings (opens Login Items & Extensions).\n"
         "2. Scroll down to Extensions and click the ⓘ next to Camera Extensions.\n"
         "3. Turn on Opal C1, then click Done.\n\n"
         "If macOS shows a \"Camera Extension Blocked\" alert, you can ignore it.";
    [a addButtonWithTitle:@"Open Settings"];
    [a addButtonWithTitle:@"Later"];
    [NSApp activateIgnoringOtherApps:YES];
    if([a runModal] != NSAlertFirstButtonReturn) { opalspy_log(@"extguide: later"); return; }
    openPane();
    waitForEnabled(600);
}

// After a fresh install the extension is registered a few seconds after Opal starts, so keep looking for 2 minutes.
static void check(int secondsLeft) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSString *s = extState();
        dispatch_async(dispatch_get_main_queue(), ^{
            if([s isEqualToString:@"waiting"]) { guide(); return; }
            if([s isEqualToString:@"enabled"] || secondsLeft <= 0) return;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ check(secondsLeft - 3); });
        });
    });
}

void opal_extguide_install(void) {
    if(getenv("OPALSPY_NO_EXT_GUIDE")) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ check(120); });
}
