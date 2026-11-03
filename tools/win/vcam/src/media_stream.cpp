#include "media_stream.h"

#include <new>

namespace opal {

namespace {

const UINT32 kAllocatorSamples = 10;  // same pool size as Microsoft's SimpleMediaSource sample
const LONGLONG kFrameDuration = 10000000LL / OPAL_VCAM_FPS;  // 100 ns units: 333333 at 30 fps

HRESULT createMediaType(IMFMediaType** out) {
    ComPtr<IMFMediaType> t;
    OPAL_RETURN_IF_FAILED(MFCreateMediaType(t.put()));
    OPAL_RETURN_IF_FAILED(t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    OPAL_RETURN_IF_FAILED(t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    OPAL_RETURN_IF_FAILED(MFSetAttributeSize(t.get(), MF_MT_FRAME_SIZE, OPAL_VCAM_WIDTH, OPAL_VCAM_HEIGHT));
    OPAL_RETURN_IF_FAILED(MFSetAttributeRatio(t.get(), MF_MT_FRAME_RATE, OPAL_VCAM_FPS, 1));
    OPAL_RETURN_IF_FAILED(MFSetAttributeRatio(t.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    OPAL_RETURN_IF_FAILED(t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    OPAL_RETURN_IF_FAILED(t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    OPAL_RETURN_IF_FAILED(t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
    OPAL_RETURN_IF_FAILED(t->SetUINT32(MF_MT_DEFAULT_STRIDE, OPAL_VCAM_WIDTH));  // NV12: luma stride = width
    OPAL_RETURN_IF_FAILED(t->SetUINT32(MF_MT_SAMPLE_SIZE, OPAL_VCAM_FRAME_BYTES));
    OPAL_RETURN_IF_FAILED(t->SetUINT32(MF_MT_AVG_BITRATE, OPAL_VCAM_FRAME_BYTES * 8 * OPAL_VCAM_FPS));
    return t.copyTo(out);
}

// The attributes Frame Server uses to classify a stream: a shareable color video-capture pin.
HRESULT setStreamAttributes(IMFAttributes* a, DWORD streamId) {
    OPAL_RETURN_IF_FAILED(a->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
    OPAL_RETURN_IF_FAILED(a->SetUINT32(MF_DEVICESTREAM_STREAM_ID, streamId));
    OPAL_RETURN_IF_FAILED(a->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    OPAL_RETURN_IF_FAILED(a->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));
    return S_OK;
}

}  // namespace

HRESULT MediaStream::create(IMFMediaSource* source, DWORD streamId, MediaStream** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    MediaStream* s = new (std::nothrow) MediaStream();
    if (!s) return E_OUTOFMEMORY;
    HRESULT hr = s->initialize(source, streamId);
    if (FAILED(hr)) {
        s->Release();
        return hr;
    }
    *out = s;
    return S_OK;
}

HRESULT MediaStream::initialize(IMFMediaSource* source, DWORD streamId) {
    source_.reset(source);
    id_ = streamId;

    OPAL_RETURN_IF_FAILED(MFCreateAttributes(attributes_.put(), 4));
    OPAL_RETURN_IF_FAILED(setStreamAttributes(attributes_.get(), id_));
    OPAL_RETURN_IF_FAILED(MFCreateEventQueue(queue_.put()));

    OPAL_RETURN_IF_FAILED(createMediaType(mediaType_.put()));
    IMFMediaType* types[] = {mediaType_.get()};
    OPAL_RETURN_IF_FAILED(MFCreateStreamDescriptor(id_, 1, types, descriptor_.put()));
    ComPtr<IMFMediaTypeHandler> handler;
    OPAL_RETURN_IF_FAILED(descriptor_->GetMediaTypeHandler(handler.put()));
    OPAL_RETURN_IF_FAILED(handler->SetCurrentMediaType(mediaType_.get()));
    // Microsoft's sample puts the same attributes on the stream descriptor as well.
    OPAL_RETURN_IF_FAILED(setStreamAttributes(descriptor_.get(), id_));
    return S_OK;
}

// ---- IUnknown ----------------------------------------------------------------------------------------------------

STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream) ||
        riid == __uuidof(IMFMediaStream2)) {
        *ppv = static_cast<IMFMediaStream2*>(this);
    } else if (riid == IID_IKsControl) {
        *ppv = static_cast<IKsControl*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaStream::AddRef() { return InterlockedIncrement(&refs_); }

STDMETHODIMP_(ULONG) MediaStream::Release() {
    ULONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
}

// ---- IMFMediaEventGenerator ----------------------------------------------------------------------------------------

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return queue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return queue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    // GetEvent may block until an event arrives: don't hold our lock while waiting (RequestSample needs it to queue).
    ComPtr<IMFMediaEventQueue> queue;
    {
        AutoLock lock(lock_);
        OPAL_RETURN_IF_FAILED(checkShutdown());
        queue = queue_;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                     const PROPVARIANT* value) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return queue_->QueueEventParamVar(type, extendedType, status, value);
}

// ---- IMFMediaStream ------------------------------------------------------------------------------------------------

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    *source = nullptr;
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return source_.copyTo(source);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    *descriptor = nullptr;
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return descriptor_.copyTo(descriptor);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;

    ComPtr<IMFSample> sample;
    OPAL_RETURN_IF_FAILED(allocator_->AllocateSample(sample.put()));
    OPAL_RETURN_IF_FAILED(writeFrame(sample.get()));
    // Absolute system (QPC) time, the clock MESourceStarted was stamped with; what both reference samples do.
    OPAL_RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
    OPAL_RETURN_IF_FAILED(sample->SetSampleDuration(kFrameDuration));
    // The token lets the requester match this sample to its request.
    if (token) OPAL_RETURN_IF_FAILED(sample->SetUnknown(MFSampleExtension_Token, token));
    return queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.get());
}

HRESULT MediaStream::writeFrame(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    OPAL_RETURN_IF_FAILED(sample->GetBufferByIndex(0, buffer.put()));

    // Allocator samples are 2D buffers whose pitch may exceed the width; honor it.
    ComPtr<IMF2DBuffer2> buffer2d;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(buffer2d.put())))) {
        BYTE* scan0 = nullptr;
        BYTE* start = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        OPAL_RETURN_IF_FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &length));
        HRESULT hr = S_OK;
        // A bottom-up (negative pitch) or too-small buffer would mean the allocator ignored our NV12 media type.
        if (pitch < OPAL_VCAM_WIDTH || (UINT64)pitch * (OPAL_VCAM_HEIGHT * 3 / 2) > length)
            hr = MF_E_BUFFERTOOSMALL;
        else
            channel_.fill(scan0, pitch);
        buffer2d->Unlock2D();
        return hr;
    }

    BYTE* data = nullptr;
    DWORD maxLength = 0;
    OPAL_RETURN_IF_FAILED(buffer->Lock(&data, &maxLength, nullptr));
    HRESULT hr = S_OK;
    if (maxLength < OPAL_VCAM_FRAME_BYTES)
        hr = MF_E_BUFFERTOOSMALL;
    else
        channel_.fill(data, OPAL_VCAM_WIDTH);
    buffer->Unlock();
    if (SUCCEEDED(hr)) hr = buffer->SetCurrentLength(OPAL_VCAM_FRAME_BYTES);
    return hr;
}

// ---- IMFMediaStream2 -----------------------------------------------------------------------------------------------

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    if (state == state_) return S_OK;
    switch (state) {
        case MF_STREAM_STATE_PAUSED:
            if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
            state_ = MF_STREAM_STATE_PAUSED;
            return S_OK;
        case MF_STREAM_STATE_RUNNING:
            return startInternal(false);
        case MF_STREAM_STATE_STOPPED:
            return stopInternal(false);
        default:
            return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    *state = state_;
    return S_OK;
}

// ---- IKsControl ----------------------------------------------------------------------------------------------------

STDMETHODIMP MediaStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* bytesReturned) {
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* bytesReturned) {
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* bytesReturned) {
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ---- called by MediaSource -----------------------------------------------------------------------------------------

HRESULT MediaStream::start(IMFMediaType* type) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    if (type) mediaType_.reset(type);  // we only offer one type, so this is it
    return startInternal(true);
}

HRESULT MediaStream::stop(bool sendEvent) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return stopInternal(sendEvent);
}

HRESULT MediaStream::setAllocator(IMFVideoSampleAllocator* allocator) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    if (state_ == MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;  // can't swap the pool under a running stream
    allocator_.reset(allocator);
    return S_OK;
}

HRESULT MediaStream::getAttributes(IMFAttributes** attributes) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return attributes_.copyTo(attributes);
}

void MediaStream::shutdown() {
    AutoLock lock(lock_);
    if (shutdown_) return;
    shutdown_ = true;
    state_ = MF_STREAM_STATE_STOPPED;
    if (queue_) queue_->Shutdown();
    channel_.close();
    allocator_.reset();
    source_.reset();  // breaks the source <-> stream reference cycle
}

HRESULT MediaStream::checkShutdown() const { return shutdown_ ? MF_E_SHUTDOWN : S_OK; }

HRESULT MediaStream::startInternal(bool sendEvent) {
    if (state_ == MF_STREAM_STATE_STOPPED) {
        // Frame Server normally hands us its allocator via IMFSampleAllocatorControl::SetDefaultAllocator before
        // starting. If it didn't, use our own system-memory allocator instead of failing.
        if (!allocator_) {
            ComPtr<IMFVideoSampleAllocatorEx> own;
            OPAL_RETURN_IF_FAILED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(own.put())));
            allocator_.reset(own.get());
            Log(L"stream: no allocator provided, using our own");
        }
        OPAL_RETURN_IF_FAILED(allocator_->InitializeSampleAllocator(kAllocatorSamples, mediaType_.get()));

        // A missing channel is not fatal: the stream runs and shows the placeholder.
        HRESULT hr = channel_.open();
        if (FAILED(hr)) Log(L"stream: frame channel unavailable (0x%08X), serving placeholder", hr);
    }
    if (sendEvent) OPAL_RETURN_IF_FAILED(queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
    state_ = MF_STREAM_STATE_RUNNING;
    Log(L"stream: running");
    return S_OK;
}

HRESULT MediaStream::stopInternal(bool sendEvent) {
    state_ = MF_STREAM_STATE_STOPPED;
    // Closing our handle removes the mapping (unless the producer still has it open), which is how the Opal app
    // learns that no app is using the camera any more.
    channel_.close();
    if (sendEvent) OPAL_RETURN_IF_FAILED(queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
    Log(L"stream: stopped");
    return S_OK;
}

}  // namespace opal
