#include "opal_camera.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <sstream>

#include "depthai/utility/Initialization.hpp"
#include "depthai/xlink/XLinkConnection.hpp"
#include "XLink/XLink.h"
#include "XLink/XLinkLog.h"

namespace opal {

using Clock = std::chrono::steady_clock;
static double now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
static const double kOffHold = 5.0;  // seconds an "off" request is held before the sensor stops

OpalCamera::OpalCamera(const std::string& templateDir) : tmpl_(templateDir, "full") {}

OpalCamera::~OpalCamera() { stop(); }

void OpalCamera::start() {
    if(thread_.joinable()) return;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void OpalCamera::stop() {
    quit_ = true;
    cv_.notify_all();
    if(thread_.joinable()) thread_.join();
}

void OpalCamera::log(const std::string& s) {
    if(onLog) onLog(s);
}

std::string OpalCamera::statusText() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

void OpalCamera::setStreaming(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    if(on) {
        wantStreaming_ = true;
        offRequestedAt_ = -1;
    } else if(wantStreaming_ && offRequestedAt_ < 0) {
        offRequestedAt_ = now();  // the control loop turns the sensor off once this has been held kOffHold seconds
    }
    cv_.notify_all();
}

void OpalCamera::setNetworks(bool faces, bool segmentation, bool hands) {
    std::lock_guard<std::mutex> lk(mu_);
    if(faces != wantFaces_ || segmentation != wantSeg_ || hands != wantHands_) networksDirty_ = true;
    wantFaces_ = faces;
    wantSeg_ = segmentation;
    wantHands_ = hands;
    cv_.notify_all();
}

void OpalCamera::apply(const CameraSettings& s) {
    std::lock_guard<std::mutex> lk(mu_);
    if(s.resolution != settings_.resolution) restartRequested_ = true;
    settings_ = s;
    cv_.notify_all();
}

void OpalCamera::send(const std::string& stream, const std::shared_ptr<dai::ADatatype>& msg) {
    auto it = in_.find(stream);
    if(it == in_.end()) return;
    try {
        it->second->send(msg);
    } catch(std::exception& e) {
        log(std::string("send ") + stream + " failed: " + e.what());
    }
}

void OpalCamera::setLed(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    if(!device_) return;
    auto b = std::make_shared<dai::Buffer>();
    // Opal's LED script: [enable, blue (GPIO 34), green (37), red (58)]
    b->setData(on ? std::vector<uint8_t>{1, 1, 1, 1} : std::vector<uint8_t>{1, 0, 0, 0});
    send("ledCtrl", b);
}

void OpalCamera::setLedEnabled(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    ledEnabled_ = on;
    if(device_ && streamingApplied_) {
        auto b = std::make_shared<dai::Buffer>();
        b->setData(on ? std::vector<uint8_t>{1, 1, 1, 1} : std::vector<uint8_t>{1, 0, 0, 0});
        send("ledCtrl", b);
    }
}

void OpalCamera::setMicGain(float times) {
    std::lock_guard<std::mutex> lk(mu_);
    if(times != micGain_) micGainDirty_ = true;
    micGain_ = times;
    cv_.notify_all();
}

void OpalCamera::setMicMuted(bool muted) {
    std::lock_guard<std::mutex> lk(mu_);
    if(muted != micMuted_) micGainDirty_ = true;
    micMuted_ = muted;
    cv_.notify_all();
}

void OpalCamera::setFaceRegion(const Face* face) {
    std::lock_guard<std::mutex> lk(mu_);
    if(!device_ || !streamingApplied_ || settings_.focusMode != FocusMode::Face || !face) return;
    // Opal moves the lens itself for faces; we let the camera's AF/AE meter on the face box instead. The region is
    // in sensor-output pixels of the ISP image (4K sensor).
    int w = 3840, h = 2160;
    auto c = std::make_shared<dai::CameraControl>();
    auto x = (uint16_t)std::max(0.f, face->xmin * w), y = (uint16_t)std::max(0.f, face->ymin * h);
    auto fw = (uint16_t)std::max(1.f, (face->xmax - face->xmin) * w), fh = (uint16_t)std::max(1.f, (face->ymax - face->ymin) * h);
    c->setAutoFocusRegion(x, y, fw, fh);
    if(settings_.exposureAuto) c->setAutoExposureRegion(x, y, fw, fh);
    c->setAutoFocusTrigger();
    send("control", c);
}

// ------------------------------------------------------------------ control messages (mirroring Opal)

void OpalCamera::sendInitialControls() {
    auto led = std::make_shared<dai::Buffer>();
    led->setData({1, 0, 0, 0});
    send("ledCtrl", led);
    auto lim = std::make_shared<dai::CameraControl>();
    lim->setAutoFocusLensLimit(120, 255);  // Opal: AF_LENS_LIMIT 120..255 right after the pipeline starts
    send("control", lim);
    networksDirty_ = true;
    sendNetworks();
    auto lens = std::make_shared<dai::CameraControl>();
    lens->setManualFocus((uint8_t)settings_.lensPosition);
    lens->setAutoFocusMode(dai::CameraControl::AutoFocusMode::OFF);
    send("control", lens);
    sendControls(settings_, settings_, true);
}

void OpalCamera::sendNetworks() {
    if(!networksDirty_) return;
    networksDirty_ = false;
    struct Net {
        const char *manip, *ctrl;
        int w, h;
        bool keepAR;
        bool on;
    };
    // each on-camera network is switched by its input ImageManip (skip frames) + NNConfig (process inputs)
    std::vector<Net> nets = {{"ctrlFaceImageManip", "ctrlFace", 300, 300, false, wantFaces_},
                             {"ctrlSegmenterImageManip", "ctrlSegmenter", 256, 144, true, wantSeg_}};
    for(auto& n : nets) {
        auto m = std::make_shared<dai::ImageManipConfig>();
        m->setResize(n.w, n.h);
        m->setKeepAspectRatio(n.keepAR);
        m->setSkipCurrentImage(!n.on);
        send(n.manip, m);
        auto c = std::make_shared<dai::NNConfig>();
        c->setProcessInputs(n.on);
        send(n.ctrl, c);
    }
    // hand pose: palm-detection (128x128) and landmark (256x256) manips, the NN config and the manager script's on/off
    // ("choke") buffer
    // (the template loader gave the landmark manip's input its own name: ...ImageManip2)
    for(auto mw : {std::make_pair("ctrlGesturesPrePdImageManip", 128), std::make_pair("ctrlGesturesPrePdImageManip2", 256)}) {
        auto m = std::make_shared<dai::ImageManipConfig>();
        m->setResize(mw.second, mw.second);
        m->setKeepAspectRatio(true);
        m->setSkipCurrentImage(!wantHands_);
        send(mw.first, m);
    }
    auto hc = std::make_shared<dai::NNConfig>();
    hc->setProcessInputs(wantHands_);
    send("ctrlGestures", hc);
    auto choke = std::make_shared<dai::Buffer>();
    choke->setData({(uint8_t)(wantHands_ ? 1 : 0)});
    send("gesturesCtrlQueueChokeEmbedded", choke);
    appliedFaces_ = wantFaces_;
    appliedSeg_ = wantSeg_;
    appliedHands_ = wantHands_;
}

void OpalCamera::sendStreaming(bool on) {
    auto led = std::make_shared<dai::Buffer>();
    led->setData(on && ledEnabled_ ? std::vector<uint8_t>{1, 1, 1, 1} : std::vector<uint8_t>{1, 0, 0, 0});
    auto c = std::make_shared<dai::CameraControl>();
    if(on) {
        send("ledCtrl", led);
        // Opal: START_STREAM + MOVE_LENS + AE_AUTO + AWB_MODE auto + SCENE_MODE face priority + EXP_COMP
        //       + CONTROL_MODE auto + SATURATION in one message
        c->setStartStreaming();
        c->setManualFocus((uint8_t)settings_.lensPosition);
        c->setAutoExposureEnable();
        c->setAutoWhiteBalanceMode(dai::CameraControl::AutoWhiteBalanceMode::AUTO);
        c->setSceneMode(dai::CameraControl::SceneMode::FACE_PRIORITY);
        c->setAutoExposureCompensation(settings_.exposureCompensation);
        c->setControlMode(dai::CameraControl::ControlMode::AUTO);
        c->setSaturation(settings_.saturation);
        send("control", c);
        // then the user's manual settings / focus mode on top
        sendControls(settings_, settings_, true);
    } else {
        // Opal's order: light off, then stop the sensor. The light script on the camera reads one message per second;
        // the off is repeated a little later (ledResendAt_) in case one was missed.
        send("ledCtrl", led);
        c->setStopStreaming();
        send("control", c);
        ledResendAt_ = now() + 1.5;
        ledResends_ = 0;
        framesAfterOff_ = 0;
    }
    streamingApplied_ = on;
    if(on) ledResendAt_ = -1;
    log(on ? "sensor on" : "sensor off");
}

void OpalCamera::sendControls(const CameraSettings& prev, const CameraSettings& cur, bool force) {
    auto c = std::make_shared<dai::CameraControl>();
    bool any = false;
    if(force || prev.exposureAuto != cur.exposureAuto || (!cur.exposureAuto && (prev.exposureUs != cur.exposureUs || prev.iso != cur.iso))) {
        if(cur.exposureAuto) c->setAutoExposureEnable();
        else c->setManualExposure((uint32_t)cur.exposureUs, (uint32_t)cur.iso);
        any = true;
    }
    if(force || prev.exposureCompensation != cur.exposureCompensation) {
        c->setAutoExposureCompensation(cur.exposureCompensation);
        any = true;
    }
    if(force || prev.saturation != cur.saturation) {
        c->setSaturation(cur.saturation);
        any = true;
    }
    if(force || prev.whiteBalanceAuto != cur.whiteBalanceAuto || (!cur.whiteBalanceAuto && prev.whiteBalanceK != cur.whiteBalanceK)) {
        if(cur.whiteBalanceAuto) {
            c->setAutoWhiteBalanceMode(dai::CameraControl::AutoWhiteBalanceMode::AUTO);
            c->setSceneMode(dai::CameraControl::SceneMode::FACE_PRIORITY);
            c->setControlMode(dai::CameraControl::ControlMode::AUTO);
        } else {
            c->setManualWhiteBalance(cur.whiteBalanceK);
        }
        any = true;
    }
    if(force || prev.antiBanding != cur.antiBanding) {
        c->setAntiBandingMode((dai::CameraControl::AntiBandingMode)cur.antiBanding);
        any = true;
    }
    if(force || prev.focusMode != cur.focusMode || (cur.focusMode == FocusMode::Manual && prev.lensPosition != cur.lensPosition)) {
        if(cur.focusMode == FocusMode::Manual) {
            c->setManualFocus((uint8_t)cur.lensPosition);
            c->setAutoFocusMode(dai::CameraControl::AutoFocusMode::OFF);
        } else {
            // Opal: AF_TRIGGER + AF_MODE auto (one-shot) + lens range; face mode re-triggers on the face box
            c->setAutoFocusMode(dai::CameraControl::AutoFocusMode::AUTO);
            c->setAutoFocusTrigger();
        }
        any = true;
    }
    if(any) send("control", c);
}

// ------------------------------------------------------------------ device loop

void OpalCamera::run() {
    // depthai silences XLink (USB layer) on init; keep its info/warnings, they explain connection problems
    dai::initialize();
    mvLogDefaultLevelSet(MVLOG_INFO);
    while(!quit_) {
        bool ran = false;
        try {
            ran = connectAndRun();
        } catch(std::exception& e) {
            log(std::string("camera error: ") + e.what());
            std::lock_guard<std::mutex> lk(mu_);
            status_ = std::string("Camera error: ") + e.what();
            state_ = CameraState::Error;
        }
        std::unique_ptr<dai::Device> dead;
        {
            std::lock_guard<std::mutex> lk(mu_);
            in_.clear();
            out_.clear();
            dead = std::move(device_);
            streamingApplied_ = false;
        }
        if(dead) {
            // closing a dead link can block inside XLink; never hold the lock (the UI calls in) and don't wait forever
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::thread([d = dead.release(), done]() mutable {
                try {
                    delete d;
                } catch(...) {
                }
                *done = true;
            }).detach();
            for(int i = 0; i < 100 && !*done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if(!*done) {
                log("closing the previous connection is stuck (USB library wedged)");
                wedged_ = true;
                return;  // nothing more can be done in this process
            }
        }
        if(quit_) break;
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(ran ? 500 : 1500), [this] { return quit_.load(); });
    }
}

bool OpalCamera::connectAndRun() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = CameraState::Searching;
        status_ = "Looking for the Opal C1";
    }
    auto devices = dai::Device::getAllAvailableDevices();
    if(devices.empty()) return false;
    {
        static std::string lastSeen;
        std::string seen;
        for(auto& d : devices) seen += d.getMxId() + "=" + XLinkDeviceStateToStr(d.state) + " ";
        if(seen != lastSeen) log("devices: " + seen);
        lastSeen = seen;
    }
    // a camera left running Opal's firmware by a previous session must be reset first
    dai::DeviceInfo info = devices.front();
    bool usable = false;
    for(auto& d : devices)
        if(d.state != X_LINK_BOOTED) {
            info = d;
            usable = true;
            break;
        }
    if(!usable) {
        log("camera is still running a previous session, resetting it");
        try {
            dai::XLinkConnection::bootBootloader(info);
        } catch(std::exception& e) {
            log(std::string("reset failed: ") + e.what());
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = CameraState::Booting;
        status_ = "Starting the camera";
        restartRequested_ = false;
        applied_ = settings_;
    }
    PipelineOptions opt;
    opt.resolution = applied_.resolution;
    opt.lensPosition = applied_.lensPosition;
    opt.antiBanding = applied_.antiBanding;
    // Opal's template sends every message as one USB transfer (chunk size 0). The app sets its own (Windows: settings)
    opt.xlinkChunkSize = chunkSize_;
    log("xlink chunk size " + std::to_string(chunkSize_.load()));
    auto pipeline = tmpl_.build(opt);
    log("booting camera " + info.getMxId());
    auto t0 = std::chrono::steady_clock::now();
    auto dev = std::make_unique<dai::Device>(pipeline, info);
    log("device opened in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()) + " ms");
    log("camera up: " + std::string(dev->getUsbSpeed() == dai::UsbSpeed::SUPER || dev->getUsbSpeed() == dai::UsbSpeed::SUPER_PLUS ? "USB 3" : "USB 2"));
    // only the camera's warnings: its info log (a line per frame) would share the USB link with the video
    dev->setLogLevel(dai::LogLevel::WARN);
    dev->setLogOutputLevel(dai::LogLevel::WARN);

    std::map<std::string, std::shared_ptr<dai::DataInputQueue>> ins;
    for(auto& s : tmpl_.inputStreams()) ins[s] = dev->getInputQueue(s, 8, false);
    std::vector<std::shared_ptr<dai::DataOutputQueue>> outs;
    for(auto& s : tmpl_.outputStreams()) {
        auto q = dev->getOutputQueue(s, 4, false);  // every output must be drained or the camera's sender stalls
        outs.push_back(q);
        if(s == "rgb") {
            q->addCallback([this](std::shared_ptr<dai::ADatatype> m) {
                auto img = std::dynamic_pointer_cast<dai::ImgFrame>(m);
                framesIn_++;
                if(!streamingApplied_) framesAfterOff_++;
                if(!img || !onFrame) return;
                auto f = std::make_shared<Frame>();
                f->width = (int)img->getWidth();
                f->height = (int)img->getHeight();
                f->data = std::move(img->getData());
                f->sequence = img->getSequenceNum();
                f->timestamp = std::chrono::duration<double>(img->getTimestamp().time_since_epoch()).count();
                f->lensPosition = img->getLensPosition();
                f->exposureUs = img->getExposureTime();
                f->iso = img->getSensitivity();
                onFrame(f);
            });
        } else if(s == "face_detection") {
            q->addCallback([this](std::shared_ptr<dai::ADatatype> m) {
                auto d = std::dynamic_pointer_cast<dai::ImgDetections>(m);
                if(!d || !onFaces) return;
                std::vector<Face> faces;
                for(auto& det : d->detections) faces.push_back({det.xmin, det.ymin, det.xmax, det.ymax, det.confidence});
                onFaces(faces, std::chrono::duration<double>(d->getTimestamp().time_since_epoch()).count());
            });
        } else if(s == "google_meet_segmentation") {
            q->addCallback([this](std::shared_ptr<dai::ADatatype> m) {
                auto nn = std::dynamic_pointer_cast<dai::NNData>(m);
                if(!nn || !onMask) return;
                // Google Meet segmentation: 2 channels (background, person) at 256x144, planar (CHW), FP16
                static bool logged = false;
                if(!logged) {
                    logged = true;
                    for(auto& t : nn->getAllLayers()) {
                        std::ostringstream ss;
                        ss << "segmentation layer " << t.name << " dims";
                        for(auto d : t.dims) ss << " " << d;
                        ss << " order " << (int)t.order << " type " << (int)t.dataType;
                        log(ss.str());
                    }
                }
                auto v = nn->getFirstLayerFp16();
                const int w = 256, h = 144, n = w * h;
                if((int)v.size() < n) return;
                auto mask = std::make_shared<Mask>();
                mask->width = w;
                mask->height = h;
                mask->data.resize(2 * n);
                auto q8 = [](float x) { return (uint8_t)std::lround(std::min(1.f, std::max(0.f, x)) * 255.f); };
                if((int)v.size() >= 2 * n) {
                    for(int i = 0; i < 2 * n; i++) mask->data[i] = q8(v[i]);
                } else {  // single-channel model: treat it as the person channel
                    for(int i = 0; i < n; i++) {
                        mask->data[n + i] = q8(v[i]);
                        mask->data[i] = 255 - mask->data[n + i];
                    }
                }
                mask->timestamp = std::chrono::duration<double>(nn->getTimestamp().time_since_epoch()).count();
                onMask(mask);
            });
        } else if(s == "handpose_python_manager") {
            q->addCallback([this](std::shared_ptr<dai::ADatatype> m) {
                auto nn = std::dynamic_pointer_cast<dai::NNData>(m);
                if(!nn || !onHand) return;
                Hand hand;
                auto type = nn->getLayerFp16("type");
                auto score = nn->getLayerFp16("lm_score");
                auto lms = nn->getLayerFp16("sqn_lms");
                hand.valid = !type.empty() && type[0] > 0 && lms.size() >= 63;
                if(!score.empty()) hand.score = score[0];
                auto hd = nn->getLayerFp16("handedness");
                if(!hd.empty()) hand.handedness = hd[0];
                if(hand.valid)
                    for(int i = 0; i < 21; i++)
                        for(int k = 0; k < 3; k++) hand.landmarks[i][k] = lms[3 * i + k];
                onHand(hand);
            });
        } else if(s == "sysinfo") {
            q->addCallback([this](std::shared_ptr<dai::ADatatype> m) {
                auto si = std::dynamic_pointer_cast<dai::SystemInformation>(m);
                if(!si || !onTelemetry) return;
                Telemetry t;
                t.chipTempC = si->chipTemperature.average;
                t.cssCpu = si->leonCssCpuUsage.average;
                t.mssCpu = si->leonMssCpuUsage.average;
                t.ddrUsed = si->ddrMemoryUsage.used;
                t.ddrTotal = si->ddrMemoryUsage.total;
                onTelemetry(t);
            });
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        device_ = std::move(dev);
        in_ = ins;
        out_ = outs;
        state_ = CameraState::Running;
        status_ = "Connected";
        sendInitialControls();
        streamingApplied_ = false;
        micGainDirty_ = true;
    }

    // control loop: apply streaming / network / setting changes, watch for disconnects and restarts
    while(!quit_) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(200));
        {
            static auto lastStats = std::chrono::steady_clock::now();
            auto t = std::chrono::steady_clock::now();
            if(t - lastStats >= std::chrono::seconds(10)) {
                double secs = std::chrono::duration<double>(t - lastStats).count();
                if(streamingApplied_) log("stats: camera frames arriving " + std::to_string((int)(framesIn_.exchange(0) / secs + 0.5)) + " fps");
                else framesIn_ = 0;
                lastStats = t;
            }
        }
        if(device_->isClosed()) {
            status_ = "Camera disconnected";
            log("camera disconnected");
            return true;
        }
        if(restartRequested_) {
            log("restarting pipeline (resolution change)");
            return true;  // run() tears the device down outside the lock
        }
        if(offRequestedAt_ >= 0 && now() - offRequestedAt_ >= kOffHold) {
            offRequestedAt_ = -1;
            wantStreaming_ = false;
        }
        if(wantStreaming_ != streamingApplied_) sendStreaming(wantStreaming_);
        if(!streamingApplied_ && ledResendAt_ >= 0 && now() >= ledResendAt_) {
            auto led = std::make_shared<dai::Buffer>();
            led->setData({1, 0, 0, 0});
            send("ledCtrl", led);
            // the camera should have stopped sending video by now
            int late = framesAfterOff_.exchange(0);
            if(late > 0) log("camera still sent " + std::to_string(late) + " frames after the sensor was stopped");
            ledResendAt_ = ++ledResends_ < 2 ? now() + 2.5 : -1;  // again at about 4 s, then done
        }
        if(networksDirty_) sendNetworks();
        if(micGainDirty_) {
            micGainDirty_ = false;
            auto a = std::make_shared<dai::AudioInConfig>();
            a->setMicGainTimes(micGain_);
            a->setDisableOutput(micMuted_);  // what Opal sends for mute (captured: micGain 31.6, disableOutput 1)
            send("audioControl", a);
        }
        if(streamingApplied_) {
            sendControls(applied_, settings_, false);
            applied_ = settings_;
        }
    }
    return true;
}

}  // namespace opal
