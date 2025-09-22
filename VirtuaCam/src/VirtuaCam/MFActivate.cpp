#include "pch.h"
#include "Guids.h"
#include "Utilities.h"
#include "MFActivate.h"
#include "Formats.h"
#include <shlwapi.h>
#include <wil/com.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "mf.lib")

HRESULT MFActivate::Initialize()
{
    m_source = winrt::make_self<MFSource>();
    RETURN_IF_FAILED(SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1));
    RETURN_IF_FAILED(SetGUID(MFT_TRANSFORM_CLSID_Attribute, CLSID_VirtuaCam));
    RETURN_IF_FAILED(m_source->Initialize(static_cast<IMFActivate*>(this)));
    return S_OK;
}

STDMETHODIMP MFActivate::ActivateObject(REFIID riid, void** ppv)
{
    RETURN_HR_IF_NULL(E_POINTER, ppv);
    *ppv = nullptr;
    return m_source->QueryInterface(riid, ppv);
}

STDMETHODIMP MFActivate::ShutdownObject()
{
    return S_OK;
}

STDMETHODIMP MFActivate::DetachObject()
{
    m_source = nullptr;
    return S_OK;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

struct ClassFactory : winrt::implements<ClassFactory, IClassFactory>
{
    STDMETHODIMP CreateInstance(IUnknown* outer, GUID const& riid, void** result) noexcept final
    {
        RETURN_HR_IF_NULL(E_POINTER, result);
        *result = nullptr;
        if (outer)
            RETURN_HR(CLASS_E_NOAGGREGATION);

        auto vcam = winrt::make_self<MFActivate>();
        RETURN_IF_FAILED(vcam->Initialize());
        return vcam.as(riid, result);
    }

    STDMETHODIMP LockServer(BOOL) noexcept final
    {
        return S_OK;
    }
};

__control_entrypoint(DllExport)
STDAPI DllCanUnloadNow()
{
    if (winrt::get_module_lock())
    {
        return S_FALSE;
    }
    winrt::clear_factory_cache();
    return S_OK;
}

_Check_return_
STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID FAR* ppv)
{
    RETURN_HR_IF_NULL(E_POINTER, ppv);
    *ppv = nullptr;

    if (IsEqualGUID(rclsid, CLSID_VirtuaCam))
        return winrt::make_self<ClassFactory>().as(riid, ppv);

    RETURN_HR(E_NOINTERFACE);
}

STDAPI DllRegisterServer()
{
    HMODULE hModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)DllRegisterServer, &hModule))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    auto modulePath = wil::GetModuleFileNameW(hModule);
    auto clsidString = VirtuaCam::Utils::Debug::GuidToWString(CLSID_VirtuaCam, false);
    std::wstring keyPath = L"Software\\Classes\\CLSID\\" + clsidString + L"\\InprocServer32";

    HKEY hKey = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    RETURN_IF_WIN32_ERROR(status);
    wil::unique_hkey key(hKey);

    const std::wstring threadingModel = L"Both";
    status = RegSetValueExW(key.get(), nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(modulePath.get()), static_cast<DWORD>((wcslen(modulePath.get()) + 1) * sizeof(WCHAR)));
    RETURN_IF_WIN32_ERROR(status);

    status = RegSetValueExW(key.get(), L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(threadingModel.c_str()), static_cast<DWORD>((threadingModel.length() + 1) * sizeof(WCHAR)));
    RETURN_IF_WIN32_ERROR(status);

    return S_OK;
}

STDAPI DllUnregisterServer()
{
    auto clsidString = VirtuaCam::Utils::Debug::GuidToWString(CLSID_VirtuaCam, false);
    std::wstring keyPath = std::wstring(L"Software\\Classes\\CLSID\\") + clsidString;
    LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath.c_str());

    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
    {
        return HRESULT_FROM_WIN32(status);
    }

    return S_OK;
}