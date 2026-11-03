// frames: host-boot Opal's firmware with the self-built depthai and stream camera frames over XLink (app mode path).
// usage: frames [seconds] [1080|4k]
#include <chrono>
#include <cstdio>
#include <iostream>
#include "depthai/depthai.hpp"

int main(int argc, char** argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    bool k4 = argc > 2 && std::string(argv[2]) == "4k";
    dai::Pipeline p;
    p.setCameraTuningBlobPath("/Users/amadeus/Developer/experiments/opal-camera/Opal.app/Contents/Resources/Opal_v0.12.bin");
    p.setXLinkChunkSize(0);  // Opal does this (no chunking)
    auto cam = p.create<dai::node::ColorCamera>();
    cam->setBoardSocket(dai::CameraBoardSocket::RGB);
    cam->setResolution(dai::ColorCameraProperties::SensorResolution::THE_4_K);
    cam->setImageOrientation(dai::CameraImageOrientation::ROTATE_180_DEG);
    if(!k4) cam->setIspScale(1, 2);
    cam->setVideoSize(k4 ? 3840 : 1920, k4 ? 2160 : 1080);
    if(getenv("AF_OFF")) { cam->initialControl.setAutoFocusMode(dai::CameraControl::AutoFocusMode::OFF); cam->initialControl.setManualFocus(126); }
    if(getenv("AF_AUTO")) cam->initialControl.setAutoFocusMode(dai::CameraControl::AutoFocusMode::AUTO);
    if(getenv("PREVIEW_1080")) cam->setPreviewSize(1920, 1080);
    if(!getenv("NO_FPS")) cam->setFps(30);
    auto out = p.create<dai::node::XLinkOut>();
    out->setStreamName("video");
    cam->video.link(out->input);
    dai::Device dev(p);
    std::cout << "usb speed " << (int)dev.getUsbSpeed() << ", sensors:";
    for(auto& kv : dev.getCameraSensorNames()) std::cout << " " << kv.second;
    std::cout << std::endl;
    auto q = dev.getOutputQueue("video", 4, false);
    auto t0 = std::chrono::steady_clock::now();
    int n = 0;
    size_t bytes = 0;
    while(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(secs)) {
        auto f = q->get<dai::ImgFrame>();
        n++;
        bytes += f->getData().size();
        if(n == 1 || n % 60 == 0)
            printf("frame %d: %ux%u type=%d size=%zu seq=%ld exp=%dus iso=%d lens=%d\n", n, f->getWidth(), f->getHeight(), (int)f->getType(),
                   f->getData().size(), (long)f->getSequenceNum(), f->getExposureTime(), f->getSensitivity(), f->getLensPosition());
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%d frames in %.1fs = %.1f fps, %.1f MB/s\n", n, s, n / s, bytes / s / 1e6);
}
