#pragma once

struct MFSource;

struct MFActivate : winrt::implements<MFActivate, CBaseAttributes<IMFActivate>>
{
public:
	STDMETHOD(ActivateObject(REFIID riid, void** ppv));
	STDMETHOD(ShutdownObject)();
	STDMETHOD(DetachObject)();

public:
	MFActivate()
	{
		SetBaseAttributesTraceName(L"ActivatorAtts");
	}

	HRESULT Initialize();

private:
#if _DEBUG
	int32_t query_interface_tearoff(winrt::guid const& id, void** object) const noexcept override
	{
		if (id == winrt::guid_of<IMFAttributes>())
		{
			this->m_inner->AddRef();
			*object = (IMFAttributes*)this;
			return S_OK;
		}

		RETURN_HR_MSG(E_NOINTERFACE, "Activator QueryInterface failed on IID %s", GUID_ToStringW(id).c_str());
	}
#endif

private:
	winrt::com_ptr<MFSource> _source;
};