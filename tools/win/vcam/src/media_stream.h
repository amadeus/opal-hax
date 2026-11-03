// The single video stream of the "Opal C1" media source: NV12 1920x1080 @ 30 fps, frames from the shared-memory
// channel. Frame Server pulls frames with RequestSample; each one is answered right away with an MEMediaSample event.
#pragma once
#include "common.h"
#include "frame_channel.h"

namespace opal {

class MediaStream final : public IMFMediaStream2, public IKsControl {
public:
    static HRESULT create(IMFMediaSource* source, DWORD streamId, MediaStream** out);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // IKsControl: no camera controls; ERROR_SET_NOT_FOUND is what KS returns for "no handler for this set".
    STDMETHODIMP KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID data, ULONG dataLength,
                            ULONG* bytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLength, LPVOID data, ULONG dataLength,
                          ULONG* bytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT event, ULONG eventLength, LPVOID data, ULONG dataLength,
                         ULONG* bytesReturned) override;

    // Called by MediaSource (which holds its own lock; the stream never calls back into the source).
    HRESULT start(IMFMediaType* type);  // stream selected in IMFMediaSource::Start: runs and sends MEStreamStarted
    HRESULT stop(bool sendEvent);
    HRESULT setAllocator(IMFVideoSampleAllocator* allocator);
    HRESULT getAttributes(IMFAttributes** attributes);
    void shutdown();
    DWORD id() const { return id_; }

private:
    MediaStream() = default;
    ~MediaStream() = default;
    HRESULT initialize(IMFMediaSource* source, DWORD streamId);
    HRESULT checkShutdown() const;
    HRESULT startInternal(bool sendEvent);
    HRESULT stopInternal(bool sendEvent);
    HRESULT writeFrame(IMFSample* sample);

    ModuleRef moduleRef_;
    LONG refs_ = 1;
    CritSec lock_;
    DWORD id_ = 0;
    bool shutdown_ = false;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    ComPtr<IMFMediaSource> source_;  // strong ref (GetMediaSource must hand it out); cycle broken by shutdown()
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFMediaType> mediaType_;
    ComPtr<IMFVideoSampleAllocator> allocator_;  // from SetDefaultAllocator, or our own if none was provided
    FrameChannel channel_;
};

}  // namespace opal
