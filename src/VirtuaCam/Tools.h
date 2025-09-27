#pragma once

#include <d2d1_1.h>
#include <ks.h>
#include <cassert>
#include "winrt/base.h"

std::string to_string(const std::wstring& ws);
std::wstring to_wstring(const std::string& s);
const std::wstring GUID_ToStringW(const GUID& guid, bool resolve = true);
const std::string GUID_ToStringA(const GUID& guid, bool resolve = true);
const std::wstring PROPVARIANT_ToString(const PROPVARIANT& pv);
void CenterWindow(HWND hwnd, bool useCursorPos);
D2D_COLOR_F HSL2RGB(const float h, const float s, const float l);
const std::wstring GetProcessName(DWORD pid);
const LSTATUS RegWriteKey(HKEY key, PCWSTR path, HKEY* outKey);
const LSTATUS RegWriteValue(HKEY key, PCWSTR name, const std::wstring& value);
const LSTATUS RegWriteValue(HKEY key, PCWSTR name, DWORD value);
HRESULT RGB32ToNV12(BYTE* input, ULONG inputSize, LONG inputStride, UINT width, UINT height, BYTE* output, ULONG ouputSize, LONG outputStride);
HANDLE GetHandleFromName(const WCHAR* name);

enum class VCamCommand;

struct BroadcastManifest {
    UINT64 frameValue;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LUID adapterLuid;
    WCHAR textureName[256];
    WCHAR fenceName[256];
    volatile VCamCommand command;
};

void TraceMFAttributes(IUnknown* unknown, PCWSTR prefix);
std::wstring PKSIDENTIFIER_ToString(PKSIDENTIFIER id, ULONG length);

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