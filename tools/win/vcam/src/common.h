// Small helpers shared by the virtual camera DLL: COM smart pointer, critical-section lock, module reference counting
// (for DllCanUnloadNow) and debug logging. Deliberately dependency-free (no WIL / C++/WinRT) so the same code builds
// with MSVC and with MinGW-w64 for the cross-compile check.
#pragma once
#include "../mf_compat.h"

namespace opal {

extern HMODULE g_module;     // this DLL
extern LONG g_objectCount;   // live COM objects created by this DLL
extern LONG g_serverLocks;   // IClassFactory::LockServer count

// Embed in every COM object so DllCanUnloadNow refuses while any of them is alive.
struct ModuleRef {
    ModuleRef() { InterlockedIncrement(&g_objectCount); }
    ~ModuleRef() { InterlockedDecrement(&g_objectCount); }
    ModuleRef(const ModuleRef&) = delete;
    ModuleRef& operator=(const ModuleRef&) = delete;
};

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(T* p) : p_(p) {
        if (p_) p_->AddRef();
    }
    ComPtr(const ComPtr& o) : ComPtr(o.p_) {}
    ComPtr& operator=(const ComPtr& o) {
        reset(o.p_);
        return *this;
    }
    ~ComPtr() { reset(); }

    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    // Releases the current pointer and returns the address for an out-parameter.
    T** put() {
        reset();
        return &p_;
    }
    void reset(T* p = nullptr) {
        if (p) p->AddRef();
        T* old = p_;
        p_ = p;
        if (old) old->Release();
    }
    // Takes ownership of an already-AddRef'ed pointer.
    void attach(T* p) {
        T* old = p_;
        p_ = p;
        if (old) old->Release();
    }
    HRESULT copyTo(T** out) const {
        if (!out) return E_POINTER;
        *out = p_;
        if (p_) p_->AddRef();
        return S_OK;
    }

private:
    T* p_ = nullptr;
};

class CritSec {
public:
    CritSec() { InitializeCriticalSection(&cs_); }
    ~CritSec() { DeleteCriticalSection(&cs_); }
    CritSec(const CritSec&) = delete;
    CritSec& operator=(const CritSec&) = delete;
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }

private:
    CRITICAL_SECTION cs_;
};

class AutoLock {
public:
    explicit AutoLock(CritSec& cs) : cs_(cs) { cs_.lock(); }
    ~AutoLock() { cs_.unlock(); }
    AutoLock(const AutoLock&) = delete;
    AutoLock& operator=(const AutoLock&) = delete;

private:
    CritSec& cs_;
};

// OutputDebugString with a "[OpalVCam pid]" prefix. The source runs inside the Frame Server service (session 0), so
// use Sysinternals DebugView with "Capture Global Win32" enabled (as administrator) to see these lines.
void Log(const wchar_t* fmt, ...);

}  // namespace opal

#define OPAL_RETURN_IF_FAILED(expr)       \
    do {                                  \
        HRESULT hr_ = (expr);             \
        if (FAILED(hr_)) return hr_;      \
    } while (0)
