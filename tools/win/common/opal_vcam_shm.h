// Shared-memory frame channel between the Opal app (producer) and the "Opal C1" Windows virtual camera media source
// (consumer, OpalVirtualCamera.dll, runs inside the Windows Camera Frame Server service).
//
// The consumer CREATES the mapping (the Frame Server runs as a service and may create Global\ objects; a normal user
// process may not) with a DACL that lets interactive users read/write it. The producer OPENS it, so it only exists while
// some app has the virtual camera open: the app treats "mapping exists and consumerHeartbeatMs is fresh" as "a video
// app is using Opal C1" and turns the camera sensor on.
//
// Frame handoff is a seqlock: the producer sets writeSeq to an odd value, copies the frame, then sets it to the next
// even value. The consumer copies the frame only if writeSeq is even and unchanged before and after its copy.
#pragma once
#include <stdint.h>

#define OPAL_VCAM_SHM_NAME_GLOBAL L"Global\\OpalVirtualCameraFrames"
#define OPAL_VCAM_SHM_NAME_LOCAL L"Local\\OpalVirtualCameraFrames"  // fallback if Global\ is not permitted
#define OPAL_VCAM_MAGIC 0x314D4143414C504FULL                     // "OPALCAM1"
#define OPAL_VCAM_VERSION 1
#define OPAL_VCAM_WIDTH 1920
#define OPAL_VCAM_HEIGHT 1080
#define OPAL_VCAM_FPS 30
#define OPAL_VCAM_HEADER_SIZE 4096
#define OPAL_VCAM_FRAME_BYTES (OPAL_VCAM_WIDTH * OPAL_VCAM_HEIGHT * 3 / 2)  // NV12, stride == width
#define OPAL_VCAM_SHM_SIZE (OPAL_VCAM_HEADER_SIZE + OPAL_VCAM_FRAME_BYTES)
// virtual camera COM class (IMFActivate) registered by OpalVirtualCamera.dll and passed to MFCreateVirtualCamera
#define OPAL_VCAM_CLSID_STRING L"{5C3E7A11-0B9D-4E52-9F3A-7C2D1E8B6A40}"
#define OPAL_VCAM_FRIENDLY_NAME L"Opal C1"

#pragma pack(push, 8)
typedef struct OpalVcamHeader {
    uint64_t magic;    // OPAL_VCAM_MAGIC, set by the consumer when it creates the mapping
    uint32_t version;  // OPAL_VCAM_VERSION
    uint32_t headerSize;  // OPAL_VCAM_HEADER_SIZE; frame data starts at this offset
    uint32_t width, height, fps;
    uint32_t fourcc;  // 'NV12'
    volatile int64_t writeSeq;             // producer seqlock (odd = frame being written)
    volatile int64_t producerHeartbeatMs;  // GetTickCount64() at the producer's last frame
    volatile int64_t consumerHeartbeatMs;  // GetTickCount64() at the consumer's last sample request
    volatile int64_t consumerFrames;       // samples delivered by the consumer (diagnostics)
    volatile int32_t mirror;               // reserved
} OpalVcamHeader;
#pragma pack(pop)
