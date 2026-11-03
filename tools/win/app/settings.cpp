#include "settings.hpp"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

namespace opal {

#define OPAL_SETTINGS_FIELDS(X)                                                                                                       \
    X(resolution) X(exposureAuto) X(exposure) X(exposureCompensation) X(saturation) X(whiteBalanceAuto) X(whiteBalanceK) X(focusMode)     \
    X(lensPosition) X(focusLock) X(antiBanding) X(micGainDb) X(micMuted) X(filtersOn) X(filter) X(filterStrength) X(mirror) X(bokeh) X(bokehRadius) \
    X(pixellate) X(pixellateLevel) X(spotlight) X(touchUp) X(touchUpAmount) X(skin) X(skinAmount)                                   \
    X(highlights) X(highlightsAmount) X(shadows) X(shadowsAmount) X(zoom) X(zoomAmount) X(faceLock) X(faceLockAmount) X(histogram)    \
    X(text) X(textLine1) X(textLine2) X(logo) X(logoPath) X(logoSideSwapped) X(gestureDraw) X(gesturePeace) X(gesturePinch)          \
    X(startAtLogin) X(usbTransferKB)

std::string AppSettings::path() {
    PWSTR appdata = nullptr;
    std::wstring dir;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        dir = std::wstring(appdata) + L"\\Opal";
        CoTaskMemFree(appdata);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring p = dir + L"\\settings.json";
    int n = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

void AppSettings::load() {
    std::ifstream f(std::filesystem::u8path(path()));
    if(!f) return;
    nlohmann::json j;
    try {
        f >> j;
    } catch(...) {
        return;
    }
#define X(name) \
    if(j.contains(#name)) try { j.at(#name).get_to(name); } catch(...) {}
    OPAL_SETTINGS_FIELDS(X)
#undef X
}

void AppSettings::save() const {
    nlohmann::json j;
#define X(name) j[#name] = name;
    OPAL_SETTINGS_FIELDS(X)
#undef X
    std::ofstream f(std::filesystem::u8path(path()));
    f << j.dump(2);
}

}  // namespace opal
