// Opal C1 standalone (flashed) webcam pipeline, reconstructed from
// -[FirmwareManager getApplicationPipelineWithImageTunning:] in Opal.app 0.26 (build 25.992),
// plus an optional "idle watcher" Script that stops the sensor when no UVC client is streaming.
#pragma once
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "depthai/depthai.hpp"

namespace opal {

static const char* kIdleWatcherScript = R"PY(
import time
import GPIO

PIN = 58          # UVC node drives this high on stream-on, low on stream-off (it's the LED pin)
GRACE = 3.0       # seconds without a client before the sensor is stopped
VERBOSE = __VERBOSE__

def ctl(on):
    c = CameraControl()
    if on:
        c.setStartStreaming()
    else:
        c.setStopStreaming()
    node.io['ctrl'].send(c)

# The UVC node owns the pin; a NON-exclusive setup lets us read the level it drives
# (read() without setup returns error code 9).
GPIO.setup(PIN, GPIO.OUT, GPIO.PULL_NONE, False)

running = True
low_since = time.monotonic()
last_log = 0.0
node.warn('opal-idle: watcher started')
while True:
    v = GPIO.read(PIN)
    now = time.monotonic()
    if v:
        low_since = None
        if not running:
            ctl(True)
            running = True
            node.warn('opal-idle: client streaming -> sensor START')
    else:
        if low_since is None:
            low_since = now
        if running and now - low_since > GRACE:
            ctl(False)
            running = False
            node.warn('opal-idle: no client -> sensor STOP')
    if VERBOSE and now - last_log > 5.0:
        node.warn('opal-idle: gpio=%d sensor_running=%d' % (v, running))
        last_log = now
    time.sleep(0.1)
)PY";

// withIdleWatcher=false reproduces Opal's original flashed pipeline exactly.
inline dai::Pipeline buildStandalonePipeline(const std::string& tuningBlob, bool withIdleWatcher, bool verbose) {
    dai::Pipeline p;
    auto cam = p.create<dai::node::ColorCamera>();
    cam->setBoardSocket(dai::CameraBoardSocket::RGB);
    cam->setInterleaved(false);
    cam->setPreviewSize(1920, 1080);
    cam->setImageOrientation(dai::CameraImageOrientation::ROTATE_180_DEG);
    cam->initialControl.setAutoFocusMode(dai::CameraControl::AutoFocusMode::AUTO);
    cam->initialControl.setAntiBandingMode(dai::CameraControl::AntiBandingMode::MAINS_60_HZ);
    if(!tuningBlob.empty()) p.setCameraTuningBlobPath(tuningBlob);
    cam->setResolution(dai::ColorCameraProperties::SensorResolution::THE_4_K);
    cam->setIspScale(1, 2);

    auto uvc = p.create<dai::node::UVC>();
    cam->video.link(uvc->input);
    uvc->setGpiosOnInit({{58, 0}});
    uvc->setGpiosOnStreamOn({{58, 1}});
    uvc->setGpiosOnStreamOff({{58, 0}});

    auto mic = p.create<dai::node::AudioMic>();
    auto uac = p.create<dai::node::UAC>();
    mic->setStreamBackMic(false);
    mic->out.link(uac->input);
    mic->initialConfig.setMicGainDecibels(28.0f);

    if(withIdleWatcher) {
        auto script = p.create<dai::node::Script>();
        std::string src = kIdleWatcherScript;
        if(const char* f = getenv("OPAL_SCRIPT_FILE")) {  // experiment hook: load script from file
            std::ifstream in(f);
            src.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        auto pos = src.find("__VERBOSE__");
        if(pos != std::string::npos) src.replace(pos, 11, verbose ? "True" : "False");
        script->setScript(src, "opal_idle_watcher");
        script->outputs["ctrl"].link(cam->inputControl);
    }
    return p;
}

}  // namespace opal
