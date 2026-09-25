// =============================================================================
// AttributeStore.h  --  IMFAttributes implemented by delegation
// =============================================================================
// IMFActivate derives from IMFAttributes, so the activation object has to
// expose all thirty attribute methods.  They are forwarded verbatim to a
// store created by MFCreateAttributes.
// =============================================================================

#pragma once

#include <mfapi.h>
#include <mfobjects.h>
#include <wil/com.h>

// Expands to the IMFAttributes method set, forwarding to `store`
// (a wil::com_ptr_nothrow<IMFAttributes> member).
#define VCAM_FORWARD_IMFATTRIBUTES(store)                                                                                             \
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return store->GetItem(k, v); }                                         \
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return store->GetItemType(k, t); }                           \
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return store->CompareItem(k, v, r); }                   \
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return store->Compare(o, m, r); }           \
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return store->GetUINT32(k, v); }                                          \
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return store->GetUINT64(k, v); }                                          \
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return store->GetDouble(k, v); }                                          \
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return store->GetGUID(k, v); }                                                \
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* n) override { return store->GetStringLength(k, n); }                              \
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* w) override { return store->GetString(k, s, n, w); }                \
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* n) override { return store->GetAllocatedString(k, s, n); }          \
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return store->GetBlobSize(k, n); }                                      \
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* w) override { return store->GetBlob(k, b, n, w); }                    \
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return store->GetAllocatedBlob(k, b, n); }              \
    STDMETHODIMP GetUnknown(REFGUID k, REFIID i, LPVOID* p) override { return store->GetUnknown(k, i, p); }                           \
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return store->SetItem(k, v); }                                       \
    STDMETHODIMP DeleteItem(REFGUID k) override { return store->DeleteItem(k); }                                                      \
    STDMETHODIMP DeleteAllItems() override { return store->DeleteAllItems(); }                                                        \
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return store->SetUINT32(k, v); }                                           \
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return store->SetUINT64(k, v); }                                           \
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return store->SetDouble(k, v); }                                           \
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return store->SetGUID(k, v); }                                              \
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return store->SetString(k, v); }                                          \
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return store->SetBlob(k, b, n); }                            \
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* p) override { return store->SetUnknown(k, p); }                                      \
    STDMETHODIMP LockStore() override { return store->LockStore(); }                                                                  \
    STDMETHODIMP UnlockStore() override { return store->UnlockStore(); }                                                              \
    STDMETHODIMP GetCount(UINT32* n) override { return store->GetCount(n); }                                                          \
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return store->GetItemByIndex(i, k, v); }                \
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return store->CopyAllItems(d); }
