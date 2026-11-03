#include "activator.h"

#include <new>

#include "media_source.h"

namespace opal {

HRESULT Activator::create(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    Activator* a = new (std::nothrow) Activator();
    if (!a) return E_OUTOFMEMORY;
    HRESULT hr = a->initialize();
    if (SUCCEEDED(hr)) hr = a->QueryInterface(riid, ppv);
    a->Release();
    return hr;
}

HRESULT Activator::initialize() {
    OPAL_RETURN_IF_FAILED(MFCreateAttributes(attributes_.put(), 2));
    // Set by both reference implementations. It asks Frame Server to pass the physical cameras a virtual camera is
    // associated with (MF_VIRTUALCAMERA_ASSOCIATED_CAMERA_SOURCES); we have none, so nothing arrives and it's harmless.
    OPAL_RETURN_IF_FAILED(attributes_->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1));
    return S_OK;
}

Activator::~Activator() {
    if (source_) source_->Release();
}

STDMETHODIMP Activator::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate)) {
        *ppv = static_cast<IMFActivate*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) Activator::AddRef() { return InterlockedIncrement(&refs_); }

STDMETHODIMP_(ULONG) Activator::Release() {
    ULONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return n;
}

// IMFActivate contract: the first call creates the object, later calls return the same one until
// ShutdownObject/DetachObject.
STDMETHODIMP Activator::ActivateObject(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    AutoLock lock(lock_);
    if (!source_) OPAL_RETURN_IF_FAILED(MediaSource::create(attributes_.get(), &source_));
    return source_->QueryInterface(riid, ppv);
}

STDMETHODIMP Activator::ShutdownObject() {
    AutoLock lock(lock_);
    if (source_) {
        source_->Shutdown();
        source_->Release();
        source_ = nullptr;
    }
    return S_OK;
}

STDMETHODIMP Activator::DetachObject() {
    AutoLock lock(lock_);
    if (source_) {
        source_->Release();  // the caller owns the source's lifetime (and its Shutdown) from now on
        source_ = nullptr;
    }
    return S_OK;
}

}  // namespace opal
