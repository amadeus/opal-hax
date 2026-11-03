// opal_camera: drives an Opal C1 in "app mode" the way Opal.app's camera service does: finds the camera (whatever
// mode it is in), boots Opal's firmware with Opal's captured pipeline, turns the sensor on/off, switches the on-camera
// networks (faces, background segmentation, hand pose) on demand, and forwards camera controls. Portable C++ (used by
// the Windows app; also builds on macOS for testing).
//
// Control sequences mirror what Opal sends (decoded from the pipecap capture, see tools/win/README.md).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "opal_pipeline.hpp"

namespace opal {

struct Frame {  // NV12, stride == width
    int width = 0, height = 0;
    std::vector<uint8_t> data;
    int64_t sequence = 0;
    double timestamp = 0;  // seconds, device clock synced to host steady clock
    int lensPosition = 0, exposureUs = 0, iso = 0;
};

struct Face {
    float xmin, ymin, xmax, ymax;  // normalized to the camera's preview (same framing as the video)
    float confidence;
};

struct Mask {  // Google Meet segmentation output as Opal uses it: 2 planes of width x height, row-major, 8 bit
    // plane 0 = channel 0 (background), plane 1 = channel 1 (person); raw values clamped to 0..1 and quantized,
    // exactly like Opal's video service (it crops the FP16 layer to two R8 images, no softmax)
    int width = 0, height = 0;
    std::vector<uint8_t> data;  // 2 * width * height
    double timestamp = 0;
};

struct Hand {  // hand landmarks from Opal's on-camera hand-pose manager
    bool valid = false;
    float score = 0, handedness = 0;
    float landmarks[21][3] = {};  // x, y normalized to the 16:9 frame, z relative
    double timestamp = 0;
};

struct Telemetry {
    float chipTempC = 0, cssCpu = 0, mssCpu = 0;
    int64_t ddrUsed = 0, ddrTotal = 0;
};

enum class CameraState { Searching, Booting, Running, Error };

enum class FocusMode { Auto, Face, Manual };

struct CameraSettings {
    Resolution resolution = Resolution::R1080;
    // exposure
    bool exposureAuto = true;
    int exposureUs = 33000, iso = 400;  // manual
    int exposureCompensation = 0;       // -3..3 (Opal's slider), auto exposure only
    int saturation = 0;                 // -10..10
    // white balance
    bool whiteBalanceAuto = true;
    int whiteBalanceK = 5000;  // manual, 1000..12000
    // focus
    FocusMode focusMode = FocusMode::Face;
    int lensPosition = 126;  // manual (0..255; Opal limits it to 120..255)
    int antiBanding = 3;     // dai::RawCameraControl::AntiBandingMode, 3 = auto
};

class OpalCamera {
   public:
    explicit OpalCamera(const std::string& templateDir);
    ~OpalCamera();

    void start();  // background thread: find, boot, run, reconnect on unplug
    void stop();

    // sensor on/off. "Off" is held a few seconds (avoids flapping when apps reopen the camera).
    void setStreaming(bool on);
    // which on-camera networks are needed (by the effects in use)
    void setNetworks(bool faces, bool segmentation, bool hands);
    void setLed(bool on);
    void setLedEnabled(bool on);       // LED lights while the sensor runs (Opal's behaviour) unless disabled
    void setMicGain(float times);  // microphone gain, linear (Opal's slider: 1..31.6 = 0..30 dB)
    // microphone mute: the camera stops sending audio (Opal's isMuteOn -> AudioInConfig.disableOutput)
    void setMicMuted(bool muted);
    // XLink chunk size for camera->host messages (bytes; 0 = whole message per USB transfer, Opal's value). Applies
    // at the next (re)connect.
    void setXLinkChunkSize(int bytes) { chunkSize_ = bytes; }
    // applies changed controls immediately; a resolution change restarts the pipeline
    void apply(const CameraSettings& s);
    // face-driven focus/exposure needs the latest face box
    void setFaceRegion(const Face* face);

    CameraState state() const { return state_; }
    std::string statusText() const;
    bool streaming() const { return streamingApplied_; }
    // set when a dead connection could not be closed: XLink is wedged in this process; the app should restart itself
    bool wedged() const { return wedged_; }

    std::function<void(std::shared_ptr<const Frame>)> onFrame;
    std::function<void(const std::vector<Face>&, double)> onFaces;
    std::function<void(std::shared_ptr<const Mask>)> onMask;
    std::function<void(const Hand&)> onHand;
    std::function<void(const Telemetry&)> onTelemetry;
    std::function<void(const std::string&)> onLog;

   private:
    void run();
    bool connectAndRun();
    void sendInitialControls();
    void sendStreaming(bool on);
    void sendNetworks();
    void sendControls(const CameraSettings& prev, const CameraSettings& cur, bool force);
    void log(const std::string& s);
    void send(const std::string& stream, const std::shared_ptr<dai::ADatatype>& msg);

    PipelineTemplate tmpl_;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> wedged_{false};
    std::atomic<int> chunkSize_{-1};
    std::atomic<int> framesIn_{0};
    std::atomic<CameraState> state_{CameraState::Searching};
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::unique_ptr<dai::Device> device_;
    std::map<std::string, std::shared_ptr<dai::DataInputQueue>> in_;
    std::vector<std::shared_ptr<dai::DataOutputQueue>> out_;
    CameraSettings settings_, applied_;
    bool restartRequested_ = false;
    bool wantStreaming_ = false, streamingApplied_ = false;
    double offRequestedAt_ = -1;
    double ledResendAt_ = -1;  // "light off" sent again 1.5 s and 4 s after the sensor stops
    int ledResends_ = 0;
    std::atomic<int> framesAfterOff_{0};
    bool wantFaces_ = false, wantSeg_ = false, wantHands_ = false;
    bool appliedFaces_ = false, appliedSeg_ = false, appliedHands_ = false;
    bool networksDirty_ = true;
    bool ledEnabled_ = true;
    float micGain_ = 25.1f;  // +28 dB, Opal's default
    bool micMuted_ = false;
    bool micGainDirty_ = true;
    std::string status_;
};

}  // namespace opal
