// Media Foundation headers for the Opal virtual camera, plus the Windows 11 virtual-camera / Frame Server declarations
// that MinGW-w64's headers lack.
//
// MSVC + Windows SDK 10.0.22000+ (the real target) has everything: mfvirtualcamera.h exists and the SDK's mfidl.h /
// mfapi.h / ksmedia.h declare the rest, so the compat block below is skipped entirely. MinGW-w64 (used only to
// syntax/type-check the code on macOS) has no mfvirtualcamera.h and none of the declarations below.
//
// Values come from the Windows SDK 10.0.22621 headers (mfidl.h, mfapi.h, ksmedia.h, mfvirtualcamera.h) and agree with
// the SDK metadata in microsoft/windows-rs (metadata/win32/{mfidl,mfapi,ksmedia,mfvirtualcamera}.rdl).
//
// Include this header before any other Media Foundation header. It includes <initguid.h> so DEFINE_GUID/EXTERN_GUID
// constants get a (selectany) definition in every translation unit: some of the GUIDs used here (e.g.
// MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES) are plain DEFINE_GUIDs that no import library is guaranteed to provide.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>
#include <ksmedia.h>

// ks.h (SDK and MinGW alike) declares IKsControl and IID_IKsControl only in its kernel-mode (_NTDDK_) section;
// user-mode code normally gets them from the DirectShow header ksproxy.h. Declare the (standard, ABI-identical)
// interface here instead of pulling in DirectShow. NTSTATUS and HRESULT are both LONG.
#if !defined(_IKsControl_)
#define _IKsControl_
struct IKsControl : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData,
                                                 ULONG DataLength, ULONG* BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData,
                                               ULONG DataLength, ULONG* BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength,
                                              ULONG* BytesReturned) = 0;
};
DEFINE_GUID(IID_IKsControl, 0x28f54685, 0x06fd, 0x11d2, 0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96);
#endif

#if __has_include(<mfvirtualcamera.h>)
#include <mfvirtualcamera.h>
#else
#define OPAL_MF_COMPAT 1
#include <devpropdef.h>

// ---- mfidl.h / mfapi.h: Frame Server stream attributes -------------------------------------------------------------
DEFINE_GUID(MF_DEVICESTREAM_STREAM_CATEGORY, 0x2939e7b8, 0xa62e, 0x4579, 0xb6, 0x74, 0xd4, 0x07, 0x3d, 0xfa, 0xbb, 0xba);
DEFINE_GUID(MF_DEVICESTREAM_STREAM_ID, 0x11bd5120, 0xd124, 0x446b, 0x88, 0xe6, 0x17, 0x06, 0x02, 0x57, 0xff, 0xf9);
DEFINE_GUID(MF_DEVICESTREAM_FRAMESERVER_SHARED, 0x1cb378e9, 0xb279, 0x41d4, 0xaf, 0x97, 0x34, 0xa2, 0x43, 0xe6, 0x83, 0x20);
DEFINE_GUID(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, 0x17145fd1, 0x1b2b, 0x423c, 0x80, 0x01, 0x2b, 0x68, 0x33, 0xed,
            0x35, 0x88);
DEFINE_GUID(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, 0x36ebdc44, 0xb12c, 0x441b, 0x89, 0xf4, 0x08, 0xb2, 0xf4, 0x1a, 0x9c,
            0xfc);

typedef enum MFFrameSourceTypes {
    MFFrameSourceTypes_Color = 0x0001,
    MFFrameSourceTypes_Infrared = 0x0002,
    MFFrameSourceTypes_Depth = 0x0004,
    MFFrameSourceTypes_Image = 0x0008,
    MFFrameSourceTypes_Custom = 0x0080
} MFFrameSourceTypes;

// ---- ksmedia.h ------------------------------------------------------------------------------------------------------
DEFINE_GUID(KSCAMERAPROFILE_Legacy, 0xb4894d81, 0x62b7, 0x4eec, 0x87, 0x40, 0x80, 0x65, 0x8c, 0x4a, 0x9d, 0x3e);

// ---- mfidl.h: IMFMediaStream2 ---------------------------------------------------------------------------------------
// (MF_STREAM_STATE itself is in MinGW's mfobjects.h.)
struct IMFMediaStream2 : public IMFMediaStream {
    virtual HRESULT STDMETHODCALLTYPE SetStreamState(MF_STREAM_STATE value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStreamState(MF_STREAM_STATE* value) = 0;
};
__CRT_UUID_DECL(IMFMediaStream2, 0xc5bc37d6, 0x75c7, 0x46a1, 0xa1, 0x32, 0x81, 0xb5, 0xf7, 0x23, 0xc2, 0x0f)

// ---- mfidl.h: IMFSampleAllocatorControl -----------------------------------------------------------------------------
typedef enum MFSampleAllocatorUsage {
    MFSampleAllocatorUsage_UsesProvidedAllocator = 0,
    MFSampleAllocatorUsage_UsesCustomAllocator = 1,
    MFSampleAllocatorUsage_DoesNotAllocate = 2
} MFSampleAllocatorUsage;

struct IMFSampleAllocatorControl : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown* pAllocator) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAllocatorUsage(DWORD dwOutputStreamID, DWORD* pdwInputStreamID,
                                                        MFSampleAllocatorUsage* peUsage) = 0;
};
__CRT_UUID_DECL(IMFSampleAllocatorControl, 0xda62b958, 0x3a38, 0x4a97, 0xbd, 0x27, 0x14, 0x9c, 0x64, 0x0c, 0x07, 0x71)

// ---- mfidl.h: sensor profiles (functions live in mfsensorgroup.dll; we resolve them at runtime) ---------------------
typedef struct SENSORPROFILEID {
    GUID Type;
    UINT32 Index;
    UINT32 Unused;
} SENSORPROFILEID;

struct IMFSensorProfile : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetProfileId(SENSORPROFILEID* pId) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddProfileFilter(UINT32 StreamId, LPCWSTR wzFilterSetString) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsMediaTypeSupported(UINT32 StreamId, IMFMediaType* pMediaType,
                                                           BOOL* pfSupported) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddBlockedControl(LPCWSTR wzBlockedControl) = 0;
};
__CRT_UUID_DECL(IMFSensorProfile, 0x22f765d1, 0x8dab, 0x4107, 0x84, 0x6d, 0x56, 0xba, 0xf7, 0x22, 0x15, 0xe7)

struct IMFSensorProfileCollection : public IUnknown {
    virtual DWORD STDMETHODCALLTYPE GetProfileCount() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProfile(DWORD Index, IMFSensorProfile** ppProfile) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddProfile(IMFSensorProfile* pProfile) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindProfile(SENSORPROFILEID* ProfileId, IMFSensorProfile** ppProfile) = 0;
    virtual void STDMETHODCALLTYPE RemoveProfileByIndex(DWORD Index) = 0;
    virtual void STDMETHODCALLTYPE RemoveProfile(SENSORPROFILEID* ProfileId) = 0;
};
__CRT_UUID_DECL(IMFSensorProfileCollection, 0xc95ea55b, 0x0187, 0x48be, 0x93, 0x53, 0x8d, 0x25, 0x07, 0x66, 0x23, 0x51)

// ---- mfvirtualcamera.h ----------------------------------------------------------------------------------------------
DEFINE_GUID(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 0xf0273718, 0x4a4d, 0x4ac5, 0xa1, 0x5d, 0x30, 0x5e, 0xb5,
            0xe9, 0x06, 0x67);

typedef enum MFVirtualCameraType { MFVirtualCameraType_SoftwareCameraSource = 0 } MFVirtualCameraType;
typedef enum MFVirtualCameraLifetime {
    MFVirtualCameraLifetime_Session = 0,
    MFVirtualCameraLifetime_System = 1
} MFVirtualCameraLifetime;
typedef enum MFVirtualCameraAccess {
    MFVirtualCameraAccess_CurrentUser = 0,
    MFVirtualCameraAccess_AllUsers = 1
} MFVirtualCameraAccess;

struct IMFCameraSyncObject : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE WaitOnSignal(DWORD timeOutInMs) = 0;
    virtual void STDMETHODCALLTYPE Shutdown() = 0;
};
__CRT_UUID_DECL(IMFCameraSyncObject, 0x6338b23a, 0x3042, 0x49d2, 0xa3, 0xea, 0xec, 0x0f, 0xed, 0x81, 0x54, 0x07)

struct IMFVirtualCamera : public IMFAttributes {
    virtual HRESULT STDMETHODCALLTYPE AddDeviceSourceInfo(LPCWSTR DeviceSourceInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddProperty(const DEVPROPKEY* pKey, DEVPROPTYPE Type, const BYTE* pbData,
                                                  ULONG cbData) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddRegistryEntry(LPCWSTR EntryName, LPCWSTR SubkeyPath, DWORD dwRegType,
                                                       const BYTE* pbData, ULONG cbData) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(IMFAsyncCallback* pCallback) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource** ppMediaSource) = 0;
    virtual HRESULT STDMETHODCALLTYPE SendCameraProperty(REFGUID propertySet, ULONG propertyId, ULONG propertyFlags,
                                                         void* propertyPayload, ULONG propertyPayloadLength,
                                                         void* data, ULONG dataLength, ULONG* dataWritten) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncEvent(REFGUID kseventSet, ULONG kseventId, ULONG kseventFlags,
                                                      HANDLE eventHandle, IMFCameraSyncObject** cameraSyncObject) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncSemaphore(REFGUID kseventSet, ULONG kseventId, ULONG kseventFlags,
                                                          HANDLE semaphoreHandle, LONG semaphoreAdjustment,
                                                          IMFCameraSyncObject** cameraSyncObject) = 0;
    virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0;
};
__CRT_UUID_DECL(IMFVirtualCamera, 0x1c08a864, 0xef6c, 0x4c75, 0xaf, 0x59, 0x5f, 0x2d, 0x68, 0xda, 0x95, 0x63)

#endif  // !__has_include(<mfvirtualcamera.h>)

// Both are exported by mfsensorgroup.dll (Windows 10 1803+ / 11). We GetProcAddress them so neither target links
// mfsensorgroup.lib. Typedef names differ from the SDK's function names on purpose (no clash with its prototypes).
typedef HRESULT(STDAPICALLTYPE* PFN_OpalMFCreateVirtualCamera)(MFVirtualCameraType type,
                                                               MFVirtualCameraLifetime lifetime,
                                                               MFVirtualCameraAccess access, LPCWSTR friendlyName,
                                                               LPCWSTR sourceId, const GUID* categories,
                                                               ULONG categoryCount, IMFVirtualCamera** virtualCamera);
typedef HRESULT(STDAPICALLTYPE* PFN_OpalMFCreateSensorProfile)(REFGUID ProfileType, UINT32 ProfileIndex,
                                                               LPCWSTR Constraints, IMFSensorProfile** ppProfile);
typedef HRESULT(STDAPICALLTYPE* PFN_OpalMFCreateSensorProfileCollection)(IMFSensorProfileCollection** ppCollection);
