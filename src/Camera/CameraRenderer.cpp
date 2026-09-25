// =============================================================================
// CameraRenderer.cpp
// =============================================================================

#include "CameraRenderer.h"
#include "Gpu.h"

#include <mferror.h>
#include <algorithm>
#include <cstring>

namespace {

// How long the broker may stay silent before the connection is considered
// dead.  The broker re-announces its current frame at least once a second.
constexpr ULONGLONG kBrokerSilenceMs = 3000;
constexpr ULONGLONG kBrokerRetryMs = 1000;
constexpr UINT kNoSignalWidth = 1280;
constexpr UINT kNoSignalHeight = 720;

DXGI_FORMAT DxgiFormatFor(REFGUID subtype)
{
    if (subtype == MFVideoFormat_NV12) return DXGI_FORMAT_NV12;
    if (subtype == MFVideoFormat_YUY2) return DXGI_FORMAT_YUY2;
    if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) return DXGI_FORMAT_B8G8R8A8_UNORM;
    return DXGI_FORMAT_UNKNOWN;
}

bool IsYuv(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_YUY2;
}

} // namespace

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

HRESULT CameraRenderer::SetDeviceManager(IMFDXGIDeviceManager* manager)
{
    if (manager == m_manager.get())
        return S_OK;
    Reset();
    m_manager = manager;
    return S_OK;
}

HRESULT CameraRenderer::Configure(UINT width, UINT height, REFGUID subtype)
{
    const DXGI_FORMAT format = DxgiFormatFor(subtype);
    RETURN_HR_IF(MF_E_INVALIDMEDIATYPE, format == DXGI_FORMAT_UNKNOWN || !width || !height);
    if (width != m_width || height != m_height || format != m_format) {
        m_width = width;
        m_height = height;
        m_format = format;
        // Everything sized by the output goes; the broker connection stays.
        m_processor.reset();
        m_enumerator.reset();
        m_inputView.reset();
        m_inputTexture.reset();
        m_outputViews.clear();
        m_target.reset();
        m_targetView.reset();
        m_staging.reset();
    }
    return S_OK;
}

void CameraRenderer::ResetDeviceObjects()
{
    m_outputViews.clear();
    m_inputView.reset();
    m_inputTexture.reset();
    m_processor.reset();
    m_enumerator.reset();
    m_videoContext.reset();
    m_videoDevice.reset();
    m_targetView.reset();
    m_target.reset();
    m_staging.reset();
    m_noSignal.reset();
    m_broker.Close();
    m_nextBrokerAttempt = 0;
    m_boundDevice = nullptr;
}

void CameraRenderer::Reset()
{
    ResetDeviceObjects();
    if (m_manager && m_deviceHandle)
        m_manager->CloseDeviceHandle(m_deviceHandle);
    m_deviceHandle = nullptr;
    m_manager.reset();
    m_privateDevice.reset();
}

// -----------------------------------------------------------------------------
// Device access
// -----------------------------------------------------------------------------

HRESULT CameraRenderer::AcquireDevice(ID3D11Device** device)
{
    *device = nullptr;
    if (m_manager) {
        if (!m_deviceHandle)
            RETURN_IF_FAILED(m_manager->OpenDeviceHandle(&m_deviceHandle));
        HRESULT hr = m_manager->LockDevice(m_deviceHandle, IID_PPV_ARGS(device), TRUE);
        if (hr == MF_E_DXGI_NEW_VIDEO_DEVICE) {
            // The frame server swapped devices: start over on the new one.
            m_manager->CloseDeviceHandle(m_deviceHandle);
            m_deviceHandle = nullptr;
            ResetDeviceObjects();
            RETURN_IF_FAILED(m_manager->OpenDeviceHandle(&m_deviceHandle));
            hr = m_manager->LockDevice(m_deviceHandle, IID_PPV_ARGS(device), TRUE);
        }
        RETURN_IF_FAILED(hr);
    } else {
        if (!m_privateDevice)
            RETURN_IF_FAILED(Gpu::CreateDevice(&m_privateDevice, D3D11_CREATE_DEVICE_VIDEO_SUPPORT));
        m_privateDevice.copy_to(device);
    }

    if (*device != m_boundDevice) {
        ResetDeviceObjects();
        m_boundDevice = *device;
    }
    return S_OK;
}

void CameraRenderer::ReleaseDevice()
{
    if (m_manager && m_deviceHandle)
        m_manager->UnlockDevice(m_deviceHandle, FALSE);
}

// -----------------------------------------------------------------------------
// Input: the broker's latest frame, or the static "NO SIGNAL" frame
// -----------------------------------------------------------------------------

HRESULT CameraRenderer::PrepareInput(ID3D11Device* device, ID3D11Texture2D** input)
{
    *input = nullptr;
    const ULONGLONG now = GetTickCount64();

    if (m_broker.IsOpen()) {
        const UINT64 advertised = m_broker.AdvertisedFrame();
        if (advertised != m_lastBrokerFrame) {
            m_lastBrokerFrame = advertised;
            m_lastBrokerFrameTick = now;
        }
        if (m_broker.IsStale() || now - m_lastBrokerFrameTick > kBrokerSilenceMs) {
            m_broker.Close();
            m_nextBrokerAttempt = now;
        }
    }
    if (!m_broker.IsOpen() && now >= m_nextBrokerAttempt) {
        if (SUCCEEDED(m_broker.Open(device, Ipc::kBrokerManifestName))) {
            m_lastBrokerFrame = m_broker.AdvertisedFrame();
            m_lastBrokerFrameTick = now;
        } else {
            m_nextBrokerAttempt = now + kBrokerRetryMs;
        }
    }

    if (m_broker.IsOpen()) {
        wil::com_ptr_nothrow<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        if (auto context4 = context.try_query<ID3D11DeviceContext4>())
            m_broker.Acquire(context4.get());
        if (m_broker.LastCopiedFrame() > 0) {
            *input = m_broker.Texture();
            (*input)->AddRef();
            return S_OK;
        }
    }

    if (!m_noSignal)
        RETURN_IF_FAILED(Gpu::CreateNoSignalTexture(device, kNoSignalWidth, kNoSignalHeight, &m_noSignal));
    *input = m_noSignal.get();
    (*input)->AddRef();
    return S_OK;
}

// -----------------------------------------------------------------------------
// Video processor: scale, letterbox and colour-convert in one blit
// -----------------------------------------------------------------------------

HRESULT CameraRenderer::PrepareProcessor(ID3D11Device* device, UINT inputWidth, UINT inputHeight)
{
    if (m_processor && inputWidth == m_processorInputWidth && inputHeight == m_processorInputHeight)
        return S_OK;

    m_processor.reset();
    m_enumerator.reset();
    m_inputView.reset();
    m_inputTexture.reset();
    m_outputViews.clear();
    m_targetView.reset();

    if (!m_videoDevice) {
        RETURN_IF_FAILED(device->QueryInterface(IID_PPV_ARGS(&m_videoDevice)));
        wil::com_ptr_nothrow<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        RETURN_IF_FAILED(context->QueryInterface(IID_PPV_ARGS(&m_videoContext)));
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = inputWidth;
    content.InputHeight = inputHeight;
    content.OutputWidth = m_width;
    content.OutputHeight = m_height;
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    RETURN_IF_FAILED(m_videoDevice->CreateVideoProcessorEnumerator(&content, &m_enumerator));

    UINT support = 0;
    RETURN_IF_FAILED(m_enumerator->CheckVideoProcessorFormat(m_format, &support));
    RETURN_HR_IF(MF_E_INVALIDMEDIATYPE, !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT));
    RETURN_IF_FAILED(m_videoDevice->CreateVideoProcessor(m_enumerator.get(), 0, &m_processor));

    auto* vc = m_videoContext.get();
    auto* vp = m_processor.get();
    vc->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vc->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);

    const RECT source{ 0, 0, (LONG)inputWidth, (LONG)inputHeight };
    const Gpu::RectF fit = Gpu::FitRect({ 0, 0, (float)m_width, (float)m_height }, inputWidth, inputHeight);
    // Even edges keep 4:2:0 chroma sites aligned.
    const RECT dest{ (LONG)fit.x & ~1L, (LONG)fit.y & ~1L,
                     ((LONG)(fit.x + fit.w + 0.5f)) & ~1L, ((LONG)(fit.y + fit.h + 0.5f)) & ~1L };
    const RECT target{ 0, 0, (LONG)m_width, (LONG)m_height };
    vc->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &source);
    vc->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dest);
    vc->VideoProcessorSetOutputTargetRect(vp, TRUE, &target);

    D3D11_VIDEO_COLOR black{};
    black.RGBA = { 0.0f, 0.0f, 0.0f, 1.0f };
    vc->VideoProcessorSetOutputBackgroundColor(vp, FALSE, &black);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputSpace{};
    inputSpace.RGB_Range = 0;   // full-range RGB
    vc->VideoProcessorSetStreamColorSpace(vp, 0, &inputSpace);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputSpace{};
    if (IsYuv(m_format)) {
        outputSpace.YCbCr_Matrix = m_height >= 720 ? 1 : 0;   // BT.709 for HD, BT.601 for SD
        outputSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    }
    vc->VideoProcessorSetOutputColorSpace(vp, &outputSpace);

    m_processorInputWidth = inputWidth;
    m_processorInputHeight = inputHeight;
    return S_OK;
}

HRESULT CameraRenderer::OutputViewFor(ID3D11Texture2D* texture, UINT subresource, ID3D11VideoProcessorOutputView** view)
{
    for (const auto& entry : m_outputViews) {
        if (entry.texture.get() == texture && entry.subresource == subresource)
            return entry.view.copy_to(view);
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC viewDesc{};
    if (desc.ArraySize > 1) {
        viewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        viewDesc.Texture2DArray.MipSlice = 0;
        viewDesc.Texture2DArray.FirstArraySlice = subresource;
        viewDesc.Texture2DArray.ArraySize = 1;
    } else {
        viewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipSlice = 0;
    }

    OutputView entry;
    RETURN_IF_FAILED(m_videoDevice->CreateVideoProcessorOutputView(texture, m_enumerator.get(), &viewDesc, &entry.view));
    entry.texture = texture;
    entry.subresource = subresource;
    // The allocator recycles a small pool of textures; bound the cache anyway.
    if (m_outputViews.size() >= 32)
        m_outputViews.clear();
    m_outputViews.push_back(entry);
    return entry.view.copy_to(view);
}

HRESULT CameraRenderer::Blit(ID3D11Texture2D* input, ID3D11VideoProcessorOutputView* output)
{
    if (input != m_inputTexture.get()) {
        m_inputView.reset();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC viewDesc{};
        viewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        RETURN_IF_FAILED(m_videoDevice->CreateVideoProcessorInputView(input, m_enumerator.get(), &viewDesc, &m_inputView));
        m_inputTexture = input;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = m_inputView.get();
    return m_videoContext->VideoProcessorBlt(m_processor.get(), output, 0, 1, &stream);
}

// -----------------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------------

HRESULT CameraRenderer::Render(IMFSample* sample)
{
    RETURN_HR_IF_NULL(E_POINTER, sample);
    RETURN_HR_IF(MF_E_NOT_INITIALIZED, !m_width || m_format == DXGI_FORMAT_UNKNOWN);

    wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));

    wil::com_ptr_nothrow<ID3D11Device> device;
    RETURN_IF_FAILED(AcquireDevice(&device));
    auto unlock = wil::scope_exit([&] { ReleaseDevice(); });

    wil::com_ptr_nothrow<ID3D11Texture2D> input;
    RETURN_IF_FAILED(PrepareInput(device.get(), &input));
    D3D11_TEXTURE2D_DESC inputDesc{};
    input->GetDesc(&inputDesc);
    RETURN_IF_FAILED(PrepareProcessor(device.get(), inputDesc.Width, inputDesc.Height));

    if (buffer.try_query<IMFDXGIBuffer>())
        return RenderToGpuSample(device.get(), input.get(), buffer.get());
    return RenderToMemorySample(device.get(), input.get(), buffer.get());
}

HRESULT CameraRenderer::RenderToGpuSample(ID3D11Device*, ID3D11Texture2D* input, IMFMediaBuffer* buffer)
{
    auto dxgiBuffer = wil::try_com_query_nothrow<IMFDXGIBuffer>(buffer);
    wil::com_ptr_nothrow<ID3D11Texture2D> texture;
    UINT subresource = 0;
    RETURN_IF_FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture)));
    RETURN_IF_FAILED(dxgiBuffer->GetSubresourceIndex(&subresource));

    wil::com_ptr_nothrow<ID3D11VideoProcessorOutputView> view;
    RETURN_IF_FAILED(OutputViewFor(texture.get(), subresource, &view));
    RETURN_IF_FAILED(Blit(input, view.get()));

    DWORD length = 0;
    if (auto buffer2d = wil::try_com_query_nothrow<IMF2DBuffer>(buffer); buffer2d && SUCCEEDED(buffer2d->GetContiguousLength(&length)))
        buffer->SetCurrentLength(length);
    return S_OK;
}

HRESULT CameraRenderer::RenderToMemorySample(ID3D11Device* device, ID3D11Texture2D* input, IMFMediaBuffer* buffer)
{
    if (!m_target) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = m_format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        RETURN_IF_FAILED(device->CreateTexture2D(&desc, nullptr, &m_target));
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        RETURN_IF_FAILED(device->CreateTexture2D(&desc, nullptr, &m_staging));
    }
    if (!m_targetView)
        RETURN_IF_FAILED(OutputViewFor(m_target.get(), 0, &m_targetView));
    RETURN_IF_FAILED(Blit(input, m_targetView.get()));

    wil::com_ptr_nothrow<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->CopyResource(m_staging.get(), m_target.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    RETURN_IF_FAILED(context->Map(m_staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    auto unmap = wil::scope_exit([&] { context->Unmap(m_staging.get(), 0); });

    // Row layout of the formats we produce: (bytes per row, rows) per plane.
    const UINT lumaBytes = m_format == DXGI_FORMAT_B8G8R8A8_UNORM ? m_width * 4
                         : m_format == DXGI_FORMAT_YUY2 ? m_width * 2 : m_width;
    const bool hasChromaPlane = m_format == DXGI_FORMAT_NV12;

    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    auto buffer2d = wil::try_com_query_nothrow<IMF2DBuffer>(buffer);
    BYTE* locked = nullptr;
    DWORD maxLength = 0;
    if (buffer2d) {
        RETURN_IF_FAILED(buffer2d->Lock2D(&scan0, &pitch));
    } else {
        RETURN_IF_FAILED(buffer->Lock(&locked, &maxLength, nullptr));
        const DWORD needed = lumaBytes * m_height + (hasChromaPlane ? m_width * (m_height / 2) : 0);
        if (maxLength < needed) {
            buffer->Unlock();
            return MF_E_BUFFERTOOSMALL;
        }
        scan0 = locked;
        pitch = (LONG)lumaBytes;
    }

    const BYTE* src = static_cast<const BYTE*>(mapped.pData);
    for (UINT y = 0; y < m_height; y++)
        memcpy(scan0 + (ptrdiff_t)pitch * y, src + (size_t)mapped.RowPitch * y, lumaBytes);
    if (hasChromaPlane) {
        const BYTE* srcChroma = src + (size_t)mapped.RowPitch * m_height;
        BYTE* dstChroma = scan0 + (ptrdiff_t)pitch * m_height;
        for (UINT y = 0; y < m_height / 2; y++)
            memcpy(dstChroma + (ptrdiff_t)pitch * y, srcChroma + (size_t)mapped.RowPitch * y, m_width);
    }

    if (buffer2d) {
        buffer2d->Unlock2D();
        DWORD length = 0;
        if (SUCCEEDED(buffer2d->GetContiguousLength(&length)))
            buffer->SetCurrentLength(length);
    } else {
        buffer->Unlock();
        buffer->SetCurrentLength(lumaBytes * m_height + (hasChromaPlane ? m_width * (m_height / 2) : 0));
    }
    return S_OK;
}
