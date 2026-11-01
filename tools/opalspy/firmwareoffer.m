// firmwareoffer: when an Opal C1 still running Opal's ORIGINAL flashed firmware is connected, offer to update it to
// our firmware (newer bootloader if needed + standalone program with the sensor idle watcher). Custom build only.
//
// Detection: the original flashed program enumerates as 03e7:f63b WITH a USB video-class (UVC, class 14) interface.
// When Opal has booted the camera itself it is also f63b but has no video interface, and our program enumerates as
// f63d. So "f63b + video interface" = original firmware. We watch for that interface being published (IOKit), which
// also catches the short window after plug-in before Opal takes the camera over.
//
// On "Update": start Opal.app/Contents/Helpers/Opal Camera Updater.app (it flashes and restarts Opal), then quit Opal
// so its camera service releases the camera. "Not Now": asked again the next time Opal starts.
// Log: ~/Library/Logs/opal-watch/opalspy.log
#import <AppKit/AppKit.h>
#import <IOKit/IOKitLib.h>

void opalspy_log(NSString *msg);

static IONotificationPortRef gPort;
static io_iterator_t gIter;
static BOOL gAsked;

static void offer(void) {
    if(gAsked) return;
    gAsked = YES;
    NSURL *updater = [NSBundle.mainBundle.bundleURL URLByAppendingPathComponent:@"Contents/Helpers/Opal Camera Updater.app"];
    if(![NSFileManager.defaultManager fileExistsAtPath:updater.path]) { opalspy_log(@"firmwareoffer: updater missing"); return; }
    opalspy_log(@"firmwareoffer: camera with original firmware detected, asking");
    NSAlert *a = [NSAlert new];
    a.alertStyle = NSAlertStyleCritical;
    a.messageText = @"Update your Opal C1's firmware?";
    a.informativeText =
        @"This camera is running Opal's original firmware. The update installs a newer bootloader (if the camera's is "
         "older) and firmware that turns the camera's sensor off when no app is using it, so it stays cool.\n\n"
         "Keep the camera plugged in until the update finishes (about a minute). Opal quits during the update and "
         "restarts afterwards.\n\n"
         "WARNING: flashing firmware can brick your camera, permanently, if it is interrupted or something goes "
         "wrong. This update is unofficial and comes with no support. Proceed at your own risk.";
    [a addButtonWithTitle:@"Not Now"];
    [a addButtonWithTitle:@"Update at My Own Risk"];
    a.buttons[1].hasDestructiveAction = YES;
    a.buttons[1].keyEquivalent = @"";  // Return / Escape never start a flash
    [NSApp activateIgnoringOtherApps:YES];
    if([a runModal] != NSAlertSecondButtonReturn) { opalspy_log(@"firmwareoffer: not now"); return; }
    opalspy_log(@"firmwareoffer: user accepted, starting the updater and quitting Opal");
    [NSWorkspace.sharedWorkspace openApplicationAtURL:updater
                                        configuration:[NSWorkspaceOpenConfiguration configuration]
                                    completionHandler:^(NSRunningApplication *app, NSError *err) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if(err) { opalspy_log([NSString stringWithFormat:@"firmwareoffer: updater failed to start: %@", err]); return; }
            [NSApp terminate:nil];
        });
    }];
}

static void matched(void *refcon, io_iterator_t it) {
    io_object_t o;
    BOOL found = NO;
    while((o = IOIteratorNext(it))) { found = YES; IOObjectRelease(o); }
    if(!found || gAsked) return;
    // let Opal finish launching / show its menu-bar item first
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ offer(); });
}

void opal_firmwareoffer_install(void) {
    if(getenv("OPALSPY_NO_FIRMWARE_OFFER")) return;
    NSMutableDictionary *m = (__bridge_transfer NSMutableDictionary *)IOServiceMatching("IOUSBHostInterface");
    m[@"idVendor"] = @0x03e7;
    m[@"idProduct"] = @0xf63b;
    m[@"bInterfaceClass"] = @14;  // video
    gPort = IONotificationPortCreate(kIOMainPortDefault);
    IONotificationPortSetDispatchQueue(gPort, dispatch_get_main_queue());
    if(IOServiceAddMatchingNotification(gPort, kIOFirstMatchNotification, (__bridge_retained CFDictionaryRef)m, matched,
                                        NULL, &gIter) != KERN_SUCCESS) {
        opalspy_log(@"firmwareoffer: IOKit watch failed");
        return;
    }
    matched(NULL, gIter);  // arm the notification and handle a camera that is already connected
    opalspy_log(@"firmwareoffer: watching for cameras with the original firmware");
}
