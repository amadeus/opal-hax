// Opal for Windows: tray app that drives the Opal C1 like Opal.app does on the Mac and publishes the result as the
// "Opal C1" virtual camera. Left-click the tray icon for the popover; right-click for the menu.
#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>
#include <commdlg.h>
#include <io.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

#include <dwmapi.h>

#include "app.hpp"
#include "fluent.hpp"
#include "installer.hpp"
#include "ui.hpp"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace opal;

static App gApp;
extern "C" const char* opal_xlink_unbound_interface(void);  // patched XLink (tools/win/xlink/patch-xlink.py)
extern "C" const char* opal_xlink_unbound_service(void);
static const UINT WM_TRAY = WM_APP + 1;
static const UINT WM_SHOW_POPOVER = WM_APP + 2;
static const UINT kTrayId = 1;
static const int kPopW = 400, kPopH = 740;  // logical pixels (Windows 11 flyout-sized)

static double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/"));
}

static std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ------------------------------------------------------------------ App helpers

void App::addLog(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu);
    log.push_back(s);
    while(log.size() > 300) log.pop_front();
    std::string p = AppSettings::path();
    p = p.substr(0, p.find_last_of("\\/")) + "\\opal.log";
    std::ofstream f(std::filesystem::u8path(p), std::ios::app);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char ts[32];
    snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    f << ts << s << "\n";
}

void App::changed() {
    settingsDirty = true;
    settingsChangedAt = nowSec();
    applySettings(*this);
}

CameraSettings App::cameraSettings() const {
    const auto& s = settings;
    CameraSettings c;
    c.resolution = s.resolution == "4K" ? Resolution::R4K : s.resolution == "1440p" ? Resolution::R1440 : s.resolution == "720p" ? Resolution::R720 : Resolution::R1080;
    c.exposureAuto = s.exposureAuto;
    // Opal's single exposure slider: lower half shortens the shutter at ISO 100, upper half raises ISO at 33 ms
    float e = std::clamp(s.exposure, 0.f, 1.f);
    if(e < 0.5f) {
        float t = e / 0.5f;
        c.exposureUs = (int)(1000 + 32000 * t * t);
        c.iso = 100;
    } else {
        c.exposureUs = 33000;
        c.iso = (int)(100 + 1500 * (e - 0.5f) / 0.5f);
    }
    c.exposureCompensation = s.exposureCompensation;
    c.saturation = s.saturation;
    c.whiteBalanceAuto = s.whiteBalanceAuto;
    c.whiteBalanceK = s.whiteBalanceK;
    c.focusMode = s.focusLock || s.focusMode == "manual" ? FocusMode::Manual : s.focusMode == "auto" ? FocusMode::Auto : FocusMode::Face;
    c.lensPosition = s.lensPosition;
    c.antiBanding = s.antiBanding;
    return c;
}

void opal::applySettings(App& app) {
    if(!app.camera) return;
    const auto& s = app.settings;
    app.camera->apply(app.cameraSettings());
    app.camera->setMicGain(std::pow(10.f, s.micGainDb / 20.f));
    app.camera->setMicMuted(s.micMuted);
}

void opal::setStartAtLogin(bool on) {
    HKEY k;
    if(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if(on) {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(p) + L"\" --background";
        RegSetValueExW(k, L"Opal", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"Opal");
    }
    RegCloseKey(k);
}

// ------------------------------------------------------------------ graphics

static bool createDevice(App& app) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // Direct2D overlay
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got;
    if(FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &app.device, &got, &app.context))) return false;
    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    app.device->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    dxgiDev->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = factory->CreateSwapChainForHwnd(app.device, app.hwnd, &sd, nullptr, nullptr, &app.swapChain);
    factory->Release();
    adapter->Release();
    dxgiDev->Release();
    return SUCCEEDED(hr);
}

static void createRTV(App& app) {
    ID3D11Texture2D* back = nullptr;
    app.swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    app.device->CreateRenderTargetView(back, nullptr, &app.rtv);
    back->Release();
}

static void resize(App& app, UINT w, UINT h) {
    if(!app.swapChain || w == 0 || h == 0) return;
    if(app.rtv) app.rtv->Release(), app.rtv = nullptr;
    app.swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    createRTV(app);
}

// ------------------------------------------------------------------ theme

// Windows' app theme (Settings > Personalization > Colors) and accent palette
static void applyTheme(App& app) {
    DWORD light = 1, sz = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD,
                 nullptr, &light, &sz);
    bool dark = light == 0;
    // AccentPalette: 8 RGBA entries, lightest to darkest; WinUI uses entry 1 ("Light 2") on dark and 4 ("Dark 1") on light
    BYTE pal[32] = {};
    DWORD psz = sizeof pal;
    ImU32 accDark = 0, accLight = 0;
    if(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette", RRF_RT_REG_BINARY, nullptr,
                    pal, &psz) == ERROR_SUCCESS &&
       psz >= 32) {
        accDark = IM_COL32(pal[4], pal[5], pal[6], 255);
        accLight = IM_COL32(pal[16], pal[17], pal[18], 255);
    }
    fluent::setTheme(dark, accDark, accLight);
    fluent::applyStyle();
    BOOL d = dark;
    DwmSetWindowAttribute(app.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &d, sizeof d);
}

static void loadFonts(App& app) {
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    std::string dir = utf8(win) + "\\Fonts\\";
    fluent::setScale(app.dpiScale);
    fluent::applyStyle();
    // Segoe UI (the Windows 11 UI font at the sizes WinUI uses; 1.33 = its ascender + descender per em)
    if(!fluent::loadFonts((dir + "segoeui.ttf").c_str(), (dir + "seguisb.ttf").c_str(), 1.33f)) app.addLog("ui: font atlas failed");
}

// ------------------------------------------------------------------ popover / tray

static void positionPopover(App& app) {
    UINT dpi = GetDpiForWindow(app.hwnd);
    if(dpi / 96.f != app.dpiScale) app.fontsDirty = true;
    app.dpiScale = dpi / 96.f;
    int w = (int)(kPopW * app.dpiScale), h = (int)(kPopH * app.dpiScale);
    NOTIFYICONIDENTIFIER id = {sizeof id};
    id.hWnd = app.hwnd;
    id.uID = kTrayId;
    RECT icon = {};
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.right - w - 12, y = work.bottom - h - 12;
    if(SUCCEEDED(Shell_NotifyIconGetRect(&id, &icon))) {
        HMONITOR mon = MonitorFromRect(&icon, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(mon, &mi);
        work = mi.rcWork;
        x = std::clamp((int)((icon.left + icon.right) / 2 - w / 2), (int)work.left + 8, (int)work.right - w - 8);
        y = icon.top > (work.top + work.bottom) / 2 ? work.bottom - h - 8 : work.top + 8;  // taskbar bottom or top
    }
    SetWindowPos(app.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

static void showPopover(App& app, bool show) {
    if(show == app.popoverVisible) return;
    app.popoverVisible = show;
    if(show) {
        positionPopover(app);
        ShowWindow(app.hwnd, SW_SHOW);
        SetForegroundWindow(app.hwnd);
    } else {
        ShowWindow(app.hwnd, SW_HIDE);
        if(app.settingsDirty) app.settings.save(), app.settingsDirty = false;
    }
}

static void trayMenu(App& app) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open Opal");
    AppendMenuW(m, MF_STRING | (app.settings.micMuted ? MF_CHECKED : 0), 5, L"Mute microphone");
    AppendMenuW(m, MF_STRING | (app.settings.startAtLogin ? MF_CHECKED : 0), 2, L"Start at login");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, L"Uninstall Opal...");
    AppendMenuW(m, MF_STRING, 3, L"Quit");
    SetForegroundWindow(app.hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, app.hwnd, nullptr);
    DestroyMenu(m);
    if(cmd == 1) showPopover(app, true);
    if(cmd == 2) {
        app.settings.startAtLogin = !app.settings.startAtLogin;
        setStartAtLogin(app.settings.startAtLogin);
        app.settings.save();
    }
    if(cmd == 3) app.quit = true;
    if(cmd == 5) {
        app.settings.micMuted = !app.settings.micMuted;
        app.changed();
    }
    if(cmd == 4) {
        app.camera->stop();
        startUninstall();
    }
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if(ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return true;
    App& app = gApp;
    switch(msg) {
        case WM_TRAY:
            // NOTIFYICON_VERSION_4: a left click sends WM_LBUTTONUP *and* NIN_SELECT, so only NIN_SELECT toggles (both
            // toggling made the popover flash open and shut). Clicking the icon while the popover is open first
            // deactivates it (which hides it), so a select right after that hide must not reopen it.
            if(LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) {
                if(app.popoverVisible) showPopover(app, false);
                else if(nowSec() - app.popoverHiddenAt > 0.3) showPopover(app, true);
            } else if(LOWORD(lp) == WM_CONTEXTMENU) {
                trayMenu(app);
            }
            return 0;
        case WM_SHOW_POPOVER:
            showPopover(app, true);
            return 0;
        case WM_ACTIVATE:
            if(LOWORD(wp) == WA_INACTIVE && !app.modalOpen && app.popoverVisible) {
                showPopover(app, false);
                app.popoverHiddenAt = nowSec();
            }
            return 0;
        case WM_KEYDOWN:
            if(wp == VK_ESCAPE && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) showPopover(app, false);
            return 0;
        case WM_NCCALCSIZE:
            if(wp) return 0;  // no frame: the client area is the whole window (the frame style is only there for DWM's shadow)
            break;
        case WM_NCHITTEST:
            return HTCLIENT;  // not resizable
        case WM_NCACTIVATE:
            return DefWindowProcW(h, msg, wp, -1);  // don't repaint the (absent) frame
        case WM_SETTINGCHANGE:
            if(lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) app.themeDirty = true;
            return 0;
        case WM_SIZE:
            resize(app, LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_DPICHANGED:
            app.fontsDirty = true;
            if(app.popoverVisible) positionPopover(app);
            return 0;
        case WM_CLOSE:
            showPopover(app, false);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------ frame processing

static void processFrame(App& app) {
    std::shared_ptr<const Frame> frame;
    std::shared_ptr<const Mask> mask;
    std::vector<Face> faces;
    double facesAt;
    Hand hand;
    bool handNew;
    {
        std::lock_guard<std::mutex> lk(app.mu);
        frame = app.frame;
        mask = app.mask;
        faces = app.faces;
        facesAt = app.facesAt;
        hand = app.hand;
        handNew = app.handNew;
        app.handNew = false;
    }
    if(!frame || frame->sequence == app.lastFrameSeq) return;
    app.lastFrameSeq = frame->sequence;
    double t = nowSec();
    float dt = app.lastFrameTime > 0 ? (float)(t - app.lastFrameTime) : 1.f / 30;
    if(dt > 0) app.fps = app.fps * 0.9f + 0.1f / dt;
    {
        static int processed = 0;
        static double since = t;
        processed++;
        if(t - since >= 10.0) {
            app.addLog("stats: app processed " + std::to_string((int)(processed / (t - since) + 0.5)) + " fps (" +
                       std::string(app.popoverVisible ? "popover open" : "popover closed") + (app.vcamInUse ? ", virtual camera in use)" : ")"));
            processed = 0;
            since = t;
        }
    }
    app.lastFrameTime = t;
    app.lastLens = frame->lensPosition;
    app.frameWidth = frame->width, app.frameHeight = frame->height;
    auto& s = app.settings;

    // gestures
    if(handNew) {
        bool peace, pinch;
        app.gestures.update(hand, s.gestureDraw, s.gesturePeace, s.gesturePinch, t, peace, pinch);
        if(peace) app.peaceAt = t;
        if(pinch) {
            s.zoom = !s.zoom;
            app.changed();
        }
    }
    // framing (Opal's AppearancePipeline: per rendered frame)
    EffectParams p;
    p.framing = app.framing.update(s.zoom, s.zoomAmount, s.faceLock, s.faceLockAmount, s.spotlight, faces, frame->width, frame->height);
    p.mirror = s.mirror;
    p.look = -1;
    if(s.filtersOn)
        for(size_t i = 0; i < app.effects.looks().size(); i++)
            if(app.effects.looks()[i].name == s.filter) p.look = (int)i;
    p.lookStrength = s.filterStrength;
    p.lookThumbs = app.popoverVisible && app.tab == 0;  // the Camera tab's Looks strip
    p.bokeh = s.bokeh;
    p.bokehRadius = s.bokehRadius;
    p.pixellate = s.pixellate;
    p.pixellateLevel = s.pixellateLevel;
    p.resolutionMultiple = s.resolution == "4K" ? 6 : s.resolution == "1440p" ? 4 : s.resolution == "720p" ? 2 : s.resolution == "360p" ? 1 : 3;
    p.touchUp = s.touchUp;
    p.touchUpAmount = s.touchUpAmount;
    p.skin = s.skin;
    p.skinAmount = s.skinAmount;
    // Opal drives the warmth from the white balance in Kelvin over its 2500..7000 K slider range
    p.skinWarmth = s.whiteBalanceAuto ? 0.5f : std::clamp((s.whiteBalanceK - 2500) / 4500.f, 0.f, 1.f);
    p.highlightsShadows = s.highlights || s.shadows;
    p.highlights = s.highlights ? s.highlightsAmount : 0.f;  // Opal sends 0 for the one that is off
    p.shadows = s.shadows ? s.shadowsAmount : 0.f;
    for(auto& f : faces)  // the skin measurement uses one face: the most confident
        if(t - facesAt < 1.0 && (!p.hasFace || f.confidence > p.face.confidence)) p.hasFace = true, p.face = f;
    bool ovA = false, ovB = false;
    app.overlay.draw(s, app.gestures.stroke, t, app.peaceAt, app.gestures.peaceX, app.gestures.peaceY, p.framing.cx, p.framing.cy, p.framing.cw,
                     p.framing.ch, ovA, ovB);
    app.effects.setOverlayEnabled(ovA, ovB);

    bool needMask = s.bokeh || s.touchUp || s.skin;
    bool ok = app.effects.process(frame->data.data(), frame->width, frame->height, needMask && mask ? mask->data.data() : nullptr,
                                  mask ? mask->width : 0, mask ? mask->height : 0, p, app.vcamInUse);
    app.readbackPending = ok && app.vcamInUse;

    if(s.histogram && app.popoverVisible && frame->sequence % 5 == 0) {
        float hist[64] = {};
        const uint8_t* y = frame->data.data();
        size_t n = (size_t)frame->width * frame->height;
        for(size_t i = 0; i < n; i += 61) hist[y[i] >> 2]++;
        memcpy(app.histogram, hist, sizeof hist);
    }
    // face-driven focus/exposure, about once a second
    static double lastFaceAf = 0;
    if(s.focusMode == "face" && !s.focusLock && !faces.empty() && t - facesAt < 0.5 && t - lastFaceAf > 1.0) {
        lastFaceAf = t;
        app.camera->setFaceRegion(&faces[0]);
    }
}

// ------------------------------------------------------------------ main

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int installerExit = 0;
    if(handleInstallerArgs(cmdLine, installerExit)) return installerExit;  // elevated --install / --uninstall helper
    if(!(cmdLine && wcsstr(cmdLine, L"--portable")) && offerInstall()) return 0;  // run from Downloads: install
    bool background = cmdLine && wcsstr(cmdLine, L"--background");
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\OpalForWindows");
    // a self-restart (--restarted) waits for the previous instance to exit
    for(int i = 0; i < 100 && GetLastError() == ERROR_ALREADY_EXISTS && cmdLine && wcsstr(cmdLine, L"--restarted"); i++) {
        CloseHandle(single);
        Sleep(100);
        single = CreateMutexW(nullptr, TRUE, L"Local\\OpalForWindows");
    }
    if(GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"OpalPopover", nullptr);
        if(other) PostMessageW(other, WM_SHOW_POPOVER, 0, 0);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    App& app = gApp;
    app.settings.load();
    {
        // depthai / XLink log to stdout/stderr: keep them (a GUI app has no console) in %APPDATA%\Opal\depthai.log
        std::string dir = AppSettings::path();
        dir = dir.substr(0, dir.find_last_of("\\/"));
        std::wstring wdir(dir.begin(), dir.end());
        FILE* f;
        _wfreopen_s(&f, (wdir + L"\\depthai.log").c_str(), L"w", stdout);
        _wfreopen_s(&f, (wdir + L"\\depthai.log").c_str(), L"a", stderr);
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        // spdlog (depthai's own log) writes to the Win32 std handles, not the CRT streams: point those at the file too
        SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
        SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
        _putenv_s("DEPTHAI_LEVEL", "debug");
    }
    app.resourcesDir = utf8(exeDir() + L"\\resources");
    app.addLog("---- Opal for Windows starting");

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OpalPopover";
    RegisterClassExW(&wc);
    // WS_THICKFRAME (with no frame drawn, see WM_NCCALCSIZE) gives the popup DWM's shadow; corners rounded like flyouts
    app.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Opal", WS_POPUP | WS_THICKFRAME, 0, 0, kPopW, kPopH, nullptr, nullptr,
                               inst, nullptr);
    {
        int round = 2;  // DWMWCP_ROUND
        DwmSetWindowAttribute(app.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
        MARGINS m = {1, 1, 1, 1};
        DwmExtendFrameIntoClientArea(app.hwnd, &m);
        SetWindowPos(app.hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if(!createDevice(app)) {
        MessageBoxW(nullptr, L"Opal needs Direct3D 11.", L"Opal", MB_ICONERROR);
        return 1;
    }
    createRTV(app);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    app.dpiScale = GetDpiForWindow(app.hwnd) / 96.f;
    applyTheme(app);
    loadFonts(app);
    app.fontsDirty = app.themeDirty = false;
    ImGui_ImplWin32_Init(app.hwnd);
    ImGui_ImplDX11_Init(app.device, app.context);

    NOTIFYICONDATAW nid = {sizeof nid};
    nid.hWnd = app.hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = wc.hIcon ? wc.hIcon : LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, app.settings.micMuted ? L"Opal (microphone off)" : L"Opal");
    Shell_NotifyIconW(NIM_ADD, &nid);
    bool tipMuted = app.settings.micMuted;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);

    // heavier setup after the tray icon is up
    double tStart = nowSec();
    {
        std::string cache = AppSettings::path();
        cache = cache.substr(0, cache.find_last_of("\\/")) + "\\shadercache";
        if(!app.effects.init(app.device, app.context, app.resourcesDir + "\\opal-template\\luts", cache, app.effectsError))
            app.addLog("effects: " + app.effectsError);
        IDXGISurface* surfA = app.effects.overlaySurface(0);
        IDXGISurface* surfB = app.effects.overlaySurface(1);
        if(surfA && surfB) app.overlay.init(surfA, surfB, Effects::kOutW, Effects::kOutH);
        if(surfA) surfA->Release();
        if(surfB) surfB->Release();
    }
    double tEffects = nowSec();
    if(!app.vcam.create(app.vcamError)) app.addLog("virtual camera: " + app.vcamError);
    {
        char b[160];
        snprintf(b, sizeof b, "startup: effects + %zu looks %.1fs, virtual camera %.1fs", app.effects.looks().size(), tEffects - tStart, nowSec() - tEffects);
        app.addLog(b);
    }
    setStartAtLogin(app.settings.startAtLogin);

    app.frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    try {
        app.camera = std::make_unique<OpalCamera>(app.resourcesDir + "\\opal-template");
    } catch(std::exception& e) {
        MessageBoxA(nullptr, (std::string("Opal resources are missing: ") + e.what()).c_str(), "Opal", MB_ICONERROR);
        return 1;
    }
    app.camera->onLog = [](const std::string& s) { gApp.addLog(s); };
    app.camera->onFrame = [](std::shared_ptr<const Frame> f) {
        {
            std::lock_guard<std::mutex> lk(gApp.mu);
            gApp.frame = std::move(f);
        }
        SetEvent(gApp.frameEvent);
    };
    app.camera->onMask = [](std::shared_ptr<const Mask> m) {
        std::lock_guard<std::mutex> lk(gApp.mu);
        gApp.mask = std::move(m);
    };
    app.camera->onFaces = [](const std::vector<Face>& f, double) {
        std::lock_guard<std::mutex> lk(gApp.mu);
        gApp.faces = f;
        gApp.facesAt = nowSec();
    };
    app.camera->onHand = [](const Hand& h) {
        std::lock_guard<std::mutex> lk(gApp.mu);
        gApp.hand = h;
        gApp.handNew = true;
    };
    app.camera->onTelemetry = [](const Telemetry& t) {
        std::lock_guard<std::mutex> lk(gApp.mu);
        gApp.telemetry = t;
        gApp.telemetryAt = nowSec();
    };
    applySettings(app);
    app.camera->setXLinkChunkSize(std::max(0, app.settings.usbTransferKB) * 1024);
    app.camera->start();
    if(!background) showPopover(app, true);

    while(!app.quit) {
        MsgWaitForMultipleObjects(1, &app.frameEvent, FALSE, app.popoverVisible ? 5 : 200, QS_ALLINPUT);
        MSG msg;
        while(PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if(msg.message == WM_QUIT) app.quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        // sensor on while the popover is open or a video app uses Opal C1 (off is held a few seconds by the camera)
        app.vcamInUse = app.vcam.inUse();
        app.camera->setStreaming(app.popoverVisible || app.vcamInUse);
        const auto& s = app.settings;
        app.camera->setNetworks(s.faceLock || s.spotlight || s.skin || (s.focusMode == "face" && !s.focusLock), s.bokeh || s.touchUp || s.skin,
                                s.gestureDraw || s.gesturePeace || s.gesturePinch);
        if(app.popoverVisible || app.vcamInUse) processFrame(app);
        if(app.camera->wedged()) {
            // XLink can't recover in-process after some USB failures: start a fresh Opal and exit
            app.addLog("restarting Opal to recover the camera connection");
            app.settings.save();
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --restarted" + (app.popoverVisible ? L"" : L" --background");
            STARTUPINFOW si = {sizeof si};
            PROCESS_INFORMATION pi;
            if(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            Shell_NotifyIconW(NIM_DELETE, &nid);
            if(single) CloseHandle(single);
            TerminateProcess(GetCurrentProcess(), 0);  // the wedged USB threads would block a normal exit
        }
        if(app.settingsDirty && nowSec() - app.settingsChangedAt > 1.0) app.settings.save(), app.settingsDirty = false;

        // the camera's USB control interface may need a one-time setup on this PC (see installer.hpp)
        static std::set<std::string> usbFixTried;
        std::string unbound = opal_xlink_unbound_interface();
        if(!unbound.empty() && !usbFixTried.count(unbound)) {
            usbFixTried.insert(unbound);
            std::string svc = opal_xlink_unbound_service();
            app.addLog("USB: control interface " + unbound + " (driver '" + svc + "') has no device interface; asking to set it up");
            app.modalOpen = true;
            int r = MessageBoxW(app.hwnd,
                                L"Opal needs to finish setting up the camera's USB connection on this PC (one time per camera mode).\n\n"
                                L"Windows will ask for permission.",
                                L"Opal", MB_OKCANCEL | MB_ICONINFORMATION | MB_TOPMOST);
            app.modalOpen = false;
            if(r == IDOK) app.addLog(fixUsbInterface(unbound) ? "USB: setup done" : "USB: setup failed or was declined");
        }

        if(tipMuted != app.settings.micMuted) {
            tipMuted = app.settings.micMuted;
            wcscpy_s(nid.szTip, tipMuted ? L"Opal (microphone off)" : L"Opal");
            nid.uFlags = NIF_TIP | NIF_SHOWTIP;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
        if(app.popoverVisible) {
            if(app.themeDirty) applyTheme(app), app.themeDirty = false;
            if(app.fontsDirty) {
                app.dpiScale = GetDpiForWindow(app.hwnd) / 96.f;
                loadFonts(app);
                ImGui_ImplDX11_InvalidateDeviceObjects();  // the font texture is rebuilt by the next NewFrame
                app.fontsDirty = false;
            }
            UiHost host;
            host.settings = &app.settings;
            for(const auto& l : app.effects.looks()) host.looks.push_back(l.name);
            // only a live picture: the last frame from before the popover was closed must not show while the sensor
            // starts again (the status shows instead)
            bool live = app.effects.preview() && app.lastFrameSeq >= 0 && nowSec() - app.lastFrameTime < 0.5;
            host.preview = live ? (ImTextureID)(intptr_t)app.effects.preview() : (ImTextureID)0;
            host.lookThumbs = live && app.effects.lookThumbs() ? (ImTextureID)(intptr_t)app.effects.lookThumbs() : (ImTextureID)0;
            host.cameraStatus = app.camera ? app.camera->statusText() : "";
            if(!live && host.cameraStatus == "Connected") host.cameraStatus = "Starting the camera";
            host.cameraStreaming = app.camera && app.camera->streaming() && live;
            host.vcamError = app.vcamError;
            host.effectsError = app.effectsError;
            host.vcamInUse = app.vcamInUse;
            host.fps = app.fps;
            {
                std::lock_guard<std::mutex> lk(app.mu);
                host.chipTempC = app.telemetry.chipTempC, host.cssCpu = app.telemetry.cssCpu, host.mssCpu = app.telemetry.mssCpu;
                host.ddrUsedMB = app.telemetry.ddrUsed / 1048576.f, host.ddrTotalMB = app.telemetry.ddrTotal / 1048576.f;
                host.telemetryFresh = app.telemetry.chipTempC > 0 && nowSec() - app.telemetryAt < 5;
            }
            // Opal.exe's CPU time over the last second, as % of one core (Task Manager shows % of all cores)
            {
                double now = nowSec();
                if(now - app.cpuSampleAt >= 1.0) {
                    FILETIME cr, ex, k, u;
                    if(GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u)) {
                        unsigned long long t = (((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                                               (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime);  // 100 ns units
                        if(app.cpuSampleAt > 0) app.pcCpu = (float)((t - app.cpuSampleTime) / 1e7 / (now - app.cpuSampleAt) * 100);
                        app.cpuSampleTime = t;
                        app.cpuSampleAt = now;
                    }
                }
            }
            host.pcCpu = app.pcCpu;
            host.frameWidth = app.frameWidth, host.frameHeight = app.frameHeight;
            host.histogram = app.histogram;
            host.lastLens = app.lastLens;
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            host.wheelLines = lines == WHEEL_PAGESCROLL ? 10.f : (float)lines;
            host.tab = app.tab;
            host.changed = [&] { app.changed(); };
            host.quit = [&] { app.quit = true; };
            host.startAtLoginChanged = [&] { setStartAtLogin(app.settings.startAtLogin); };
            host.chooseLogo = [&] {
                wchar_t file[MAX_PATH] = L"";
                OPENFILENAMEW ofn = {sizeof ofn};
                ofn.hwndOwner = app.hwnd;
                ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST;
                app.modalOpen = true;
                if(GetOpenFileNameW(&ofn)) {
                    app.settings.logoPath = utf8(file);
                    app.changed();
                }
                app.modalOpen = false;
            };
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            drawUI(host);
            app.tab = host.tab;
            ImGui::Render();
            ImVec4 bg = ImGui::ColorConvertU32ToFloat4(fluent::C().bg);
            const float clear[4] = {bg.x, bg.y, bg.z, 1};
            app.context->OMSetRenderTargets(1, &app.rtv, nullptr);
            app.context->ClearRenderTargetView(app.rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            app.swapChain->Present(1, 0);
        }
        // the virtual camera frame: read back after the UI is presented, so waiting for the GPU doesn't stall the popover
        if(app.readbackPending) {
            app.readbackPending = false;
            static std::vector<uint8_t> out;
            if(app.effects.readNV12(out) && app.vcamInUse) app.vcam.write(out.data(), out.size());
        }
    }

    app.settings.save();
    app.camera->stop();
    app.vcam.destroy();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    MFShutdown();
    CoUninitialize();
    if(single) CloseHandle(single);
    return 0;
}
