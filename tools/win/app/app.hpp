// app: shared state of the Opal for Windows tray app (main.cpp owns it; ui.cpp draws the popover from a UiHost).
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../core/opal_camera.hpp"
#include "effects.hpp"
#include "framing.hpp"
#include "gestures.hpp"
#include "overlay.hpp"
#include "settings.hpp"
#include "vcam_producer.hpp"

namespace opal {

struct App {
    AppSettings settings;
    bool settingsDirty = false;
    double settingsChangedAt = 0;

    std::unique_ptr<OpalCamera> camera;
    Effects effects;
    Overlay overlay;
    Framing framing;
    Gestures gestures;
    VirtualCameraProducer vcam;
    std::string vcamError, effectsError, resourcesDir;

    // latest data from the camera threads
    std::mutex mu;
    std::shared_ptr<const Frame> frame;
    std::shared_ptr<const Mask> mask;
    std::vector<Face> faces;
    double facesAt = 0;
    Hand hand;
    bool handNew = false;
    Telemetry telemetry;
    double telemetryAt = -10;
    std::deque<std::string> log;
    HANDLE frameEvent = nullptr;

    // derived
    bool vcamInUse = false;
    double peaceAt = -10;
    int64_t lastFrameSeq = -1;
    double lastFrameTime = 0;
    int lastLens = 126;
    float fps = 0;
    int frameWidth = 0, frameHeight = 0;
    float pcCpu = -1;  // Opal.exe CPU, % of one core (status readout)
    double cpuSampleAt = 0;
    unsigned long long cpuSampleTime = 0;
    float histogram[64] = {};

    // window / graphics
    HWND hwnd = nullptr;
    bool popoverVisible = false, modalOpen = false;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain1* swapChain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    float dpiScale = 1.f;
    bool fontsDirty = true, themeDirty = true;
    double popoverHiddenAt = -10;  // when a click outside closed the popover (a click on the tray icon then must not reopen it)
    int tab = 0;
    bool readbackPending = false;
    bool quit = false;

    void addLog(const std::string& s);
    void changed();  // a setting changed: apply + save soon
    CameraSettings cameraSettings() const;
};

void applySettings(App& app);
void setStartAtLogin(bool on);

}  // namespace opal
