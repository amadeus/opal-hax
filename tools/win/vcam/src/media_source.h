// The "Opal C1" media source loaded by the Windows Camera Frame Server: one live NV12 stream (MediaStream).
// Structure follows Microsoft's VirtualCamera SimpleMediaSource sample and smourier/VCamSample, the two known-working
// MFCreateVirtualCamera sources: IMFMediaSourceEx + IMFGetService + IKsControl + IMFSampleAllocatorControl.
#pragma once
#include "common.h"

namespace opal {

class MediaStream;

class MediaSource final : public IMFMediaSourceEx,
                          public IMFGetService,
                          public IKsControl,
                          public IMFSampleAllocatorControl {
public:
    // activationAttributes: the activator's attribute store, copied into the source attributes.
    static HRESULT create(IMFAttributes* activationAttributes, MediaSource** out);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                       const PROPVARIANT* startPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* ppv) override;

    // IKsControl: no camera controls exposed.
    STDMETHODIMP KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID data, ULONG dataLength,
                            ULONG* bytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLength, LPVOID data, ULONG dataLength,
                          ULONG* bytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT event, ULONG eventLength, LPVOID data, ULONG dataLength,
                         ULONG* bytesReturned) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD streamId, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) override;

private:
    enum class State { Stopped, Started, Shutdown };

    MediaSource() = default;
    ~MediaSource();
    HRESULT initialize(IMFAttributes* activationAttributes);
    HRESULT checkShutdown() const;
    HRESULT addSensorProfiles();

    static const DWORD kStreamId = 0;

    ModuleRef moduleRef_;
    LONG refs_ = 1;
    CritSec lock_;
    State state_ = State::Stopped;
    bool streamActive_ = false;  // stream started since creation / last Stop: decides MENewStream vs MEUpdatedStream
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFPresentationDescriptor> descriptor_;
    MediaStream* stream_ = nullptr;  // owned reference
};

}  // namespace opal
