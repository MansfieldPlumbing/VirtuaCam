// =============================================================================
// SharedFrame.cpp  --  shared-frame IPC implementation
// =============================================================================

#include "SharedFrame.h"

#include <d3d12.h>
#include <dxgi1_2.h>
#include <sddl.h>

namespace Ipc {

const wchar_t* const kBrokerManifestName = L"Local\\DirectPort_Producer_Manifest_VirtuaCast_Broker";

std::wstring ProducerManifestName(DWORD pid)
{
    return L"DirectPort_Producer_Manifest_" + std::to_wstring(pid);
}

LUID AdapterLuidOf(ID3D11Device* device)
{
    LUID luid{};
    wil::com_ptr_nothrow<IDXGIDevice> dxgi;
    wil::com_ptr_nothrow<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
    {
        luid = desc.AdapterLuid;
    }
    return luid;
}

wil::unique_handle OpenNamedSharedHandle(const wchar_t* name)
{
    // The D3D12 device is only a name resolver here; it is created once and
    // kept for the life of the process.
    static ID3D12Device* const resolver = [] {
        ID3D12Device* device = nullptr;
        return SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ? device : nullptr;
    }();

    wil::unique_handle handle;
    if (resolver && name && *name)
        resolver->OpenSharedHandleByName(name, GENERIC_ALL, handle.put());
    return handle;
}

// Shared objects are created with a DACL granting Authenticated Users access
// so the camera frame server (LOCAL SERVICE) can open what the user's
// session creates.
namespace {
struct SharedSecurity {
    wil::unique_hlocal_security_descriptor descriptor;
    SECURITY_ATTRIBUTES attributes{ sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE };

    SharedSecurity()
    {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, nullptr)) {
            descriptor.reset(sd);
            attributes.lpSecurityDescriptor = sd;
        }
    }
};
}

// -----------------------------------------------------------------------------
// FramePublisher
// -----------------------------------------------------------------------------

HRESULT FramePublisher::OpenForProcess(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, UINT bindFlags)
{
    const auto pid = std::to_wstring(GetCurrentProcessId());
    return Open(device, width, height, format, ProducerManifestName(GetCurrentProcessId()),
                L"Local\\DirectPortTexture_" + pid, L"Local\\DirectPortFence_" + pid, bindFlags);
}

HRESULT FramePublisher::Open(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                             const std::wstring& manifestName, const std::wstring& textureName,
                             const std::wstring& fenceName, UINT bindFlags)
{
    Close();
    RETURN_HR_IF_NULL(E_POINTER, device);
    RETURN_IF_FAILED(device->QueryInterface(IID_PPV_ARGS(&m_device)));
    m_format = format;
    m_bindFlags = bindFlags;

    SharedSecurity security;
    m_mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &security.attributes, PAGE_READWRITE,
                                       0, sizeof(BroadcastManifest), manifestName.c_str()));
    RETURN_LAST_ERROR_IF_NULL(m_mapping.get());
    m_view = static_cast<BroadcastManifest*>(MapViewOfFile(m_mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest)));
    if (!m_view) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        m_mapping.reset();
        return hr;
    }
    ZeroMemory(m_view, sizeof(BroadcastManifest));

    const HRESULT hr = CreateSharedObjects(width, height, textureName, fenceName);
    if (FAILED(hr))
        Close();
    return hr;
}

HRESULT FramePublisher::Recreate(UINT width, UINT height, const std::wstring& textureName, const std::wstring& fenceName)
{
    RETURN_HR_IF(E_UNEXPECTED, !m_view);
    return CreateSharedObjects(width, height, textureName, fenceName);
}

HRESULT FramePublisher::CreateSharedObjects(UINT width, UINT height, const std::wstring& textureName, const std::wstring& fenceName)
{
    RETURN_HR_IF(E_INVALIDARG, !width || !height || textureName.size() >= 256 || fenceName.size() >= 256);

    wil::com_ptr_nothrow<ID3D11Texture2D> texture;
    wil::com_ptr_nothrow<ID3D11Fence> fence;
    wil::unique_handle textureHandle, fenceHandle;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = m_format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | m_bindFlags;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    RETURN_IF_FAILED(m_device->CreateTexture2D(&desc, nullptr, &texture));
    RETURN_IF_FAILED(m_device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));

    SharedSecurity security;
    wil::com_ptr_nothrow<IDXGIResource1> resource;
    RETURN_IF_FAILED(texture->QueryInterface(IID_PPV_ARGS(&resource)));
    RETURN_IF_FAILED(resource->CreateSharedHandle(&security.attributes, GENERIC_ALL, textureName.c_str(), textureHandle.put()));
    RETURN_IF_FAILED(fence->CreateSharedHandle(&security.attributes, GENERIC_ALL, fenceName.c_str(), fenceHandle.put()));

    m_texture = std::move(texture);
    m_fence = std::move(fence);
    m_textureHandle = std::move(textureHandle);
    m_fenceHandle = std::move(fenceHandle);
    m_width = width;
    m_height = height;
    m_frameValue = 0;

    // Names first, frame counter last: a consumer that reads a half-updated
    // manifest fails to open the objects and simply retries later.
    wcscpy_s(m_view->textureName, textureName.c_str());
    wcscpy_s(m_view->fenceName, fenceName.c_str());
    m_view->width = width;
    m_view->height = height;
    m_view->format = m_format;
    m_view->adapterLuid = AdapterLuidOf(m_device.get());
    m_view->command = ManifestCommand::None;
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&m_view->frameValue), 0);
    return S_OK;
}

void FramePublisher::Close()
{
    if (m_view) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    m_mapping.reset();
    m_textureHandle.reset();
    m_fenceHandle.reset();
    m_texture.reset();
    m_fence.reset();
    m_device.reset();
    m_width = m_height = 0;
    m_frameValue = 0;
}

UINT64 FramePublisher::Publish(ID3D11DeviceContext* context)
{
    if (!m_view || !context)
        return 0;
    wil::com_ptr_nothrow<ID3D11DeviceContext4> context4;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context4))))
        return 0;
    const UINT64 value = ++m_frameValue;
    context4->Signal(m_fence.get(), value);
    context->Flush();
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&m_view->frameValue), static_cast<LONG64>(value));
    return value;
}

// -----------------------------------------------------------------------------
// FrameSubscription
// -----------------------------------------------------------------------------

HRESULT FrameSubscription::Open(ID3D11Device* device, const std::wstring& manifestName, bool mipmapped)
{
    Close();
    RETURN_HR_IF_NULL(E_POINTER, device);

    m_mapping.reset(OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str()));
    if (!m_mapping)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    m_view = static_cast<const BroadcastManifest*>(MapViewOfFile(m_mapping.get(), FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest)));
    if (!m_view) {
        m_mapping.reset();
        return HRESULT_FROM_WIN32(GetLastError());
    }
    m_manifestName = manifestName;

    auto fail = [this](HRESULT hr) { Close(); return hr; };

    // Snapshot the names; IsStale() compares against these.
    WCHAR textureName[256], fenceName[256];
    memcpy(textureName, m_view->textureName, sizeof(textureName));
    memcpy(fenceName, m_view->fenceName, sizeof(fenceName));
    textureName[255] = fenceName[255] = L'\0';
    m_textureName = textureName;
    m_fenceName = fenceName;
    if (m_textureName.empty() || m_fenceName.empty())
        return fail(HRESULT_FROM_WIN32(ERROR_NOT_READY));

    wil::com_ptr_nothrow<ID3D11Device1> device1;
    wil::com_ptr_nothrow<ID3D11Device5> device5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) || FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))))
        return fail(E_NOINTERFACE);

    HRESULT hr = device1->OpenSharedResourceByName(m_textureName.c_str(),
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&m_shared));
    if (FAILED(hr))
        return fail(hr);

    wil::unique_handle fenceHandle = OpenNamedSharedHandle(m_fenceName.c_str());
    if (!fenceHandle)
        return fail(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    hr = device5->OpenSharedFence(fenceHandle.get(), IID_PPV_ARGS(&m_fence));
    if (FAILED(hr))
        return fail(hr);

    D3D11_TEXTURE2D_DESC desc{};
    m_shared->GetDesc(&desc);
    m_width = desc.Width;
    m_height = desc.Height;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.ArraySize = 1;
    if (mipmapped) {
        desc.MipLevels = 0;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    } else {
        desc.MipLevels = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = 0;
    }
    hr = device->CreateTexture2D(&desc, nullptr, &m_private);
    if (FAILED(hr))
        return fail(hr);
    hr = device->CreateShaderResourceView(m_private.get(), nullptr, &m_privateView);
    if (FAILED(hr))
        return fail(hr);

    m_frameEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    m_lastCopied = 0;
    ArmFrameEvent();
    return S_OK;
}

void FrameSubscription::Close()
{
    m_privateView.reset();
    m_private.reset();
    m_fence.reset();
    m_shared.reset();
    m_frameEvent.reset();
    if (m_view) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    m_mapping.reset();
    m_textureName.clear();
    m_fenceName.clear();
    m_width = m_height = 0;
    m_lastCopied = 0;
}

bool FrameSubscription::IsStale() const
{
    if (!m_view)
        return false;
    // Bounded compare: the producer may be rewriting the name right now.
    return wcsncmp(m_view->textureName, m_textureName.c_str(), 256) != 0 ||
           wcsncmp(m_view->fenceName, m_fenceName.c_str(), 256) != 0;
}

void FrameSubscription::ArmFrameEvent()
{
    if (m_fence && m_frameEvent)
        m_fence->SetEventOnCompletion(m_lastCopied + 1, m_frameEvent.get());
}

bool FrameSubscription::Acquire(ID3D11DeviceContext4* context, bool generateMips)
{
    if (!m_view || !context)
        return false;
    const UINT64 latest = LoadFrameValue(m_view);
    if (latest <= m_lastCopied) {
        // A producer that restarted its counter (Recreate) begins again at 1.
        if (latest != 0 && latest < m_lastCopied)
            m_lastCopied = 0;
        else
            return false;
    }

    context->Wait(m_fence.get(), latest);
    context->CopySubresourceRegion(m_private.get(), 0, 0, 0, 0, m_shared.get(), 0, nullptr);
    if (generateMips)
        context->GenerateMips(m_privateView.get());
    m_lastCopied = latest;
    ArmFrameEvent();
    return true;
}

} // namespace Ipc
