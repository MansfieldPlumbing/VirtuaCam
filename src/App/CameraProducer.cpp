// =============================================================================
// CameraProducer.cpp  --  Physical webcam -> shared frame
// =============================================================================
// The camera is opened through an asynchronous Source Reader that shares our
// D3D11 device (MF_SOURCE_READER_D3D_MANAGER) and does decode/colour
// conversion on the GPU, so frames arrive as textures and are copied into
// the shared texture without touching system memory.  Each finished read
// schedules the next one; nothing polls.
//
// Mode choice: the largest native mode up to 1920x1080, then the highest
// frame rate up to 60 fps -- not a hard-coded size.
// =============================================================================

#include "Producers.h"
#include "Gpu.h"
#include "SharedFrame.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/implements.h>
#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

class CameraReader final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFSourceReaderCallback>
{
public:
    HRESULT Start(const std::wstring& symbolicLink, DWORD mainThread);
    void Stop();

    // IMFSourceReaderCallback
    STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG, IMFSample* sample) override;
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    HRESULT ChooseMode();
    HRESULT Deliver(IMFSample* sample);
    void Fail() { PostThreadMessageW(m_mainThread, WM_QUIT, ProducerExitSourceLost, 0); }

    wil::critical_section m_lock;
    bool m_stopped = false;
    DWORD m_mainThread = 0;
    wil::com_ptr_nothrow<ID3D11Device> m_device;
    wil::com_ptr_nothrow<ID3D11DeviceContext> m_context;
    wil::com_ptr_nothrow<IMFDXGIDeviceManager> m_manager;
    wil::com_ptr_nothrow<IMFSourceReader> m_reader;
    Ipc::FramePublisher m_publisher;
    UINT m_width = 0;
    UINT m_height = 0;
    std::vector<BYTE> m_flip;   // only for bottom-up system-memory frames
};

HRESULT CameraReader::Start(const std::wstring& symbolicLink, DWORD mainThread)
{
    m_mainThread = mainThread;
    RETURN_IF_FAILED(Gpu::CreateDevice(&m_device, D3D11_CREATE_DEVICE_VIDEO_SUPPORT));
    m_device->GetImmediateContext(&m_context);

    UINT resetToken = 0;
    RETURN_IF_FAILED(MFCreateDXGIDeviceManager(&resetToken, &m_manager));
    RETURN_IF_FAILED(m_manager->ResetDevice(m_device.get(), resetToken));

    wil::com_ptr_nothrow<IMFAttributes> sourceAttributes;
    RETURN_IF_FAILED(MFCreateAttributes(&sourceAttributes, 2));
    RETURN_IF_FAILED(sourceAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
    RETURN_IF_FAILED(sourceAttributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symbolicLink.c_str()));
    wil::com_ptr_nothrow<IMFMediaSource> source;
    RETURN_IF_FAILED(MFCreateDeviceSource(sourceAttributes.get(), &source));

    wil::com_ptr_nothrow<IMFAttributes> readerAttributes;
    RETURN_IF_FAILED(MFCreateAttributes(&readerAttributes, 4));
    RETURN_IF_FAILED(readerAttributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback*>(this)));
    RETURN_IF_FAILED(readerAttributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m_manager.get()));
    RETURN_IF_FAILED(readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
    RETURN_IF_FAILED(readerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    RETURN_IF_FAILED(MFCreateSourceReaderFromMediaSource(source.get(), readerAttributes.get(), &m_reader));

    RETURN_IF_FAILED(ChooseMode());
    return m_reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
}

HRESULT CameraReader::ChooseMode()
{
    const DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    wil::com_ptr_nothrow<IMFMediaType> best;
    UINT64 bestScore = 0;
    for (DWORD i = 0;; i++) {
        wil::com_ptr_nothrow<IMFMediaType> type;
        if (FAILED(m_reader->GetNativeMediaType(stream, i, &type)))
            break;
        UINT32 w = 0, h = 0, num = 0, den = 1;
        if (FAILED(MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &w, &h)))
            continue;
        MFGetAttributeRatio(type.get(), MF_MT_FRAME_RATE, &num, &den);
        const UINT64 area = (UINT64)w * h;
        const UINT fps = den ? num / den : 0;
        // Prefer <= 1080p; among those the largest, then the fastest (<= 60).
        const UINT64 score = (area <= 1920ull * 1080 ? (1ull << 62) : 0) +
                             (area <= 1920ull * 1080 ? area : (1ull << 40) - area) * 64 + std::min(fps, 60u);
        if (score > bestScore) {
            bestScore = score;
            best = type;
        }
    }
    RETURN_HR_IF_NULL(MF_E_INVALIDMEDIATYPE, best.get());
    RETURN_IF_FAILED(m_reader->SetCurrentMediaType(stream, nullptr, best.get()));

    // Ask for BGRA with alpha (copy-compatible with our shared texture);
    // fall back to BGRX, which the broker samples just as well.
    UINT32 w = 0, h = 0;
    RETURN_IF_FAILED(MFGetAttributeSize(best.get(), MF_MT_FRAME_SIZE, &w, &h));
    HRESULT hr = E_FAIL;
    for (const GUID& subtype : { MFVideoFormat_ARGB32, MFVideoFormat_RGB32 }) {
        wil::com_ptr_nothrow<IMFMediaType> output;
        RETURN_IF_FAILED(MFCreateMediaType(&output));
        RETURN_IF_FAILED(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        RETURN_IF_FAILED(output->SetGUID(MF_MT_SUBTYPE, subtype));
        RETURN_IF_FAILED(MFSetAttributeSize(output.get(), MF_MT_FRAME_SIZE, w, h));
        hr = m_reader->SetCurrentMediaType(stream, nullptr, output.get());
        if (SUCCEEDED(hr))
            break;
    }
    RETURN_IF_FAILED(hr);
    m_width = w;
    m_height = h;
    return S_OK;
}

HRESULT CameraReader::Deliver(IMFSample* sample)
{
    wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));

    if (auto dxgi = wil::try_com_query_nothrow<IMFDXGIBuffer>(buffer)) {
        wil::com_ptr_nothrow<ID3D11Texture2D> texture;
        UINT subresource = 0;
        RETURN_IF_FAILED(dxgi->GetResource(IID_PPV_ARGS(&texture)));
        RETURN_IF_FAILED(dxgi->GetSubresourceIndex(&subresource));
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (!m_publisher.IsOpen())
            RETURN_IF_FAILED(m_publisher.OpenForProcess(m_device.get(), m_width, m_height, desc.Format));
        const D3D11_BOX box{ 0, 0, 0, std::min(m_width, desc.Width), std::min(m_height, desc.Height), 1 };
        m_context->CopySubresourceRegion(m_publisher.Texture(), 0, 0, 0, 0, texture.get(), subresource, &box);
    } else {
        // System-memory frame (no GPU path on this driver): upload it.
        if (!m_publisher.IsOpen())
            RETURN_IF_FAILED(m_publisher.OpenForProcess(m_device.get(), m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM));
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        auto buffer2d = wil::try_com_query_nothrow<IMF2DBuffer>(buffer);
        RETURN_HR_IF_NULL(E_NOINTERFACE, buffer2d.get());
        RETURN_IF_FAILED(buffer2d->Lock2D(&scan0, &pitch));
        auto unlock = wil::scope_exit([&] { buffer2d->Unlock2D(); });
        const BYTE* rows = scan0;
        UINT rowPitch = (UINT)pitch;
        if (pitch < 0) {
            const size_t rowBytes = (size_t)m_width * 4;
            m_flip.resize(rowBytes * m_height);
            for (UINT y = 0; y < m_height; y++)
                memcpy(m_flip.data() + rowBytes * y, scan0 + (ptrdiff_t)pitch * y, rowBytes);
            rows = m_flip.data();
            rowPitch = (UINT)rowBytes;
        }
        m_context->UpdateSubresource(m_publisher.Texture(), 0, nullptr, rows, rowPitch, 0);
    }
    m_publisher.Publish(m_context.get());
    return S_OK;
}

STDMETHODIMP CameraReader::OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG, IMFSample* sample)
{
    auto lock = m_lock.lock();
    if (m_stopped)
        return S_OK;
    if (FAILED(status) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
        Fail();   // camera unplugged or taken over by an exclusive app
        return S_OK;
    }
    if (sample)
        Deliver(sample);
    if (FAILED(m_reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr)))
        Fail();
    return S_OK;
}

void CameraReader::Stop()
{
    {
        auto lock = m_lock.lock();
        m_stopped = true;
    }
    if (m_reader)
        m_reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS);
    auto lock = m_lock.lock();
    m_reader.reset();
    m_publisher.Close();
}

} // namespace

int RunCameraProducer(const std::wstring& symbolicLink)
{
    if (symbolicLink.empty())
        return ProducerExitBadArguments;
    if (FAILED(MFStartup(MF_VERSION)))
        return ProducerExitStartFailed;
    auto shutdown = wil::scope_exit([] { MFShutdown(); });

    auto reader = Microsoft::WRL::Make<CameraReader>();
    if (!reader || FAILED(reader->Start(symbolicLink, GetCurrentThreadId())))
        return ProducerExitStartFailed;
    const int code = RunMessageLoop();
    reader->Stop();
    return code;
}
