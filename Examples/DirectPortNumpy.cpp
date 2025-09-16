#include "DirectPort.h"
#include <vector>
#include <string>
#include <stdexcept>
#include <memory>
#include <map>
#include <algorithm>
#include <d3dcompiler.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <cmath>
#include <stdexcept>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "Synchronization.lib")

using namespace Microsoft::WRL;
using namespace DirectPort;

namespace {
    const char* g_blitShaderHLSL = R"(
        Texture2D    g_texture : register(t0);
        SamplerState g_sampler : register(s0);
        struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
        PSInput VSMain(uint id : SV_VertexID) {
            PSInput result;
            float2 uv = float2((id << 1) & 2, id & 2);
            result.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
            result.uv = uv;
            return result;
        }
        float4 PSMain(PSInput input) : SV_TARGET { return g_texture.Sample(g_sampler, input.uv); }
    )";
    
    std::wstring string_to_wstring(const std::string& str) {
        if (str.empty()) return std::wstring();
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
        std::wstring wstr(size, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), wstr.data(), size);
        return wstr;
    }

    void validate_stream_name(const std::string& name) {
        if (name.empty() || name.length() > 64) {
            throw std::invalid_argument("Stream name must be 1-64 characters.");
        }
        for (char c : name) {
            if (!isalnum(c) && c != '_' && c != '-') {
                throw std::invalid_argument("Stream name can only contain alphanumerics, underscores, and hyphens.");
            }
        }
    }

    HANDLE get_handle_from_name(const WCHAR* name) {
        ComPtr<ID3D12Device> d3d12Device;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device)))) {
            return NULL; 
        }
        HANDLE handle = nullptr;
        d3d12Device->OpenSharedHandleByName(name, GENERIC_ALL, &handle);
        return handle;
    }
    
    bool get_manifest_from_pid(DWORD pid, BroadcastManifest& manifest) {
        const std::vector<std::wstring> prefixes = { L"D3D12_Producer_Manifest_", L"DirectPort_Producer_Manifest_" };
        for (const auto& prefix : prefixes) {
            std::wstring manifestName = prefix + std::to_wstring(pid);
            HANDLE hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
            if (hManifest) {
                BroadcastManifest* pView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
                if (pView) {
                    memcpy(&manifest, pView, sizeof(BroadcastManifest));
                    UnmapViewOfFile(pView);
                    CloseHandle(hManifest);
                    return true;
                }
                CloseHandle(hManifest);
            }
        }
        return false;
    }
}

struct Texture::Impl {
    ComPtr<ID3D11Texture2D> d3d11Texture;
    ComPtr<ID3D11ShaderResourceView> d3d11SRV;
    ComPtr<ID3D11RenderTargetView> d3d11RTV;
    ComPtr<ID3D12Resource> d3d12Resource;
    
    UINT32 width = 0;
    UINT32 height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool is_d3d11 = false;
    bool is_d3d12 = false;
};
Texture::Texture() : pImpl(std::make_unique<Impl>()) {}
Texture::~Texture() = default;
uint32_t Texture::get_width() const { return pImpl->width; }
uint32_t Texture::get_height() const { return pImpl->height; }
DXGI_FORMAT Texture::get_format() const { return pImpl->format; }
uintptr_t Texture::get_d3d11_texture_ptr() { return reinterpret_cast<uintptr_t>(pImpl->d3d11Texture.Get()); }
uintptr_t Texture::get_d3d11_srv_ptr() { return reinterpret_cast<uintptr_t>(pImpl->d3d11SRV.Get()); }
uintptr_t Texture::get_d3d11_rtv_ptr() { return reinterpret_cast<uintptr_t>(pImpl->d3d11RTV.Get()); }
uintptr_t Texture::get_d3d12_resource_ptr() { return reinterpret_cast<uintptr_t>(pImpl->d3d12Resource.Get()); }

struct Consumer::Impl {
    DWORD pid = 0;
    HANDLE hProcess = nullptr;
    UINT64 lastSeenFrame = 0;
    std::shared_ptr<Texture> sharedTexture;
    std::shared_ptr<Texture> privateTexture;
    ComPtr<ID3D11Fence> d3d11Fence;
    ComPtr<ID3D12Fence> d3d12Fence;
    void* pDeviceContext = nullptr;
    bool is_d3d11_producer = false;
};
Consumer::Consumer() : pImpl(std::make_unique<Impl>()) {}
Consumer::~Consumer() {
    if (pImpl->hProcess) CloseHandle(pImpl->hProcess);
}
bool Consumer::is_alive() const {
    if (!pImpl || !pImpl->hProcess) return false;
    return WaitForSingleObject(pImpl->hProcess, 0) == WAIT_TIMEOUT;
}
std::shared_ptr<Texture> Consumer::get_texture() { return pImpl->privateTexture; }
std::shared_ptr<Texture> Consumer::get_shared_texture() { return pImpl->sharedTexture; }
unsigned long Consumer::get_pid() const { return pImpl->pid; }
bool Consumer::wait_for_frame() {
    if (!pImpl || !is_alive()) return false;
    BroadcastManifest currentManifest;
    if (!get_manifest_from_pid(pImpl->pid, currentManifest)) return false;
    UINT64 latestFrame = currentManifest.frameValue;

    if (latestFrame > pImpl->lastSeenFrame) {
        if (pImpl->is_d3d11_producer && pImpl->d3d11Fence) {
            auto* ctx = reinterpret_cast<ID3D11DeviceContext4*>(pImpl->pDeviceContext);
            ctx->Wait(pImpl->d3d11Fence.Get(), latestFrame);
            pImpl->lastSeenFrame = latestFrame;
            return true;
        } else if (!pImpl->is_d3d11_producer && pImpl->d3d12Fence) {
            UINT64 expectedFrame = pImpl->lastSeenFrame;
            if (WaitOnAddress(&currentManifest.frameValue, &expectedFrame, sizeof(UINT64), 16) == TRUE) {
                if (!get_manifest_from_pid(pImpl->pid, currentManifest)) return false;
                latestFrame = currentManifest.frameValue;

                if (latestFrame > pImpl->lastSeenFrame) {
                    pImpl->lastSeenFrame = latestFrame;
                    return true;
                }
            } else {
                if (!get_manifest_from_pid(pImpl->pid, currentManifest)) return false;
                latestFrame = currentManifest.frameValue;
                if (latestFrame > pImpl->lastSeenFrame) {
                    pImpl->lastSeenFrame = latestFrame;
                    return true;
                }
            }
        }
    }
    return false;
}

struct Producer::Impl {
    ComPtr<ID3D11Fence> d3d11Fence;
    ComPtr<ID3D12Fence> d3d12Fence;
    UINT64 frameValue = 0;
    HANDLE hManifest = nullptr;
    BroadcastManifest* pManifestView = nullptr;
    HANDLE hTextureHandle = nullptr;
    HANDLE hFenceHandle = nullptr;
    void* pDeviceContext = nullptr;
    std::shared_ptr<Texture> sourceTexture;
    bool is_d3d11_producer = false;
};
Producer::Producer() : pImpl(std::make_unique<Impl>()) {}
Producer::~Producer() {
    if (pImpl->pManifestView) UnmapViewOfFile(pImpl->pManifestView);
    if (pImpl->hManifest) CloseHandle(pImpl->hManifest);
    if (pImpl->hTextureHandle) CloseHandle(pImpl->hTextureHandle);
    if (pImpl->hFenceHandle) CloseHandle(pImpl->hFenceHandle);
}
void Producer::signal_frame() {
    pImpl->frameValue++;
    if (pImpl->is_d3d11_producer && pImpl->d3d11Fence) {
        reinterpret_cast<ID3D11DeviceContext4*>(pImpl->pDeviceContext)->Signal(pImpl->d3d11Fence.Get(), pImpl->frameValue);
    } else if (!pImpl->is_d3d11_producer && pImpl->d3d12Fence) {
        reinterpret_cast<ID3D12CommandQueue*>(pImpl->pDeviceContext)->Signal(pImpl->d3d12Fence.Get(), pImpl->frameValue);
    }

    if (pImpl->pManifestView) {
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&pImpl->pManifestView->frameValue), pImpl->frameValue);
        if (!pImpl->is_d3d11_producer) {
            WakeByAddressAll(&pImpl->pManifestView->frameValue);
        }
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
struct Window::Impl {
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain> d3d11swapChain;
    ComPtr<ID3D11RenderTargetView> d3d11rtv;
    
    ComPtr<IDXGISwapChain3> d3d12swapChain;
    ComPtr<ID3D12Resource> d3d12RenderTargets[2];
    ComPtr<ID3D12DescriptorHeap> d3d12RtvHeap;
    UINT d3d12RtvDescriptorSize = 0;
    UINT d3d12FrameIndex = 0;

    bool is_d3d11 = false;
    bool is_d3d12 = false;
};
Window::Window() : pImpl(std::make_unique<Impl>()) {}
Window::~Window() { if (pImpl->hwnd) { DestroyWindow(pImpl->hwnd); } }
bool Window::process_events() {
    if (!pImpl || !pImpl->hwnd) return false;
    MSG msg = {};
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { pImpl->hwnd = nullptr; return false; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return IsWindow(pImpl->hwnd);
}
void Window::present(bool vsync) { 
    if (pImpl->is_d3d11) {
        if (pImpl->d3d11swapChain) pImpl->d3d11swapChain->Present(vsync ? 1 : 0, 0); 
    } else {
        if (pImpl->d3d12swapChain) {
            pImpl->d3d12swapChain->Present(vsync ? 1 : 0, 0);
            pImpl->d3d12FrameIndex = pImpl->d3d12swapChain->GetCurrentBackBufferIndex();
        }
    }
}
void Window::set_title(const std::string& title) { if (pImpl && pImpl->hwnd) SetWindowTextW(pImpl->hwnd, string_to_wstring(title).c_str()); }
uint32_t Window::get_width() const {
    if (!pImpl->hwnd) return 0;
    RECT rect;
    GetClientRect(pImpl->hwnd, &rect);
    return static_cast<uint32_t>(rect.right - rect.left);
}

uint32_t Window::get_height() const {
    if (!pImpl->hwnd) return 0;
    RECT rect;
    GetClientRect(pImpl->hwnd, &rect);
    return static_cast<uint32_t>(rect.bottom - rect.top);
}

struct DeviceD3D11::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    
    ComPtr<ID3D11VertexShader> blitVS;
    ComPtr<ID3D11PixelShader> blitPS;
    ComPtr<ID3D11SamplerState> blitSampler;

    std::map<std::vector<uint8_t>, ComPtr<ID3D11PixelShader>> shaderCache;
    LUID adapterLuid = {};
};

DeviceD3D11::DeviceD3D11() : pImpl(std::make_unique<Impl>()) {}
DeviceD3D11::~DeviceD3D11() = default;

std::shared_ptr<DeviceD3D11> DeviceD3D11::create() {
    auto self = std::shared_ptr<DeviceD3D11>(new DeviceD3D11());
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    #ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
    #endif
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &self->pImpl->device, nullptr, &self->pImpl->context);
    if (FAILED(hr)) throw std::runtime_error("Failed to create D3D11 device. HRESULT: " + std::to_string(hr));

    self->pImpl->device.As(&self->pImpl->device1);
    self->pImpl->device.As(&self->pImpl->device5);
    self->pImpl->context.As(&self->pImpl->context4);
    if (!self->pImpl->device5 || !self->pImpl->context4) throw std::runtime_error("D3D11.5 interfaces (required for fences) not supported.");

    ComPtr<IDXGIDevice> dxgiDevice;
    self->pImpl->device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc;
    adapter->GetDesc(&desc);
    self->pImpl->adapterLuid = desc.AdapterLuid;
    
    ComPtr<ID3DBlob> vsBlob, psBlob;
    HRESULT compile_hr = D3DCompile(g_blitShaderHLSL, strlen(g_blitShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
    if (FAILED(compile_hr)) throw std::runtime_error("Failed to compile internal blit VS. HRESULT: " + std::to_string(compile_hr));
    compile_hr = D3DCompile(g_blitShaderHLSL, strlen(g_blitShaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, nullptr);
    if (FAILED(compile_hr)) throw std::runtime_error("Failed to compile internal blit PS. HRESULT: " + std::to_string(compile_hr));

    self->pImpl->device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &self->pImpl->blitVS);
    self->pImpl->device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &self->pImpl->blitPS);
    
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    self->pImpl->device->CreateSamplerState(&sampDesc, &self->pImpl->blitSampler);
    
    return self;
}

std::shared_ptr<Texture> DeviceD3D11::create_texture(uint32_t width, uint32_t height, DXGI_FORMAT format, const void* data, size_t data_size) {
    auto tex = std::shared_ptr<Texture>(new Texture());
    tex->pImpl->is_d3d11 = true;
    tex->pImpl->width = width;
    tex->pImpl->height = height;
    tex->pImpl->format = format;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    
    D3D11_SUBRESOURCE_DATA* initial_data_ptr = nullptr;
    D3D11_SUBRESOURCE_DATA initial_data = {};
    if (data && data_size > 0) {
        UINT pitch = 0;
        if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            pitch = width * 4;
        } else if (format == DXGI_FORMAT_R16_FLOAT || format == DXGI_FORMAT_R8G8_UNORM) {
            pitch = width * 2;
        } else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            pitch = width * 8;
        } else if (format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
            pitch = width * 16;
        } else if (format == DXGI_FORMAT_R8_UNORM) {
            pitch = width * 1;
        } else {
            throw std::runtime_error("Unsupported DXGI_FORMAT for initial data with D3D11::create_texture. Cannot calculate pitch reliably.");
        }

        if (pitch == 0) {
            throw std::runtime_error("Calculated pitch is zero for D3D11::create_texture. Invalid format or dimensions.");
        }

        if (data_size < (size_t)pitch * height) {
             throw std::runtime_error("Initial data_size is too small for the specified dimensions and format for D3D11::create_texture.");
        }
        
        initial_data.pSysMem = data;
        initial_data.SysMemPitch = pitch;
        initial_data_ptr = &initial_data;
    }
    
    HRESULT hr = pImpl->device->CreateTexture2D(&desc, initial_data_ptr, &tex->pImpl->d3d11Texture);
    if (FAILED(hr)) { throw std::runtime_error("Failed to create D3D11 texture. HRESULT: " + std::to_string(hr)); }
    
    hr = pImpl->device->CreateShaderResourceView(tex->pImpl->d3d11Texture.Get(), nullptr, &tex->pImpl->d3d11SRV);
    if (FAILED(hr)) { throw std::runtime_error("Failed to create D3D11 SRV. HRESULT: " + std::to_string(hr)); }
    
    hr = pImpl->device->CreateRenderTargetView(tex->pImpl->d3d11Texture.Get(), nullptr, &tex->pImpl->d3d11RTV);
    if (FAILED(hr)) { throw std::runtime_error("Failed to create D3D11 RTV. HRESULT: " + std::to_string(hr)); }

    return tex;
}

std::shared_ptr<Producer> DeviceD3D11::create_producer(const std::string& stream_name, std::shared_ptr<Texture> texture) {
    if (!texture || !texture->pImpl->is_d3d11 || !texture->pImpl->d3d11Texture) {
        throw std::invalid_argument("Provided texture is not a valid D3D11 texture, or is null.");
    }
    validate_stream_name(stream_name);
    auto prod = std::shared_ptr<Producer>(new Producer());
    prod->pImpl->is_d3d11_producer = true;
    prod->pImpl->pDeviceContext = pImpl->context4.Get();
    prod->pImpl->sourceTexture = texture; 

    D3D11_TEXTURE2D_DESC sharedTexDesc;
    texture->pImpl->d3d11Texture->GetDesc(&sharedTexDesc);
    sharedTexDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    sharedTexDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; 

    ComPtr<ID3D11Texture2D> sharedTextureForHandle;
    HRESULT hr = pImpl->device->CreateTexture2D(&sharedTexDesc, nullptr, &sharedTextureForHandle);
    if (FAILED(hr)) { throw std::runtime_error("Failed to create D3D11 shared output texture. HRESULT: " + std::to_string(hr)); }

    hr = pImpl->device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&prod->pImpl->d3d11Fence));
    if (FAILED(hr)) { throw std::runtime_error("Failed to create D3D11 shared fence. HRESULT: " + std::to_string(hr)); }
    
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) {
        throw std::runtime_error("Failed to convert SDDL string to security descriptor. GetLastError: " + std::to_string(GetLastError()));
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    
    DWORD pid = GetCurrentProcessId();
    std::wstring w_stream_name = string_to_wstring(stream_name);
    std::wstring textureName = L"Global\\DirectPort_Texture_" + std::to_wstring(pid) + L"_" + w_stream_name;
    std::wstring fenceName = L"Global\\DirectPort_Fence_" + std::to_wstring(pid) + L"_" + w_stream_name;
    std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(pid);

    ComPtr<IDXGIResource1> dxgiResource;
    sharedTextureForHandle.As(&dxgiResource);
    hr = dxgiResource->CreateSharedHandle(&sa, GENERIC_ALL, textureName.c_str(), &prod->pImpl->hTextureHandle);
    if (FAILED(hr)) { LocalFree(sd); throw std::runtime_error("Failed to create shared handle for texture. HRESULT: " + std::to_string(hr)); }

    hr = prod->pImpl->d3d11Fence->CreateSharedHandle(&sa, GENERIC_ALL, fenceName.c_str(), &prod->pImpl->hFenceHandle);
    if (FAILED(hr)) { CloseHandle(prod->pImpl->hTextureHandle); LocalFree(sd); throw std::runtime_error("Failed to create shared handle for fence. HRESULT: " + std::to_string(hr)); }
    
    prod->pImpl->hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    LocalFree(sd);
    if (!prod->pImpl->hManifest) { CloseHandle(prod->pImpl->hTextureHandle); CloseHandle(prod->pImpl->hFenceHandle); throw std::runtime_error("Failed to create file mapping for manifest. GetLastError: " + std::to_string(GetLastError())); }
    
    prod->pImpl->pManifestView = (BroadcastManifest*)MapViewOfFile(prod->pImpl->hManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    if (!prod->pImpl->pManifestView) { CloseHandle(prod->pImpl->hTextureHandle); CloseHandle(prod->pImpl->hFenceHandle); CloseHandle(prod->pImpl->hManifest); throw std::runtime_error("Failed to map view of file for manifest. GetLastError: " + std::to_string(GetLastError())); }
    
    ZeroMemory(prod->pImpl->pManifestView, sizeof(BroadcastManifest));
    prod->pImpl->pManifestView->width = texture->get_width();
    prod->pImpl->pManifestView->height = texture->get_height();
    prod->pImpl->pManifestView->format = texture->get_format();
    prod->pImpl->pManifestView->adapterLuid = pImpl->adapterLuid;
    wcscpy_s(prod->pImpl->pManifestView->textureName, _countof(prod->pImpl->pManifestView->textureName), textureName.c_str());
    wcscpy_s(prod->pImpl->pManifestView->fenceName, _countof(prod->pImpl->pManifestView->fenceName), fenceName.c_str());

    texture->pImpl->d3d11Texture = sharedTextureForHandle; 
    hr = pImpl->device->CreateShaderResourceView(sharedTextureForHandle.Get(), nullptr, &texture->pImpl->d3d11SRV);
    if (FAILED(hr)) { throw std::runtime_error("Failed to recreate SRV for shared texture. HRESULT: " + std::to_string(hr)); }
    hr = pImpl->device->CreateRenderTargetView(sharedTextureForHandle.Get(), nullptr, &texture->pImpl->d3d11RTV);
    if (FAILED(hr)) { throw std::runtime_error("Failed to recreate RTV for shared texture. HRESULT: " + std::to_string(hr)); }

    return prod;
}

std::shared_ptr<Consumer> DeviceD3D11::connect_to_producer(unsigned long pid) {
    auto cons = std::shared_ptr<Consumer>(new Consumer());
    cons->pImpl->pid = pid;
    cons->pImpl->pDeviceContext = pImpl->context4.Get();

    BroadcastManifest manifest;
    if (!get_manifest_from_pid(pid, manifest)) {
        return nullptr;
    }

    if (std::wstring(manifest.textureName).find(L"D3D12_Texture_") != std::wstring::npos) {
        cons->pImpl->is_d3d11_producer = false;
    } else {
        cons->pImpl->is_d3d11_producer = true;
    }
    
    cons->pImpl->hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!cons->pImpl->hProcess) {
        return nullptr;
    }

    HANDLE hFence = get_handle_from_name(manifest.fenceName);
    if (!hFence) { CloseHandle(cons->pImpl->hProcess); return nullptr; }
    HRESULT hr;
    if (cons->pImpl->is_d3d11_producer) {
        hr = pImpl->device5->OpenSharedFence(hFence, IID_PPV_ARGS(&cons->pImpl->d3d11Fence));
    } else {
        ComPtr<ID3D12Device> tempD3D12Device;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&tempD3D12Device)))) {
            CloseHandle(hFence); CloseHandle(cons->pImpl->hProcess); return nullptr;
        }
        hr = tempD3D12Device->OpenSharedHandle(hFence, IID_PPV_ARGS(&cons->pImpl->d3d12Fence));
    }
    CloseHandle(hFence); 
    if (FAILED(hr)) { CloseHandle(cons->pImpl->hProcess); return nullptr; }

    cons->pImpl->sharedTexture = std::shared_ptr<Texture>(new Texture());
    HANDLE hTexture = get_handle_from_name(manifest.textureName);
    if (!hTexture) { CloseHandle(cons->pImpl->hProcess); return nullptr; }
    if (cons->pImpl->is_d3d11_producer) {
        cons->pImpl->sharedTexture->pImpl->is_d3d11 = true;
        hr = pImpl->device1->OpenSharedResource1(hTexture, IID_PPV_ARGS(&cons->pImpl->sharedTexture->pImpl->d3d11Texture));
        if (FAILED(hr)) { CloseHandle(hTexture); CloseHandle(cons->pImpl->hProcess); return nullptr; }
        hr = pImpl->device->CreateShaderResourceView(cons->pImpl->sharedTexture->pImpl->d3d11Texture.Get(), nullptr, &cons->pImpl->sharedTexture->pImpl->d3d11SRV);
        if (FAILED(hr)) { CloseHandle(hTexture); CloseHandle(cons->pImpl->hProcess); return nullptr; }
    } else {
        cons->pImpl->sharedTexture->pImpl->is_d3d12 = true;
        ComPtr<ID3D12Device> tempD3D12Device;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&tempD3D12Device)))) {
            CloseHandle(hTexture); CloseHandle(cons->pImpl->hProcess); return nullptr;
        }
        hr = tempD3D12Device->OpenSharedHandle(hTexture, IID_PPV_ARGS(&cons->pImpl->sharedTexture->pImpl->d3d12Resource));
        if (FAILED(hr)) { CloseHandle(hTexture); CloseHandle(cons->pImpl->hProcess); return nullptr; }
    }
    CloseHandle(hTexture);
    
    cons->pImpl->sharedTexture->pImpl->width = manifest.width;
    cons->pImpl->sharedTexture->pImpl->height = manifest.height;
    cons->pImpl->sharedTexture->pImpl->format = manifest.format;

    cons->pImpl->privateTexture = create_texture(manifest.width, manifest.height, manifest.format);
    
    return cons;
}

void DeviceD3D11::copy_texture(std::shared_ptr<Texture> source, std::shared_ptr<Texture> destination) {
    if (!source || !destination || !source->pImpl->is_d3d11 || !destination->pImpl->is_d3d11 ||
        !source->pImpl->d3d11Texture || !destination->pImpl->d3d11Texture) {
        throw std::invalid_argument("Invalid D3D11 source or destination texture for copy_texture. Check for null or incorrect API type.");
    }
    if (source->get_width() != destination->get_width() ||
        source->get_height() != destination->get_height() ||
        source->get_format() != destination->get_format()) {
        throw std::invalid_argument("Source and destination textures must have matching dimensions and format for D3D11::copy_texture.");
    }

    pImpl->context->CopyResource(destination->pImpl->d3d11Texture.Get(), source->pImpl->d3d11Texture.Get());
}

void DeviceD3D11::apply_shader(std::shared_ptr<Texture> output, const std::vector<uint8_t>& shader_bytes, const std::string& entry_point, const std::vector<std::shared_ptr<Texture>>& inputs, const std::vector<uint8_t>& constants) {
    if (!output || !output->pImpl->is_d3d11 || !output->pImpl->d3d11RTV) {
        throw std::invalid_argument("Invalid D3D11 output texture for apply_shader (must be D3D11 and have RTV).");
    }

    ComPtr<ID3D11PixelShader> ps;
    if (shader_bytes.empty() || (shader_bytes.size() == 1 && shader_bytes[0] == '\0')) {
        static ComPtr<ID3D11PixelShader> defaultBlackPS;
        if (!defaultBlackPS) {
            ComPtr<ID3DBlob> psBlob;
            HRESULT compile_hr = D3DCompile("float4 PSMain() : SV_TARGET { return float4(0.0,0.0,0.0,1.0); }", strlen("float4 PSMain() : SV_TARGET { return float4(0.0,0.0,0.0,1.0); }"), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, nullptr);
            if (FAILED(compile_hr)) throw std::runtime_error("Failed to compile internal black PS for apply_shader. HRESULT: " + std::to_string(compile_hr));
            pImpl->device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &defaultBlackPS);
        }
        ps = defaultBlackPS;
    } else {
        auto it = pImpl->shaderCache.find(shader_bytes);
        if (it != pImpl->shaderCache.end()) {
            ps = it->second;
        } else {
            ComPtr<ID3DBlob> psBlob, errorBlob;
            HRESULT hr = D3DCompile(shader_bytes.data(), shader_bytes.size(), "hlsl_shader", nullptr, nullptr, entry_point.c_str(), "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errorBlob);
            if (SUCCEEDED(hr)) {
                hr = pImpl->device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
                if (FAILED(hr)) { throw std::runtime_error("Failed to create pixel shader from HLSL blob. HRESULT: " + std::to_string(hr)); }
            } else {
                hr = pImpl->device->CreatePixelShader(shader_bytes.data(), shader_bytes.size(), nullptr, &ps);
                if (FAILED(hr)) {
                     if (errorBlob) throw std::runtime_error("HLSL compile failed for apply_shader: " + std::string((char*)errorBlob->GetBufferPointer()));
                     else throw std::runtime_error("Failed to create pixel shader from HLSL or CSO bytes. HRESULT: " + std::to_string(hr));
                }
            }
            pImpl->shaderCache[shader_bytes] = ps;
        }
    }

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)output->get_width(), (float)output->get_height(), 0.0f, 1.0f };
    pImpl->context->RSSetViewports(1, &vp);
    ID3D11RenderTargetView* rtv = output->pImpl->d3d11RTV.Get();
    pImpl->context->OMSetRenderTargets(1, &rtv, nullptr);
    pImpl->context->VSSetShader(pImpl->blitVS.Get(), nullptr, 0);
    pImpl->context->PSSetShader(ps.Get(), nullptr, 0);

    std::vector<ID3D11ShaderResourceView*> srvs;
    srvs.reserve(inputs.size());
    for(const auto& input : inputs) {
        if (!input || !input->pImpl->is_d3d11 || !input->pImpl->d3d11SRV) {
            throw std::invalid_argument("Invalid D3D11 input texture provided for apply_shader (must be D3D11 and have SRV).");
        }
        srvs.push_back(input->pImpl->d3d11SRV.Get());
    }
    if (!srvs.empty()) {
        pImpl->context->PSSetShaderResources(0, (UINT)srvs.size(), srvs.data());
        pImpl->context->PSSetSamplers(0, 1, pImpl->blitSampler.GetAddressOf());
    }

    if (!constants.empty()) {
        ComPtr<ID3D11Buffer> cb;
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = static_cast<UINT>((constants.size() + 15) & ~15); 
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = pImpl->device->CreateBuffer(&desc, nullptr, &cb);
        if (FAILED(hr)) { throw std::runtime_error("Failed to create constant buffer for apply_shader. HRESULT: " + std::to_string(hr)); }

        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = pImpl->context->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) { throw std::runtime_error("Failed to map constant buffer for apply_shader. HRESULT: " + std::to_string(hr)); }
        memcpy(mapped.pData, constants.data(), constants.size());
        pImpl->context->Unmap(cb.Get(), 0);
        pImpl->context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
    }
    
    pImpl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pImpl->context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = { nullptr };
    pImpl->context->PSSetShaderResources(0, (UINT)srvs.size(), nullSRV);
    ID3D11Buffer* nullCB[] = { nullptr };
    pImpl->context->PSSetConstantBuffers(0, 1, nullCB);
}

void DeviceD3D11::blit(std::shared_ptr<Texture> source, std::shared_ptr<Window> destination) {
    if (!source || !destination || !source->pImpl->is_d3d11 || !destination->pImpl->is_d3d11 ||
        !source->pImpl->d3d11SRV || !destination->pImpl->d3d11rtv) {
        throw std::invalid_argument("Invalid D3D11 source texture or window for blit. Check for null or incorrect API type.");
    }
    ID3D11RenderTargetView* rtv = destination->pImpl->d3d11rtv.Get();
    pImpl->context->OMSetRenderTargets(1, &rtv, nullptr);
    RECT clientRect;
    GetClientRect(destination->pImpl->hwnd, &clientRect);
    D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top), 0.0f, 1.0f};
    pImpl->context->RSSetViewports(1, &vp);
    pImpl->context->VSSetShader(pImpl->blitVS.Get(), nullptr, 0);
    pImpl->context->PSSetShader(pImpl->blitPS.Get(), nullptr, 0);
    pImpl->context->PSSetSamplers(0, 1, pImpl->blitSampler.GetAddressOf());
    ID3D11ShaderResourceView* srv = source->pImpl->d3d11SRV.Get();
    pImpl->context->PSSetShaderResources(0, 1, &srv);
    pImpl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pImpl->context->Draw(3, 0);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    pImpl->context->PSSetShaderResources(0, 1, nullSRV);
}

void DeviceD3D11::clear(std::shared_ptr<Window> window, float r, float g, float b, float a) {
    if (!window || !window->pImpl->is_d3d11 || !window->pImpl->d3d11rtv) {
        throw std::invalid_argument("Invalid D3D11 window for clear. Check for null or incorrect API type.");
    }
    const float clearColor[] = {r, g, b, a};
    pImpl->context->ClearRenderTargetView(window->pImpl->d3d11rtv.Get(), clearColor);
}

std::shared_ptr<Window> DeviceD3D11::create_window(uint32_t width, uint32_t height, const std::string& title) {
    auto win = std::shared_ptr<Window>(new Window());
    win->pImpl->is_d3d11 = true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"DirectPortD3D11WindowClass";
    if (!RegisterClassExW(&wc)) {
    }

    win->pImpl->hwnd = CreateWindowExW(0, wc.lpszClassName, string_to_wstring(title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!win->pImpl->hwnd) throw std::runtime_error("Failed to create D3D11 window. GetLastError: " + std::to_string(GetLastError()));
    
    ComPtr<IDXGIFactory1> dxgiFactory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgiFactory);
    if (FAILED(hr)) { DestroyWindow(win->pImpl->hwnd); throw std::runtime_error("Failed to create DXGI Factory 1. HRESULT: " + std::to_string(hr)); }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = win->pImpl->hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = dxgiFactory->CreateSwapChain(pImpl->device.Get(), &scd, &win->pImpl->d3d11swapChain);
    if (FAILED(hr)) { DestroyWindow(win->pImpl->hwnd); throw std::runtime_error("Failed to create D3D11 swap chain. HRESULT: " + std::to_string(hr)); }
    
    ComPtr<ID3D11Texture2D> backBuffer;
    hr = win->pImpl->d3d11swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) { DestroyWindow(win->pImpl->hwnd); throw std::runtime_error("Failed to get D3D11 back buffer. HRESULT: " + std::to_string(hr)); }
    
    hr = pImpl->device->CreateRenderTargetView(backBuffer.Get(), nullptr, &win->pImpl->d3d11rtv);
    if (FAILED(hr)) { DestroyWindow(win->pImpl->hwnd); throw std::runtime_error("Failed to create D3D11 RTV for window. HRESULT: " + std::to_string(hr)); }
    
    ShowWindow(win->pImpl->hwnd, SW_SHOW);
    UpdateWindow(win->pImpl->hwnd);
    return win;
}

void DeviceD3D11::resize_window(std::shared_ptr<Window> window) {
    if (!window || !window->pImpl->is_d3d11 || !window->pImpl->d3d11swapChain) {
        throw std::invalid_argument("Invalid D3D11 window for resize_window. Check for null or incorrect API type.");
    }
    
    pImpl->context->OMSetRenderTargets(0, nullptr, nullptr);
    window->pImpl->d3d11rtv.Reset();

    HRESULT hr = window->pImpl->d3d11swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to resize D3D11 swap chain. HRESULT: " + std::to_string(hr));
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = window->pImpl->d3d11swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to get back buffer after D3D11 resize. HRESULT: " + std::to_string(hr));
    }
    
    hr = pImpl->device->CreateRenderTargetView(backBuffer.Get(), nullptr, &window->pImpl->d3d11rtv);
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create new D3D11 RTV after resize. HRESULT: " + std::to_string(hr));
    }
}

void DeviceD3D11::blit_texture_to_region(std::shared_ptr<Texture> source, std::shared_ptr<Texture> destination,
                                        uint32_t dest_x, uint32_t dest_y, uint32_t dest_width, uint32_t dest_height) {
    if (!source || !destination || !source->pImpl->is_d3d11 || !destination->pImpl->is_d3d11 ||
        !source->pImpl->d3d11SRV || !destination->pImpl->d3d11RTV) {
        throw std::invalid_argument("Invalid D3D11 source or destination texture for blit_texture_to_region. Check for null or incorrect API type.");
    }
    if (dest_width == 0 || dest_height == 0) return;

    pImpl->context->OMSetRenderTargets(1, destination->pImpl->d3d11RTV.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp = {
        (float)dest_x,
        (float)dest_y,
        (float)dest_width,
        (float)dest_height,
        0.0f,
        1.0f
    };
    pImpl->context->RSSetViewports(1, &vp);

    pImpl->context->VSSetShader(pImpl->blitVS.Get(), nullptr, 0);
    pImpl->context->PSSetShader(pImpl->blitPS.Get(), nullptr, 0);
    pImpl->context->PSSetSamplers(0, 1, pImpl->blitSampler.GetAddressOf());
    
    ID3D11ShaderResourceView* srv = source->pImpl->d3d11SRV.Get();
    pImpl->context->PSSetShaderResources(0, 1, &srv);

    pImpl->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pImpl->context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    pImpl->context->PSSetShaderResources(0, 1, nullSRV);
}

struct DeviceD3D12::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent;
    UINT64 fenceValue = 1;
    UINT64 frameFenceValues[2] = {};
    LUID adapterLuid;

    ComPtr<ID3D12RootSignature> blitRootSignature;
    ComPtr<ID3D12PipelineState> blitPSO;
    ComPtr<ID3D12DescriptorHeap> blitSrvHeap;
    ComPtr<ID3D12DescriptorHeap> blitRtvHeap;

    std::map<std::vector<uint8_t>, ComPtr<ID3D12PipelineState>> psoCache;
    ComPtr<ID3D12RootSignature> shaderRootSignature;
};

void DeviceD3D12::WaitForGpu() {
    const UINT64 currentFenceValue = pImpl->fenceValue;
    pImpl->commandQueue->Signal(pImpl->fence.Get(), currentFenceValue);

    if (pImpl->fence->GetCompletedValue() < currentFenceValue) {
        pImpl->fence->SetEventOnCompletion(currentFenceValue, pImpl->fenceEvent);
        WaitForSingleObject(pImpl->fenceEvent, INFINITE);
    }
    pImpl->fenceValue++;
}

DeviceD3D12::DeviceD3D12() : pImpl(std::make_unique<Impl>()) {}
DeviceD3D12::~DeviceD3D12() { if(pImpl->fenceEvent) CloseHandle(pImpl->fenceEvent); }

std::shared_ptr<DeviceD3D12> DeviceD3D12::create() {
    auto self = std::shared_ptr<DeviceD3D12>(new DeviceD3D12());
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&self->pImpl->device));
    if (FAILED(hr)) throw std::runtime_error("Failed to create D3D12 device. HRESULT: " + std::to_string(hr));

    self->pImpl->adapterLuid = self->pImpl->device->GetAdapterLuid();

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = self->pImpl->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&self->pImpl->commandQueue));
    if (FAILED(hr)) throw std::runtime_error("Failed to create D3D12 command queue. HRESULT: " + std::to_string(hr));

    hr = self->pImpl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&self->pImpl->commandAllocator));
    if (FAILED(hr)) throw std::runtime_error("Failed to create D3D12 command allocator. HRESULT: " + std::to_string(hr));

    hr = self->pImpl->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, self->pImpl->commandAllocator.Get(), nullptr, IID_PPV_ARGS(&self->pImpl->commandList));
    if (FAILED(hr)) throw std::runtime_error("Failed to create D3D12 command list. HRESULT: " + std::to_string(hr));
    self->pImpl->commandList->Close();
    
    hr = self->pImpl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&self->pImpl->fence));
    if (FAILED(hr)) throw std::runtime_error("Failed to create D3D12 fence. HRESULT: " + std::to_string(hr));
    self->pImpl->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!self->pImpl->fenceEvent) throw std::runtime_error("Failed to create fence event. GetLastError: " + std::to_string(GetLastError()));

    D3D12_DESCRIPTOR_RANGE blitRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER blitRootParam = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, {1, &blitRange}, D3D12_SHADER_VISIBILITY_PIXEL };
    
    D3D12_STATIC_SAMPLER_DESC blitSampler = {};
    blitSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    blitSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    blitSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    blitSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    blitSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    blitSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    blitSampler.MinLOD = 0.0f;
    blitSampler.MaxLOD = D3D12_FLOAT32_MAX;
    blitSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    blitSampler.ShaderRegister = 0;
    blitSampler.RegisterSpace = 0;

    D3D12_ROOT_SIGNATURE_DESC blitRootSigDesc = { 1, &blitRootParam, 1, &blitSampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    
    ComPtr<ID3DBlob> blitSignatureBlob, blitErrorBlob;
    hr = D3D12SerializeRootSignature(&blitRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blitSignatureBlob, &blitErrorBlob);
    if (FAILED(hr)) { if(blitErrorBlob) throw std::runtime_error("Failed to serialize blit root signature: " + std::string((char*)blitErrorBlob->GetBufferPointer())); else throw std::runtime_error("Failed to serialize blit root signature. HRESULT: " + std::to_string(hr)); }
    hr = self->pImpl->device->CreateRootSignature(0, blitSignatureBlob->GetBufferPointer(), blitSignatureBlob->GetBufferSize(), IID_PPV_ARGS(&self->pImpl->blitRootSignature));
    if (FAILED(hr)) throw std::runtime_error("Failed to create blit root signature. HRESULT: " + std::to_string(hr));

    ComPtr<ID3DBlob> blitVS, blitPS;
    D3DCompile(g_blitShaderHLSL, strlen(g_blitShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &blitVS, nullptr);
    D3DCompile(g_blitShaderHLSL, strlen(g_blitShaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &blitPS, nullptr);

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizerDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizerDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.MultisampleEnable = FALSE;
    rasterizerDesc.AntialiasedLineEnable = FALSE;
    rasterizerDesc.ForcedSampleCount = 0;
    rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = { FALSE,FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL, };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) blendDesc.RenderTarget[i] = defaultRenderTargetBlendDesc;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC blitPsoDesc = {};
    blitPsoDesc.InputLayout = { nullptr, 0 };
    blitPsoDesc.pRootSignature = self->pImpl->blitRootSignature.Get();
    blitPsoDesc.VS = { blitVS->GetBufferPointer(), blitVS->GetBufferSize() };
    blitPsoDesc.PS = { blitPS->GetBufferPointer(), blitPS->GetBufferSize() };
    blitPsoDesc.RasterizerState = rasterizerDesc;
    blitPsoDesc.BlendState = blendDesc;
    blitPsoDesc.DepthStencilState.DepthEnable = FALSE;
    blitPsoDesc.DepthStencilState.StencilEnable = FALSE;
    blitPsoDesc.SampleMask = UINT_MAX;
    blitPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    blitPsoDesc.NumRenderTargets = 1;
    blitPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    blitPsoDesc.SampleDesc.Count = 1;
    blitPsoDesc.SampleDesc.Quality = 0;
    hr = self->pImpl->device->CreateGraphicsPipelineState(&blitPsoDesc, IID_PPV_ARGS(&self->pImpl->blitPSO));
    if (FAILED(hr)) throw std::runtime_error("Failed to create blit PSO. HRESULT: " + std::to_string(hr));
    
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = self->pImpl->device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&self->pImpl->blitSrvHeap));
    if (FAILED(hr)) throw std::runtime_error("Failed to create blit SRV heap. HRESULT: " + std::to_string(hr));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = self->pImpl->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&self->pImpl->blitRtvHeap));
    if (FAILED(hr)) throw std::runtime_error("Failed to create blit RTV heap. HRESULT: " + std::to_string(hr));

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    
    D3D12_ROOT_PARAMETER rootParameters[2] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable = {1, &ranges[0]};
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[1].Descriptor = {0,0};
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC applyShaderSampler = {};
    applyShaderSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    applyShaderSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    applyShaderSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    applyShaderSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    applyShaderSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    applyShaderSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    applyShaderSampler.MinLOD = 0.0f;
    applyShaderSampler.MaxLOD = D3D12_FLOAT32_MAX;
    applyShaderSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC applyShaderRootSigDesc = { _countof(rootParameters), rootParameters, 1, &applyShaderSampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    
    ComPtr<ID3DBlob> applyShaderSignatureBlob, applyShaderErrorBlob;
    hr = D3D12SerializeRootSignature(&applyShaderRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &applyShaderSignatureBlob, &applyShaderErrorBlob);
    if (FAILED(hr)) { if(applyShaderErrorBlob) throw std::runtime_error("Failed to serialize apply_shader root signature: " + std::string((char*)applyShaderErrorBlob->GetBufferPointer())); else throw std::runtime_error("Failed to serialize apply_shader root signature. HRESULT: " + std::to_string(hr)); }
    hr = self->pImpl->device->CreateRootSignature(0, applyShaderSignatureBlob->GetBufferPointer(), applyShaderSignatureBlob->GetBufferSize(), IID_PPV_ARGS(&self->pImpl->shaderRootSignature));
    if (FAILED(hr)) throw std::runtime_error("Failed to create apply_shader root signature. HRESULT: " + std::to_string(hr));

    return self;
}

std::shared_ptr<Texture> DeviceD3D12::create_texture(uint32_t width, uint32_t height, DXGI_FORMAT format, const void* data, size_t data_size) {
    auto tex = std::shared_ptr<Texture>(new Texture());
    tex->pImpl->is_d3d12 = true;
    tex->pImpl->width = width;
    tex->pImpl->height = height;
    tex->pImpl->format = format;

    D3D12_HEAP_PROPERTIES heapProps = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    
    HRESULT hr = pImpl->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex->pImpl->d3d12Resource));
    if (FAILED(hr)) { throw std::runtime_error("Failed to create D3D12 texture. HRESULT: " + std::to_string(hr)); }
    
    if (data && data_size > 0) {
        ComPtr<ID3D12Resource> uploadHeap;
        UINT64 uploadBufferSize;
        pImpl->device->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadBufferSize);

        D3D12_HEAP_PROPERTIES uploadHeapProps = {D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Width = uploadBufferSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;

        hr = pImpl->device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadHeap));
        if (FAILED(hr)) { throw std::runtime_error("Failed to create D3D12 upload heap. HRESULT: " + std::to_string(hr)); }
        
        void* p;
        hr = uploadHeap->Map(0, nullptr, &p);
        if (FAILED(hr)) { throw std::runtime_error("Failed to map D3D12 upload heap. HRESULT: " + std::to_string(hr)); }
        memcpy(p, data, data_size);
        uploadHeap->Unmap(0, nullptr);

        pImpl->commandAllocator->Reset();
        pImpl->commandList->Reset(pImpl->commandAllocator.Get(), nullptr);

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = uploadHeap.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        pImpl->device->GetCopyableFootprints(&desc, 0, 1, 0, &src.PlacedFootprint, nullptr, nullptr, nullptr);
        
        D3D12_TEXTURE_COPY_LOCATION dst = { tex->pImpl->d3d12Resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = tex->pImpl->d3d12Resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pImpl->commandList->ResourceBarrier(1, &barrier);
        
        pImpl->commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        pImpl->commandList->ResourceBarrier(1, &barrier);

        pImpl->commandList->Close();
        ID3D12CommandList* lists[] = { pImpl->commandList.Get() };
        pImpl->commandQueue->ExecuteCommandLists(1, lists);
        
        WaitForGpu();
    }
    return tex;
}

std::shared_ptr<Producer> DeviceD3D12::create_producer(const std::string& stream_name, std::shared_ptr<Texture> texture) {
    if (!texture || !texture->pImpl->is_d3d12 || !texture->pImpl->d3d12Resource) {
        throw std::invalid_argument("Provided texture is not a valid D3D12 texture.");
    }
    validate_stream_name(stream_name);
    auto prod = std::shared_ptr<Producer>(new Producer());
    prod->pImpl->is_d3d11_producer = false;
    prod->pImpl->pDeviceContext = pImpl->commandQueue.Get();
    prod->pImpl->sourceTexture = texture;
    
    HRESULT hr = pImpl->device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&prod->pImpl->d3d12Fence));
    if (FAILED(hr)) { throw std::runtime_error("Failed to create D3D12 shared fence. HRESULT: " + std::to_string(hr)); }

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) {
        throw std::runtime_error("Failed to convert SDDL string to security descriptor. GetLastError: " + std::to_string(GetLastError()));
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    
    DWORD pid = GetCurrentProcessId();
    std::wstring w_stream_name = string_to_wstring(stream_name);
    std::wstring textureName = L"Global\\D3D12_Texture_" + std::to_wstring(pid) + L"_" + w_stream_name;
    std::wstring fenceName = L"Global\\D3D12_Fence_" + std::to_wstring(pid) + L"_" + w_stream_name;
    std::wstring manifestName = L"D3D12_Producer_Manifest_" + std::to_wstring(pid);
    
    hr = pImpl->device->CreateSharedHandle(texture->pImpl->d3d12Resource.Get(), &sa, GENERIC_ALL, textureName.c_str(), &prod->pImpl->hTextureHandle);
    if (FAILED(hr)) { LocalFree(sd); throw std::runtime_error("Failed to create shared handle for texture. HRESULT: " + std::to_string(hr)); }

    hr = pImpl->device->CreateSharedHandle(prod->pImpl->d3d12Fence.Get(), &sa, GENERIC_ALL, fenceName.c_str(), &prod->pImpl->hFenceHandle);
    if (FAILED(hr)) { CloseHandle(prod->pImpl->hTextureHandle); LocalFree(sd); throw std::runtime_error("Failed to create shared handle for fence. HRESULT: " + std::to_string(hr)); }

    prod->pImpl->hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    LocalFree(sd);
    if (!prod->pImpl->hManifest) { CloseHandle(prod->pImpl->hTextureHandle); CloseHandle(prod->pImpl->hFenceHandle); throw std::runtime_error("Failed to create file mapping for manifest. GetLastError: " + std::to_string(GetLastError())); }
    
    prod->pImpl->pManifestView = (BroadcastManifest*)MapViewOfFile(prod->pImpl->hManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    if (!prod->pImpl->pManifestView) { CloseHandle(prod->pImpl->hTextureHandle); CloseHandle(prod->pImpl->hFenceHandle); CloseHandle(prod->pImpl->hManifest); throw std::runtime_error("Failed to map view of file for manifest. GetLastError: " + std::to_string(GetLastError())); }
    
    ZeroMemory(prod->pImpl->pManifestView, sizeof(BroadcastManifest));
    prod->pImpl->pManifestView->width = texture->get_width();
    prod->pImpl->pManifestView->height = texture->get_height();
    prod->pImpl->pManifestView->format = texture->get_format();
    prod->pImpl->pManifestView->adapterLuid = pImpl->adapterLuid;
    wcscpy_s(prod->pImpl->pManifestView->textureName, _countof(prod->pImpl->pManifestView->textureName), textureName.c_str());
    wcscpy_s(prod->pImpl->pManifestView->fenceName, _countof(prod->pImpl->pManifestView->fenceName), fenceName.c_str());

    return prod;
}

std::shared_ptr<Consumer> DeviceD3D12::connect_to_producer(unsigned long pid) {
    auto cons = std::shared_ptr<Consumer>(new Consumer());
    cons->pImpl->pid = pid;
    cons->pImpl->pDeviceContext = pImpl->commandQueue.Get();

    BroadcastManifest manifest;
    if (!get_manifest_from_pid(pid, manifest)) {
        return nullptr;
    }

    if (std::wstring(manifest.textureName).find(L"DirectPort_Texture_") != std::wstring::npos) {
        cons->pImpl->is_d3d11_producer = true;
    } else {
        cons->pImpl->is_d3d11_producer = false;
    }
    
    cons->pImpl->hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!cons->pImpl->hProcess) { return nullptr; }

    HANDLE hFence = get_handle_from_name(manifest.fenceName);
    if (!hFence) { CloseHandle(cons->pImpl->hProcess); return nullptr; }
    HRESULT hr;
    if (cons->pImpl->is_d3d11_producer) {
        ComPtr<ID3D11Device> tempD3D11Device;
        ComPtr<ID3D11Device5> tempD3D11Device5;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &tempD3D11Device, nullptr, nullptr))) {
            CloseHandle(hFence); CloseHandle(cons->pImpl->hProcess); return nullptr;
        }
        tempD3D11Device.As(&tempD3D11Device5);
        if (!tempD3D11Device5) { CloseHandle(hFence); CloseHandle(cons->pImpl->hProcess); return nullptr; }
        hr = tempD3D11Device5->OpenSharedFence(hFence, IID_PPV_ARGS(&cons->pImpl->d3d11Fence));
    } else {
        hr = pImpl->device->OpenSharedHandle(hFence, IID_PPV_ARGS(&cons->pImpl->d3d12Fence));
    }
    CloseHandle(hFence);
    if (FAILED(hr)) { CloseHandle(cons->pImpl->hProcess); return nullptr; }
    
    cons->pImpl->sharedTexture = std::shared_ptr<Texture>(new Texture());
    HANDLE hTexture = get_handle_from_name(manifest.textureName);
    if (!hTexture) { CloseHandle(cons->pImpl->hProcess); return nullptr; }
    if (cons->pImpl->is_d3d11_producer) {
        cons->pImpl->sharedTexture->pImpl->is_d3d11 = true;
        ComPtr<ID3D11Device> tempD3D11Device;
        ComPtr<ID3D11Device1> tempD3D11Device1;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &tempD3D11Device, nullptr, nullptr))) {
            CloseHandle(hTexture); CloseHandle(cons->pImpl->hProcess); return nullptr;
        }
        tempD3D11Device.As(&tempD3D11Device1);
        if (!tempD3D11Device1) { CloseHandle(hTexture); CloseHandle(cons->pImpl->hProcess); return nullptr; }
        hr = tempD3D11Device1->OpenSharedResource1(hTexture, IID_PPV_ARGS(&cons->pImpl->sharedTexture->pImpl->d3d11Texture));
        if (FAILED(hr)) { CloseHandle(hTexture); CloseHandle(cons->pImpl->hProcess); return nullptr; }
        hr = tempD3D11Device->CreateShaderResourceView(cons->pImpl->sharedTexture->pImpl->d3d11Texture.Get(), nullptr, &cons->pImpl->sharedTexture->pImpl->d3d11SRV);
        if (FAILED(hr)) { CloseHandle(hTexture); CloseHandle(cons->pImpl->hProcess); return nullptr; }
    } else {
        cons->pImpl->sharedTexture->pImpl->is_d3d12 = true;
        hr = pImpl->device->OpenSharedHandle(hTexture, IID_PPV_ARGS(&cons->pImpl->sharedTexture->pImpl->d3d12Resource));
        if (FAILED(hr)) { CloseHandle(hTexture); CloseHandle(cons->pImpl->hProcess); return nullptr; }
    }
    CloseHandle(hTexture);
    
    cons->pImpl->sharedTexture->pImpl->width = manifest.width;
    cons->pImpl->sharedTexture->pImpl->height = manifest.height;
    cons->pImpl->sharedTexture->pImpl->format = manifest.format;

    cons->pImpl->privateTexture = create_texture(manifest.width, manifest.height, manifest.format);

    return cons;
}

void DeviceD3D12::copy_texture(std::shared_ptr<Texture> source, std::shared_ptr<Texture> destination) {
    if (!source || !destination || !source->pImpl->is_d3d12 || !destination->pImpl->is_d3d12 ||
        !source->pImpl->d3d12Resource || !destination->pImpl->d3d12Resource) {
        throw std::invalid_argument("Invalid D3D12 source or destination texture for copy_texture. Check for null or incorrect API type.");
    }
    if (source->get_width() != destination->get_width() ||
        source->get_height() != destination->get_height() ||
        source->get_format() != destination->get_format()) {
        throw std::invalid_argument("Source and destination textures must have matching dimensions and format for D3D12::copy_texture.");
    }

    pImpl->commandAllocator->Reset();
    pImpl->commandList->Reset(pImpl->commandAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = { source->pImpl->d3d12Resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE };
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition = { destination->pImpl->d3d12Resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST };
    pImpl->commandList->ResourceBarrier(2, barriers);

    pImpl->commandList->CopyResource(destination->pImpl->d3d12Resource.Get(), source->pImpl->d3d12Resource.Get());

    std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
    std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
    pImpl->commandList->ResourceBarrier(2, barriers);

    pImpl->commandList->Close();
    ID3D12CommandList* ppCommandLists[] = { pImpl->commandList.Get() };
    pImpl->commandQueue->ExecuteCommandLists(1, ppCommandLists);
    
    WaitForGpu();
}

void DeviceD3D12::apply_shader(std::shared_ptr<Texture> output, const std::vector<uint8_t>& shader_bytes, const std::string& entry_point, const std::vector<std::shared_ptr<Texture>>& inputs, const std::vector<uint8_t>& constants) {
    if (!output || !output->pImpl->is_d3d12 || !output->pImpl->d3d12Resource) {
        throw std::invalid_argument("Invalid D3D12 output texture for apply_shader (must be D3D12).");
    }
    
    HRESULT hr;

    ComPtr<ID3D12PipelineState> pso;
    if (shader_bytes.empty() || (shader_bytes.size() == 1 && shader_bytes[0] == '\0')) {
        static ComPtr<ID3D12PipelineState> defaultBlackPSO;
        if (!defaultBlackPSO) {
            ComPtr<ID3DBlob> vsBlob, psBlob;
            HRESULT compile_hr = D3DCompile(g_blitShaderHLSL, strlen(g_blitShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
            if (FAILED(compile_hr)) throw std::runtime_error("Failed to compile internal VS for default black PS. HRESULT: " + std::to_string(compile_hr));
            compile_hr = D3DCompile("float4 PSMain() : SV_TARGET { return float4(0.0,0.0,0.0,1.0); }", strlen("float4 PSMain() : SV_TARGET { return float4(0.0,0.0,0.0,1.0); }"), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, nullptr);
            if (FAILED(compile_hr)) throw std::runtime_error("Failed to compile default black PS. HRESULT: " + std::to_string(compile_hr));
            
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
            psoDesc.pRootSignature = pImpl->shaderRootSignature.Get();
            psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
            psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
            psoDesc.RasterizerState = { D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE };
            psoDesc.BlendState.RenderTarget[0] = { FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL };
            psoDesc.SampleMask = UINT_MAX;
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psoDesc.NumRenderTargets = 1;
            psoDesc.RTVFormats[0] = output->get_format();
            psoDesc.SampleDesc.Count = 1;
            pImpl->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&defaultBlackPSO));
        }
        pso = defaultBlackPSO;
    } else {
        auto it = pImpl->psoCache.find(shader_bytes);
        if (it != pImpl->psoCache.end()) {
            pso = it->second;
        } else {
            ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
            HRESULT compile_hr = D3DCompile(g_blitShaderHLSL, strlen(g_blitShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
            if (FAILED(compile_hr)) throw std::runtime_error("Failed to compile internal VS for apply_shader. HRESULT: " + std::to_string(compile_hr));

            hr = D3DCompile(shader_bytes.data(), shader_bytes.size(), "hlsl_shader", nullptr, nullptr, entry_point.c_str(), "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errorBlob);
            if (FAILED(hr)) {
                if (errorBlob) {
                    throw std::runtime_error("HLSL compile failed for apply_shader: " + std::string((char*)errorBlob->GetBufferPointer()));
                } else {
                    ComPtr<ID3DBlob> csoBlob;
                    if (FAILED(D3DCreateBlob(shader_bytes.size(), &csoBlob))) throw std::runtime_error("Failed to create blob for CSO. HRESULT: " + std::to_string(hr));
                    memcpy(csoBlob->GetBufferPointer(), shader_bytes.data(), shader_bytes.size());
                    psBlob = csoBlob;
                }
            }
            
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
            psoDesc.pRootSignature = pImpl->shaderRootSignature.Get();
            psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
            psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
            psoDesc.RasterizerState = { D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE };
            psoDesc.BlendState.RenderTarget[0] = { FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL };
            psoDesc.SampleMask = UINT_MAX;
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psoDesc.NumRenderTargets = 1;
            psoDesc.RTVFormats[0] = output->get_format();
            psoDesc.SampleDesc.Count = 1;
            
            hr = pImpl->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
            if (FAILED(hr)) { throw std::runtime_error("Failed to create graphics pipeline state for shader. HRESULT: " + std::to_string(hr)); }
            pImpl->psoCache[shader_bytes] = pso;
        }
    }

    pImpl->commandAllocator->Reset();
    pImpl->commandList->Reset(pImpl->commandAllocator.Get(), pso.Get());
    pImpl->commandList->SetGraphicsRootSignature(pImpl->shaderRootSignature.Get());

    ComPtr<ID3D12DescriptorHeap> srvHeap;
    if (!inputs.empty()) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = (UINT)inputs.size();
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = pImpl->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap));
        if (FAILED(hr)) { throw std::runtime_error("Failed to create SRV descriptor heap for shader inputs. HRESULT: " + std::to_string(hr)); }

        UINT srvSize = pImpl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
        for (const auto& input : inputs) {
            if (!input || !input->pImpl->is_d3d12 || !input->pImpl->d3d12Resource) {
                throw std::invalid_argument("Invalid D3D12 input texture for apply_shader (must be D3D12).");
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = input->get_format();
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            pImpl->device->CreateShaderResourceView(input->pImpl->d3d12Resource.Get(), &srvDesc, srvHandle);
            srvHandle.ptr += srvSize;
        }
    }

    ComPtr<ID3D12Resource> cbUploadHeap;
    if (!constants.empty()) {
        D3D12_HEAP_PROPERTIES uploadHeap = { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = (constants.size() + 255) & ~255;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = pImpl->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbUploadHeap));
        if (FAILED(hr)) { throw std::runtime_error("Failed to create D3D12 constant buffer. HRESULT: " + std::to_string(hr)); }
        void* p;
        hr = cbUploadHeap->Map(0, nullptr, &p);
        if (FAILED(hr)) { throw std::runtime_error("Failed to map D3D12 constant buffer. HRESULT: " + std::to_string(hr)); }
        memcpy(p, constants.data(), constants.size());
        cbUploadHeap->Unmap(0, nullptr);
        pImpl->commandList->SetGraphicsRootConstantBufferView(1, cbUploadHeap->GetGPUVirtualAddress());
    }

    if (!inputs.empty()) {
        ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
        pImpl->commandList->SetDescriptorHeaps(_countof(heaps), heaps);
        pImpl->commandList->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output->pImpl->d3d12Resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pImpl->commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = pImpl->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
    if (FAILED(hr)) { throw std::runtime_error("Failed to create RTV descriptor heap for shader output. HRESULT: " + std::to_string(hr)); }
    rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    pImpl->device->CreateRenderTargetView(output->pImpl->d3d12Resource.Get(), nullptr, rtvHandle);

    pImpl->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)output->get_width(), (float)output->get_height(), 0.0f, 1.0f };
    D3D12_RECT sr = { 0, 0, (LONG)output->get_width(), (LONG)output->get_height() };
    pImpl->commandList->RSSetViewports(1, &vp);
    pImpl->commandList->RSSetScissorRects(1, &sr);
    pImpl->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pImpl->commandList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    pImpl->commandList->ResourceBarrier(1, &barrier);
    
    pImpl->commandList->Close();
    ID3D12CommandList* lists[] = { pImpl->commandList.Get() };
    pImpl->commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu();
}

void DeviceD3D12::blit(std::shared_ptr<Texture> source, std::shared_ptr<Window> destination) {
    if (!source || !destination || !source->pImpl->is_d3d12 || !destination->pImpl->is_d3d12 ||
        !source->pImpl->d3d12Resource || !destination->pImpl->d3d12swapChain) {
        throw std::invalid_argument("Invalid D3D12 source texture or window for blit. Check for null or incorrect API type.");
    }
    auto& winImpl = *destination->pImpl;
    
    pImpl->commandAllocator->Reset();
    pImpl->commandList->Reset(pImpl->commandAllocator.Get(), pImpl->blitPSO.Get());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = source->get_format();
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    pImpl->device->CreateShaderResourceView(source->pImpl->d3d12Resource.Get(), &srvDesc, pImpl->blitSrvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = { source->pImpl->d3d12Resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition = { winImpl.d3d12RenderTargets[winImpl.d3d12FrameIndex].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
    pImpl->commandList->ResourceBarrier(2, barriers);

    RECT clientRect; GetClientRect(destination->pImpl->hwnd, &clientRect);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top), 0.0f, 1.0f };
    D3D12_RECT sr = { 0, 0, (LONG)vp.Width, (LONG)vp.Height };
    
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = winImpl.d3d12RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += winImpl.d3d12FrameIndex * winImpl.d3d12RtvDescriptorSize;

    pImpl->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    pImpl->commandList->RSSetViewports(1, &vp);
    pImpl->commandList->RSSetScissorRects(1, &sr);
    pImpl->commandList->SetGraphicsRootSignature(pImpl->blitRootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { pImpl->blitSrvHeap.Get() };
    pImpl->commandList->SetDescriptorHeaps(1, heaps);
    pImpl->commandList->SetGraphicsRootDescriptorTable(0, pImpl->blitSrvHeap->GetGPUDescriptorHandleForHeapStart());
    pImpl->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pImpl->commandList->DrawInstanced(3, 1, 0, 0);

    std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
    std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
    pImpl->commandList->ResourceBarrier(2, barriers);
    
    pImpl->commandList->Close();
    ID3D12CommandList* lists[] = { pImpl->commandList.Get() };
    pImpl->commandQueue->ExecuteCommandLists(1, lists);
}

void DeviceD3D12::clear(std::shared_ptr<Window> window, float r, float g, float b, float a) {
    if (!window || !window->pImpl->is_d3d12 || !window->pImpl->d3d12RtvHeap) {
        throw std::invalid_argument("Invalid D3D12 window for clear. Check for null or incorrect API type.");
    }
    auto& winImpl = *window->pImpl;

    pImpl->commandAllocator->Reset();
    pImpl->commandList->Reset(pImpl->commandAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { winImpl.d3d12RenderTargets[winImpl.d3d12FrameIndex].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
    pImpl->commandList->ResourceBarrier(1, &barrier);

    const float clearColor[] = { r, g, b, a };
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = winImpl.d3d12RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += winImpl.d3d12FrameIndex * winImpl.d3d12RtvDescriptorSize;
    pImpl->commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    pImpl->commandList->ResourceBarrier(1, &barrier);
    
    pImpl->commandList->Close();
    ID3D12CommandList* lists[] = { pImpl->commandList.Get() };
    pImpl->commandQueue->ExecuteCommandLists(1, lists);
}

std::shared_ptr<Window> DeviceD3D12::create_window(uint32_t width, uint32_t height, const std::string& title) {
    auto win = std::shared_ptr<Window>(new Window());
    win->pImpl->is_d3d12 = true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"DirectPortD3D12WindowClass";
    if (!RegisterClassExW(&wc)) {
    }

    win->pImpl->hwnd = CreateWindowExW(0, wc.lpszClassName, string_to_wstring(title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!win->pImpl->hwnd) throw std::runtime_error("Failed to create D3D12 window. GetLastError: " + std::to_string(GetLastError()));
    
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { DestroyWindow(win->pImpl->hwnd); throw std::runtime_error("Failed to create DXGI Factory 4. HRESULT: " + std::to_string(hr)); }


    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    hr = factory->CreateSwapChainForHwnd(pImpl->commandQueue.Get(), win->pImpl->hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1);
    if (FAILED(hr)) { DestroyWindow(win->pImpl->hwnd); throw std::runtime_error("Failed to create D3D12 swap chain. HRESULT: " + std::to_string(hr)); }
    swapChain1.As(&win->pImpl->d3d12swapChain);
    win->pImpl->d3d12FrameIndex = win->pImpl->d3d12swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 2;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = pImpl->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&win->pImpl->d3d12RtvHeap));
    if (FAILED(hr)) { DestroyWindow(win->pImpl->hwnd); throw std::runtime_error("Failed to create D3D12 RTV heap for window. HRESULT: " + std::to_string(hr)); }
    win->pImpl->d3d12RtvDescriptorSize = pImpl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = win->pImpl->d3d12RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < 2; n++) {
        hr = win->pImpl->d3d12swapChain->GetBuffer(n, IID_PPV_ARGS(&win->pImpl->d3d12RenderTargets[n]));
        if (FAILED(hr)) { DestroyWindow(win->pImpl->hwnd); throw std::runtime_error("Failed to get D3D12 back buffer for window. HRESULT: " + std::to_string(hr)); }
        pImpl->device->CreateRenderTargetView(win->pImpl->d3d12RenderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += win->pImpl->d3d12RtvDescriptorSize;
    }

    ShowWindow(win->pImpl->hwnd, SW_SHOW);
    UpdateWindow(win->pImpl->hwnd);
    return win;
}

void DeviceD3D12::resize_window(std::shared_ptr<Window> window) {
    if (!window || !window->pImpl->is_d3d12 || !window->pImpl->d3d12swapChain) {
        throw std::invalid_argument("Invalid D3D12 window for resize_window.");
    }
    auto& winImpl = *window->pImpl;

    WaitForGpu();

    for (int i = 0; i < 2; ++i) {
        winImpl.d3d12RenderTargets[i].Reset();
        pImpl->frameFenceValues[i] = pImpl->fenceValue;
    }

    HRESULT hr = winImpl.d3d12swapChain->ResizeBuffers(2, 0, 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to resize D3D12 swap chain. HRESULT: " + std::to_string(hr));
    }
    
    winImpl.d3d12FrameIndex = winImpl.d3d12swapChain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = winImpl.d3d12RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < 2; n++) {
        hr = winImpl.d3d12swapChain->GetBuffer(n, IID_PPV_ARGS(&winImpl.d3d12RenderTargets[n]));
        if (FAILED(hr)) { throw std::runtime_error("Failed to get D3D12 back buffer after resize. HRESULT: " + std::to_string(hr)); }
        pImpl->device->CreateRenderTargetView(winImpl.d3d12RenderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += winImpl.d3d12RtvDescriptorSize;
    }
}

void DeviceD3D12::blit_texture_to_region(std::shared_ptr<Texture> source, std::shared_ptr<Texture> destination,
                                        uint32_t dest_x, uint32_t dest_y, uint32_t dest_width, uint32_t dest_height) {
    if (!source || !destination || !source->pImpl->is_d3d12 || !destination->pImpl->is_d3d12 ||
        !source->pImpl->d3d12Resource || !destination->pImpl->d3d12Resource) {
        throw std::invalid_argument("Invalid D3D12 source or destination texture for blit_texture_to_region. Check for null or incorrect API type.");
    }
    if (dest_width == 0 || dest_height == 0) return;

    pImpl->commandAllocator->Reset();
    pImpl->commandList->Reset(pImpl->commandAllocator.Get(), pImpl->blitPSO.Get());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = source->get_format();
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    pImpl->device->CreateShaderResourceView(source->pImpl->d3d12Resource.Get(), &srvDesc, pImpl->blitSrvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = { source->pImpl->d3d12Resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition = { destination->pImpl->d3d12Resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET };
    pImpl->commandList->ResourceBarrier(2, barriers);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = pImpl->blitRtvHeap->GetCPUDescriptorHandleForHeapStart();
    pImpl->device->CreateRenderTargetView(destination->pImpl->d3d12Resource.Get(), nullptr, rtvHandle);
    pImpl->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    D3D12_VIEWPORT vp = {
        (float)dest_x,
        (float)dest_y,
        (float)dest_width,
        (float)dest_height,
        0.0f,
        1.0f
    };
    D3D12_RECT sr = {
        (LONG)dest_x,
        (LONG)dest_y,
        (LONG)(dest_x + dest_width),
        (LONG)(dest_y + dest_height)
    };
    pImpl->commandList->RSSetViewports(1, &vp);
    pImpl->commandList->RSSetScissorRects(1, &sr);

    pImpl->commandList->SetGraphicsRootSignature(pImpl->blitRootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { pImpl->blitSrvHeap.Get() };
    pImpl->commandList->SetDescriptorHeaps(1, heaps);
    pImpl->commandList->SetGraphicsRootDescriptorTable(0, pImpl->blitSrvHeap->GetGPUDescriptorHandleForHeapStart());
    pImpl->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pImpl->commandList->DrawInstanced(3, 1, 0, 0);

    std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
    std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
    pImpl->commandList->ResourceBarrier(2, barriers);
    
    pImpl->commandList->Close();
    ID3D12CommandList* lists[] = { pImpl->commandList.Get() };
    pImpl->commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu();
}

std::vector<ProducerInfo> DirectPort::discover() {
    std::vector<ProducerInfo> discovered;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return discovered;
    PROCESSENTRY32W pe32 = {sizeof(PROCESSENTRY32W)};
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            BroadcastManifest manifest;
            if (get_manifest_from_pid(pe32.th32ProcessID, manifest)) {
                std::wstring exeFileName(pe32.szExeFile);
                std::transform(exeFileName.begin(), exeFileName.end(), exeFileName.begin(), ::towlower);
                std::wstring type_str;
                if (wcsstr(manifest.textureName, L"D3D12_Texture_") != nullptr) {
                    if (wcsstr(exeFileName.c_str(), L"multiplexer") != nullptr) type_str = L"D3D12 Multiplexer";
                    else if (wcsstr(exeFileName.c_str(), L"camera") != nullptr) type_str = L"D3D12 Camera";
                    else if (wcsstr(exeFileName.c_str(), L"shaderfilter") != nullptr) type_str = L"D3D12 Shader Filter";
                    else if (wcsstr(exeFileName.c_str(), L"producer") != nullptr) type_str = L"D3D12 Producer";
                    else type_str = L"Python D3D12 Producer";
                } else {
                    if (wcsstr(exeFileName.c_str(), L"multiplexer") != nullptr) type_str = L"D3D11 Multiplexer";
                    else if (wcsstr(exeFileName.c_str(), L"camera") != nullptr) type_str = L"D3D11 Camera";
                    else if (wcsstr(exeFileName.c_str(), L"shaderfilter") != nullptr) type_str = L"D3D11 Shader Filter";
                    else if (wcsstr(exeFileName.c_str(), L"producer") != nullptr) type_str = L"D3D11 Producer";
                    else type_str = L"Python D3D11 Producer";
                }

                std::wstring w_stream_name(manifest.textureName);
                size_t pid_str_len = std::to_wstring(pe32.th32ProcessID).length();
                size_t first_underscore_after_pid = w_stream_name.find(L"_", w_stream_name.find(std::to_wstring(pe32.th32ProcessID)) + pid_str_len);
                
                if (first_underscore_after_pid != std::wstring::npos) {
                    w_stream_name = w_stream_name.substr(first_underscore_after_pid + 1);
                } else {
                    w_stream_name = L"Unknown";
                }
                
                discovered.push_back({pe32.th32ProcessID, pe32.szExeFile, w_stream_name, type_str});
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    return discovered;
}