#include "vcam_producer.hpp"

#include <mfapi.h>
#include <mfidl.h>

#include <cstring>

#include "../common/opal_vcam_shm.h"
#if __has_include(<mfvirtualcamera.h>)
#include <mfvirtualcamera.h>
#else
#include "../vcam/mf_compat.h"  // MinGW: declarations missing from its SDK headers
#endif

namespace opal {

typedef HRESULT(WINAPI* CreateVirtualCameraFn)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess, LPCWSTR, LPCWSTR, const GUID*, ULONG,
                                                IMFVirtualCamera**);

VirtualCameraProducer::~VirtualCameraProducer() { destroy(); }

bool VirtualCameraProducer::create(std::string& err) {
    if(vcam_) return true;
    HMODULE m = LoadLibraryW(L"mfsensorgroup.dll");
    auto fn = m ? (CreateVirtualCameraFn)GetProcAddress(m, "MFCreateVirtualCamera") : nullptr;
    if(!fn) {
        err = "This version of Windows has no virtual camera support (Windows 11 required).";
        return false;
    }
    HRESULT hr = fn(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser, OPAL_VCAM_FRIENDLY_NAME,
                    OPAL_VCAM_CLSID_STRING, nullptr, 0, &vcam_);
    if(FAILED(hr)) {
        char buf[160];
        snprintf(buf, sizeof buf, "Could not create the Opal C1 virtual camera (0x%08lx). Is OpalVirtualCamera.dll registered? Run install.ps1 as administrator.",
                 (unsigned long)hr);
        err = buf;
        vcam_ = nullptr;
        return false;
    }
    hr = vcam_->Start(nullptr);
    if(FAILED(hr)) {
        char buf[120];
        snprintf(buf, sizeof buf, "The Opal C1 virtual camera did not start (0x%08lx).", (unsigned long)hr);
        err = buf;
        vcam_->Release();
        vcam_ = nullptr;
        return false;
    }
    return true;
}

void VirtualCameraProducer::destroy() {
    if(view_) UnmapViewOfFile(view_);
    if(mapping_) CloseHandle(mapping_);
    view_ = nullptr;
    mapping_ = nullptr;
    if(vcam_) {
        vcam_->Remove();
        vcam_->Shutdown();
        vcam_->Release();
        vcam_ = nullptr;
    }
}

bool VirtualCameraProducer::openMapping() {
    if(view_) {
        // the media source recreates the mapping when an app reopens the camera; drop ours if it went stale
        auto* h = (OpalVcamHeader*)view_;
        if(h->magic == OPAL_VCAM_MAGIC) return true;
        UnmapViewOfFile(view_);
        CloseHandle(mapping_);
        view_ = nullptr;
        mapping_ = nullptr;
    }
    ULONGLONG now = GetTickCount64();
    if(now - lastOpenTry_ < 500) return false;
    lastOpenTry_ = now;
    for(auto name : {OPAL_VCAM_SHM_NAME_GLOBAL, OPAL_VCAM_SHM_NAME_LOCAL}) {
        mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        if(mapping_) break;
    }
    if(!mapping_) return false;
    view_ = (uint8_t*)MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, OPAL_VCAM_SHM_SIZE);
    if(!view_ || ((OpalVcamHeader*)view_)->magic != OPAL_VCAM_MAGIC) {
        if(view_) UnmapViewOfFile(view_);
        CloseHandle(mapping_);
        view_ = nullptr;
        mapping_ = nullptr;
        return false;
    }
    return true;
}

bool VirtualCameraProducer::inUse() {
    if(!openMapping()) return false;
    auto* h = (OpalVcamHeader*)view_;
    int64_t beat = h->consumerHeartbeatMs;
    int64_t age = (int64_t)GetTickCount64() - beat;
    if(beat <= 0 || age > 5000) {
        // our handle would keep the mapping alive after the media source closed it; let go so a stale mapping never
        // looks like an open camera (it is reopened on the next check once an app starts the camera again)
        UnmapViewOfFile(view_);
        CloseHandle(mapping_);
        view_ = nullptr;
        mapping_ = nullptr;
        return false;
    }
    return age < 1500;
}

void VirtualCameraProducer::write(const uint8_t* nv12, size_t size) {
    if(!openMapping() || size < OPAL_VCAM_FRAME_BYTES) return;
    auto* h = (OpalVcamHeader*)view_;
    // seqlock: odd while writing
    int64_t seq = InterlockedIncrement64((volatile LONG64*)&h->writeSeq);
    if(!(seq & 1)) seq = InterlockedIncrement64((volatile LONG64*)&h->writeSeq);
    memcpy(view_ + h->headerSize, nv12, OPAL_VCAM_FRAME_BYTES);
    InterlockedIncrement64((volatile LONG64*)&h->writeSeq);
    InterlockedExchange64((volatile LONG64*)&h->producerHeartbeatMs, (LONG64)GetTickCount64());
}

}  // namespace opal
