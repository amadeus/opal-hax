// vcam_producer: app side of the "Opal C1" virtual camera. Creates the virtual camera for this session
// (MFCreateVirtualCamera, Windows 11) pointing at OpalVirtualCamera.dll's COM class, and feeds it frames through the
// shared memory described in common/opal_vcam_shm.h. The media source creates that memory when a video app opens the
// camera, so "mapping present + fresh consumer heartbeat" also tells us when a video app is using Opal C1.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>

struct IMFVirtualCamera;

namespace opal {

class VirtualCameraProducer {
   public:
    ~VirtualCameraProducer();
    // registers the virtual camera with Windows for this session; err explains failures (DLL not registered, ...)
    bool create(std::string& err);
    void destroy();
    // true while a video app is pulling frames
    bool inUse();
    // write one NV12 1920x1080 frame (no-op if no app has the camera open)
    void write(const uint8_t* nv12, size_t size);
    bool created() const { return vcam_ != nullptr; }

   private:
    bool openMapping();
    IMFVirtualCamera* vcam_ = nullptr;
    HANDLE mapping_ = nullptr;
    uint8_t* view_ = nullptr;
    ULONGLONG lastOpenTry_ = 0;
};

}  // namespace opal
