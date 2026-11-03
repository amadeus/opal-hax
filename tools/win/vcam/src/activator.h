// The COM class registered under OPAL_VCAM_CLSID_STRING. MFCreateVirtualCamera only records that CLSID; Frame Server
// (and the Frame Server Monitor, and the registering app during validation) CoCreate it in their own processes and
// call IMFActivate::ActivateObject to get the media source. IMFAttributes is forwarded to an internal store.
#pragma once
#include "common.h"

namespace opal {

class MediaSource;

class Activator final : public IMFActivate {
public:
    static HRESULT create(REFIID riid, void** ppv);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

    // IMFAttributes (forwarded to attributes_)
    STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* value) override { return attributes_->GetItem(key, value); }
    STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override {
        return attributes_->GetItemType(key, type);
    }
    STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) override {
        return attributes_->CompareItem(key, value, result);
    }
    STDMETHODIMP Compare(IMFAttributes* theirs, MF_ATTRIBUTES_MATCH_TYPE type, BOOL* result) override {
        return attributes_->Compare(theirs, type, result);
    }
    STDMETHODIMP GetUINT32(REFGUID key, UINT32* value) override { return attributes_->GetUINT32(key, value); }
    STDMETHODIMP GetUINT64(REFGUID key, UINT64* value) override { return attributes_->GetUINT64(key, value); }
    STDMETHODIMP GetDouble(REFGUID key, double* value) override { return attributes_->GetDouble(key, value); }
    STDMETHODIMP GetGUID(REFGUID key, GUID* value) override { return attributes_->GetGUID(key, value); }
    STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override {
        return attributes_->GetStringLength(key, length);
    }
    STDMETHODIMP GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length) override {
        return attributes_->GetString(key, value, size, length);
    }
    STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length) override {
        return attributes_->GetAllocatedString(key, value, length);
    }
    STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override { return attributes_->GetBlobSize(key, size); }
    STDMETHODIMP GetBlob(REFGUID key, UINT8* buffer, UINT32 size, UINT32* blobSize) override {
        return attributes_->GetBlob(key, buffer, size, blobSize);
    }
    STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** buffer, UINT32* size) override {
        return attributes_->GetAllocatedBlob(key, buffer, size);
    }
    STDMETHODIMP GetUnknown(REFGUID key, REFIID riid, LPVOID* ppv) override {
        return attributes_->GetUnknown(key, riid, ppv);
    }
    STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT value) override { return attributes_->SetItem(key, value); }
    STDMETHODIMP DeleteItem(REFGUID key) override { return attributes_->DeleteItem(key); }
    STDMETHODIMP DeleteAllItems() override { return attributes_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID key, UINT32 value) override { return attributes_->SetUINT32(key, value); }
    STDMETHODIMP SetUINT64(REFGUID key, UINT64 value) override { return attributes_->SetUINT64(key, value); }
    STDMETHODIMP SetDouble(REFGUID key, double value) override { return attributes_->SetDouble(key, value); }
    STDMETHODIMP SetGUID(REFGUID key, REFGUID value) override { return attributes_->SetGUID(key, value); }
    STDMETHODIMP SetString(REFGUID key, LPCWSTR value) override { return attributes_->SetString(key, value); }
    STDMETHODIMP SetBlob(REFGUID key, const UINT8* buffer, UINT32 size) override {
        return attributes_->SetBlob(key, buffer, size);
    }
    STDMETHODIMP SetUnknown(REFGUID key, IUnknown* value) override { return attributes_->SetUnknown(key, value); }
    STDMETHODIMP LockStore() override { return attributes_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return attributes_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* count) override { return attributes_->GetCount(count); }
    STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value) override {
        return attributes_->GetItemByIndex(index, key, value);
    }
    STDMETHODIMP CopyAllItems(IMFAttributes* dest) override { return attributes_->CopyAllItems(dest); }

private:
    Activator() = default;
    ~Activator();
    HRESULT initialize();

    ModuleRef moduleRef_;
    LONG refs_ = 1;
    CritSec lock_;
    ComPtr<IMFAttributes> attributes_;
    MediaSource* source_ = nullptr;  // owned reference; created on the first ActivateObject
};

}  // namespace opal
