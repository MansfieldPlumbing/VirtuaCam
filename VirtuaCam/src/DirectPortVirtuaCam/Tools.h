#pragma once

// General Helpers
std::string to_string(const std::wstring& ws);
std::wstring to_wstring(const std::string& s);
const std::wstring GUID_ToStringW(const GUID& guid, bool resolve = true);
const std::string GUID_ToStringA(const GUID& guid, bool resolve = true);
const std::wstring PROPVARIANT_ToString(const PROPVARIANT& pv);
D2D1_COLOR_F HSL2RGB(const float h, const float s, const float l);
const std::wstring GetProcessName(DWORD pid);
const LSTATUS RegWriteKey(HKEY key, PCWSTR path, HKEY* outKey);
const LSTATUS RegWriteValue(HKEY key, PCWSTR name, const std::wstring& value);
const LSTATUS RegWriteValue(HKEY key, PCWSTR name, DWORD value);
HRESULT RGB32ToNV12(BYTE* input, ULONG inputSize, LONG inputStride, UINT width, UINT height, BYTE* output, ULONG ouputSize, LONG outputStride);
HANDLE GetHandleFromName(const WCHAR* name);

struct BroadcastManifest {
    UINT64 frameValue;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LUID adapterLuid;
    WCHAR textureName[256];
    WCHAR fenceName[256];
};

// Media Foundation Helpers (from MFTools.h)
void TraceMFAttributes(IUnknown* unknown, PCWSTR prefix);
std::wstring PKSIDENTIFIER_ToString(PKSIDENTIFIER id, ULONG length);

template <class IFACE = IMFAttributes>
struct CBaseAttributes : public IFACE
{
protected:
	wil::com_ptr_nothrow<IMFAttributes> _attributes;
	std::wstring _trace;

	CBaseAttributes() :
		_trace(L"Atts")
	{
		THROW_IF_FAILED(MFCreateAttributes(&_attributes, 0));
	}

	void SetBaseAttributesTraceName(std::wstring trace)
	{
		_trace = trace;
	}

public:
	STDMETHODIMP GetItem(REFGUID guidKey, PROPVARIANT* value)
	{
		RETURN_HR_IF(E_INVALIDARG, !value);
		assert(_attributes);
		return _attributes->GetItem(guidKey, value);
	}

	STDMETHODIMP GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType)
	{
		RETURN_HR_IF(E_INVALIDARG, !pType);
		*pType = (MF_ATTRIBUTE_TYPE)0;
		assert(_attributes);
		return _attributes->GetItemType(guidKey, pType);
	}

	STDMETHODIMP CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult)
	{
		RETURN_HR_IF(E_INVALIDARG, !pbResult);
		assert(_attributes);
		return _attributes->CompareItem(guidKey, Value, pbResult);
	}

	STDMETHODIMP Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult)
	{
		RETURN_HR_IF(E_INVALIDARG, !pTheirs || !pbResult);
		assert(_attributes);
		return _attributes->Compare(pTheirs, MatchType, pbResult);
	}

	STDMETHODIMP GetUINT32(REFGUID guidKey, UINT32* punValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !punValue);
		*punValue = 0;
		assert(_attributes);
		return _attributes->GetUINT32(guidKey, punValue);
	}

	STDMETHODIMP GetUINT64(REFGUID guidKey, UINT64* punValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !punValue);
		*punValue = 0;
		assert(_attributes);
		return _attributes->GetUINT64(guidKey, punValue);
	}

	STDMETHODIMP GetDouble(REFGUID guidKey, double* pfValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !pfValue);
		*pfValue = 0;
		assert(_attributes);
		return _attributes->GetDouble(guidKey, pfValue);
	}

	STDMETHODIMP GetGUID(REFGUID guidKey, GUID* pguidValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !pguidValue);
		ZeroMemory(pguidValue, 16);
		assert(_attributes);
		return _attributes->GetGUID(guidKey, pguidValue);
	}

	STDMETHODIMP GetStringLength(REFGUID guidKey, UINT32* pcchLength)
	{
		RETURN_HR_IF(E_INVALIDARG, !pcchLength);
		*pcchLength = 0;
		assert(_attributes);
		return _attributes->GetStringLength(guidKey, pcchLength);
	}

	STDMETHODIMP GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength)
	{
		assert(_attributes);
		return _attributes->GetString(guidKey, pwszValue, cchBufSize, pcchLength);
	}

	STDMETHODIMP GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength)
	{
		RETURN_HR_IF(E_INVALIDARG, !ppwszValue || !pcchLength);
		*ppwszValue = 0;
		*pcchLength = 0;
		assert(_attributes);
		return _attributes->GetAllocatedString(guidKey, ppwszValue, pcchLength);
	}

	STDMETHODIMP GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize)
	{
		RETURN_HR_IF(E_INVALIDARG, !pcbBlobSize);
		assert(_attributes);
		return _attributes->GetBlobSize(guidKey, pcbBlobSize);
	}

	STDMETHODIMP GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize)
	{
		assert(_attributes);
		return _attributes->GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize);
	}

	STDMETHODIMP GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize)
	{
		RETURN_HR_IF(E_INVALIDARG, !ppBuf || !pcbSize);
		assert(_attributes);
		return _attributes->GetAllocatedBlob(guidKey, ppBuf, pcbSize);
	}

	STDMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv)
	{
		RETURN_HR_IF(E_INVALIDARG, !ppv);
		assert(_attributes);
		return _attributes->GetUnknown(guidKey, riid, ppv);
	}

	STDMETHODIMP SetItem(REFGUID guidKey, REFPROPVARIANT value)
	{
		assert(_attributes);
		return _attributes->SetItem(guidKey, value);
	}

	STDMETHODIMP DeleteItem(REFGUID guidKey)
	{
		assert(_attributes);
		return _attributes->DeleteItem(guidKey);
	}

	STDMETHODIMP DeleteAllItems()
	{
		assert(_attributes);
		return _attributes->DeleteAllItems();
	}

	STDMETHODIMP SetUINT32(REFGUID guidKey, UINT32 value)
	{
		assert(_attributes);
		return _attributes->SetUINT32(guidKey, value);
	}

	STDMETHODIMP SetUINT64(REFGUID guidKey, UINT64 value)
	{
		assert(_attributes);
		return _attributes->SetUINT64(guidKey, value);
	}

	STDMETHODIMP SetDouble(REFGUID guidKey, double value)
	{
		assert(_attributes);
		return _attributes->SetDouble(guidKey, value);
	}

	STDMETHODIMP SetGUID(REFGUID guidKey, REFGUID value)
	{
		assert(_attributes);
		return _attributes->SetGUID(guidKey, value);
	}

	STDMETHODIMP SetString(REFGUID guidKey, LPCWSTR value)
	{
		assert(_attributes);
		return _attributes->SetString(guidKey, value);
	}

	STDMETHODIMP SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbBufSize)
	{
		assert(_attributes);
		return _attributes->SetBlob(guidKey, pBuf, cbBufSize);
	}

	STDMETHODIMP SetUnknown(REFGUID guidKey, IUnknown* value)
	{
		assert(_attributes);
		return _attributes->SetUnknown(guidKey, value);
	}

	STDMETHODIMP LockStore()
	{
		assert(_attributes);
		return _attributes->LockStore();
	}

	STDMETHODIMP UnlockStore()
	{
		assert(_attributes);
		return _attributes->UnlockStore();
	}

	STDMETHODIMP GetCount(UINT32* pcItems)
	{
		RETURN_HR_IF(E_INVALIDARG, !pcItems);
		assert(_attributes);
		return _attributes->GetCount(pcItems);
	}

	STDMETHODIMP GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue)
	{
		assert(_attributes);
		return _attributes->GetItemByIndex(unIndex, pguidKey, pValue);
	}

	STDMETHODIMP CopyAllItems(IMFAttributes* pDest)
	{
		RETURN_HR_IF(E_INVALIDARG, !pDest);
		assert(_attributes);
		return _attributes->CopyAllItems(pDest);
	}

	HRESULT SerializeToStream(DWORD dwOptions, IStream* pStm)
	{
		RETURN_HR_IF(E_INVALIDARG, !pStm);
		assert(_attributes);
		return MFSerializeAttributesToStream(_attributes.get(), dwOptions, pStm);
	}

	HRESULT DeserializeFromStream(DWORD dwOptions, IStream* pStm)
	{
		RETURN_HR_IF(E_INVALIDARG, !pStm);
		assert(_attributes);
		return MFDeserializeAttributesFromStream(_attributes.get(), dwOptions, pStm);
	}

	HRESULT SerializeToBlob(UINT8** buffer, UINT32* size)
	{
		RETURN_HR_IF(E_INVALIDARG, !_attributes || !size);
		assert(_attributes);

		*buffer = NULL;
		*size = 0;

		UINT32 cbSize = 0;
		RETURN_IF_FAILED(MFGetAttributesAsBlobSize(_attributes.get(), &cbSize));

		auto pBuffer = (BYTE*)CoTaskMemAlloc(cbSize);
		RETURN_IF_NULL_ALLOC(pBuffer);

		auto hr = MFGetAttributesAsBlob(_attributes.get(), pBuffer, cbSize);
		if (SUCCEEDED(hr))
		{
			*buffer = pBuffer;
			*size = cbSize;
		}
		else
		{
			CoTaskMemFree(pBuffer);
		}
		return hr;
	}

	HRESULT DeserializeFromBlob(const UINT8* buffer, UINT size)
	{
		RETURN_HR_IF(E_INVALIDARG, !buffer || !size);
		assert(_attributes);
		return MFInitAttributesFromBlob(_attributes.get(), buffer, size);
	}

	HRESULT GetRatio(REFGUID guidKey, UINT32* pnNumerator, UINT32* punDenominator)
	{
		RETURN_HR_IF(E_INVALIDARG, !pnNumerator || !punDenominator);
		assert(_attributes);
		return MFGetAttributeRatio(_attributes.get(), guidKey, pnNumerator, punDenominator);
	}

	HRESULT SetRatio(REFGUID guidKey, UINT32 unNumerator, UINT32 unDenominator)
	{
		assert(_attributes);
		return MFSetAttributeRatio(_attributes.get(), guidKey, unNumerator, unDenominator);
	}

	HRESULT GetSize(REFGUID guidKey, UINT32* punWidth, UINT32* punHeight)
	{
		RETURN_HR_IF(E_INVALIDARG, !punWidth || !punWidth);
		assert(_attributes);
		return MFGetAttributeSize(_attributes.get(), guidKey, punWidth, punHeight);
	}

	HRESULT SetSize(REFGUID guidKey, UINT32 unWidth, UINT32 unHeight)
	{
		assert(_attributes);
		return MFSetAttributeSize(_attributes.get(), guidKey, unWidth, unHeight);
	}
};

_Ret_range_(== , _expr)
inline bool assert_true(bool _expr)
{
    assert(_expr);
    return _expr;
}

namespace wil
{
    template<typename T>
    wil::unique_cotaskmem_array_ptr<T> make_unique_cotaskmem_array(size_t numOfElements)
    {
        wil::unique_cotaskmem_array_ptr<T> arr;
        auto cb = sizeof(wil::details::element_traits<T>::type) * numOfElements;
        void* ptr = ::CoTaskMemAlloc(cb);
        if (ptr != nullptr)
        {
            ZeroMemory(ptr, cb);
            arr.reset(reinterpret_cast<typename wil::details::element_traits<T>::type*>(ptr), numOfElements);
        }
        return arr;
    }
}

namespace winrt
{
    template<> inline bool is_guid_of<IMFMediaSourceEx>(guid const& id) noexcept
    {
        return is_guid_of<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>(id);
    }

    template<> inline bool is_guid_of<IMFMediaSource2>(guid const& id) noexcept
    {
        return is_guid_of<IMFMediaSource2, IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>(id);
    }

    template<> inline bool is_guid_of<IMFMediaStream2>(guid const& id) noexcept
    {
        return is_guid_of<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>(id);
    }

    template<> inline bool is_guid_of<IMFActivate>(guid const& id) noexcept
    {
        return is_guid_of<IMFActivate, IMFAttributes>(id);
    }
}

struct registry_traits
{
    using type = HKEY;

    static void close(type value) noexcept
    {
        WINRT_VERIFY_(ERROR_SUCCESS, RegCloseKey(value));
    }

    static constexpr type invalid() noexcept
    {
        return nullptr;
    }
};