// In-proc COM server entry points for OpalVirtualCamera.dll (exported through OpalVirtualCamera.def).
//
// Registration must be machine-wide (HKLM): the class is instantiated by the Frame Server services (LocalService /
// LocalSystem), which don't see a user's HKCU classes. That is also why the DLL must sit in a folder those accounts can
// read (e.g. C:\Program Files\Opal\), not under a user profile.
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#include <new>

#include "../../common/opal_vcam_shm.h"
#include "activator.h"

namespace opal {

HMODULE g_module = nullptr;
LONG g_objectCount = 0;
LONG g_serverLocks = 0;

void Log(const wchar_t* fmt, ...) {
    const size_t size = 512;
    wchar_t line[size] = {};
    int n = swprintf(line, 64, L"[OpalVCam %lu] ", GetCurrentProcessId());
    if (n < 0) n = 0;
    va_list args;
    va_start(args, fmt);
    vswprintf(line + n, size - n - 2, fmt, args);  // leave room for "\n"
    va_end(args);
    line[size - 2] = 0;  // terminated even if the message was truncated
    size_t len = wcslen(line);
    line[len] = L'\n';
    line[len + 1] = 0;
    OutputDebugStringW(line);
}

namespace {

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    // Static object: lifetime is the module's; DllCanUnloadNow tracks LockServer instead.
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return Activator::create(riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock)
            InterlockedIncrement(&g_serverLocks);
        else
            InterlockedDecrement(&g_serverLocks);
        return S_OK;
    }
};

ClassFactory g_factory;

const wchar_t kClassKey[] = L"Software\\Classes\\CLSID\\" OPAL_VCAM_CLSID_STRING;

HRESULT setStringValue(HKEY key, const wchar_t* name, const wchar_t* value) {
    DWORD bytes = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
    LSTATUS status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), bytes);
    return HRESULT_FROM_WIN32(status);
}

HRESULT writeKey(const wchar_t* path, const wchar_t* defaultValue, const wchar_t* name, const wchar_t* value) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                     &key, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    HRESULT hr = setStringValue(key, nullptr, defaultValue);
    if (SUCCEEDED(hr) && name) hr = setStringValue(key, name, value);
    RegCloseKey(key);
    return hr;
}

}  // namespace
}  // namespace opal

using namespace opal;

// Normally declared by <olectl.h>, which WIN32_LEAN_AND_MEAN keeps out.
STDAPI DllRegisterServer();
STDAPI DllUnregisterServer();

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() { return (g_objectCount == 0 && g_serverLocks == 0) ? S_OK : S_FALSE; }

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    CLSID clsid;
    if (FAILED(CLSIDFromString(OPAL_VCAM_CLSID_STRING, &clsid)) || rclsid != clsid) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return E_FAIL;

    HRESULT hr = writeKey(kClassKey, L"Opal C1 Virtual Camera", nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        wchar_t inproc[128];
        swprintf(inproc, 128, L"%ls\\InprocServer32", kClassKey);
        // "Both": Frame Server activates from MTA threads; we're free-threaded internally (critical sections).
        hr = writeKey(inproc, path, L"ThreadingModel", L"Both");
    }
    if (FAILED(hr)) DllUnregisterServer();
    return hr;
}

STDAPI DllUnregisterServer() {
    LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClassKey);
    if (status == ERROR_FILE_NOT_FOUND) return S_OK;
    return HRESULT_FROM_WIN32(status);
}
