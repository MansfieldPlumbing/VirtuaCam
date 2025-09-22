#pragma once

#include <d2d1_1.h>
#include <ks.h>
#include <cassert>
#include "winrt/base.h"
#include <wrl/client.h>
#include <wil/com.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mfidl.h>

#define VIRTUCAM_CONTROLS_SHM_NAME L"Global\\VirtuaCam_Controls_Block_v1"

enum class VirtuaCamCommand {
    None = 0,
};

struct VirtuaCamControls {
    long brightness;
    long contrast;
    long saturation;

    VirtuaCamControls() :
        brightness(0), contrast(100), saturation(100) {}
};

namespace VirtuaCam::Utils
{
    namespace Debug
    {
        std::wstring GuidToWString(const GUID& guid, bool resolveKnown = true);
    }

    namespace Win32
    {
        void CenterWindowRelativeToCursor(HWND windowHandle);
        HANDLE GetHandleFromObjectName(const WCHAR* objectName);
    }

    template<typename T>
    wil::unique_cotaskmem_array_ptr<T> make_unique_cotaskmem_array(size_t elementCount)
    {
        wil::unique_cotaskmem_array_ptr<T> arr;
        size_t allocationSize = sizeof(typename wil::details::element_traits<T>::type) * elementCount;
        void* memory = ::CoTaskMemAlloc(allocationSize);
        if (memory != nullptr)
        {
            ZeroMemory(memory, allocationSize);
            arr.reset(reinterpret_cast<typename wil::details::element_traits<T>::type*>(memory), elementCount);
        }
        return arr;
    }
}

struct BroadcastManifest {
    UINT64 frameValue;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LUID adapterLuid;
    WCHAR textureName[256];
    WCHAR fenceName[256];
};

struct ProducerConnection {
    bool isConnected = false;
    DWORD producerPid = 0;
    HANDLE hManifest = nullptr;
    BroadcastManifest* pManifestView = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
    Microsoft::WRL::ComPtr<ID3D11Fence> sharedFence;
    UINT64 lastSeenFrame = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> privateTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> privateSRV;
};