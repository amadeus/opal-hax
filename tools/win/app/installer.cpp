#include "installer.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <cfgmgr32.h>

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace opal {

static std::wstring selfPath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

static std::wstring selfDir() { return fs::path(selfPath()).parent_path().wstring(); }

std::wstring installDir() {
    PWSTR pf = nullptr;
    std::wstring d = L"C:\\Program Files\\Opal";
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &pf))) {
        d = std::wstring(pf) + L"\\Opal";
        CoTaskMemFree(pf);
    }
    return d;
}

bool runningFromInstallDir() {
    std::error_code ec;
    return fs::equivalent(selfDir(), installDir(), ec);
}

// relaunch this exe elevated (normal Windows admin prompt) and wait for it
static bool runElevated(const std::wstring& args, DWORD& exitCode) {
    std::wstring exe = selfPath();
    SHELLEXECUTEINFOW si = {sizeof si};
    si.fMask = SEE_MASK_NOCLOSEPROCESS;
    si.lpVerb = L"runas";
    si.lpFile = exe.c_str();
    si.lpParameters = args.c_str();
    si.nShow = SW_HIDE;
    if(!ShellExecuteExW(&si) || !si.hProcess) return false;
    WaitForSingleObject(si.hProcess, INFINITE);
    GetExitCodeProcess(si.hProcess, &exitCode);
    CloseHandle(si.hProcess);
    return true;
}

// stop every Opal.exe running from the install folder (except us)
static void stopInstalledOpal() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {sizeof pe};
    std::wstring dir = installDir();
    for(BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if(pe.th32ProcessID == GetCurrentProcessId() || _wcsicmp(pe.szExeFile, L"Opal.exe") != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if(!h) continue;
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        std::error_code ec;
        if(QueryFullProcessImageNameW(h, 0, path, &n) && fs::equivalent(fs::path(path).parent_path(), dir, ec)) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 3000);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

static HRESULT callDllEntry(const std::wstring& dll, const char* entry) {
    HMODULE m = LoadLibraryW(dll.c_str());
    if(!m) return HRESULT_FROM_WIN32(GetLastError());
    auto fn = (HRESULT(__stdcall*)())GetProcAddress(m, entry);
    HRESULT hr = fn ? fn() : E_NOINTERFACE;
    FreeLibrary(m);
    return hr;
}

// installer log (the elevated helper writes it, the normal process shows it when something fails)
static std::wstring installLogPath() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"OpalInstall.log";
}

static void installLog(const std::wstring& line) {
    FILE* f = _wfopen(installLogPath().c_str(), L"a, ccs=UTF-8");
    if(!f) return;
    fwprintf(f, L"%ls\n", line.c_str());
    fclose(f);
}

static std::wstring errText(DWORD e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"";
    if(buf) LocalFree(buf);
    while(!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s + L" (" + std::to_wstring(e) + L")";
}

// copy one file over an installed one. A file that is in use (the virtual camera DLL stays loaded in Windows' camera
// service after a video app used Opal C1; a running Opal.exe) can't be overwritten but can be renamed: move it aside,
// copy the new one in, and delete the old one at the next restart.
static bool installFile(const fs::path& src, const fs::path& dst) {
    if(CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
    DWORD e = GetLastError();
    for(int i = 0; i < 10; i++) {
        fs::path aside = dst;
        aside += L".old" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(i);
        if(!MoveFileExW(dst.c_str(), aside.c_str(), 0)) continue;
        MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        if(CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            installLog(L"in use, replaced (old copy removed at restart): " + dst.wstring());
            return true;
        }
        e = GetLastError();
        MoveFileExW(aside.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);  // put the old one back
        break;
    }
    installLog(L"could not install " + dst.wstring() + L": " + errText(e));
    return false;
}

static int doInstall(const std::wstring& from) {
    DeleteFileW(installLogPath().c_str());
    std::wstring to = installDir();
    installLog(L"installing from " + from + L" to " + to);
    stopInstalledOpal();
    std::error_code ec;
    std::wstring dll = to + L"\\OpalVirtualCamera.dll";
    if(fs::exists(dll)) callDllEntry(dll, "DllUnregisterServer");
    fs::create_directories(to, ec);
    // old copies moved aside by a previous update (those still loaded stay until the restart)
    for(auto it = fs::recursive_directory_iterator(to, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        if(it->path().extension().wstring().rfind(L".old", 0) == 0 && it->is_regular_file(ec)) DeleteFileW(it->path().c_str());
    bool ok = true;
    for(auto it = fs::recursive_directory_iterator(from, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        fs::path rel = fs::relative(it->path(), from, ec);
        fs::path dst = fs::path(to) / rel;
        if(it->is_directory(ec)) {
            fs::create_directories(dst, ec);
            continue;
        }
        fs::create_directories(dst.parent_path(), ec);
        if(!installFile(it->path(), dst)) ok = false;
    }
    if(ec) installLog(L"listing " + from + L" failed: " + std::wstring(ec.message().begin(), ec.message().end()));
    if(!ok || ec) return 2;
    // the Camera Frame Server service loads this DLL from Program Files
    HRESULT hr = callDllEntry(dll, "DllRegisterServer");
    if(FAILED(hr)) installLog(L"registering the camera failed: " + errText((DWORD)hr));
    return SUCCEEDED(hr) ? 0 : 3;
}

static int doUninstall() {
    std::wstring to = installDir();
    stopInstalledOpal();
    std::wstring dll = to + L"\\OpalVirtualCamera.dll";
    callDllEntry(dll, "DllUnregisterServer");
    HKEY k;
    if(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, L"Opal");
        RegCloseKey(k);
    }
    // delete what we can now; files in use (the DLL if a camera app still holds it) go at the next restart
    std::error_code ec;
    for(auto it = fs::recursive_directory_iterator(to, ec); it != fs::recursive_directory_iterator(); ++it) {
        if(!it->is_regular_file(ec)) continue;
        if(!DeleteFileW(it->path().c_str())) MoveFileExW(it->path().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    fs::remove_all(to, ec);
    if(ec) MoveFileExW(to.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 0;
}

// elevated: give the camera's control interface a device interface GUID and restart it
static int doUsbFix(const std::wstring& id) {
    DEVINST dn;
    if(CM_Locate_DevNodeW(&dn, (DEVINSTID_W)id.c_str(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return 4;
    HKEY k;
    if(CM_Open_DevNode_Key(dn, KEY_SET_VALUE | KEY_READ, 0, RegDisposition_OpenAlways, &k, CM_REGISTRY_HARDWARE) != CR_SUCCESS) return 5;
    // REG_MULTI_SZ, the Myriad X interface GUID XLink also looks for
    static const wchar_t guids[] = L"{504E1220-E189-413A-BDEC-ECFFAF3A3731}\0";
    LONG r = RegSetValueExW(k, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, (const BYTE*)guids, sizeof(guids));
    RegCloseKey(k);
    if(r != ERROR_SUCCESS) return 6;
    // restart the function so WinUSB registers the interface
    if(CM_Disable_DevNode(dn, CM_DISABLE_UI_NOT_OK) == CR_SUCCESS) CM_Enable_DevNode(dn, 0);
    else CM_Reenumerate_DevNode(dn, CM_REENUMERATE_SYNCHRONOUS);
    return 0;
}

bool fixUsbInterface(const std::string& instanceId) {
    std::wstring id(instanceId.begin(), instanceId.end());
    DWORD code = 1;
    return runElevated(L"--usb-fix \"" + id + L"\"", code) && code == 0;
}

bool handleInstallerArgs(const wchar_t* cmdLine, int& exitCode) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool handled = false;
    for(int i = 1; i < argc && !handled; i++) {
        if(wcscmp(argv[i], L"--install") == 0 && i + 1 < argc) {
            exitCode = doInstall(argv[i + 1]);
            handled = true;
        } else if(wcscmp(argv[i], L"--usb-fix") == 0 && i + 1 < argc) {
            exitCode = doUsbFix(argv[i + 1]);
            handled = true;
        } else if(wcscmp(argv[i], L"--uninstall") == 0) {
            exitCode = doUninstall();
            handled = true;
        }
    }
    LocalFree(argv);
    (void)cmdLine;
    return handled;
}

bool offerInstall() {
    if(runningFromInstallDir()) return false;
    int r = MessageBoxW(nullptr,
                        L"Install Opal for Windows?\n\nOpal will be installed in Program Files and the \"Opal C1\" camera will be added for "
                        L"your video apps. Windows will ask for permission.",
                        L"Opal", MB_OKCANCEL | MB_ICONQUESTION);
    if(r != IDOK) return true;
    DWORD code = 1;
    std::wstring args = L"--install \"" + selfDir() + L"\"";
    if(!runElevated(args, code)) return true;  // admin prompt declined
    if(code != 0) {
        std::wstring msg = code == 3 ? L"Opal was copied, but the Opal C1 camera could not be registered." : L"Installing Opal failed.";
        // the reason, from the installer log
        if(FILE* f = _wfopen(installLogPath().c_str(), L"r, ccs=UTF-8")) {
            wchar_t line[1024];
            std::wstring details;
            while(fgetws(line, 1024, f))
                if(wcsstr(line, L"could not") || wcsstr(line, L"failed")) details += line;
            fclose(f);
            if(!details.empty()) msg += L"\n\n" + details;
        }
        msg += L"\n(Log: " + installLogPath() + L")";
        MessageBoxW(nullptr, msg.c_str(), L"Opal", MB_ICONERROR);
        if(code != 3) return true;
    }
    // start the installed copy as the normal (not elevated) user
    std::wstring exe = installDir() + L"\\Opal.exe";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if(CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, installDir().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return true;
}

void startUninstall() {
    if(MessageBoxW(nullptr, L"Remove Opal and the Opal C1 camera from this PC?\n\nYour settings stay in %APPDATA%\\Opal.", L"Opal",
                   MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    // run the uninstaller from a temp copy: the installed exe can't delete itself
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring copy = std::wstring(tmp) + L"OpalUninstall.exe";
    CopyFileW(selfPath().c_str(), copy.c_str(), FALSE);
    SHELLEXECUTEINFOW si = {sizeof si};
    si.lpVerb = L"runas";
    si.lpFile = copy.c_str();
    si.lpParameters = L"--uninstall";
    si.nShow = SW_HIDE;
    if(ShellExecuteExW(&si)) ExitProcess(0);  // the uninstaller stops this process anyway
}

}  // namespace opal
