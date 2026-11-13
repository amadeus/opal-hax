// popoverstate: tells Opal's camera-driver service (opalcore, another process) whether the popover is open, through
// the Darwin notification state "com.amadeus.oc.popover-open" (1 = open, 0 = closed). opalcore's stuck-stream guard
// needs it: with the popover open Opal streams for its own preview with no meeting app attached, which is legitimate.
#import <AppKit/AppKit.h>
#import <notify.h>

void opalspy_log(NSString *s);

static int gToken = -1;

static void publish(BOOL open) {
    if(gToken < 0 && notify_register_check("com.amadeus.oc.popover-open", &gToken) != NOTIFY_STATUS_OK) return;
    notify_set_state(gToken, open ? 1 : 0);
    notify_post("com.amadeus.oc.popover-open");
}

void opal_popoverstate_install(void) {
    publish(NO);
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:NSPopoverDidShowNotification object:nil queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *n) { publish(YES); }];
    [nc addObserverForName:NSPopoverDidCloseNotification object:nil queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification *n) { publish(NO); }];
    opalspy_log(@"popoverstate: publishing popover open/closed for the camera service");
}
