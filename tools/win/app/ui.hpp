// ui: the popover's contents (Windows 11 style, see fluent.hpp), laid out like Opal.app's popover: live preview on top,
// then Camera / Effects / Manual / Audio / Settings, and a footer with the camera status.
// No Windows headers: main.cpp fills UiHost on Windows, preview/ fills it on macOS for screenshots.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "settings.hpp"

namespace opal {

struct UiHost {
    AppSettings* settings = nullptr;
    std::vector<std::string> looks;  // Opal's short look names
    ImTextureID preview = 0;         // live picture (16:9); 0 until the first frame
    ImTextureID lookThumbs = 0;      // the Looks strip's pictures: 1 + looks.size() tiles side by side (none, then each look)
    std::string cameraStatus;
    std::string vcamError, effectsError;
    bool vcamInUse = false, cameraStreaming = false;
    float fps = 0, chipTempC = 0, cssCpu = 0, mssCpu = 0;
    // status readout over the preview (like the Mac app's): telemetry younger than 5 s, frame size, Opal.exe's CPU
    bool telemetryFresh = false;
    float ddrUsedMB = 0, ddrTotalMB = 0;
    int frameWidth = 0, frameHeight = 0;
    float pcCpu = -1;  // % of one core, -1 = unknown
    const float* histogram = nullptr;  // 64 bins
    int lastLens = 126;
    float wheelLines = 3;  // Windows' "lines to scroll" setting
    int tab = 0;
    std::function<void()> changed, quit, chooseLogo, startAtLoginChanged;
};

void drawUI(UiHost& host);

}  // namespace opal
