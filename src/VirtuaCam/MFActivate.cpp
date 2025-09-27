#include "pch.h"
#include "Undocumented.h"
#include "Tools.h"
#include "Enumerator.h"
#include "FrameGenerator.h"
#include "MFStream.h"
#include "MFSource.h"
#include "MFActivate.h"

HRESULT MFActivate::Initialize()
{
	_source = winrt::make_self<MFSource>();
	RETURN_IF_FAILED(SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1));
	RETURN_IF_FAILED(SetGUID(MFT_TRANSFORM_CLSID_Attribute, CLSID_VCam));
	RETURN_IF_FAILED(_source->Initialize(this));
	return S_OK;
}

STDMETHODIMP MFActivate::ActivateObject(REFIID riid, void** ppv)
{
	RETURN_HR_IF_NULL(E_POINTER, ppv);
	*ppv = nullptr;

	UINT32 pid = 0;
	if (SUCCEEDED(GetUINT32(MF_FRAMESERVER_CLIENTCONTEXT_CLIENTPID, &pid)) && pid)
	{
		auto name = GetProcessName(pid);
	}
	RETURN_IF_FAILED_MSG(_source->QueryInterface(riid, ppv), "Activator::ActivateObject failed on IID %s", GUID_ToStringW(riid).c_str());
	return S_OK;
}

STDMETHODIMP MFActivate::ShutdownObject()
{
	return S_OK;
}

STDMETHODIMP MFActivate::DetachObject()
{
	_source = nullptr;
	return S_OK;
}