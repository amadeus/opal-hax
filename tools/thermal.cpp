// Opal C1 thermal experiment. Host-boots Opal's own device firmware (nothing is flashed).
// usage: thermal <phaseA_s> <phaseB_s> <phaseC_s>
//   A: IMX582 4K30 streaming (sensor+ISP busy, frames counted via metadata-only XLinkOut)
//   B: sensor stopped via CameraControl::setStopStreaming
//   C: sensor restarted via setStartStreaming (verifies frames resume)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "depthai/depthai.hpp"
#include "depthai/xlink/XLinkConnection.hpp"

extern "C" {  // minimal libusb prototypes (linked against Opal.app's bundled libusb-1.0.0.dylib)
int libusb_init(void** ctx);
void* libusb_open_device_with_vid_pid(void* ctx, uint16_t vid, uint16_t pid);
int libusb_control_transfer(void* h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, unsigned char* d, uint16_t len, unsigned int timeout);
void libusb_close(void* h);
void libusb_exit(void* ctx);
}

// this unit's sensor is an IMX378 (firmware boot log) -> Opal's IMX378 tuning blob
static const char* TUNING = getenv("OPAL_TUNING") ? getenv("OPAL_TUNING") : "/Users/amadeus/Developer/experiments/opal-camera/Opal.app/Contents/Resources/Opal_v0.12.bin";
using clk = std::chrono::steady_clock;

static bool uvcReset() {  // same request Opal's +[DeviceUtils resetDevice] sends
    void* ctx = nullptr;
    if(libusb_init(&ctx) != 0) return false;
    void* h = libusb_open_device_with_vid_pid(ctx, 0x03e7, 0xf63b);
    if(!h) { libusb_exit(ctx); return false; }
    int r = libusb_control_transfer(h, 0x00, 0xF5, 0x0DA1, 0, nullptr, 0, 1000);
    printf("uvc reset control transfer -> %d\n", r);
    libusb_close(h);
    libusb_exit(ctx);
    return true;
}

static bool findBootloader(dai::DeviceInfo& out, int timeoutMs) {
    auto t0 = clk::now();
    while(std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count() < timeoutMs) {
        for(auto& d : dai::XLinkConnection::getAllConnectedDevices(X_LINK_ANY_STATE, false)) {
            if(d.state == X_LINK_BOOTLOADER || d.state == X_LINK_UNBOOTED) { out = d; return true; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int pA = argc > 1 ? atoi(argv[1]) : 120, pB = argc > 2 ? atoi(argv[2]) : 120, pC = argc > 3 ? atoi(argv[3]) : 20;

    dai::DeviceInfo info;
    if(!findBootloader(info, 200)) {
        if(!uvcReset()) { printf("no camera found\n"); return 1; }
        if(!findBootloader(info, 15000)) { printf("camera did not come back in bootloader\n"); return 1; }
    }
    printf("bootloader device: %s\n", info.getMxId().c_str());

    // experiment switches (env): OUT=meta|preview|none  TUNE=0|1  RES=4k|1080
    auto env = [](const char* k, const char* d) { const char* v = getenv(k); return std::string(v ? v : d); };
    std::string OUT = env("OUT", "meta"), TUNE = env("TUNE", "1"), RES = env("RES", "4k");
    printf("config: OUT=%s TUNE=%s RES=%s\n", OUT.c_str(), TUNE.c_str(), RES.c_str());

    dai::Pipeline p;
    if(TUNE == "1") p.setCameraTuningBlobPath(TUNING);
    auto cam = p.create<dai::node::ColorCamera>();
    cam->setBoardSocket(dai::CameraBoardSocket::RGB);
    cam->setResolution(RES == "1080" ? dai::ColorCameraProperties::SensorResolution::THE_1080_P : dai::ColorCameraProperties::SensorResolution::THE_4_K);
    cam->setFps(30);
    cam->setPreviewSize(300, 300);
    cam->setInterleaved(env("INTERLEAVED", "0") == "1");
    auto ctrlIn = p.create<dai::node::XLinkIn>();
    ctrlIn->setStreamName("control");
    ctrlIn->out.link(cam->inputControl);
    if(OUT != "none") {
        auto meta = p.create<dai::node::XLinkOut>();
        meta->setStreamName("meta");
        if(OUT == "meta") meta->setMetadataOnly(true);
        meta->input.setBlocking(false);
        meta->input.setQueueSize(2);
        (OUT == "preview" ? cam->preview : cam->video).link(meta->input);
    }

    auto t0 = clk::now();
    dai::Device dev(p, info, dai::UsbSpeed::SUPER);
    printf("booted Opal fw + pipeline in %lld ms, usb=%d\n",
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count(), (int)dev.getUsbSpeed());
    dev.addLogCallback([](dai::LogMessage m) { printf("[device %s] %s\n", m.nodeIdName.c_str(), m.payload.c_str()); });
    dev.setLogLevel(dai::LogLevel::INFO);

    auto ctrlQ = dev.getInputQueue("control");
    std::atomic<long> frames{0};
    std::shared_ptr<dai::DataOutputQueue> metaQ;
    if(OUT != "none") {
        metaQ = dev.getOutputQueue("meta", 4, false);
        metaQ->addCallback([&](std::shared_ptr<dai::ADatatype>) { frames++; });
    }

    auto runPhase = [&](const char* name, int secs) {
        long f0 = frames;
        auto ts = clk::now();
        for(int i = 0; i < secs; i += 5) {
            if(dev.isClosed()) { printf("%s t=%ds DEVICE LINK LOST\n", name, i); exit(2); }
            dai::ChipTemperature t;
            try { t = dev.getChipTemperature(); } catch(std::exception& e) { printf("%s t=%ds RPC FAILED: %s\n", name, i, e.what()); exit(2); }
            auto css = dev.getLeonCssCpuUsage(), mss = dev.getLeonMssCpuUsage();
            double el = std::chrono::duration<double>(clk::now() - ts).count();
            printf("%s t=%4ds avg=%.2fC css=%.1f mss=%.1f upa=%.1f dss=%.1f cpuCSS=%.0f%% cpuMSS=%.0f%% fps=%.1f\n", name, i, t.average, t.css, t.mss, t.upa, t.dss,
                   css.average * 100, mss.average * 100, el > 0 ? (frames - f0) / el : 0.0);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    };

    runPhase("A-streaming", pA);
    { dai::CameraControl c; c.setStopStreaming(); ctrlQ->send(c); printf("-> sent setStopStreaming\n"); }
    runPhase("B-stopped  ", pB);
    { dai::CameraControl c; c.setStartStreaming(); ctrlQ->send(c); printf("-> sent setStartStreaming\n"); }
    runPhase("C-restarted", pC);
    printf("done; closing (camera will reset back to its flashed webcam firmware)\n");
    return 0;
}
