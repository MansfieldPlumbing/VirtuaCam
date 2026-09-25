// =============================================================================
// CameraModule.cpp  --  COM server entry points for VirtuaCamSource.dll
// =============================================================================
// The DLL exposes one class, CLSID_VirtuaCamSource, whose instances are
// CameraActivate objects.  VirtuaCam.exe announces the camera with
// MFCreateVirtualCamera(CLSID); the Camera Frame Server then loads this DLL
// in its own process and activates the source.
// =============================================================================

#include "CameraSource.h"
#include "CameraIds.h"

#include <wrl/module.h>
#include <string>

using namespace Microsoft::WRL;

namespace {

HMODULE g_module = nullptr;

class CameraClassFactory final : public ClassFactory<>
{
public:
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override
    {
        RETURN_HR_IF_NULL(E_POINTER, object);
        *object = nullptr;
        RETURN_HR_IF(CLASS_E_NOAGGREGATION, outer != nullptr);
        ComPtr<CameraActivate> activate;
        RETURN_IF_FAILED(MakeAndInitialize<CameraActivate>(&activate));
        return activate.CopyTo(riid, object);
    }
};

std::wstring ClassKeyPath()
{
    return std::wstring(L"Software\\Classes\\CLSID\\") + kVirtuaCamSourceClsidString;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        Module<InProc>::GetModule();   // WRL object counting for DllCanUnloadNow
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object)
{
    RETURN_HR_IF_NULL(E_POINTER, object);
    *object = nullptr;
    RETURN_HR_IF(CLASS_E_CLASSNOTAVAILABLE, clsid != CLSID_VirtuaCamSource);
    auto factory = Make<CameraClassFactory>();
    RETURN_IF_NULL_ALLOC(factory.Get());
    return factory.CopyTo(riid, object);
}

STDAPI DllCanUnloadNow()
{
    return Module<InProc>::GetModule().GetObjectCount() == 0 ? S_OK : S_FALSE;
}

// Per-machine registration: the frame server runs as LOCAL SERVICE and only
// sees HKLM classes, so this needs an elevated regsvr32 (the installer does it).
STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH]{};
    RETURN_LAST_ERROR_IF(GetModuleFileNameW(g_module, path, ARRAYSIZE(path)) == 0);

    const std::wstring server = ClassKeyPath() + L"\\InprocServer32";
    const DWORD pathBytes = (DWORD)((wcslen(path) + 1) * sizeof(wchar_t));
    RETURN_IF_WIN32_ERROR(RegSetKeyValueW(HKEY_LOCAL_MACHINE, ClassKeyPath().c_str(), nullptr, REG_SZ,
        kVirtuaCamFriendlyName, (DWORD)sizeof(kVirtuaCamFriendlyName)));
    RETURN_IF_WIN32_ERROR(RegSetKeyValueW(HKEY_LOCAL_MACHINE, server.c_str(), nullptr, REG_SZ, path, pathBytes));
    static const wchar_t threading[] = L"Both";
    RETURN_IF_WIN32_ERROR(RegSetKeyValueW(HKEY_LOCAL_MACHINE, server.c_str(), L"ThreadingModel", REG_SZ,
        threading, (DWORD)sizeof(threading)));
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClassKeyPath().c_str());
    return (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(status);
}
