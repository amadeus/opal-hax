// settings: everything the user can change, persisted as JSON in %APPDATA%\Opal\settings.json.
// Field names follow Opal.app's OC* defaults keys where there is one.
#pragma once
#include <string>

namespace opal {

struct AppSettings {
    // camera
    std::string resolution = "1080p";  // 4K, 1440p, 1080p, 720p
    bool exposureAuto = true;
    float exposure = 0.5f;  // manual exposure slider 0..1 (ISO at 33 ms, then shorter exposure at ISO 100, like Opal)
    int exposureCompensation = 0;  // -3..3
    int saturation = 0;            // -10..10
    bool whiteBalanceAuto = true;
    int whiteBalanceK = 5000;  // Opal's slider: 2500..7000 K
    std::string focusMode = "face";  // face, auto, manual
    int lensPosition = 126;
    bool focusLock = false;  // OCIsFocusLockOn: keep the current lens position
    int antiBanding = 3;     // 0 off, 1 50 Hz, 2 60 Hz, 3 auto
    float micGainDb = 28.f;  // Opal's default mic gain (+28 dB)
    bool micMuted = false;   // Opal's isMuteOn (the camera stops sending audio)
    // looks + effects (OC*)
    // (ranges and defaults are Opal's, from its settings table)
    bool filtersOn = true;
    std::string filter = "I2";  // OCSelectedFilter
    float filterStrength = 1.f;  // OCFilters 0..1
    bool mirror = true;          // OCIsMirrorOn
    bool bokeh = false;          // OCIsBokehOn
    float bokehRadius = 7.6f;    // OCBokeh 0.4..11 (blur radius in pixels at 1080p; scaled with the camera resolution)
    bool pixellate = false;      // OCIsPixellateOn
    int pixellateLevel = 0;      // OCPixellate 0..4 (block = {4,8,10,20,40} x resolution multiple)
    bool spotlight = false;      // OCIsSpotlightOn (no amount)
    bool touchUp = false;        // OCIsTouchUpOn
    float touchUpAmount = 0.152f;  // OCTouchUp 0.1..0.5
    bool skin = false;             // OCIsSkinAdjustmentOn
    float skinAmount = 0.2f;       // OCSkinAdjustment 0..0.5 (warmth follows the white balance, as in Opal)
    bool highlights = false;
    float highlightsAmount = 0.f;  // OCHighlights -1..1
    bool shadows = false;
    float shadowsAmount = 0.f;  // OCShadows -1..1
    bool zoom = false;          // OCIsZoomOn
    float zoomAmount = 0.15f;   // OCZoom: crop = frame / (1 + 1.5 x)
    bool faceLock = false;      // OCIsFaceLockOn (auto-framing)
    float faceLockAmount = 0.25f;  // OCFaceLock: same crop size, centred on the faces
    bool histogram = false;
    // overlays
    bool text = false;  // OCIsTextOn
    std::string textLine1, textLine2;
    bool logo = false;  // OCIsLogoOn
    std::string logoPath;
    bool logoSideSwapped = false;
    // gestures
    bool gestureDraw = false, gesturePeace = false, gesturePinch = false;
    // app
    bool startAtLogin = true;
    // camera->PC USB transfer size in KB; 0 = whole message per transfer (Opal's value). 64 KB pieces capped video
    // at ~21 fps on the owner's PC; the drops once blamed on whole-frame transfers were the duplicate-stream bug.
    int usbTransferKB = 0;

    static std::string path();
    void load();
    void save() const;
};

}  // namespace opal
