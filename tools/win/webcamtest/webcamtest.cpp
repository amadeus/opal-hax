// OpalWebcamTest: why does the camera's flashed webcam program keep restarting on Windows?
// Loads the program into the camera's memory (nothing is flashed) with the camera's log on, twice:
//   A = what is flashed on the owner's camera (Opal's standalone pipeline + the sensor idle watcher)
//   B = Opal's original standalone pipeline (no idle watcher)
// and reports whether each one keeps running while Windows sets up the webcam and microphone, and what the camera logs.
// Quit Opal first. Output goes to the console and to OpalWebcamTest.log next to the exe.
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "../../standalone.hpp"
#include "depthai/depthai.hpp"
#include "depthai/utility/Initialization.hpp"
#include "XLink/XLink.h"

extern "C" const char* opal_xlink_unbound_interface(void);  // patched XLink (tools/win/xlink/patch-xlink.py)

using clk = std::chrono::steady_clock;
static FILE* gLog;
static clk::time_point gT0 = clk::now();

static std::mutex gOutMu;

static void out(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(gOutMu);
    char b[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    double t = std::chrono::duration<double>(clk::now() - gT0).count();
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    snprintf(line, sizeof line, "%02d:%02d:%02d (+%6.1fs)  %s\n", st.wHour, st.wMinute, st.wSecond, t, b);
    fputs(line, stdout);
    fflush(stdout);
    if(gLog) fputs(line, gLog), fflush(gLog);
}

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

// the camera sits in its bootloader for a few seconds after every restart: catch it there
static bool waitForBootloader(dai::DeviceInfo& info, int timeoutMs) {
    auto end = clk::now() + std::chrono::milliseconds(timeoutMs);
    std::string lastSeen = "?";
    while(clk::now() < end) {
        auto devs = dai::Device::getAllAvailableDevices();
        std::string seen;
        for(auto& d : devs) seen += std::string(XLinkDeviceStateToStr(d.state)) + " ";
        if(seen != lastSeen) out("camera: %s", seen.empty() ? "(not visible to XLink: running its flashed program, or restarting)" : seen.c_str());
        lastSeen = seen;
        for(auto& d : devs)
            if(d.state == X_LINK_BOOTLOADER || d.state == X_LINK_UNBOOTED) {
                info = d;
                return true;
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// Opal.exe's one-time USB setup for a new camera mode (needs admin)
static void offerUsbFix() {
    const char* id = opal_xlink_unbound_interface();
    if(!id || !*id) return;
    out("USB: the camera's control interface (%s) has no device interface yet; running Opal's one-time USB setup (admin prompt)", id);
    std::wstring opal = L"C:\\Program Files\\Opal\\Opal.exe";
    std::wstring args = L"--usb-fix \"" + std::wstring(id, id + strlen(id)) + L"\"";
    SHELLEXECUTEINFOW si = {sizeof si};
    si.fMask = SEE_MASK_NOCLOSEPROCESS;
    si.lpVerb = L"runas";
    si.lpFile = opal.c_str();
    si.lpParameters = args.c_str();
    si.nShow = SW_HIDE;
    if(ShellExecuteExW(&si) && si.hProcess) {
        WaitForSingleObject(si.hProcess, 30000);
        DWORD code = 1;
        GetExitCodeProcess(si.hProcess, &code);
        CloseHandle(si.hProcess);
        out("USB: setup %s", code == 0 ? "done" : "failed");
    } else {
        out("USB: setup was declined or Opal.exe is not installed");
    }
}

// returns true if the program kept running for the whole time
static bool runVariant(const char* name, bool idleWatcher, const std::string& tuning, int secs) {
    out("==== %s: waiting for the camera's bootloader...", name);
    for(int attempt = 0; attempt < 3; attempt++) {
        dai::DeviceInfo info;
        if(!waitForBootloader(info, 60000)) {
            out("%s: the camera never showed up in its bootloader (is Opal still running? quit it from the tray)", name);
            return false;
        }
        try {
            auto p = opal::buildStandalonePipeline(tuning, idleWatcher, true);
            auto config = p.getDeviceConfig();
            config.board.uvcEnable = true;  // the webcam (UVC) firmware mode, like the flashed program
            out("%s: loading the program into the camera (RAM only)", name);
            dai::DeviceBase dev(config, info);
            dev.addLogCallback([](dai::LogMessage m) { out("  [camera %s] %s: %s", m.nodeIdName.c_str(), m.level == dai::LogLevel::CRITICAL ? "CRITICAL" : m.level == dai::LogLevel::ERR ? "ERROR" : m.level == dai::LogLevel::WARN ? "warn" : "info", m.payload.c_str()); });
            dev.setLogLevel(dai::LogLevel::WARN);
            dev.setLogOutputLevel(dai::LogLevel::OFF);
            dev.startPipeline(p);
            out("%s: running. Windows now sets up the Opal C1 webcam and microphone; watching for %d s", name, secs);
            for(int i = 0; i <= secs; i++) {
                if(dev.isClosed()) {
                    out("%s: CONNECTION LOST after %d s: the camera restarted", name, i);
                    return false;
                }
                if(i % 5 == 0) {
                    try {
                        auto t = dev.getChipTemperature();
                        auto css = dev.getLeonCssCpuUsage(), mss = dev.getLeonMssCpuUsage();
                        out("%s: t=%2ds alive, chip %.1f C, CPU %.0f%% / %.0f%%", name, i, t.average, css.average * 100, mss.average * 100);
                    } catch(std::exception& e) {
                        out("%s: t=%2ds camera stopped answering: %s", name, i, e.what());
                        return false;
                    }
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            out("%s: kept running for %d s", name, secs);
            return true;
        } catch(std::exception& e) {
            out("%s: could not start: %s", name, e.what());
            offerUsbFix();
        }
    }
    return false;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    gLog = _wfopen((exeDir() + L"\\OpalWebcamTest.log").c_str(), L"w");
    out("Opal webcam test. Quit Opal (tray icon > Quit) before running this. Nothing is flashed.");
    std::string tuning = utf8(exeDir() + L"\\Opal_v0.12.bin");
    if(GetFileAttributesW((exeDir() + L"\\Opal_v0.12.bin").c_str()) == INVALID_FILE_ATTRIBUTES) {
        out("Opal_v0.12.bin is missing next to the exe");
        return 1;
    }
    dai::initialize();
    bool a = runVariant("A (flashed now: with sensor idle watcher)", true, tuning, 40);
    bool b = runVariant("B (Opal's original, no idle watcher)", false, tuning, 40);
    out("==== result: A %s, B %s", a ? "OK" : "RESTARTED", b ? "OK" : "RESTARTED");
    out("Log saved to OpalWebcamTest.log next to this exe. Press Enter to close.");
    if(gLog) fclose(gLog);
    getchar();
    return 0;
}
