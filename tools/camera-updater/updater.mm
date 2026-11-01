// Opal Camera Updater — bundled in Opal.app/Contents/Helpers. Started by Opal (opalspy firmwareoffer.m) after the
// user accepted the update prompt; Opal quits itself so its camera service lets go of the camera.
//
//   1. wait until Opal and its camera service are gone; pause the plug-in launcher (it would restart Opal)
//   2. put the camera into its bootloader (vendor "UVC device reset" request, as Opal itself does)
//   3. if the flashed bootloader is older than the one embedded in Opal's depthai: flash the bootloader
//      (UNTESTED here — the owner's camera was already updated with tools/bin/opaltool blflash, same calls)
//   4. flash the standalone webcam program with the idle watcher (tools/standalone.hpp, same as opaltool flash idle),
//      anti-banding 50/60 Hz from the Mac's region
//   5. wait for the camera to come back as 03e7:f63d, restore the launcher, start Opal
// Log: ~/Library/Logs/opal-watch/camera-update.log
#import <AppKit/AppKit.h>
#include <spawn.h>
#include <sys/wait.h>
#include <chrono>
#include <thread>

#include "depthai/device/DeviceBootloader.hpp"
#include "depthai/xlink/XLinkConnection.hpp"
#include "../standalone.hpp"

extern "C" {
int libusb_init(void** ctx);
void* libusb_open_device_with_vid_pid(void* ctx, uint16_t vid, uint16_t pid);
int libusb_control_transfer(void* h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, unsigned char* d, uint16_t len, unsigned int timeout);
void libusb_close(void* h);
void libusb_exit(void* ctx);
}
extern char** environ;
using clk = std::chrono::steady_clock;

static NSString* const kAgentLabel = @"com.amadeus.oc.OpalCamera.startOnUsbPlugged.agent";
static NSString* const kAgentPlist = @"/Library/LaunchAgents/com.amadeus.oc.OpalCamera.startOnUsbPlugged.agent.plist";

static NSTextField* gStatus;
static NSProgressIndicator* gBar;
static NSButton* gButton;
static NSWindow* gWindow;

static void ulog(NSString* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    NSString* msg = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSDateFormatter* f = [NSDateFormatter new];
    f.dateFormat = @"yyyy-MM-dd HH:mm:ss";
    NSString* line = [NSString stringWithFormat:@"%@ %@\n", [f stringFromDate:NSDate.date], msg];
    NSString* dir = [@"~/Library/Logs/opal-watch" stringByExpandingTildeInPath];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* path = [dir stringByAppendingPathComponent:@"camera-update.log"];
    NSFileHandle* h = [NSFileHandle fileHandleForWritingAtPath:path];
    if(!h) { [line writeToFile:path atomically:NO encoding:NSUTF8StringEncoding error:nil]; return; }
    [h seekToEndOfFile];
    [h writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
    [h closeFile];
}

static void ui(NSString* status, double progress /* <0: indeterminate */) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if(status) gStatus.stringValue = status;
        if(progress < 0) { gBar.indeterminate = YES; [gBar startAnimation:nil]; }
        else { [gBar stopAnimation:nil]; gBar.indeterminate = NO; gBar.doubleValue = progress; }
    });
}

static int run(NSArray<NSString*>* args) {
    std::vector<std::string> s;
    for(NSString* a in args) s.push_back(a.UTF8String);
    std::vector<char*> argv;
    for(auto& x : s) argv.push_back((char*)x.c_str());
    argv.push_back(nullptr);
    pid_t pid;
    if(posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static bool running(const char* name) { return run(@[ @"/usr/bin/pgrep", @"-x", @(name) ]) == 0; }

static bool usbPresent(uint16_t pid) {
    void* ctx = nullptr;
    if(libusb_init(&ctx) != 0) return false;
    void* h = libusb_open_device_with_vid_pid(ctx, 0x03e7, pid);
    if(h) libusb_close(h);
    libusb_exit(ctx);
    return h != nullptr;
}

static bool uvcReset() {
    void* ctx = nullptr;
    if(libusb_init(&ctx) != 0) return false;
    bool sent = false;
    for(uint16_t pid : {(uint16_t)0xf63b, (uint16_t)0xf63d}) {
        void* h = libusb_open_device_with_vid_pid(ctx, 0x03e7, pid);
        if(!h) continue;
        int r = libusb_control_transfer(h, 0x00, 0xF5, 0x0DA1, 0, nullptr, 0, 1000);
        ulog(@"reset request to 03e7:%04x -> %d", pid, r);
        libusb_close(h);
        sent = true;
        break;
    }
    libusb_exit(ctx);
    return sent;
}

static bool waitForBootloader(dai::DeviceInfo& out, int timeoutMs) {
    auto t0 = clk::now();
    while(std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count() < timeoutMs) {
        for(auto& d : dai::XLinkConnection::getAllConnectedDevices(X_LINK_ANY_STATE, false))
            if(d.state == X_LINK_BOOTLOADER) { out = d; return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

static bool toBootloader(dai::DeviceInfo& info) {
    if(waitForBootloader(info, 300)) return true;
    for(int attempt = 0; attempt < 3; attempt++) {
        if(!uvcReset()) { std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }
        if(waitForBootloader(info, 15000)) return true;
    }
    return false;
}

static dai::CameraControl::AntiBandingMode mainsForRegion(NSString** label) {
    // countries on 60 Hz mains; everything else is 50 Hz (Japan is split: east 50, west 60 -> auto)
    NSSet* sixty = [NSSet setWithArray:@[ @"US", @"CA", @"MX", @"BR", @"CO", @"VE", @"EC", @"PE", @"BO", @"GT", @"HN", @"SV", @"NI",
                                          @"CR", @"PA", @"CU", @"DO", @"PR", @"HT", @"BS", @"BZ", @"JM", @"TT", @"GU", @"KR", @"TW",
                                          @"PH", @"SA", @"LR", @"SR", @"GY" ]];
    NSString* cc = NSLocale.currentLocale.countryCode ?: @"US";
    if([cc isEqualToString:@"JP"]) { *label = @"auto (JP)"; return dai::CameraControl::AntiBandingMode::AUTO; }
    if([sixty containsObject:cc]) { *label = [cc stringByAppendingString:@" 60 Hz"]; return dai::CameraControl::AntiBandingMode::MAINS_60_HZ; }
    *label = [cc stringByAppendingString:@" 50 Hz"];
    return dai::CameraControl::AntiBandingMode::MAINS_50_HZ;
}

// Returns nil on success, else an error message.
static NSString* update(NSString* opalApp) {
    ui(@"Waiting for Opal to quit…", -1);
    auto t0 = clk::now();
    while((running("Opal") || running("OpalCameraDeviceService")) &&
          std::chrono::duration_cast<std::chrono::seconds>(clk::now() - t0).count() < 30)
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if(running("Opal") || running("OpalCameraDeviceService")) return @"Opal is still running. Quit Opal and try again.";

    ui(@"Switching the camera to update mode…", -1);
    dai::DeviceInfo info;
    if(!toBootloader(info)) return @"The camera did not enter update mode. Unplug it, plug it back in, and try again.";

    NSString* tuning = [opalApp stringByAppendingPathComponent:@"Contents/Resources/Opal_v0.12.bin"];
    NSString* mainsLabel = nil;
    auto mains = mainsForRegion(&mainsLabel);
    auto embedded = dai::DeviceBootloader::getEmbeddedBootloaderVersion();
    bool needBootloader;
    {
        dai::DeviceBootloader bl(info);
        auto flashed = bl.getFlashedVersion();
        needBootloader = flashed < embedded;
        ulog(@"bootloader on camera %s, bundled %s -> %@", flashed.toString().c_str(), embedded.toString().c_str(),
             needBootloader ? @"update" : @"keep");
        if(!needBootloader) {
            ui(@"Installing camera firmware… keep the camera plugged in.", 0);
            bool ok;
            std::string err;
            std::tie(ok, err) = bl.flash([](float p) { ui(nil, p * 100); }, opal::buildStandalonePipeline(tuning.UTF8String, true, false, mains), false);
            ulog(@"firmware flash (%@): %s %s", mainsLabel, ok ? "OK" : "FAILED", err.c_str());
            if(!ok) return [NSString stringWithFormat:@"Installing the firmware failed: %s", err.c_str()];
        }
    }
    if(needBootloader) {
        // Same calls as tools/bin/opaltool blflash, then the program flash through the freshly booted bootloader.
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if(!toBootloader(info)) return @"The camera did not re-enter update mode. Unplug it, plug it back in, and try again.";
        dai::DeviceBootloader bl(info, true /*allowFlashingBootloader*/);
        ui(@"Updating the camera's bootloader… keep the camera plugged in.", 0);
        bool ok;
        std::string err;
        std::tie(ok, err) = bl.flashBootloader([](float p) { ui(nil, p * 50); });
        ulog(@"bootloader flash: %s %s", ok ? "OK" : "FAILED", err.c_str());
        if(!ok) return [NSString stringWithFormat:@"Updating the bootloader failed: %s", err.c_str()];
        ui(@"Installing camera firmware… keep the camera plugged in.", 50);
        std::tie(ok, err) = bl.flash([](float p) { ui(nil, 50 + p * 50); }, opal::buildStandalonePipeline(tuning.UTF8String, true, false, mains), false);
        ulog(@"firmware flash (%@): %s %s", mainsLabel, ok ? "OK" : "FAILED", err.c_str());
        if(!ok) return [NSString stringWithFormat:@"Installing the firmware failed: %s", err.c_str()];
    }

    ui(@"Restarting the camera…", -1);
    t0 = clk::now();
    while(!usbPresent(0xf63d) && std::chrono::duration_cast<std::chrono::seconds>(clk::now() - t0).count() < 40)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if(!usbPresent(0xf63d)) {
        ulog(@"camera not seen as 03e7:f63d after flashing");
        return @"The firmware was written, but the camera hasn't come back yet. Unplug it and plug it back in.";
    }
    ulog(@"camera is back as 03e7:f63d");
    return nil;
}

@interface Delegate : NSObject <NSApplicationDelegate>
@end
@implementation Delegate
- (void)applicationDidFinishLaunching:(NSNotification*)n {
    gWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 440, 140)
                                          styleMask:NSWindowStyleMaskTitled
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    gWindow.title = @"Opal Camera Update";
    NSView* v = gWindow.contentView;
    gStatus = [NSTextField wrappingLabelWithString:@"Preparing…"];
    gStatus.frame = NSMakeRect(20, 80, 400, 40);
    [v addSubview:gStatus];
    gBar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 56, 400, 20)];
    gBar.minValue = 0;
    gBar.maxValue = 100;
    [v addSubview:gBar];
    gButton = [NSButton buttonWithTitle:@"Close" target:NSApp action:@selector(terminate:)];
    gButton.frame = NSMakeRect(340, 14, 80, 30);
    gButton.enabled = NO;
    [v addSubview:gButton];
    [gWindow center];
    [gWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    // .../Opal.app/Contents/Helpers/Opal Camera Updater.app -> .../Opal.app
    NSString* opalApp = [[[NSBundle.mainBundle.bundlePath stringByDeletingLastPathComponent] stringByDeletingLastPathComponent]
                         stringByDeletingLastPathComponent];
    NSString* uid = [NSString stringWithFormat:@"%d", getuid()];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ulog(@"---- camera update started (Opal at %@)", opalApp);
        run(@[ @"/bin/launchctl", @"bootout", [NSString stringWithFormat:@"gui/%@/%@", uid, kAgentLabel] ]);
        NSString* err = nil;
        try {
            err = update(opalApp);
        } catch(const std::exception& e) {
            err = [NSString stringWithFormat:@"Update error: %s", e.what()];
        }
        ulog(@"result: %@", err ?: @"success");
        run(@[ @"/bin/launchctl", @"bootstrap", [@"gui/" stringByAppendingString:uid], kAgentPlist ]);
        [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:opalApp]
                                            configuration:[NSWorkspaceOpenConfiguration configuration]
                                        completionHandler:nil];
        dispatch_async(dispatch_get_main_queue(), ^{
            [gBar stopAnimation:nil];
            gBar.indeterminate = NO;
            gBar.doubleValue = err ? 0 : 100;
            gStatus.stringValue = err ?: @"Your Opal C1 is updated. Opal has been restarted.";
            gButton.enabled = YES;
            gButton.keyEquivalent = @"\r";
        });
    });
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a { return YES; }
@end

int main(int argc, const char** argv) {
    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        Delegate* d = [Delegate new];
        app.delegate = d;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        [app run];
    }
    return 0;
}
