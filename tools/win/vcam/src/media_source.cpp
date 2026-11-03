#include "media_source.h"

#include <new>

#include "media_stream.h"

namespace opal {

namespace {

PROPVARIANT systemTimeVariant() {
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = MFGetSystemTime();
    return v;
}

IUnknown* asUnknown(MediaStream* s) { return static_cast<IMFMediaStream*>(s); }

}  // namespace

HRESULT MediaSource::create(IMFAttributes* activationAttributes, MediaSource** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    MediaSource* s = new (std::nothrow) MediaSource();
    if (!s) return E_OUTOFMEMORY;
    HRESULT hr = s->initialize(activationAttributes);
    if (FAILED(hr)) {
        Log(L"source: initialize failed 0x%08X", hr);
        s->Shutdown();  // breaks the stream -> source reference so Release can free it
        s->Release();
        return hr;
    }
    *out = s;
    return S_OK;
}

MediaSource::~MediaSource() {
    if (stream_) {
        stream_->shutdown();
        stream_->Release();
    }
}

HRESULT MediaSource::initialize(IMFAttributes* activationAttributes) {
    OPAL_RETURN_IF_FAILED(MFCreateAttributes(attributes_.put(), 4));
    if (activationAttributes) OPAL_RETURN_IF_FAILED(activationAttributes->CopyAllItems(attributes_.get()));

    HRESULT hr = addSensorProfiles();
    if (FAILED(hr)) Log(L"source: sensor profiles not set (0x%08X)", hr);

    OPAL_RETURN_IF_FAILED(MFCreateEventQueue(queue_.put()));
    OPAL_RETURN_IF_FAILED(MediaStream::create(static_cast<IMFMediaSourceEx*>(this), kStreamId, &stream_));

    ComPtr<IMFStreamDescriptor> streamDescriptor;
    OPAL_RETURN_IF_FAILED(stream_->GetStreamDescriptor(streamDescriptor.put()));
    IMFStreamDescriptor* descriptors[] = {streamDescriptor.get()};
    OPAL_RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, descriptors, descriptor_.put()));
    Log(L"source: created");
    return S_OK;
}

// Both reference sources publish a sensor profile collection; Microsoft's sample notes the Legacy profile is
// mandatory so that profile-aware apps can use the camera. One profile: our only stream, up to 30 fps.
HRESULT MediaSource::addSensorProfiles() {
    // mfsensorgroup.dll is a system DLL that Frame Server has loaded already; never freed, because the profile objects
    // we create live in it.
    HMODULE dll = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) return HRESULT_FROM_WIN32(GetLastError());
    auto createCollection = reinterpret_cast<PFN_OpalMFCreateSensorProfileCollection>(
        reinterpret_cast<void*>(GetProcAddress(dll, "MFCreateSensorProfileCollection")));
    auto createProfile = reinterpret_cast<PFN_OpalMFCreateSensorProfile>(
        reinterpret_cast<void*>(GetProcAddress(dll, "MFCreateSensorProfile")));
    if (!createCollection || !createProfile) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    ComPtr<IMFSensorProfileCollection> collection;
    OPAL_RETURN_IF_FAILED(createCollection(collection.put()));
    ComPtr<IMFSensorProfile> legacy;
    OPAL_RETURN_IF_FAILED(createProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, legacy.put()));
    OPAL_RETURN_IF_FAILED(legacy->AddProfileFilter(kStreamId, L"((RES==;FRT<=30,1;SUT==))"));
    OPAL_RETURN_IF_FAILED(collection->AddProfile(legacy.get()));
    return attributes_->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, collection.get());
}

HRESULT MediaSource::checkShutdown() const { return state_ == State::Shutdown ? MF_E_SHUTDOWN : S_OK; }

// ---- IUnknown ------------------------------------------------------------------------------------------------------

STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource) ||
        riid == __uuidof(IMFMediaSourceEx)) {
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    } else if (riid == __uuidof(IMFGetService)) {
        *ppv = static_cast<IMFGetService*>(this);
    } else if (riid == IID_IKsControl) {
        *ppv = static_cast<IKsControl*>(this);
    } else if (riid == __uuidof(IMFSampleAllocatorControl)) {
        *ppv = static_cast<IMFSampleAllocatorControl*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaSource::AddRef() { return InterlockedIncrement(&refs_); }

STDMETHODIMP_(ULONG) MediaSource::Release() {
    ULONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
}

// ---- IMFMediaEventGenerator ----------------------------------------------------------------------------------------

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return queue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return queue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    // May block: wait without holding our lock.
    ComPtr<IMFMediaEventQueue> queue;
    {
        AutoLock lock(lock_);
        OPAL_RETURN_IF_FAILED(checkShutdown());
        queue = queue_;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                     const PROPVARIANT* value) {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return queue_->QueueEventParamVar(type, extendedType, status, value);
}

// ---- IMFMediaSource ------------------------------------------------------------------------------------------------

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    *descriptor = nullptr;
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return descriptor_->Clone(descriptor);
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                                const PROPVARIANT* startPosition) {
    if (!descriptor || !startPosition) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());

    DWORD count = 0;
    OPAL_RETURN_IF_FAILED(descriptor->GetStreamDescriptorCount(&count));
    if (count != 1) return E_INVALIDARG;
    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> streamDescriptor;
    OPAL_RETURN_IF_FAILED(descriptor->GetStreamDescriptorByIndex(0, &selected, streamDescriptor.put()));
    DWORD id = 0;
    OPAL_RETURN_IF_FAILED(streamDescriptor->GetStreamIdentifier(&id));
    if (id != kStreamId) return E_INVALIDARG;

    PROPVARIANT startTime = systemTimeVariant();  // live source: "now" on the system clock, ignoring startPosition
    if (selected) {
        OPAL_RETURN_IF_FAILED(descriptor_->SelectStream(0));
        ComPtr<IMFMediaType> type;  // the caller's choice; with a single offered type this is ours anyway
        ComPtr<IMFMediaTypeHandler> handler;
        if (SUCCEEDED(streamDescriptor->GetMediaTypeHandler(handler.put()))) handler->GetCurrentMediaType(type.put());
        // MENewStream on the first start, MEUpdatedStream when restarting a stream the client already has.
        OPAL_RETURN_IF_FAILED(
            queue_->QueueEventParamUnk(streamActive_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, asUnknown(stream_)));
        OPAL_RETURN_IF_FAILED(stream_->start(type.get()));  // queues MEStreamStarted on the stream
        streamActive_ = true;
    } else if (streamActive_) {
        OPAL_RETURN_IF_FAILED(descriptor_->DeselectStream(0));
        OPAL_RETURN_IF_FAILED(stream_->stop(false));
        streamActive_ = false;
    }
    OPAL_RETURN_IF_FAILED(queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &startTime));
    state_ = State::Started;
    Log(L"source: started (stream %ls)", selected ? L"selected" : L"not selected");
    return S_OK;
}

STDMETHODIMP MediaSource::Stop() {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    if (state_ != State::Started) return MF_E_INVALID_STATE_TRANSITION;

    PROPVARIANT stopTime = systemTimeVariant();
    if (streamActive_) {
        OPAL_RETURN_IF_FAILED(stream_->stop(true));  // queues MEStreamStopped on the stream
        streamActive_ = false;
    }
    OPAL_RETURN_IF_FAILED(descriptor_->DeselectStream(0));
    state_ = State::Stopped;
    OPAL_RETURN_IF_FAILED(queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &stopTime));
    Log(L"source: stopped");
    return S_OK;
}

STDMETHODIMP MediaSource::Pause() {
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return MF_E_INVALID_STATE_TRANSITION;  // a live camera can't pause
}

STDMETHODIMP MediaSource::Shutdown() {
    AutoLock lock(lock_);
    // Idempotent on purpose: VCamSample found that a second Shutdown (e.g. IMFVirtualCamera::Shutdown after Frame
    // Server already shut the source down) failing with MF_E_SHUTDOWN broke removing the camera.
    if (state_ == State::Shutdown) return S_OK;
    state_ = State::Shutdown;
    if (queue_) queue_->Shutdown();
    if (stream_) {
        stream_->shutdown();  // also closes the frame channel
        stream_->Release();
        stream_ = nullptr;
    }
    descriptor_.reset();
    Log(L"source: shut down");
    return S_OK;
}

// ---- IMFMediaSourceEx ----------------------------------------------------------------------------------------------

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    return attributes_.copyTo(attributes);
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    if (streamId != kStreamId) return MF_E_NOT_FOUND;
    return stream_->getAttributes(attributes);
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*) {
    // Informational only: we fill frames on the CPU, so the DXGI device manager isn't needed.
    return S_OK;
}

// ---- IMFGetService -------------------------------------------------------------------------------------------------

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// ---- IKsControl ----------------------------------------------------------------------------------------------------

STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* bytesReturned) {
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* bytesReturned) {
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* bytesReturned) {
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ---- IMFSampleAllocatorControl -------------------------------------------------------------------------------------

STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD streamId, IUnknown* allocator) {
    if (!allocator) return E_POINTER;
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    if (streamId != kStreamId) return MF_E_NOT_FOUND;
    ComPtr<IMFVideoSampleAllocator> videoAllocator;
    OPAL_RETURN_IF_FAILED(allocator->QueryInterface(IID_PPV_ARGS(videoAllocator.put())));
    return stream_->setAllocator(videoAllocator.get());
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) {
    if (!inputStreamId || !usage) return E_POINTER;
    AutoLock lock(lock_);
    OPAL_RETURN_IF_FAILED(checkShutdown());
    if (streamId != kStreamId) return MF_E_NOT_FOUND;
    *inputStreamId = streamId;
    // Like Microsoft's sample: samples come from the allocator Frame Server gives us (we fall back to our own).
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

}  // namespace opal
