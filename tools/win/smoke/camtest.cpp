// camtest: run the OpalCamera controller (the Windows app's camera core) on this Mac: boot, stream with faces +
// segmentation on, toggle settings, report what arrives. Writes no images. usage: camtest <template dir> [seconds]
#include <atomic>
#include <cstdio>
#include "../core/opal_camera.hpp"

int main(int argc, char** argv) {
    int secs = argc > 2 ? atoi(argv[2]) : 20;
    opal::OpalCamera cam(argv[1]);
    std::atomic<int> frames{0}, faces{0}, masks{0}, hands{0};
    std::atomic<double> lastLuma{0}, lastPerson{0};
    std::atomic<int> faceCount{0}, lens{0}, iso{0};
    cam.onLog = [](const std::string& s) { printf("[cam] %s\n", s.c_str()); fflush(stdout); };
    cam.onFrame = [&](std::shared_ptr<const opal::Frame> f) {
        frames++;
        if(frames % 30 == 0) {
            double sum = 0;
            for(int i = 0; i < f->width * f->height; i += 97) sum += f->data[i];
            lastLuma = sum / (f->width * f->height / 97.0);
            lens = f->lensPosition;
            iso = f->iso;
        }
    };
    cam.onFaces = [&](const std::vector<opal::Face>& v, double) { faces++; faceCount = (int)v.size(); };
    cam.onMask = [&](std::shared_ptr<const opal::Mask> m) {
        masks++;
        double s = 0;
        for(float p : m->data) s += p;
        lastPerson = s / m->data.size();
    };
    cam.onHand = [&](const opal::Hand& h) { hands++; };
    cam.onTelemetry = [&](const opal::Telemetry& t) { printf("  temp %.1fC css %.0f%% mss %.0f%%\n", t.chipTempC, t.cssCpu * 100, t.mssCpu * 100); };
    cam.start();
    cam.setNetworks(true, true, false);
    cam.setStreaming(true);
    opal::CameraSettings s;
    for(int t = 1; t <= secs; t++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        printf("t=%2d state=%d frames=%d faces=%d(%d) masks=%d hands=%d luma=%.0f person=%.2f lens=%d iso=%d\n", t, (int)cam.state(), frames.load(), faces.load(),
               faceCount.load(), masks.load(), hands.load(), lastLuma.load(), lastPerson.load(), lens.load(), iso.load());
        if(t == 8) { s.exposureAuto = false; s.exposureUs = 10000; s.iso = 200; cam.apply(s); printf("-> manual exposure 10ms ISO200\n"); }
        if(t == 11) { s.exposureAuto = true; s.focusMode = opal::FocusMode::Manual; s.lensPosition = 200; cam.apply(s); printf("-> auto exposure, manual focus 200\n"); }
        if(t == 14) { cam.setNetworks(true, false, true); printf("-> segmentation off, hands on\n"); }
        if(t == secs - 4) { cam.setStreaming(false); printf("-> streaming off (held 5s)\n"); }
    }
    cam.stop();
}
