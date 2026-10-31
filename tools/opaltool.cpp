// opaltool: test / flash Opal C1 standalone pipelines using Opal.app's own libdepthai-core.
//   opaltool hostboot <orig|idle> <seconds>   host-boot (RAM only, nothing flashed) and monitor
//   opaltool blversion                         print bootloader version (camera rebooted to bootloader)
//   opaltool flash <orig|idle>                 flash the application pipeline (bootloader untouched)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "depthai/device/DeviceBootloader.hpp"
#include "depthai/xlink/XLinkConnection.hpp"
#include "standalone.hpp"

extern "C" {
int libusb_init(void** ctx);
void* libusb_open_device_with_vid_pid(void* ctx, uint16_t vid, uint16_t pid);
int libusb_control_transfer(void* h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, unsigned char* d, uint16_t len, unsigned int timeout);
void libusb_close(void* h);
void libusb_exit(void* ctx);
}

using clk = std::chrono::steady_clock;
static std::string tuning() {
    const char* t = getenv("OPAL_TUNING");
    return t ? t : "/Users/amadeus/Developer/experiments/opal-camera/Opal.app/Contents/Resources/Opal_v0.12.bin";
}

static bool uvcReset() {
    void* ctx = nullptr;
    if(libusb_init(&ctx) != 0) return false;
    void* h = libusb_open_device_with_vid_pid(ctx, 0x03e7, 0xf63b);
    if(!h) { libusb_exit(ctx); return false; }
    int r = libusb_control_transfer(h, 0x00, 0xF5, 0x0DA1, 0, nullptr, 0, 1000);
    printf("uvc reset -> %d\n", r);
    libusb_close(h);
    libusb_exit(ctx);
    return true;
}

static bool waitFor(dai::DeviceInfo& out, bool wantBootloaderOnly, int timeoutMs) {
    auto t0 = clk::now();
    while(std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count() < timeoutMs) {
        for(auto& d : dai::XLinkConnection::getAllConnectedDevices(X_LINK_ANY_STATE, false)) {
            if(d.state == X_LINK_BOOTLOADER || (!wantBootloaderOnly && d.state == X_LINK_UNBOOTED)) { out = d; return true; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return false;
}

static bool toBootloader(dai::DeviceInfo& info, bool bootloaderOnly) {
    if(waitFor(info, bootloaderOnly, 200)) return true;
    if(!uvcReset()) { printf("no Opal webcam (03e7:f63b) found\n"); return false; }
    if(!waitFor(info, bootloaderOnly, 15000)) { printf("camera did not enter bootloader\n"); return false; }
    return true;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if(argc < 2) { printf("usage: see header\n"); return 1; }
    std::string cmd = argv[1];
    bool idle = argc > 2 && std::string(argv[2]) == "idle";

    if(cmd == "blversion") {
        dai::DeviceInfo info;
        if(!toBootloader(info, true)) return 1;
        dai::DeviceBootloader bl(info);
        printf("bootloader version: %s type=%d\n", bl.getVersion().toString().c_str(), (int)bl.getType());
        return 0;
    }

    if(cmd == "blflash") {
        // Upgrade the bootloader to the one embedded in Opal.app's libdepthai-core (same as Opal's own updater).
        // For bootloader < 0.0.12 depthai jumps to USB ROM, RAM-boots the NEW bootloader, and that performs the write.
        dai::DeviceInfo info;
        if(!toBootloader(info, true)) return 1;
        dai::DeviceBootloader bl(info, true /*allowFlashingBootloader*/);
        printf("flashed bootloader version: %s, running (RAM) bootloader version: %s, embedded=%d\n",
               bl.getFlashedVersion().toString().c_str(), bl.getVersion().toString().c_str(), (int)bl.isEmbeddedVersion());
        bool ok;
        std::string err;
        std::tie(ok, err) = bl.flashBootloader([](float pr) { printf("\rflashing bootloader %3.0f%%", pr * 100); });
        printf("\nbootloader flash %s %s\n", ok ? "OK" : "FAILED:", err.c_str());
        return ok ? 0 : 3;
    }

    if(cmd == "hostboot") {
        int secs = argc > 3 ? atoi(argv[3]) : 60;
        dai::DeviceInfo info;
        if(!toBootloader(info, false)) return 1;
        auto p = opal::buildStandalonePipeline(tuning(), idle, true);
        auto t0 = clk::now();
        // like Opal's DeviceManager: UVC firmware mode requires BoardConfig.uvcEnable when host-booting
        auto config = p.getDeviceConfig();
        config.board.uvcEnable = true;
        dai::DeviceBase dev(config, info);
        printf("firmware up in %lld ms, starting pipeline\n", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count());
        dev.addLogCallback([](dai::LogMessage m) { printf("  [cam %s] %s\n", m.nodeIdName.c_str(), m.payload.c_str()); });
        dev.setLogLevel(dai::LogLevel::WARN);
        dev.startPipeline(p);
        printf("host-booted %s pipeline in %lld ms\n", idle ? "IDLE-WATCHER" : "ORIGINAL",
               (long long)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count());
        for(int i = 0; i <= secs; i += 5) {
            if(dev.isClosed()) { printf("LINK LOST at t=%d\n", i); return 2; }
            try {
                auto t = dev.getChipTemperature();
                auto css = dev.getLeonCssCpuUsage();
                printf("t=%4ds avg=%.2fC css=%.1f mss=%.1f upa=%.1f dss=%.1f cpuCSS=%.0f%%\n", i, t.average, t.css, t.mss, t.upa, t.dss, css.average * 100);
            } catch(std::exception& e) { printf("RPC FAILED at t=%d: %s\n", i, e.what()); return 2; }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        printf("done\n");
        return 0;
    }

    if(cmd == "flash") {
        dai::DeviceInfo info;
        if(!toBootloader(info, true)) return 1;
        dai::DeviceBootloader bl(info);
        printf("bootloader version: %s\n", bl.getVersion().toString().c_str());
        auto p = opal::buildStandalonePipeline(tuning(), idle, false);
        bool ok;
        std::string err;
        std::tie(ok, err) = bl.flash([](float pr) { printf("\rflashing %3.0f%%", pr * 100); }, p, getenv("OPAL_COMPRESS") != nullptr);
        printf("\nflash %s %s\n", ok ? "OK" : "FAILED:", err.c_str());
        return ok ? 0 : 3;
    }
    printf("unknown command\n");
    return 1;
}
