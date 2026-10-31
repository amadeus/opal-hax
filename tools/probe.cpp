// Opal C1 telemetry probe, built against Opal.app's own libdepthai-core (audioproc branch, 2.15.1).
// usage: probe            -> list devices only (passive)
//        probe connect N  -> attach to running camera, poll telemetry N seconds, then close
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "depthai/device/Device.hpp"
#include "depthai/xlink/XLinkConnection.hpp"

static const char* stateName(XLinkDeviceState_t s) {
    switch(s) {
        case X_LINK_BOOTED: return "BOOTED";
        case X_LINK_UNBOOTED: return "UNBOOTED";
        case X_LINK_BOOTLOADER: return "BOOTLOADER";
        case X_LINK_FLASH_BOOTED: return "FLASH_BOOTED";
        default: return "ANY/OTHER";
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    auto devs = dai::XLinkConnection::getAllConnectedDevices(X_LINK_ANY_STATE, false);
    printf("found %zu device(s)\n", devs.size());
    for(auto& d : devs) printf("  name=%s state=%s mxid=%s\n", d.desc.name, stateName(d.state), d.getMxId().c_str());
    if(argc < 2 || strcmp(argv[1], "connect") != 0 || devs.empty()) return 0;
    int seconds = argc > 2 ? atoi(argv[2]) : 15;

    auto t0 = std::chrono::steady_clock::now();
    dai::Device dev(dai::OpenVINO::DEFAULT_VERSION, devs[0], dai::UsbSpeed::SUPER);
    printf("attached in %lld ms\n", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());

    dev.addLogCallback([](dai::LogMessage m) { printf("[device %s] %s\n", m.nodeIdName.c_str(), m.payload.c_str()); });
    try { dev.setLogLevel(dai::LogLevel::INFO); dev.setSystemInformationLoggingRate(1.0f); } catch(std::exception& e) { printf("log cfg err: %s\n", e.what()); }

    auto tryp = [](const char* what, auto fn) { try { fn(); } catch(std::exception& e) { printf("%s: ERROR %s\n", what, e.what()); } };
    tryp("mxid", [&] { printf("mxid=%s usb=%d pipelineRunning=%d\n", dev.getMxId().c_str(), (int)dev.getUsbSpeed(), (int)dev.isPipelineRunning()); });
    tryp("sensors", [&] { for(auto& kv : dev.getCameraSensorNames()) printf("sensor socket=%d name=%s\n", (int)kv.first, kv.second.c_str()); });

    for(int i = 0; i < seconds; i++) {
        tryp("poll", [&] {
            auto t = dev.getChipTemperature();
            auto css = dev.getLeonCssCpuUsage();
            auto mss = dev.getLeonMssCpuUsage();
            auto ddr = dev.getDdrMemoryUsage();
            printf("t=%2ds temp avg=%.1fC css=%.1f mss=%.1f upa=%.1f dss=%.1f | cpu css=%.0f%% mss=%.0f%% | ddr %lld/%lld MiB\n", i, t.average, t.css, t.mss, t.upa,
                   t.dss, css.average * 100, mss.average * 100, (long long)ddr.used / (1 << 20), (long long)ddr.total / (1 << 20));
        });
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    printf("closing (camera will reset)\n");
    return 0;
}
