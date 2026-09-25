// =============================================================================
// SharedFrame.h  --  The shared-frame IPC contract
// =============================================================================
// Every VirtuaCam component talks to every other one through exactly one
// mechanism: a named, memory-mapped BroadcastManifest that points at a named
// shared D3D11 texture and a named shared D3D11 fence.
//
//   Producer (any process)          Broker (VirtuaCam.exe)        Camera (frame server)
//   ----------------------          ----------------------        ---------------------
//   DirectPort_Producer_Manifest_<pid>  ──►  composite  ──►  Local\DirectPort_Producer_
//                                                           Manifest_VirtuaCast_Broker
//
// The layout of BroadcastManifest and the object names below are a wire
// format shared with external producers; do not change them.  (The
// "DirectPort" prefix inside the object-name strings is historical and is
// kept only so existing producers keep working.)  A producer publishes a frame by
//   1. writing the texture,
//   2. Signal()ing the fence with a strictly increasing value,
//   3. storing that same value in BroadcastManifest::frameValue.
// A consumer that sees frameValue advance waits on the fence for that value
// (on the GPU) before reading the texture.
// =============================================================================

#pragma once

#include <windows.h>
#include <d3d11_4.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <string>

namespace Ipc {

// Reserved for control messages written by consumers.  Always None today.
enum class ManifestCommand : UINT32 { None = 0 };

// Wire format.  Field order, sizes and packing are fixed.
struct BroadcastManifest {
    UINT64      frameValue;          // Last fence value signalled for a finished frame
    UINT        width;
    UINT        height;
    DXGI_FORMAT format;
    LUID        adapterLuid;         // Shared textures only open on the same adapter
    WCHAR       textureName[256];
    WCHAR       fenceName[256];
    volatile ManifestCommand command;
};

// --- Object names -----------------------------------------------------------
// Producers:  mapping "DirectPort_Producer_Manifest_<pid>",
//             texture "Local\DirectPortTexture_<pid>", fence "Local\DirectPortFence_<pid>".
// Broker:     mapping "Local\DirectPort_Producer_Manifest_VirtuaCast_Broker",
//             texture/fence "Local\VirtuaCast_Broker_Texture" / "..._Fence"
//             (a numeric suffix is appended when the broker output is resized,
//             because the previous named objects may still be held open).
std::wstring ProducerManifestName(DWORD pid);
extern const wchar_t* const kBrokerManifestName;

// Reads a manifest's frame counter without tearing.
inline UINT64 LoadFrameValue(const BroadcastManifest* m)
{
    return static_cast<UINT64>(InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(const_cast<UINT64*>(&m->frameValue)), 0, 0));
}

// Returns the LUID of the adapter a device was created on.
LUID AdapterLuidOf(ID3D11Device* device);

// Opens a named NT handle to a shared fence.  D3D11 has no by-name fence
// open, so this goes through ID3D12Device::OpenSharedHandleByName on a single
// lazily-created, process-wide D3D12 device (never one per call).
wil::unique_handle OpenNamedSharedHandle(const wchar_t* name);

// -----------------------------------------------------------------------------
// FramePublisher  --  producer side
// -----------------------------------------------------------------------------
// Owns a shared texture + fence + manifest.  Render or copy into Texture(),
// then call Publish() on the same device context.
class FramePublisher {
public:
    FramePublisher() = default;
    FramePublisher(const FramePublisher&) = delete;
    FramePublisher& operator=(const FramePublisher&) = delete;
    ~FramePublisher() { Close(); }

    // manifestName/textureName/fenceName follow the conventions above.
    // bindFlags are added to D3D11_BIND_SHADER_RESOURCE.
    HRESULT Open(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                 const std::wstring& manifestName, const std::wstring& textureName,
                 const std::wstring& fenceName, UINT bindFlags = 0);

    // Convenience for producer processes: uses the per-PID names.
    HRESULT OpenForProcess(ID3D11Device* device, UINT width, UINT height,
                           DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM, UINT bindFlags = 0);

    // Replaces texture and fence (e.g. on resize) while keeping the manifest
    // mapping alive, so consumers holding the manifest notice the new names.
    HRESULT Recreate(UINT width, UINT height, const std::wstring& textureName, const std::wstring& fenceName);

    void Close();

    // Signals the fence and advertises the new frame.  Returns the frame value.
    UINT64 Publish(ID3D11DeviceContext* context);

    bool IsOpen() const { return m_view != nullptr; }
    ID3D11Texture2D* Texture() const { return m_texture.get(); }
    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }

private:
    HRESULT CreateSharedObjects(UINT width, UINT height, const std::wstring& textureName, const std::wstring& fenceName);

    wil::com_ptr_nothrow<ID3D11Device5>   m_device;
    wil::com_ptr_nothrow<ID3D11Texture2D> m_texture;
    wil::com_ptr_nothrow<ID3D11Fence>     m_fence;
    wil::unique_handle m_textureHandle;
    wil::unique_handle m_fenceHandle;
    wil::unique_handle m_mapping;
    BroadcastManifest* m_view = nullptr;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    UINT   m_bindFlags = 0;
    UINT   m_width = 0;
    UINT   m_height = 0;
    UINT64 m_frameValue = 0;
};

// -----------------------------------------------------------------------------
// FrameSubscription  --  consumer side
// -----------------------------------------------------------------------------
// Keeps a producer's manifest mapped for as long as the subscription lives
// (no per-frame OpenFileMapping), opens its texture/fence on a given device
// and copies each new frame into a private, sampleable texture.
class FrameSubscription {
public:
    FrameSubscription() = default;
    FrameSubscription(const FrameSubscription&) = delete;
    FrameSubscription& operator=(const FrameSubscription&) = delete;
    ~FrameSubscription() { Close(); }

    // Fails cheaply (no D3D work) when the manifest does not exist yet.
    // mipmapped: give the private copy a full mip chain for quality downscaling.
    HRESULT Open(ID3D11Device* device, const std::wstring& manifestName, bool mipmapped = false);
    void Close();

    bool IsOpen() const { return m_view != nullptr; }

    // True when the producer re-pointed its manifest at different objects
    // (resize) -- the caller should Close() and Open() again.
    bool IsStale() const;

    // Latest frame value advertised by the producer.
    UINT64 AdvertisedFrame() const { return m_view ? LoadFrameValue(m_view) : 0; }
    UINT64 LastCopiedFrame() const { return m_lastCopied; }
    bool HasNewFrame() const { return AdvertisedFrame() > m_lastCopied; }

    // If a newer frame is advertised: GPU-wait on the fence, copy it into the
    // private texture (and regenerate mips when requested).  Returns true
    // when the private texture changed.
    bool Acquire(ID3D11DeviceContext4* context, bool generateMips = false);

    // Auto-reset event signalled when the producer's fence passes the next
    // frame value.  Re-armed by Acquire(); null if the device lacks support.
    HANDLE FrameEvent() const { return m_frameEvent.get(); }

    ID3D11Texture2D*          Texture() const { return m_private.get(); }
    ID3D11ShaderResourceView* View() const { return m_privateView.get(); }
    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }
    const std::wstring& ManifestName() const { return m_manifestName; }

private:
    void ArmFrameEvent();

    std::wstring m_manifestName;
    wil::unique_handle m_mapping;
    const BroadcastManifest* m_view = nullptr;
    std::wstring m_textureName;
    std::wstring m_fenceName;
    wil::com_ptr_nothrow<ID3D11Texture2D>          m_shared;
    wil::com_ptr_nothrow<ID3D11Fence>              m_fence;
    wil::com_ptr_nothrow<ID3D11Texture2D>          m_private;
    wil::com_ptr_nothrow<ID3D11ShaderResourceView> m_privateView;
    wil::unique_event_nothrow m_frameEvent;
    UINT   m_width = 0;
    UINT   m_height = 0;
    UINT64 m_lastCopied = 0;
};

} // namespace Ipc
